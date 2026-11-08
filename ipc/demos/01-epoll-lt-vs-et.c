/* 同一根管道上对比 EPOLLIN 的水平触发、边缘触发和 ONESHOT。
 * 对照 v6.6.160 fs/eventpoll.c ep_send_events()：
 *   LT 送出后仍就绪则挂回 rdllist；ET 不挂回；ONESHOT 清掉兴趣掩码。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/utsname.h>
#include <unistd.h>

#define NBYTES 4
#define WAIT_MS 200

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void make_pipe(int fds[2])
{
	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
		die("pipe2");
}

static int new_epoll(void)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");
	return ep;
}

static void add_fd(int ep, int fd, uint32_t events)
{
	struct epoll_event ev = {
		.events = events,
		.data.fd = fd,
	};
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
		die("epoll_ctl ADD");
}

/* 每次 epoll_wait 命中只读 1 字节，直到超时。返回命中次数。 */
static int count_one_byte_reads(int ep, int fd)
{
	int hits = 0;
	unsigned char buf;

	for (;;) {
		struct epoll_event ev;
		int n = epoll_wait(ep, &ev, 1, WAIT_MS);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait");
		}
		if (n == 0)
			break;
		if (read(fd, &buf, 1) != 1)
			die("read");
		hits++;
		if (hits > NBYTES + 2)
			die("too many hits");
	}
	return hits;
}

static int drain(int fd)
{
	int n = 0;
	unsigned char buf;
	for (;;) {
		ssize_t r = read(fd, &buf, 1);
		if (r == 1) {
			n++;
			continue;
		}
		if (r < 0 && errno == EAGAIN)
			return n;
		die("drain");
	}
}

static void lt_case(void)
{
	int fds[2], ep, hits;
	make_pipe(fds);
	if (write(fds[1], "abcd", NBYTES) != NBYTES)
		die("write lt");
	ep = new_epoll();
	add_fd(ep, fds[0], EPOLLIN);
	hits = count_one_byte_reads(ep, fds[0]);
	printf("lt_hits=%d\n", hits);
	close(ep);
	close(fds[0]);
	close(fds[1]);
}

static void et_case(void)
{
	int fds[2], ep, hits, left;
	make_pipe(fds);
	if (write(fds[1], "abcd", NBYTES) != NBYTES)
		die("write et");
	ep = new_epoll();
	add_fd(ep, fds[0], EPOLLIN | EPOLLET);
	hits = count_one_byte_reads(ep, fds[0]);
	left = drain(fds[0]);
	printf("et_hits=%d et_unread_after_first=%d\n", hits, left);
	close(ep);
	close(fds[0]);
	close(fds[1]);
}

static int wait_once(int ep)
{
	struct epoll_event ev;
	int n;
	do {
		n = epoll_wait(ep, &ev, 1, WAIT_MS);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		die("epoll_wait oneshot");
	return n;
}

static void oneshot_case(void)
{
	int fds[2], ep, first, second, after;
	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLONESHOT,
	};
	make_pipe(fds);
	if (write(fds[1], "xy", 2) != 2)
		die("write oneshot");
	ep = new_epoll();
	ev.data.fd = fds[0];
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) < 0)
		die("epoll_ctl ONESHOT");
	first = wait_once(ep);
	second = wait_once(ep);
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &ev) < 0)
		die("epoll_ctl MOD");
	after = wait_once(ep);
	printf("oneshot_first=%d oneshot_second=%d oneshot_after_mod=%d\n",
	       first, second, after);
	close(ep);
	close(fds[0]);
	close(fds[1]);
}

int main(void)
{
	struct utsname u;
	if (uname(&u) == 0)
		printf("kernel=%s\n", u.release);
	lt_case();
	et_case();
	oneshot_case();
	return 0;
}
