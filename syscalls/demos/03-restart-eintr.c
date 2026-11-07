/* SIGALRM during a blocking read, with and without SA_RESTART.
 * A writer thread delivers one byte after the timer, so a restarted
 * read can still complete.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

struct writer_arg {
	int fd;
	int delay_ms;
};

static void nop(int sig)
{
	(void)sig;
}

static void *writer(void *p)
{
	struct writer_arg *a = p;
	struct timespec ts = {
		.tv_sec = a->delay_ms / 1000,
		.tv_nsec = (long)(a->delay_ms % 1000) * 1000000L,
	};
	char c = 'Z';

	nanosleep(&ts, NULL);
	if (write(a->fd, &c, 1) != 1)
		perror("write");
	return NULL;
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000L +
	       (b->tv_nsec - a->tv_nsec) / 1000000L;
}

static void once(const char *name, int sa_flags)
{
	int fds[2];
	struct sigaction sa;
	struct writer_arg arg;
	struct itimerval it, zero;
	struct timespec t0, t1;
	pthread_t th;
	char buf = 0;
	ssize_t n;
	int err;

	if (pipe(fds) != 0) {
		perror("pipe");
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = nop;
	sa.sa_flags = sa_flags;
	if (sigaction(SIGALRM, &sa, NULL) != 0) {
		perror("sigaction");
		return;
	}

	arg.fd = fds[1];
	arg.delay_ms = 200;
	if (pthread_create(&th, NULL, writer, &arg) != 0) {
		perror("pthread_create");
		return;
	}

	memset(&it, 0, sizeof(it));
	it.it_value.tv_usec = 80 * 1000;
	if (setitimer(ITIMER_REAL, &it, NULL) != 0)
		perror("setitimer");

	clock_gettime(CLOCK_MONOTONIC, &t0);
	n = read(fds[0], &buf, 1);
	err = errno;
	clock_gettime(CLOCK_MONOTONIC, &t1);

	printf("%s n=%zd errno=%d (%s) byte=0x%02x elapsed_ms=%ld\n",
	       name, n, n < 0 ? err : 0, n < 0 ? strerror(err) : "-",
	       (unsigned)buf, elapsed_ms(&t0, &t1));

	pthread_join(th, NULL);
	memset(&zero, 0, sizeof(zero));
	setitimer(ITIMER_REAL, &zero, NULL);
	close(fds[0]);
	close(fds[1]);
}

int main(void)
{
	struct utsname u;

	if (uname(&u) == 0)
		printf("uname.release=%s\n", u.release);
	once("no_SA_RESTART", 0);
	once("SA_RESTART", SA_RESTART);
	return 0;
}
