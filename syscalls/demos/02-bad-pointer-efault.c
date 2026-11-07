/* Pass a non-canonical / unmapped user pointer into read and write. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static void report(const char *what, ssize_t n)
{
	printf("%s n=%zd errno=%d (%s)\n",
	       what, n, n < 0 ? errno : 0, n < 0 ? strerror(errno) : "-");
}

int main(void)
{
	struct utsname u;
	char stack_byte = 'A';
	ssize_t n;
	int fd, devnull;

	if (uname(&u) == 0)
		printf("uname.release=%s\n", u.release);

	/* A low address is in the user half, but this process has no mapping. */
	n = write(STDOUT_FILENO, (void *)1, 1);
	report("write(1, (void *)1, 1)", n);

	fd = open("/dev/zero", O_RDONLY);
	if (fd < 0) {
		perror("open /dev/zero");
		return 1;
	}
	n = read(fd, (void *)1, 16);
	report("read(/dev/zero, (void *)1, 16)", n);
	close(fd);

	/* A kernel-half pointer fails the user-address check before the copy. */
	n = write(STDOUT_FILENO, (void *)-1, 1);
	report("write(1, (void *)-1, 1)", n);

	devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0) {
		perror("open /dev/null");
		return 1;
	}
	n = write(devnull, &stack_byte, 1);
	report("write(/dev/null, &stack_byte, 1)", n);
	close(devnull);
	return 0;
}
