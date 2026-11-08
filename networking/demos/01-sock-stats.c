/* TCP/UDP sockets and /proc/net/sockstat.
 * Kernel structures in the notes are v6.6.160; this process only prints
 * what the running kernel reports.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

static void dump_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char buf[512];

	printf("--- %s ---\n", path);
	if (!f) {
		printf("open failed: %s\n", strerror(errno));
		return;
	}
	while (fgets(buf, sizeof(buf), f))
		fputs(buf, stdout);
	fclose(f);
}

static int listen_tcp(int *port_out)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 1) < 0 ||
	    getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
		close(fd);
		return -1;
	}
	*port_out = ntohs(addr.sin_port);
	return fd;
}

int main(void)
{
	struct utsname u;
	int lfd, cfd, afd, ufd, port = 0;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	char cmd[160];

	if (uname(&u) == 0)
		printf("kernel=%s\n", u.release);
	else
		printf("kernel=unknown\n");

	printf("before\n");
	dump_file("/proc/net/sockstat");

	lfd = listen_tcp(&port);
	cfd = socket(AF_INET, SOCK_STREAM, 0);
	ufd = socket(AF_INET, SOCK_DGRAM, 0);
	if (lfd < 0 || cfd < 0 || ufd < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}
	afd = accept(lfd, NULL, NULL);
	if (afd < 0) {
		perror("accept");
		return 1;
	}
	addr.sin_port = 0;
	if (bind(ufd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("udp bind");
		return 1;
	}
	if (getsockname(ufd, (struct sockaddr *)&addr, &alen) < 0) {
		perror("getsockname");
		return 1;
	}

	printf("tcp_listen_port=%d udp_port=%d fds=%d,%d,%d,%d\n",
	       port, ntohs(addr.sin_port), lfd, cfd, afd, ufd);
	printf("after\n");
	dump_file("/proc/net/sockstat");

	snprintf(cmd, sizeof(cmd), "ss -m -tni sport = %d 2>/dev/null | head -n 40", port);
	printf("--- %s ---\n", cmd);
	fflush(stdout);
	if (system(cmd) != 0)
		printf("ss failed\n");

	close(afd);
	close(cfd);
	close(lfd);
	close(ufd);
	return 0;
}
