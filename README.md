带signal无锁共享内存队列使用手册


简单使用手册，具体请参考 shm_queue.c 最后面的例子。

写者：

	// 创建或打开队列
	long shmkey = 0x1234;
	int element_size = 64; // 基本块的大小，如果数据小于1个块，就按1个块存储，否则，存储到连续多个块中，只有第一个块有块头部信息
	int element_count = 1024; // 队列的长度，总共有多少个块
	struct sq_head_t *sq = sq_create(0x1234, element_size, element_count);

	// 如果需要开启signal通知功能，设置一下通知的参数
	sq_set_sigparam(sq, SIGUSR1, 1, 1);
	// 现在可以开始写数据了
	char *data = "要写什么就写什么，毫不犹豫";
	if(sq_put(sq, data, strlen(data))<0)
	{
		// 队列满了。。。
	}
读者：


	void siguser1(int signo) // dummy signal handler
	{
	   (void)signo;
	}
	// 打开共享内存队列
	struct sq_head_t *sq = sq_open(0x1234); //shmkey=0x1234
	// 如果需要signal通知，需要向系统注册一个signal handler
	signal(SIGUSR1, siguser1);
	// 向队列注册我们的pid以便接收通知
	// 如果你的进程需要fork多个进程，一定好保证在sq_register_signal()调用之前进行
	int sigindex = sq_register_signal(sq);
	// 进入读循环
	while(1)
	{
		char buffer[1024];
		int len = sq_get(sq, buffer, sizeof(buffer));
		if(len<0) // 读失败
		{
		}
		else if(len==0) // 没有数据，继续做其它操作，然后等待，这里可以进入select/epoll_wait等待
		{
			sq_sigon(sq, sigindex); // 打开signal通知
			sleep(10);
			sq_sigoff(sq, sigindex); // 关闭signal通知
		}
		else // 收到数据了
		{
		}
	}



TODO:
  
 在shm_queue.c中的函数attach_shm()中
   为了防止这段内存被操作系统swap掉。并且由于此操作风险高，仅超级用户可以执行。
系统调用 mlock 家族允许程序在物理内存上锁住它的部分或全部地址空间。
这将阻止Linux 将这个内存页调度到交换空间（swap space），即使该程序已有一段时间没有访问这段空间。
一个严格时间相关的程序可能会希望锁住物理内存，因为内存页面调出调入的时间延迟可能太长或过于不可预知。
安全性要求较高的应用程序可能希望防止敏感数据被换出到交换文件中，因为这样在程序结束后，攻击者可能从交换文件中恢复出这些数据。


需注意的是，仅分配内存并调用 mlock 并不会为调用进程锁定这些内存，
因为对应的分页可能是写时复制（copy-on-write）的。
因此，你应该在每个页面中写入一个假的值：

eg:
const int alloc_size = 32 * 1024 * 1024;
char* memory = malloc (alloc_size); 
mlock (memory, alloc_size);
size_t i; size_t page_size = getpagesize (); 
for (i = 0; i < alloc_size; i += page_size) 
memory[i] = 0;


 这样针对每个内存分页的写入操作会强制 Linux 为当前进程分配一个独立、私有的内存页。

要解除锁定，可以用同样的参数调用 munlock。
如果你希望程序的全部地址空间被锁定在物理内存中，请用 mlockall。
这个系统调用接受一个参数；如果指定 MCL_CURRENT，则仅仅当前已分配的内存会被锁定，
之后分配的内存则不会；MCL_FUTURE 则会锁定之后分配的所有内存。
使用 MCL_CURRENT|MCL_FUTURE 将已经及将来分配的所有内存锁定在物理内存中。
锁定大量的内存，尤其是通过 mlockall，对整个系统而言可能是危险的。
不加选择的内存加锁会把您的系统折磨到死机，因为其余进程被迫争夺更少的资源的使用权，
并且会更快地被交换进出物理内存（这被称之为 thrashing）。
如果你锁定了太多的内存，Linux 系统将整体缺乏必需的内存空间并开始杀死进程。

出于这个原因，只有具有超级用户权限的进程才能利用 mlock 或 mlockall 锁定内存。
如果一个并无超级用户权限的进程调用了这些系统调用将会失败、得到返回值 -1 并得到 errno 错误号 EPERM
munlock 系统调用会将当前进程锁定的所有内存解锁，包括经由 mlock 或 mlockall 锁定的所有区间。
具体参考：http://blog.csdn.net/wangpengqi/article/details/16341935