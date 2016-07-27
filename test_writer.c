/*************************************************************************
	> File Name: test_writer.c
	> Author: wk
	> Mail: 18402927708@163.com
	> Created Time: Tue 26 Jul 2016 06:04:54 PM CST
 ************************************************************************/


#include"shm_queue.h"
 #include<signal.h>
int main()
{
    // 创建或打开队列
	long shmkey = 0x1234;
	int element_size = 64; // 基本块的大小，如果数据小于1个块，就按1个块存储，否则，存储到连续多个块中，只有第一个块有块头部信息
	int element_count = 1024; // 队列的长度，总共有多少个块
	struct sq_head_t *sq = sq_create(0x1234, element_size, element_count);

	// 如果需要开启signal通知功能，设置一下通知的参数
	sq_set_sigparam(sq, SIGUSR1, 1, 2);

	// 现在可以开始写数据了
	char *data = "hello hello I am writer";
	if(sq_put(sq, data, strlen(data))<0)
	{
		// 队列满了。。。
	}
      sq_destroy(sq);
	return 0;
}

