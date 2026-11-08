/* Pass one pipe fd across an AF_UNIX SOCK_STREAM socketpair with SCM_RIGHTS. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

static int send_fd(int sock, int fd)
{
	char byte = 'Z';
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg;
	struct cmsghdr *cmsg;

	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

static int recv_fd(int sock, int *fd_out, char *byte_out)
{
	char byte = 0;
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg;
	struct cmsghdr *cmsg;
	ssize_t n;

	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	n = recvmsg(sock, &msg, 0);
	if (n != 1)
		return -1;
	*byte_out = byte;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(fd_out, CMSG_DATA(cmsg), sizeof(*fd_out));
			return 0;
		}
	}
	return -1;
}

int main(void)
{
	struct utsname u;
	int sv[2], pfd[2], got = -1;
	char byte = 0, back = 0;

	if (uname(&u) == 0)
		printf("kernel=%s\n", u.release);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 || pipe(pfd) < 0) {
		perror("setup");
		return 1;
	}
	if (send_fd(sv[0], pfd[1]) < 0) {
		perror("sendmsg");
		return 1;
	}
	close(pfd[1]);
	if (recv_fd(sv[1], &got, &byte) < 0) {
		perror("recvmsg");
		return 1;
	}
	if (write(got, "Q", 1) != 1 || read(pfd[0], &back, 1) != 1) {
		perror("pipe");
		return 1;
	}
	printf("payload=%c passed_fd_works=%d read_back=%c\n",
	       byte, got >= 0, back);
	close(got);
	close(pfd[0]);
	close(sv[0]);
	close(sv[1]);
	return 0;
}
