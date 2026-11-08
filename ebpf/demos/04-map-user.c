/* 用户态创建 BPF_MAP_TYPE_ARRAY，写入再读出。
 * 需要 CAP_BPF（或 CAP_SYS_ADMIN）。本机非 root 的 capability 集是空的，用 root 跑。
 * 指令和命令在 v6.6.160 的 uapi 里已经存在；本文件按宿主机头文件编译。 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/bpf.h>

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, cmd, attr, size);
}

static int map_create(void)
{
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.map_type = BPF_MAP_TYPE_ARRAY;
	attr.key_size = sizeof(uint32_t);
	attr.value_size = sizeof(uint64_t);
	attr.max_entries = 1;
	memcpy(attr.map_name, "demo_array", sizeof("demo_array"));
	return sys_bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
}

static int map_update(int fd, uint32_t key, uint64_t value)
{
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.map_fd = fd;
	attr.key = (uint64_t)(unsigned long)&key;
	attr.value = (uint64_t)(unsigned long)&value;
	attr.flags = BPF_ANY;
	return sys_bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
}

static int map_lookup(int fd, uint32_t key, uint64_t *value)
{
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.map_fd = fd;
	attr.key = (uint64_t)(unsigned long)&key;
	attr.value = (uint64_t)(unsigned long)value;
	return sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

int main(void)
{
	int fd = map_create();
	uint64_t got = 0;
	uint32_t key = 0;

	if (fd < 0) {
		fprintf(stderr, "BPF_MAP_CREATE: %s\n", strerror(errno));
		return 1;
	}
	if (map_update(fd, key, 42) < 0) {
		fprintf(stderr, "BPF_MAP_UPDATE_ELEM: %s\n", strerror(errno));
		return 1;
	}
	if (map_lookup(fd, key, &got) < 0) {
		fprintf(stderr, "BPF_MAP_LOOKUP_ELEM: %s\n", strerror(errno));
		return 1;
	}
	printf("map_fd=%d key=%u value=%llu\n", fd, key, (unsigned long long)got);
	close(fd);
	return got == 42 ? 0 : 1;
}
