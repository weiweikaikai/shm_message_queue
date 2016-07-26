/*
 * shm_queue.h
 * Declaration of a shm queue
 *
 *  Created on: 2016.7.10
 *  Author: WK <18402927708@163.com>
 *
 *  Based on transaction pool, features:  基于事务池，特征
 *  1) support single writer but multiple reader processes/threads    支持单写多读进程/线程 
 *  2) support timestamping for each data       支持为每个数据打时间戳
 *  3) support auto detecting and skipping corrupted elements 支持自动检测和跳过损坏的元素
 *  4) support variable user data size  支持可变的用户数据的大小
 *  5) use highly optimized gettimeofday() to speedup sys time  使用高度优化的gettimeofday()加速系统时间
 */
#ifndef __SHM_QUEUE_HEADER__
#define __SHM_QUEUE_HEADER__

#ifndef BOOL
#define BOOL int
#endif

#ifndef NULL
#define NULL 0
#endif

// Switch on this macro for compiling a test program 在编写测试程序，宏开关
#ifndef SQ_FOR_TEST
#define SQ_FOR_TEST	0
#endif

typedef unsigned short u16_t;
typedef unsigned int u32_t;
typedef unsigned long long u64_t;

// Maximum bytes allowed for a queue data 队列数据所允许的最大字节数
#define MAX_SQ_DATA_LENGTH	65536   //2^16

struct sq_head_t;

// Create a shm queue
// Parameters:
//     shm_key      - shm key
//     ele_size     - preallocated size for each element  为每个数据项预分配大小
//     ele_count    - preallocated number of elements     数据项的个数
// Returns a shm queue pointer or NULL if failed    
struct sq_head_t *sq_create(u64_t shm_key, int ele_size, int ele_count);

// Open an existing shm queue for reading data
struct sq_head_t *sq_open(u64_t shm_key);

// Set signal parameters if you wish to enable signaling on data write
// Parameters:
//      sq           - shm_queue pointer returned by sq_create
//      signum       - sig num to be sent to the reader, e.g. SIGUSR1
//      sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//      sig_proc_num - send signal to up to this number of processes once
// Returns 0 on success, < 0 on failure
int sq_set_sigparam(struct sq_head_t *sq, int signum, int sig_ele_num, int sig_proc_num);

// Register the current process ID, so that it will be able to recived signal
// Note: you don't need to unregister the current process ID, it will be removed
// automatically next time register_signal is called if it no longer exists
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns a signal index for sq_sigon/sq_sigoff, or < 0 on failure
int sq_register_signal(struct sq_head_t *sq);

// Turn on/off signaling for current process
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
//      sigindex - returned by sq_register_signal()
// Returns 0 on success, -1 if parameter is bad
int sq_sigon(struct sq_head_t *sq, int sigindex);
int sq_sigoff(struct sq_head_t *sq, int sigindex);

// Destroy queue created by sq_create()
void sq_destroy(struct sq_head_t *queue);

// Add data to end of shm queue
// Returns 0 on success or
//     -1 - invalid parameter
//     -2 - shm queue is full
// Note: here we assume only one process can put to the queue
//     for multi-thread/process support, you need to introduce a lock by yourself
int sq_put(struct sq_head_t *queue, void *data, int datalen);

// Retrieve data
// On success, buf is filled with the first queue data
// this function is multi-thread/multi-process safe
// Returns the data length or
//      0 - no data in queue
//     -1 - invalid parameter
int sq_get(struct sq_head_t *queue, void *buf, int buf_sz, struct timeval *enqueue_time);

// Get usage rate
// Returns a number from 0 to 99
int sq_get_usage(struct sq_head_t *queue);

// Get number of used blocks
int sq_get_used_blocks(struct sq_head_t *queue);

// If a queue operation failed, call this function to get an error reason
const char *sq_errorstr();

#endif

