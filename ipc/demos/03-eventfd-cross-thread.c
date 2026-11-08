/* 线程 A 向 eventfd 写入 1，线程 B 在 epoll_wait 上醒来并读出计数。 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/utsname.h>
#include <unistd.h>

static int efd;
static atomic_int armed;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void *waiter(void *arg)
{
	int ep = *(int *)arg;
	struct epoll_event ev;
	uint64_t cnt = 0;
	int n;
	ssize_t r;

	atomic_store_explicit(&armed, 1, memory_order_release);
	do {
		n = epoll_wait(ep, &ev, 1, 2000);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		die("epoll_wait");
	if (n == 0)
		die("epoll_wait timeout");

	r = read(efd, &cnt, sizeof(cnt));
	if (r != (ssize_t)sizeof(cnt))
		die("read eventfd");
	printf("waiter_events=0x%x count=%llu\n", ev.events,
	       (unsigned long long)cnt);
	return NULL;
}

int main(void)
{
	struct utsname u;
	int ep;
	pthread_t th;
	struct epoll_event reg = {
		.events = EPOLLIN,
	};
	uint64_t one = 1;

	if (uname(&u) == 0)
		printf("kernel=%s\n", u.release);

	efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0)
		die("eventfd");
	ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");
	reg.data.fd = efd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &reg) < 0)
		die("epoll_ctl");

	if (pthread_create(&th, NULL, waiter, &ep) != 0)
		die("pthread_create");

	while (atomic_load_explicit(&armed, memory_order_acquire) == 0)
		; /* 等 B 已经把 fd 挂进 epoll，再写，避免和 ADD 抢同一拍 */

	if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		die("write eventfd");

	if (pthread_join(th, NULL) != 0)
		die("pthread_join");

	close(ep);
	close(efd);
	return 0;
}
