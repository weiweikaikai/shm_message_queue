/*
 * shm_queue.c
 * Implementation of a shm queue
 *
 *  Created on: 2016.7.10
 *      Author: WK <18402927708@163.com>
 *
 *  Based on implementation of transaction queue
 */
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <dirent.h>
#include <signal.h>
#include "shm_queue.h"

#define START_TOKEN    0x0000db03 // token to martk the valid start of a node

#define MAX_READER_PROC_NUM	64 // maximum allowable processes to be signaled when data arrived

#define CAS32(ptr, val_old, val_new)({ char ret; __asm__ __volatile__("lock; cmpxchgl %2,%0; setz %1": "+m"(*ptr), "=q"(ret): "r"(val_new),"a"(val_old): "memory"); ret;})

static char errmsg[256];

const char *sq_errorstr()
{
	return errmsg;
}

struct sq_node_head_t
{
	u32_t start_token; // 0x0000db03, if the head position is corrupted, find next start token
	u32_t datalen; // length of stored data in this node
	struct timeval enqueue_time;

	// the actual data are stored here
	unsigned char data[0];

} __attribute__((packed));

struct sq_head_t
{
	int ele_size;
	int ele_count;

	volatile int head_pos; // head position in the queue, pointer for reading
	volatile int tail_pos; // tail position in the queue, pointer for writting

	int data_signum; // signum to send to the reader processes if requested
	int sig_node_num; // send signal to processes when data node excceeds this count
	int sig_process_num; // send signal to up to this number of processes each time

	volatile int pidnum; // number of processes currently registered for signal delivery
	volatile pid_t pidset[MAX_READER_PROC_NUM]; // registered pid list
	volatile uint8_t sigmask[(MAX_READER_PROC_NUM+7)/8]; // bit map for pid waiting on signal

	struct sq_node_head_t nodes[0];
};

// Increase head/tail by val
#define SQ_ADD_HEAD(queue, val) 	(((queue)->head_pos+(val))%((queue)->ele_count+1))
#define SQ_ADD_TAIL(queue, val) 	(((queue)->tail_pos+(val))%((queue)->ele_count+1))

// Next position after head/tail
#define SQ_NEXT_HEAD(queue) 	SQ_ADD_HEAD(queue, 1)
#define SQ_NEXT_TAIL(queue) 	SQ_ADD_TAIL(queue, 1)

#define SQ_ADD_POS(queue, pos, val)     (((pos)+(val))%((queue)->ele_count+1))

#define SQ_IS_QUEUE_FULL(queue) 	(SQ_NEXT_TAIL(queue)==(queue)->head_pos)
#define SQ_IS_QUEUE_EMPTY(queue)	((queue)->tail_pos==(queue)->head_pos)

#define SQ_EMPTY_NODES(queue) 	(((queue)->head_pos+(queue)->ele_count-(queue)->tail_pos) % ((queue)->ele_count+1))
#define SQ_USED_NODES(queue) 	((queue)->ele_count - SQ_EMPTY_NODES(queue))

#define SQ_EMPTY_NODES2(queue, head) (((head)+(queue)->ele_count-(queue)->tail_pos) % ((queue)->ele_count+1)) 
#define SQ_USED_NODES2(queue, head) ((queue)->ele_count - SQ_EMPTY_NODES2(queue, head))

// The size of a node
#define SQ_NODE_SIZE_ELEMENT(ele_size)	(sizeof(struct sq_node_head_t)+ele_size)
#define SQ_NODE_SIZE(queue)            	(SQ_NODE_SIZE_ELEMENT((queue)->ele_size))

// Convert an index to a node_head pointer
#define SQ_GET(queue, idx) ((struct sq_node_head_t *)(((char*)(queue)->nodes) + (idx)*SQ_NODE_SIZE(queue)))

// Estimate how many nodes are needed by this length
#define SQ_NUM_NEEDED_NODES(queue, datalen) 	((datalen) + sizeof(struct sq_node_head_t) + SQ_NODE_SIZE(queue) -1) / SQ_NODE_SIZE(queue)


// optimized gettimeofday
#include "opt_time.h"

static inline int is_pid_valid(pid_t pid)
{
	if(pid==0) return 0;

	char piddir[256];
	snprintf(piddir, sizeof(piddir), "/proc/%u", pid);
	DIR *d = opendir(piddir);
	if(d==NULL)
		return 0;
	closedir(d);
	return 1;
}

static inline void verify_and_remove_bad_pids(struct sq_head_t *sq)
{
	int i;
	int oldpidnum = (int)sq->pidnum;
	int newpidnum = oldpidnum;
	// test and remove invalid pids so that they won't be signaled
	if(newpidnum<0 || newpidnum>MAX_READER_PROC_NUM)
	{
		newpidnum = MAX_READER_PROC_NUM;
		if(!CAS32(&sq->pidnum, oldpidnum, newpidnum))
			return;
	}
	for(i=newpidnum-1; i>=0 && !is_pid_valid((pid_t)sq->pidset[i]); i--)
	{
		sq_sigoff(sq, i);
		if(!CAS32(&sq->pidnum, i+1, i)) // conflict detected
			break;
	}
	for(i--; i>=0; i--)
	{
		pid_t oldpid = (pid_t)sq->pidset[i];
		if(!is_pid_valid(oldpid))
		{
			sq_sigoff(sq, i);
			CAS32(&sq->pidset[i], oldpid, 0); // if conflict occurs, simply ignore it
		}
	}
}


// Set signal parameters if you wish to enable signaling on data write
// Parameters:
//      sq           - shm_queue pointer returned by sq_create
//      signum       - sig num to be sent to the reader, e.g. SIGUSR1
//      sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//      sig_proc_num - send signal to up to this number of processes once
// Returns 0 on success, < 0 on failure
int sq_set_sigparam(struct sq_head_t *sq, int signum, int sig_ele_num, int sig_proc_num)
{
	sq->data_signum = signum;
	sq->sig_node_num = sig_ele_num;
	sq->sig_process_num = sig_proc_num;
	verify_and_remove_bad_pids(sq);

	if(sq->pidnum>0) // print the registered pids
	{
		int i;
		printf("Registered pids: %u", (uint32_t)sq->pidset[0]);
		for(i=1; i<sq->pidnum; i++)
			printf(", %u", (uint32_t)sq->pidset[i]);
		printf("\n");
	}

	return 0;
}

// Register the current process ID, so that it will be able to recived signal
// Note: you don't need to unregister the current process ID, it will be removed
// automatically next time register_signal is called if it no longer exists
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns a signal index for sq_sigon/sq_sigoff, or < 0 on failure
int sq_register_signal(struct sq_head_t *sq)
{
	pid_t pid = getpid();
	verify_and_remove_bad_pids(sq);

	int i;
	for(i=0; i<sq->pidnum; i++)
	{
		if(!sq->pidset[i])
		{
			// if i is taken by someone else, try next
			// else set pidset[i] to our pid and return i
			if(CAS32(&sq->pidset[i], 0, pid))
				return i;
		}
	}

	while(1) // CAS loop
	{
		int pidnum = (int)sq->pidnum;
		if(pidnum>=MAX_READER_PROC_NUM)
		{
			snprintf(errmsg, sizeof(errmsg), "pid num exceeds maximum of %u", MAX_READER_PROC_NUM);
			return -1;
		}
		if(CAS32(&sq->pidnum, pidnum, pidnum+1))
		{
			sq->pidset[pidnum] = (volatile pid_t)pid;
			return pidnum;
		}
	}
}


// Turn on/off signaling for current process
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
//      sigindex - returned by sq_register_signal()
// Returns 0 on success, -1 if parameter is bad
int sq_sigon(struct sq_head_t *sq, int sigindex)
{
	if((uint32_t)sigindex<(uint32_t)sq->pidnum)
	{
		__sync_fetch_and_or(sq->sigmask+(sigindex/8), (uint8_t)1<<(sigindex%8));
		return 0;
	}
	snprintf(errmsg, sizeof(errmsg), "sigindex is invalid");
	return -1;
}

int sq_sigoff(struct sq_head_t *sq, int sigindex)
{
	if((uint32_t)sigindex<(uint32_t)sq->pidnum)
	{
		__sync_fetch_and_and(sq->sigmask+(sigindex/8), (uint8_t)~(1U<<(sigindex%8)));
		return 0;
	}
	snprintf(errmsg, sizeof(errmsg), "sigindex is invalid");
	return -1;
}


// shm operation wrapper
static char *attach_shm(long iKey, long iSize, int iFlag)
{
	int shmid;
	char* shm;

	if((shmid=shmget(iKey, iSize, iFlag)) < 0)
	{
		printf("shmget(key=%ld, size=%ld): %s\n", iKey, iSize, strerror(errno)); 
		return NULL;
	}

	if((shm=shmat(shmid, NULL ,0))==(char *)-1)
	{
		perror("shmat");
		return NULL;
	}
/*
	// avoid swapping
	if(mlock(shm, iSize)<0)
	{
		perror("mlock");
		shmdt(shm);
		return NULL;
	}
*/
	return shm;
}

// shm operation wrapper
static struct sq_head_t *open_shm_queue(long shm_key, long ele_size, long ele_count, int create)
{
	long allocate_size;
	struct sq_head_t *shm;

	if(create)
	{
		ele_size = (((ele_size + 7)>>3) << 3); // align to 8 bytes
		// We need an extra element for ending control
		allocate_size = sizeof(struct sq_head_t) + SQ_NODE_SIZE_ELEMENT(ele_size)*(ele_count+1);
		// Align to 4MB boundary
		allocate_size = (allocate_size + (4UL<<20) - 1) & (~((4UL<<20)-1));
		printf("shm size needed for queue - %lu.\n", allocate_size);
	}
	else
	{
		allocate_size = 0;
	}

	if (!(shm = (struct sq_head_t *)attach_shm(shm_key, allocate_size, 0666)))
	{
		if (!create) return NULL;
		if (!(shm = (struct sq_head_t *)attach_shm(shm_key, allocate_size, 0666|IPC_CREAT)))
			return NULL;

		memset(shm, 0, allocate_size);
		shm->ele_size = ele_size;
		shm->ele_count = ele_count;
		return shm;
	}
	else if(create) // verify parameters if open for writing
	{
		if(shm->ele_size!=ele_size || shm->ele_count!=ele_count)
		{
			printf("shm parameters mismatched: \n");
			printf("    given:  ele_size=%ld, ele_count=%ld\n", ele_size, ele_count);
			printf("    in shm: ele_size=%d, ele_count=%d\n", shm->ele_size, shm->ele_count);
			shmdt(shm);
			return NULL;
		}
	}

	return shm;
}


// Create a shm queue
// Parameters:
//     shm_key      - shm key
//     ele_size     - preallocated size for each element
//     ele_count    - preallocated number of elements
// Returns a shm queue pointer or NULL if failed
struct sq_head_t *sq_create(u64_t shm_key, int ele_size, int ele_count)
{
	struct sq_head_t *queue;

	if(ele_size<=0 || ele_count<=0 || shm_key<=0) // invalid parameter
	{
		snprintf(errmsg, sizeof(errmsg), "Bad argument");
		return NULL;
	}

	queue = open_shm_queue(shm_key, ele_size, ele_count, 1);
	if(queue==NULL)
	{
		snprintf(errmsg, sizeof(errmsg), "Get shm failed");
		return NULL;
	}
	return queue;
}

// Open an existing shm queue for reading data
struct sq_head_t *sq_open(u64_t shm_key)
{
	struct sq_head_t *queue = open_shm_queue(shm_key, 0, 0, 0);
	if(queue==NULL)
	{
		snprintf(errmsg, sizeof(errmsg), "Open shm failed");
		return NULL;
	}
	return queue;
}

// Destroy TP created by sq_create()
void sq_destroy(struct sq_head_t *queue)
{
	shmdt(queue);
	// do nothing for now
}


// Add data to end of shm queue
// Returns 0 on success or
//     -1 - invalid parameter
//     -2 - shm queue is full
int sq_put(struct sq_head_t *queue, void *data, int datalen)
{
	u32_t idx;
	struct sq_node_head_t *node;
	int nr_nodes;
	int new_tail;

	if(queue==NULL || data==NULL || datalen<=0 || datalen>MAX_SQ_DATA_LENGTH)
	{
		snprintf(errmsg, sizeof(errmsg), "Bad argument");
		return -1;
	}

	// calculate the number of nodes needed
	nr_nodes = SQ_NUM_NEEDED_NODES(queue, datalen);

	if(SQ_EMPTY_NODES(queue)<nr_nodes)
	{
		snprintf(errmsg, sizeof(errmsg), "Not enough for new data");
		return -2;
	}

	idx = queue->tail_pos;
	node = SQ_GET(queue, idx);
	new_tail = SQ_ADD_TAIL(queue, nr_nodes);

	if(new_tail < queue->tail_pos) // wrapped back
	{
		// We need a set of continuous nodes
		// So skip the empty nodes at the end, and begin allocation at index 0
		idx = 0;
		new_tail = nr_nodes;
		node = SQ_GET(queue, 0);

		if(queue->head_pos-1 < nr_nodes)
		{
			snprintf(errmsg, sizeof(errmsg), "Not enough for new data");
			return -2; // not enough empty nodes
		}
	}

	// initialize the new node
	node->start_token = START_TOKEN;
	node->datalen = datalen;
	opt_gettimeofday(&node->enqueue_time, NULL);
	memcpy(node->data, data, datalen);
	queue->tail_pos = new_tail;

	// now signal the reader wait on queue
	if(queue->data_signum && // needs signaling
		SQ_USED_NODES(queue)>=queue->sig_node_num) // element num reached
	{
		int i, nr;
		// signal at most queue->sig_process_num processes
		for(i=0,nr=0; i<(int)queue->pidnum && nr<queue->sig_process_num; i++)
		{
			if(queue->pidset[i] && queue->sigmask[i/8] & 1<<(i%8))
			{
				kill((pid_t)queue->pidset[i], SIGUSR1);
				nr ++;
				sq_sigoff(queue, i); // avoids being signaled again
			}
		}
	}
	return 0;
}

int sq_get_usage(struct sq_head_t *queue)
{
	return queue->ele_count? ((SQ_USED_NODES(queue))*100)/queue->ele_count : 0;
}

int sq_get_used_blocks(struct sq_head_t *queue)
{
	return SQ_USED_NODES(queue);
}

// Retrieve data
// On success, buf is filled with the first queue data
// Returns the data length or
//     0  - no data in queue
//     -1 - invalid parameter
int sq_get(struct sq_head_t *queue, void *buf, int buf_sz, struct timeval *enqueue_time)
{
	struct sq_node_head_t *node;

	int nr_nodes, datalen;
	int old_head, new_head, head;

	if(queue==NULL || buf==NULL || buf_sz<1)
	{
		snprintf(errmsg, sizeof(errmsg), "Bad argument");
		return -1;
	}

	head = old_head = queue->head_pos;
	do
	{
		if(queue->tail_pos==head) // end of queue
		{
			if(head!=old_head && CAS32(&queue->head_pos, old_head, head))
			{
				new_head = head;
				datalen = 0;
				break;
			}
			// head_pos not advanced or changed by someone else, simply returns
			return 0;
		}

		node = SQ_GET(queue, head);
		if(node->start_token!=START_TOKEN)
		{
			head = SQ_ADD_POS(queue, head, 1);
			continue;
		}
		datalen = node->datalen;
		nr_nodes = SQ_NUM_NEEDED_NODES(queue, datalen);
		if(SQ_USED_NODES2(queue, head) < nr_nodes)
		{
			head = SQ_ADD_POS(queue, head, 1);
			continue;
		}
		new_head = SQ_ADD_POS(queue, head, nr_nodes);
		if(CAS32(&queue->head_pos, old_head, new_head))
		{
			if(enqueue_time)
				*enqueue_time = node->enqueue_time;
			if(datalen > buf_sz)
			{
				snprintf(errmsg, sizeof(errmsg), "Data length(%u) exceeds supplied buffer size of %u", datalen, buf_sz);
				return -2;
			}
			memcpy(buf, node->data, datalen);
			break;
		}
		else // head_pos changed by someone else, start over
		{
			old_head = queue->head_pos;
			head = old_head;
		}
	} while(1);

	while(old_head!=new_head)
	{
		node = SQ_GET(queue, old_head);
		// reset start_token so that this node will not be treated as a starting node of data
		node->start_token = 0;
		old_head = SQ_ADD_POS(queue, old_head, 1);
	}

	return datalen;
}

#if SQ_FOR_TEST

//
// Below is a test program, please compile with:
// gcc -o sqtest -DSQ_FOR_TEST shm_queue.c
//

void test_put(struct sq_head_t *queue, int count, char *msg)
{
	int msg_len = strlen(msg);
	int i;

	// set signal parameters, this is generally done by the writter
	sq_set_sigparam(queue, SIGUSR1, 1, 1);

	for(i=0; i<count; i++)
	{
		static char m[1024*1024];
		sprintf(m, "[%d] %s", i, msg);
		if(sq_put(queue, m, strlen(m))<0)
		{
			printf("put msg[%d] failed: %s\n", i, sq_errorstr());
			return;
		}
	}
	printf("put successfully\n");
}

// dummy signal handler, for only purpose of not being killed by SIGUSR1
void siguser1(int signo)
{
   (void)signo;
}

void test_get(struct sq_head_t *queue, int proc_count, int count)
{
	int i;
	int pid = 0;
	static char m[1024*1024];


	for(i=1; i<=proc_count; i++)
	{
		if(fork()==0)
		{
			pid=i;
			break;
		}
	}

	// Note: the reader process should register a handler for signum(SIGUSR1)
	// beforer registering signal in shm queue
	signal(SIGUSR1, siguser1);
	// If your server forks several processes, register should be called after fork()
	int sigindex = sq_register_signal(queue);

	if(pid)
	{
		for(i=0; i<count; i++)
		{
			struct timeval tv;
			int l = sq_get(queue, m, sizeof(m), &tv);
			if(l<0)
			{
				printf("sq_get failed: %s\n", sq_errorstr());
				break;
			}
			if(l==0)
			{
				sq_sigon(queue, sigindex); // now we are entering into signalable sleep
				sleep(10);
				sq_sigoff(queue, sigindex); // no longer needs signal
				i --;
				continue;
			}
			// if we are able to retrieve data from queue, always
			// try it without sleeping
			m[l] = 0;
			printf("pid[%d] msg[%d] len[%d]: %s\n", pid, i, l, m);
		}
		exit(0);
	}
	while(wait(NULL)>0);
}

int main(int argc, char *argv[])
{
	struct sq_head_t *queue;
	long key;

	if(argc<3)
	{
badarg:
		printf("usage: \n");
		printf("     %s open <key>\n", argv[0]);
		printf("     %s create <key> <element_size> <element_count>\n", argv[0]);
		printf("\n");
		return -1;
	}

	if(strncasecmp(argv[2], "0x", 2)==0)
		key = strtoul(argv[2]+2, NULL, 16);
	else
		key = strtoul(argv[2], NULL, 10); 

	if(strcmp(argv[1], "open")==0)
	{
		queue = sq_open(key);
	}
	else if(strcmp(argv[1], "create")==0 && argc==5)
	{
		queue = sq_create(key, strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10));
	}
	else
	{
		goto badarg;
	}

	if(queue==NULL)
	{
		printf("failed to open shm queue: %s\n", sq_errorstr());
		return -1;
	}

	while(1)
	{
		static char cmd[1024*1024];
		printf("available commands: \n");
		printf("  put <msg_count> <msg>\n");
		printf("  get <concurrent_proc_count> <msg_count>\n");
		printf("  quit\n");
		printf("cmd>"); fflush(stdout);
		if(gets(cmd)==NULL)
			return 0;
		if(strncmp(cmd, "put ", 4)==0)
		{
			char *pstr = cmd + 4;
			while(isspace(*pstr)) pstr ++;
			int count = atoi(pstr);
			if(count<1) count = 1;
			while(isdigit(*pstr)) pstr ++;
			while(isspace(*pstr)) pstr ++;
			test_put(queue, count, pstr);
		}
		else if(strncmp(cmd, "get ", 4)==0)
		{
			char *pstr = cmd + 4;
			while(isspace(*pstr)) pstr ++;
			int proc_count = atoi(pstr);
			if(proc_count<1) proc_count = 1;
			while(isdigit(*pstr)) pstr ++;
			while(isspace(*pstr)) pstr ++;
			int count = atoi(pstr);
			if(count<1) count = 1;
			test_get(queue, proc_count, count);
		}
		else if(strncmp(cmd, "quit", 4)==0 || strncmp(cmd, "exit", 4)==0)
		{
			return 0;
		}
	}
	return 0;
}

#endif
