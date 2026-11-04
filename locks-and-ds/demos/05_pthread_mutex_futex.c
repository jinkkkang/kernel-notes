/*
 * 用户态：pthread mutex 无争用只做原子，争用才进 futex(2)
 * 对照 locks-and-ds/09-futex.md
 *
 * 快路径不进内核。主线程先独自 lock/unlock。
 * 然后自己持锁，再拉起等待线程，让它们堵住，最后 unlock。
 * 等待时间应接近主线程持锁的那段 sleep。
 *
 * 观测（RECIPE，见 09）：
 *   strace -f -e futex ./05_pthread_mutex_futex
 *   bpftrace -e 'tracepoint:syscalls:sys_enter_futex { @[args->op] = count(); }'
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define UNCONTENDED 1000
#define NWAITERS    4
#define HOLD_US     200000

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int entered;

static long long mono_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *worker(void *arg)
{
	int id = (int)(long)arg;
	long long t0, dt_ms;

	t0 = mono_ns();
	if (pthread_mutex_lock(&mu) != 0) {
		perror("pthread_mutex_lock");
		exit(1);
	}
	dt_ms = (mono_ns() - t0) / 1000000LL;
	entered++;
	printf("waiter=%d blocked_ms=%lld\n", id, dt_ms);
	fflush(stdout);
	if (pthread_mutex_unlock(&mu) != 0) {
		perror("pthread_mutex_unlock");
		exit(1);
	}
	return (void *)(long)dt_ms;
}

int main(void)
{
	pthread_t th[NWAITERS];
	int i, rc;
	long long worst = 0;

	for (i = 0; i < UNCONTENDED; i++) {
		if (pthread_mutex_lock(&mu) != 0 || pthread_mutex_unlock(&mu) != 0) {
			perror("uncontended mutex");
			return 1;
		}
	}
	printf("phase=uncontended locks=%d\n", UNCONTENDED);
	fflush(stdout);

	if (pthread_mutex_lock(&mu) != 0) {
		perror("hold");
		return 1;
	}
	for (i = 0; i < NWAITERS; i++) {
		rc = pthread_create(&th[i], NULL, worker, (void *)(long)i);
		if (rc != 0) {
			errno = rc;
			perror("pthread_create");
			return 1;
		}
	}
	/* 让等待者走完用户态自旋，停进 futex。 */
	usleep(HOLD_US);
	printf("phase=unlock waiters=%d\n", NWAITERS);
	fflush(stdout);
	if (pthread_mutex_unlock(&mu) != 0) {
		perror("release");
		return 1;
	}

	for (i = 0; i < NWAITERS; i++) {
		void *ret = NULL;

		rc = pthread_join(th[i], &ret);
		if (rc != 0) {
			errno = rc;
			perror("pthread_join");
			return 1;
		}
		if ((long long)(long)ret > worst)
			worst = (long long)(long)ret;
	}

	printf("phase=done entered=%d worst_blocked_ms=%lld\n", entered, worst);
	fflush(stdout);
	if (entered != NWAITERS || worst < HOLD_US / 1000 / 2)
		return 1;
	return 0;
}
