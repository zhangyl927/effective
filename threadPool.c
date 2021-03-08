#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "threadPool.h"

#define true 1
#define false 0
#define DEFAULT_TIME 10

int is_thread_alive(pthread_t tid);

int threadpool_free(threadpool_t *pool);


/* 线程池中的工作线程 */
void *threadpool_thread(void *threadpool)
{
	threadpool_t *pool = (threadpool_t *)threadpool;
	threadpool_task_t task;

	while (true) {
		/* 刚创建线程，阻塞等待，当任务队列上有任务时唤醒接收*/
		pthread_mutex_lock(&(pool->lock));

		/* queue_size == 0 时队列中没有任务，阻塞等待 */
		while (pool->queue_size==0 && !(pool->shutdown)) {
			printf("thread 0x%x is waiting\n", (unsigned int)pthread_self());			/* 打印线程自身的ID */
			pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));					/* 1、阻塞 2、释放锁 3、加锁 */

			/* 清除指定数目的空闲线程，如要结束的线程个数大于0， 结束线程 */
			if (pool->wait_exit_thr_num > 0) {
				pool->wait_exit_thr_num--;              // ??????????????

				/* 如果线程池里线程个数大于最小值时(表示已扩容过)，可以结束当前线程 */
				if (pool->live_thr_num > pool->min_thr_num) {
					printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
					pool->live_thr_num--;												/* 存活线程数 -1 */
					pthread_mutex_unlock(&(pool->lock));

					pthread_exit(NULL);													/* 线程退出 */
				}
			}
		}

		/* 如果指定了true, 要关闭线程池里德每个线程，自行退出处理 --销毁线程池 */
		if (pool->shutdown) {
			pthread_mutex_unlock(&(pool->lock));
			printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
			pthread_detach(pthread_self());					                            /* 分离子线程，运行完自动释放资源 */
			pthread_exit(NULL);															/* 线程退出 */
		}

		/* 从任务队列里获取任务，出队操作 */
		task.function = pool->task_queue[pool->queue_front].function;
		task.arg = pool->task_queue[pool->queue_front].arg;

		pool->queue_front = (pool->queue_front+1)%(pool->queue_max_size);				/* 出队，模拟环形队列 */
		pool->queue_size--;

		/* 通知可以有新的任务添加进来 */
		pthread_cond_broadcast(&(pool->queue_not_full));

		/* 任务取出后， 释放线程池锁 */
		pthread_mutex_unlock(&(pool->lock));

		/* 执行任务 */
		printf("thread 0x%x start working\n", (unsigned int)pthread_self());
		pthread_mutex_lock(&(pool->thread_counter));
		pool->busy_thr_num++;															/* 忙状态线程数 +1 */
		pthread_mutex_unlock(&(pool->thread_counter));

		(*(task.function))(task.arg);													/* 执行回调函数 */

		/* 任务结束处理 */
		printf("thread 0x%x end working\n", (unsigned int)pthread_self());
		pthread_mutex_lock(&(pool->thread_counter));
		pool->busy_thr_num--;															/* 处理掉一个任务，忙线程 -1 */
		pthread_mutex_unlock(&(pool->thread_counter));
	}

	return NULL;
}


/*管理线程*/
void *adjust_thread(void *threadpool)
{
	threadpool_t *pool =  (threadpool_t *)threadpool;
	while (!pool->shutdown) {
		sleep(DEFAULT_TIME);							/* 定时，管理线程每隔10s 唤醒管理线程池*/

		pthread_mutex_lock(&(pool->lock));
		int queue_size = pool->queue_size;				/* 任务数 */
		int live_thr_num = pool->live_thr_num;			/* 存活线程数 */
		pthread_mutex_unlock(&(pool->lock));

		pthread_mutex_lock(&(pool->thread_counter));
		int busy_thr_num = pool->busy_thr_num;			/* 繁忙线程数 */
		pthread_mutex_unlock(&(pool->thread_counter));

		/* 创建新线程*/
		if (queue_size>=10 && live_thr_num<pool->max_thr_num) {
			pthread_mutex_lock(&(pool->lock));
			int k = 0;

			/* 一次增加 10 个线程*/
			for (int i=0; i<pool->max_thr_num && k<10 && pool->live_thr_num<pool->max_thr_num; ++i) {
				if (pool->threads[i]==0 || !is_thread_alive(pool->threads[i])) {
					pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);
					k++;
					pool->live_thr_num++;
				}
			}

			pthread_mutex_unlock(&(pool->lock));
		}

		/* 销毁空闲线程, live_thr_num>pool->min_thr_num 表明 已经扩增过了，当有空闲线程可以销毁； */
		if (busy_thr_num*2<live_thr_num && live_thr_num>pool->min_thr_num) {
			/* 一次销毁 10 个线程，随机 10 个*/
			pthread_mutex_lock(&(pool->lock));
			pool->wait_exit_thr_num = 10;
			pthread_mutex_unlock(&(pool->lock));

			for (int i=0; i<10; ++i) {
				/*通知处于空闲状态的线程，唤醒 threadpool_thread 中阻塞在 队列不为空 上的线程，在其函数中自己销毁*/
				pthread_cond_signal(&(pool->queue_not_empty));
			}
		}
	}

	return NULL;
}


/* 创建线程池 */
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size)
{
	int i;
	threadpool_t *pool = NULL;

	do {
		if ((pool=(threadpool_t*)malloc(sizeof(threadpool_t))) == NULL) {
			printf("malloc threadpool fail");
			break;
		}

		pool->min_thr_num = min_thr_num;
		pool->max_thr_num = max_thr_num;
		pool->live_thr_num = min_thr_num;
		pool->busy_thr_num = 0;
		pool->wait_exit_thr_num = 0;
		pool->queue_front = 0;
		pool->queue_rear = 0;
		pool->queue_max_size = queue_max_size;
		pool->shutdown = false;

		/* 根据最大线程数，开辟工作线程数组，并清零 */
		pool->threads = (pthread_t*)malloc(sizeof(pthread_t)*max_thr_num);
		if (pool->threads == NULL) {
			printf("malloc threads fail");
			break;
		}
		memset(pool->threads, 0, sizeof(pthread_t)*max_thr_num);

		/* 给任务队列开辟空间 */
		pool->task_queue = (threadpool_task_t*)malloc(sizeof(threadpool_task_t)*queue_max_size);
		if (pool->task_queue == NULL) {
			printf("malloc task_queue fail");
			break;
		}

		/* 初始化互斥锁和条件变量 */
		if (pthread_mutex_init(&(pool->lock), NULL) != 0
		  	|| pthread_mutex_init(&(pool->thread_counter), NULL) != 0
		  	|| pthread_cond_init(&(pool->queue_not_full), NULL) != 0
		  	|| pthread_cond_init(&(pool->queue_not_empty), NULL) != 0) 
		{
			printf("init the lock or cond fail");
			break;
		}

		/* 启动 min_thr_num 个 work thread 和一个管理者线程 */
		for (int i=0; i<min_thr_num; ++i)
		{
			pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void*)pool);		/* pool 指向当前线程池 */
			printf("start thread 0x%x ...\n", (unsigned int)pool->threads[i]);
		}
		pthread_create(&(pool->adjust_tid), NULL, adjust_thread, (void*)pool);				/* 创建管理者线程 */

		return pool;

	} while (0);		/* while (0) 可用于跳出，while(0)+break 代替 goto; */

	threadpool_free(pool);
	return NULL;
}


/* 向线程池中添加任务 */
int threadpool_add(threadpool_t *pool, void* (*function)(void* arg), void *arg)
{
	pthread_mutex_lock(&(pool->lock));

	/* 队列为满时阻塞 */
	while ((pool->queue_size==pool->queue_max_size) && !(pool->shutdown))
	{
		pthread_cond_wait(&(pool->queue_not_full), &(pool->lock));
	}

	if (pool->shutdown) {
		pthread_cond_broadcast(&(pool->queue_not_empty));	   /* broadcast 惊群效应, 唤醒所有阻塞在队列上的线程，让其自动销毁  */
		pthread_mutex_unlock(&(pool->lock));
		return 0;
	}

	/* 清空工作线程 调用的回调函数 的参数arg */
	if (pool->task_queue[pool->queue_rear].arg != NULL) {
        free(pool->task_queue[pool->queue_rear].arg);
		pool->task_queue[pool->queue_rear].arg == NULL;
	}

	/* 添加任务到任务队列里 */
	pool->task_queue[pool->queue_rear].function = function;
	pool->task_queue[pool->queue_rear].arg = arg;
	pool->queue_rear = (pool->queue_rear+1)%(pool->queue_max_size);             /* 队尾指针移动，模拟环形 */
	pool->queue_size++;

	/* 添加完任务后，队列不为空，唤醒线程池中等待处理任务的线程 */
	pthread_cond_signal(&(pool->queue_not_empty));
	pthread_mutex_unlock(&(pool->lock));

	return 0;
}


/* 销毁线程 */
int threadpool_destroy(threadpool_t *pool)
{
	int i;
	if (pool == NULL) return -1;
	pool->shutdown = true;

	/* 销毁管理线程 */
	pthread_join(pool->adjust_tid, NULL);

	for (i = 0; i<pool->live_thr_num; ++i) {
		/* 通知所有空闲线程，唤醒 threadpool_thread 中阻塞在 队列不为空 上的线程，在其函数中自己销毁*/
		pthread_cond_broadcast(&(pool->queue_not_empty));
	}

	for (int i=0; i<pool->live_thr_num; ++i) {
		pthread_join(pool->threads[i], NULL);			/* 主线程阻塞，等待其他线程退出  */
	}

	threadpool_free(pool);								/* 释放资源 */

	return 0;
}

/* 释放线程池资源 */
int threadpool_free(threadpool_t *pool)
{
	if (pool == NULL) return -1;

	if (pool->task_queue) {
		free(pool->task_queue);
	}
	if (pool->threads) {
		free(pool->threads);
		pthread_mutex_lock(&(pool->lock));
		pthread_mutex_destroy(&(pool->lock));		/*??????????*/
		pthread_mutex_lock(&(pool->thread_counter));
		pthread_mutex_destroy(&(pool->thread_counter));
		pthread_cond_destroy(&(pool->queue_not_empty));
		pthread_cond_destroy(&(pool->queue_not_full));
	}

	free(pool);
	pool = NULL;

	return 0;
}

/* 测试线程是否存活 */
int is_thread_alive(pthread_t tid)
{
	int kill_rc = pthread_kill(tid, 0);			/* 发 0号信号，测试线程是否存活 */
	if (kill_rc == 0) {
		return false;
	}

	return true;
}

/* 线程池中的线程，模拟处理业务 */
void *process(void *arg)
{
	printf("thread 0x%x working on task %d\n", (unsigned int)pthread_self(), *(unsigned int*)arg);
	sleep(1);												/* 模拟小写转大写 */
	printf("task %d is end\n", *(unsigned int*)arg);

	return NULL;
}

int main(void)
{
	threadpool_t *thp = threadpool_create(3, 100, 100);				/* 创建线程池，池中最小 3 个进程，最大 100，队列最大100 */
	printf("pool inited");

	int num[20];												   
	for (int i=0; i<20; ++i)										/* 模拟任务产生 */
	{
		num[i] = i;
		printf("add task %d\n", i);

		threadpool_add(thp, process, (void*)&num[i]);				/* 向线程池添加任务 */
	}

	sleep(10);														/* 等子线程完成任务 */

	threadpool_destroy(thp);

	return 0;
}