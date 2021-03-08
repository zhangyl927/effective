#include <pthread.h>

typedef struct {
	void *(*function)(void *);					/* 函数指针，回调函数 */
	void *arg;									/* 回调函数参数*/
} threadpool_task_t;							/* 任务结构体*/


/* 线程池相关信息*/
typedef struct {
	pthread_mutex_t lock;						/* 锁住本结构体*/
	pthread_mutex_t thread_counter;				/* 记录忙状态线程个数的锁*/

	pthread_cond_t queue_not_full;				/* 当任务队列满时，阻塞*/
	pthread_cond_t queue_not_empty;				/* 当任务队列空时，阻塞*/

	pthread_t *threads;							/* 数组：存放线程池中每个线程的 tid*/
	pthread_t adjust_tid;						/* 管理线程 tid */
	threadpool_task_t *task_queue;				/* 任务队列(数组首地址)*/

	int min_thr_num;							/* 线程池最小线程数*/
	int max_thr_num;							/* 线程池最大线程数*/
	int live_thr_num;							/* 当前存活线程数*/
	int busy_thr_num;							/* 当前繁忙线程数*/
	int wait_exit_thr_num;						/* 要销毁的线程个数*/

	int queue_front;							/* task_queue 队头下标*/
	int queue_rear;								/* task_queue 对尾下标*/
	int queue_size;								/* 队列中实际任务数*/
	int queue_max_size;							/* 队列容量*/

	int shutdown;								/* 线程池使用状态，true or false; true:关闭线程池； false:使用线程池*/
} threadpool_t;