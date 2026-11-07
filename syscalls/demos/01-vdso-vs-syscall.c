/* Compare libc clock_gettime (vDSO on x86-64) with the syscall instruction.
 * Also time a raw getpid syscall. libc getpid() may be cached and is not used.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define N 200000

static unsigned long long rdtsc(void)
{
	unsigned lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

int main(void)
{
	struct utsname u;
	struct timespec ts;
	unsigned long long t0, t1;
	volatile unsigned long sink = 0;
	long i, rc;

	if (uname(&u) == 0)
		printf("uname.release=%s machine=%s\n", u.release, u.machine);

	t0 = rdtsc();
	for (i = 0; i < N; i++) {
		rc = clock_gettime(CLOCK_MONOTONIC, &ts);
		sink += (unsigned long)ts.tv_nsec;
	}
	t1 = rdtsc();
	printf("libc_clock_gettime n=%d last_rc=%ld cycles_per_call=%.1f sink=%lu\n",
	       N, rc, (double)(t1 - t0) / N, sink);

	t0 = rdtsc();
	for (i = 0; i < N; i++) {
		rc = syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &ts);
		sink += (unsigned long)ts.tv_nsec;
	}
	t1 = rdtsc();
	printf("syscall_clock_gettime n=%d last_rc=%ld errno=%d cycles_per_call=%.1f\n",
	       N, rc, rc < 0 ? errno : 0, (double)(t1 - t0) / N);

	t0 = rdtsc();
	for (i = 0; i < N; i++)
		sink += (unsigned long)syscall(__NR_getpid);
	t1 = rdtsc();
	printf("syscall_getpid n=%d cycles_per_call=%.1f sink=%lu\n",
	       N, (double)(t1 - t0) / N, sink);

	if (sink == 0)
		return 1;
	return 0;
}
