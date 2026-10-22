/*
 * 用户态：普通 ++ 丢更新 vs C11 _Atomic 修复
 * 对照内核 atomic_t / atomic_inc
 */
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#define NTHREADS 16
#define ITER     2000000

static volatile long plain_counter; /* volatile 防优化掉，仍非原子 */
static atomic_long atomic_counter;

static void *plain_worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < ITER; i++) {
		/* 显式读-改-写，放大交错窗口 */
		long tmp = plain_counter;
		tmp = tmp + 1;
		plain_counter = tmp;
	}
	return NULL;
}

static void *atomic_worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < ITER; i++)
		atomic_fetch_add_explicit(&atomic_counter, 1,
					  memory_order_relaxed);
	return NULL;
}

int main(void)
{
	pthread_t th[NTHREADS];
	long expect = (long)NTHREADS * ITER;
	int i;

	plain_counter = 0;
	for (i = 0; i < NTHREADS; i++)
		pthread_create(&th[i], NULL, plain_worker, NULL);
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);

	atomic_store(&atomic_counter, 0);
	for (i = 0; i < NTHREADS; i++)
		pthread_create(&th[i], NULL, atomic_worker, NULL);
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);

	printf("threads=%d iterations_each=%d\n", NTHREADS, ITER);
	printf("expected                  = %ld\n", expect);
	printf("plain RMW (racy)          = %ld  %s\n", plain_counter,
	       plain_counter == expect ? "(lucky equal — 再跑几次通常会丢)" : "(LOST UPDATES)");
	printf("atomic_fetch_add          = %ld  %s\n",
	       (long)atomic_load(&atomic_counter),
	       atomic_load(&atomic_counter) == expect ? "(OK)" : "(BUG)");

	return atomic_load(&atomic_counter) == expect ? 0 : 1;
}
