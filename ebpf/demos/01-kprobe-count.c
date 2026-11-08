/* 挂到 kprobe PMU 的一个非 static 符号上，对 array[0] 计数。
 * 需要 CAP_BPF 和 CAP_PERFMON。符号不存在或 perf_event_open 失败时打印原因并返回非 0。
 * 断点怎么种进指令流不在这个程序里，它只做 perf_event_open + PERF_EVENT_IOC_SET_BPF。 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>

#include "bpf_insn.h"

#ifndef __NR_perf_event_open
#define __NR_perf_event_open 298
#endif

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
	memcpy(attr.map_name, "kp_cnt", sizeof("kp_cnt"));
	return sys_bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
}

static int map_lookup(int fd, uint64_t *value)
{
	uint32_t key = 0;
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.map_fd = fd;
	attr.key = (uint64_t)(unsigned long)&key;
	attr.value = (uint64_t)(unsigned long)value;
	return sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

static int load_kprobe(int map_fd, char *log, uint32_t log_sz)
{
	struct bpf_insn insns[] = {
		BPF_LD_MAP_FD(BPF_REG_1, map_fd),
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
		BPF_ST_MEM(BPF_W, BPF_REG_10, -4, 0),
		BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 4),
		BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_0, 0),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, 1),
		BPF_STX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, 0),
		BPF_MOV64_IMM(BPF_REG_0, 0),
		BPF_EXIT_INSN(),
	};
	union bpf_attr attr;
	char license[] = "GPL";

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = BPF_PROG_TYPE_KPROBE;
	attr.insn_cnt = sizeof(insns) / sizeof(insns[0]);
	attr.insns = (uint64_t)(unsigned long)insns;
	attr.license = (uint64_t)(unsigned long)license;
	attr.log_level = 1;
	attr.log_size = log_sz;
	attr.log_buf = (uint64_t)(unsigned long)log;
	memcpy(attr.prog_name, "kp_count", sizeof("kp_count"));
	return sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
}

static int kprobe_type(void)
{
	int fd = open("/sys/bus/event_source/devices/kprobe/type", O_RDONLY);
	char buf[32];
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return atoi(buf);
}

int main(void)
{
	const char *symbol = "__x64_sys_getpid";
	struct perf_event_attr pe;
	char log[8192];
	int map_fd, prog_fd, pfd, type, i;
	uint64_t value = 0;

	memset(log, 0, sizeof(log));
	type = kprobe_type();
	if (type < 0) {
		fprintf(stderr, "kprobe PMU type: %s\n", strerror(errno));
		return 1;
	}
	map_fd = map_create();
	if (map_fd < 0) {
		fprintf(stderr, "BPF_MAP_CREATE: %s\n", strerror(errno));
		return 1;
	}
	prog_fd = load_kprobe(map_fd, log, sizeof(log));
	if (prog_fd < 0) {
		fprintf(stderr, "BPF_PROG_LOAD: %s\n%s\n", strerror(errno), log);
		return 1;
	}

	memset(&pe, 0, sizeof(pe));
	pe.size = sizeof(pe);
	pe.type = type;
	pe.config = 0; /* kprobe，不是 kretprobe */
	pe.kprobe_func = (uint64_t)(unsigned long)symbol;
	pe.sample_period = 1;
	pe.wakeup_events = 1;

	pfd = syscall(__NR_perf_event_open, &pe, -1, 0, -1, 0);
	if (pfd < 0) {
		fprintf(stderr, "perf_event_open(%s): %s\n", symbol, strerror(errno));
		return 1;
	}
	if (ioctl(pfd, PERF_EVENT_IOC_SET_BPF, prog_fd) < 0) {
		fprintf(stderr, "PERF_EVENT_IOC_SET_BPF: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(pfd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
		fprintf(stderr, "PERF_EVENT_IOC_ENABLE: %s\n", strerror(errno));
		return 1;
	}

	for (i = 0; i < 100; i++)
		getpid();

	if (map_lookup(map_fd, &value) < 0) {
		fprintf(stderr, "BPF_MAP_LOOKUP_ELEM: %s\n", strerror(errno));
		return 1;
	}
	printf("symbol=%s hits=%llu\n", symbol, (unsigned long long)value);
	close(pfd);
	close(prog_fd);
	close(map_fd);
	return value > 0 ? 0 : 1;
}
