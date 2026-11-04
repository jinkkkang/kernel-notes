/*
 * 用户态：同一份 tmpfs 文件，用 read+write、splice、sendfile、copy_file_range 各拷一份。
 * 对照 vfs/05b-splice-and-zerocopy.md
 *
 * 默认目录 /dev/shm（tmpfs）。可用环境变量 SPLICE_DEMO_DIR 换到别的可写目录。
 * 两个普通文件直接 splice 应返回 EINVAL：splice(2) 至少一端得是管道。
 *
 * 每次测量在拷贝前后各读一次 /proc/self/io。
 * 起点那次 read 的返回字节数会进 rchar，并让 syscr 加 1。
 * 打印前把这两项减掉，剩下的是拷贝本身。wchar / read_bytes 不受这次 read 影响。
 *
 * 观测（RECIPE，见 05b）：本环境没有 bpftrace，tracing 也没有权限。
 *   bpftrace -e 'kprobe:do_splice { @[comm] = count(); }'
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define FILE_BYTES (256 * 1024)
#define COPY_BUF   4096

struct ioacct {
	unsigned long long rchar;
	unsigned long long wchar;
	unsigned long long syscr;
	unsigned long long syscw;
	unsigned long long read_bytes;
	unsigned long long write_bytes;
};

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static int read_io(struct ioacct *a, size_t *nbytes)
{
	char buf[512];
	char *line;
	ssize_t n;
	int fd;

	memset(a, 0, sizeof(*a));
	fd = open("/proc/self/io", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return -1;
	if ((size_t)n == sizeof(buf) - 1)
		return -1;
	if (nbytes)
		*nbytes = (size_t)n;
	buf[n] = '\0';

	line = buf;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		unsigned long long v;

		if (nl)
			*nl = '\0';
		if (sscanf(line, "rchar: %llu", &v) == 1)
			a->rchar = v;
		else if (sscanf(line, "wchar: %llu", &v) == 1)
			a->wchar = v;
		else if (sscanf(line, "syscr: %llu", &v) == 1)
			a->syscr = v;
		else if (sscanf(line, "syscw: %llu", &v) == 1)
			a->syscw = v;
		else if (sscanf(line, "read_bytes: %llu", &v) == 1)
			a->read_bytes = v;
		else if (sscanf(line, "write_bytes: %llu", &v) == 1)
			a->write_bytes = v;
		if (!nl)
			break;
		line = nl + 1;
	}
	return 0;
}

static void io_delta(struct ioacct *d, const struct ioacct *after,
		      const struct ioacct *before)
{
	d->rchar = after->rchar - before->rchar;
	d->wchar = after->wchar - before->wchar;
	d->syscr = after->syscr - before->syscr;
	d->syscw = after->syscw - before->syscw;
	d->read_bytes = after->read_bytes - before->read_bytes;
	d->write_bytes = after->write_bytes - before->write_bytes;
}

static void io_drop_snapshot(struct ioacct *d, size_t snap_bytes)
{
	d->rchar -= snap_bytes;
	d->syscr -= 1;
}

static int write_pattern(int fd, size_t len)
{
	unsigned char buf[COPY_BUF];
	size_t done = 0;

	while (done < len) {
		size_t chunk = len - done;
		size_t i;
		ssize_t n;

		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		for (i = 0; i < chunk; i++)
			buf[i] = (unsigned char)((done + i) & 0xff);
		n = write(fd, buf, chunk);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

static int files_match(int src, int dst, size_t len)
{
	unsigned char a[COPY_BUF], b[COPY_BUF];
	size_t done = 0;

	if (lseek(src, 0, SEEK_SET) < 0 || lseek(dst, 0, SEEK_SET) < 0)
		return -1;
	while (done < len) {
		size_t chunk = len - done;
		size_t got_a = 0, got_b = 0;

		if (chunk > sizeof(a))
			chunk = sizeof(a);
		while (got_a < chunk) {
			ssize_t n = read(src, a + got_a, chunk - got_a);

			if (n < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (n == 0)
				return 0;
			got_a += (size_t)n;
		}
		while (got_b < chunk) {
			ssize_t n = read(dst, b + got_b, chunk - got_b);

			if (n < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (n == 0)
				return 0;
			got_b += (size_t)n;
		}
		if (memcmp(a, b, chunk) != 0)
			return 0;
		done += chunk;
	}
	return 1;
}

static int copy_rw(int in, int out, size_t len, int *nread, int *nwrite)
{
	unsigned char buf[COPY_BUF];
	size_t done = 0;

	*nread = 0;
	*nwrite = 0;
	if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
		return -1;
	while (done < len) {
		ssize_t n = read(in, buf, sizeof(buf));
		size_t left;
		size_t off = 0;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		(*nread)++;
		left = (size_t)n;
		while (off < left) {
			ssize_t w = write(out, buf + off, left - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (w == 0)
				return -1;
			(*nwrite)++;
			off += (size_t)w;
		}
		done += left;
	}
	return done == len ? 0 : -1;
}

static int copy_splice(int in, int out, int pipe_r, int pipe_w, size_t len,
		        int *calls)
{
	size_t done = 0;

	*calls = 0;
	if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
		return -1;
	while (done < len) {
		ssize_t n = splice(in, NULL, pipe_w, NULL, len - done, 0);
		size_t left;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		(*calls)++;
		left = (size_t)n;
		while (left) {
			ssize_t m = splice(pipe_r, NULL, out, NULL, left, 0);

			if (m < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (m == 0)
				return -1;
			(*calls)++;
			left -= (size_t)m;
		}
		done += (size_t)n;
	}
	return done == len ? 0 : -1;
}

static int copy_sendfile(int in, int out, size_t len, int *calls)
{
	size_t done = 0;

	*calls = 0;
	if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
		return -1;
	while (done < len) {
		ssize_t n = sendfile(out, in, NULL, len - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		(*calls)++;
		done += (size_t)n;
	}
	return done == len ? 0 : -1;
}

static int copy_cfr(int in, int out, size_t len, int *calls)
{
	loff_t off_in = 0;
	loff_t off_out = 0;
	size_t done = 0;

	*calls = 0;
	while (done < len) {
		ssize_t n = copy_file_range(in, &off_in, out, &off_out,
					     len - done, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		(*calls)++;
		done += (size_t)n;
	}
	return done == len ? 0 : -1;
}

static void measure(const char *name, int (*fn)(void *), void *arg, int *out_rc)
{
	struct ioacct before, after, d;
	size_t snap_bytes = 0;
	int rc;

	if (read_io(&before, &snap_bytes) != 0)
		die("read /proc/self/io");
	rc = fn(arg);
	if (read_io(&after, NULL) != 0)
		die("read /proc/self/io");
	io_delta(&d, &after, &before);
	io_drop_snapshot(&d, snap_bytes);
	printf("method=%s ok=%d rchar=%llu wchar=%llu syscr=%llu syscw=%llu read_bytes=%llu write_bytes=%llu snap_bytes=%zu\n",
	       name, rc == 0, d.rchar, d.wchar, d.syscr, d.syscw,
	       d.read_bytes, d.write_bytes, snap_bytes);
	fflush(stdout);
	*out_rc = rc;
}

struct rw_arg {
	int in, out, nread, nwrite;
};
struct splice_arg {
	int in, out, pipe_r, pipe_w, calls;
};
struct one_arg {
	int in, out, calls;
};

static int run_rw(void *p)
{
	struct rw_arg *a = p;

	return copy_rw(a->in, a->out, FILE_BYTES, &a->nread, &a->nwrite);
}
static int run_splice(void *p)
{
	struct splice_arg *a = p;

	return copy_splice(a->in, a->out, a->pipe_r, a->pipe_w, FILE_BYTES,
			   &a->calls);
}
static int run_sendfile(void *p)
{
	struct one_arg *a = p;

	return copy_sendfile(a->in, a->out, FILE_BYTES, &a->calls);
}
static int run_cfr(void *p)
{
	struct one_arg *a = p;

	return copy_cfr(a->in, a->out, FILE_BYTES, &a->calls);
}

static int open_out(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		die(path);
	return fd;
}

int main(void)
{
	const char *base = getenv("SPLICE_DEMO_DIR");
	char dir[] = "/dev/shm/splice-demo-XXXXXX";
	char src_path[128], rw_path[128], sp_path[128], sf_path[128], cf_path[128];
	struct statfs st;
	int src, rw, sp, sf, cf, pipefd[2], pipe_sz;
	int rc_rw, rc_sp, rc_sf, rc_cf;
	int match_rw, match_sp, match_sf, match_cf;
	struct rw_arg rw_a;
	struct splice_arg sp_a;
	struct one_arg sf_a, cf_a;
	ssize_t bad;

	if (!base)
		base = mkdtemp(dir);
	if (!base)
		die("mkdtemp");
	if (statfs(base, &st) != 0)
		die("statfs");

	snprintf(src_path, sizeof(src_path), "%s/src", base);
	snprintf(rw_path, sizeof(rw_path), "%s/rw", base);
	snprintf(sp_path, sizeof(sp_path), "%s/splice", base);
	snprintf(sf_path, sizeof(sf_path), "%s/sendfile", base);
	snprintf(cf_path, sizeof(cf_path), "%s/cfr", base);

	src = open(src_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (src < 0)
		die(src_path);
	if (write_pattern(src, FILE_BYTES) != 0)
		die("write pattern");
	rw = open_out(rw_path);
	sp = open_out(sp_path);
	sf = open_out(sf_path);
	cf = open_out(cf_path);
	if (pipe(pipefd) != 0)
		die("pipe");
	pipe_sz = fcntl(pipefd[0], F_GETPIPE_SZ);
	if (pipe_sz < 0)
		die("F_GETPIPE_SZ");

	/* 两个普通文件。失败路径，不放进 io 窗口。 */
	errno = 0;
	bad = splice(src, NULL, rw, NULL, FILE_BYTES, 0);
	printf("fstype_magic=0x%lx tmpfs=%d dir=%s bytes=%d pipe_sz=%d\n",
	       (unsigned long)st.f_type, st.f_type == TMPFS_MAGIC, base,
	       FILE_BYTES, pipe_sz);
	printf("splice_file_to_file=%s errno=%d\n",
	       (bad < 0 && errno == EINVAL) ? "EINVAL" : "unexpected", errno);
	fflush(stdout);
	if (bad >= 0 || errno != EINVAL)
		return 1;
	if (lseek(src, 0, SEEK_SET) < 0 || lseek(rw, 0, SEEK_SET) < 0)
		die("lseek reset");

	rw_a = (struct rw_arg){ .in = src, .out = rw };
	sp_a = (struct splice_arg){
		.in = src, .out = sp, .pipe_r = pipefd[0], .pipe_w = pipefd[1]
	};
	sf_a = (struct one_arg){ .in = src, .out = sf };
	cf_a = (struct one_arg){ .in = src, .out = cf };

	measure("read_write", run_rw, &rw_a, &rc_rw);
	measure("splice", run_splice, &sp_a, &rc_sp);
	measure("sendfile", run_sendfile, &sf_a, &rc_sf);
	measure("copy_file_range", run_cfr, &cf_a, &rc_cf);

	printf("calls read_write=%d/%d splice=%d sendfile=%d copy_file_range=%d\n",
	       rw_a.nread, rw_a.nwrite, sp_a.calls, sf_a.calls, cf_a.calls);

	match_rw = files_match(src, rw, FILE_BYTES);
	match_sp = files_match(src, sp, FILE_BYTES);
	match_sf = files_match(src, sf, FILE_BYTES);
	match_cf = files_match(src, cf, FILE_BYTES);
	printf("match read_write=%d splice=%d sendfile=%d copy_file_range=%d\n",
	       match_rw, match_sp, match_sf, match_cf);
	fflush(stdout);

	close(src);
	close(rw);
	close(sp);
	close(sf);
	close(cf);
	close(pipefd[0]);
	close(pipefd[1]);
	unlink(src_path);
	unlink(rw_path);
	unlink(sp_path);
	unlink(sf_path);
	unlink(cf_path);
	if (base == dir)
		rmdir(dir);

	if (rc_rw || rc_sp || rc_sf || rc_cf)
		return 1;
	if (match_rw != 1 || match_sp != 1 || match_sf != 1 || match_cf != 1)
		return 1;
	return 0;
}
