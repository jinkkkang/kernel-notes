/* 写端写入 1 字节，读端用 EPOLLET 等到 EPOLLIN，再把字节读走。
 * 唤醒来自 pipe_write() 的 wake_up_interruptible_sync_poll(rd_wait, EPOLLIN|EPOLLRDNORM)。
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

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void dump_fdinfo(int fd, const char *tag)
{
	char path[64], buf[512];
	ssize_t n;
	int info;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	info = open(path, O_RDONLY | O_CLOEXEC);
	if (info < 0)
		die("open fdinfo");
	n = read(info, buf, sizeof(buf) - 1);
	if (n < 0)
		die("read fdinfo");
	buf[n] = '\0';
	close(info);
	printf("--- %s fdinfo ---\n%s", tag, buf);
}

static int wait_in(int ep, int timeout_ms, uint32_t *events)
{
	struct epoll_event ev;
	int n;
	do {
		n = epoll_wait(ep, &ev, 1, timeout_ms);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		die("epoll_wait");
	if (n == 0) {
		*events = 0;
		return 0;
	}
	*events = ev.events;
	return n;
}

int main(void)
{
	struct utsname u;
	int fds[2], ep, n;
	uint32_t ev = 0;
	char byte = 0;
	struct epoll_event reg = {
		.events = EPOLLIN | EPOLLET,
	};

	if (uname(&u) == 0)
		printf("kernel=%s\n", u.release);

	/* 读端保持默认阻塞。写 1 字节不会把默认 16 页的管道写满。 */
	if (pipe2(fds, O_CLOEXEC) < 0)
		die("pipe2");

	ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");
	reg.data.fd = fds[0];
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &reg) < 0)
		die("epoll_ctl");

	n = wait_in(ep, 0, &ev);
	printf("before_write hits=%d events=0x%x\n", n, ev);

	if (write(fds[1], "Z", 1) != 1)
		die("write");

	n = wait_in(ep, 1000, &ev);
	printf("after_write hits=%d events=0x%x\n", n, ev);

	if (read(fds[0], &byte, 1) != 1)
		die("read");
	printf("read_byte=%c\n", byte);

	n = wait_in(ep, 200, &ev);
	printf("after_read hits=%d events=0x%x\n", n, ev);

	printf("F_GETPIPE_SZ=%d\n", fcntl(fds[0], F_GETPIPE_SZ));
	dump_fdinfo(ep, "epfd");
	dump_fdinfo(fds[0], "pipe_r");

	close(ep);
	close(fds[0]);
	close(fds[1]);
	return 0;
}
