/* 装一段返回 XDP_DROP 的 XDP 程序，对 array[0] 加一，再用 BPF_PROG_TEST_RUN
 * 喂一帧。不挂网卡。需要 CAP_BPF 和 CAP_NET_ADMIN。
 * 字节码按 v6.6.160 的 bpf_insn 编码手写；编译用的是宿主机的 linux/bpf.h。 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/bpf.h>

#include "bpf_insn.h"

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
	attr.value_size = sizeof(uint32_t);
	attr.max_entries = 1;
	memcpy(attr.map_name, "xdp_cnt", sizeof("xdp_cnt"));
	return sys_bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
}

static int map_lookup(int fd, uint32_t *value)
{
	uint32_t key = 0;
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.map_fd = fd;
	attr.key = (uint64_t)(unsigned long)&key;
	attr.value = (uint64_t)(unsigned long)value;
	return sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

static int load_xdp(int map_fd, char *log, uint32_t log_sz)
{
	struct bpf_insn insns[] = {
		BPF_LD_MAP_FD(BPF_REG_1, map_fd),
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
		BPF_ST_MEM(BPF_W, BPF_REG_10, -4, 0),
		BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 3),
		BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_0, 0),
		BPF_ALU32_IMM(BPF_ADD, BPF_REG_1, 1),
		BPF_STX_MEM(BPF_W, BPF_REG_0, BPF_REG_1, 0),
		BPF_MOV64_IMM(BPF_REG_0, XDP_DROP),
		BPF_EXIT_INSN(),
	};
	union bpf_attr attr;
	char license[] = "GPL";

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = BPF_PROG_TYPE_XDP;
	attr.insn_cnt = sizeof(insns) / sizeof(insns[0]);
	attr.insns = (uint64_t)(unsigned long)insns;
	attr.license = (uint64_t)(unsigned long)license;
	attr.log_level = 1;
	attr.log_size = log_sz;
	attr.log_buf = (uint64_t)(unsigned long)log;
	memcpy(attr.prog_name, "xdp_drop_cnt", sizeof("xdp_drop_cnt"));
	return sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
}

static int test_run(int prog_fd, uint32_t *retval)
{
	unsigned char pkt[64];
	unsigned char out[64];
	union bpf_attr attr;

	memset(pkt, 0, sizeof(pkt));
	pkt[12] = 0x08;
	pkt[13] = 0x00;
	memset(&attr, 0, sizeof(attr));
	attr.test.prog_fd = prog_fd;
	attr.test.data_in = (uint64_t)(unsigned long)pkt;
	attr.test.data_out = (uint64_t)(unsigned long)out;
	attr.test.data_size_in = sizeof(pkt);
	attr.test.data_size_out = sizeof(out);
	attr.test.repeat = 1;
	if (sys_bpf(BPF_PROG_TEST_RUN, &attr, sizeof(attr)) < 0)
		return -1;
	*retval = attr.test.retval;
	return 0;
}

int main(void)
{
	char log[8192];
	int map_fd, prog_fd;
	uint32_t value = 0, retval = 0;

	memset(log, 0, sizeof(log));
	map_fd = map_create();
	if (map_fd < 0) {
		fprintf(stderr, "BPF_MAP_CREATE: %s\n", strerror(errno));
		return 1;
	}
	prog_fd = load_xdp(map_fd, log, sizeof(log));
	if (prog_fd < 0) {
		fprintf(stderr, "BPF_PROG_LOAD: %s\n%s\n", strerror(errno), log);
		return 1;
	}
	if (test_run(prog_fd, &retval) < 0) {
		fprintf(stderr, "BPF_PROG_TEST_RUN: %s\n", strerror(errno));
		return 1;
	}
	if (map_lookup(map_fd, &value) < 0) {
		fprintf(stderr, "BPF_MAP_LOOKUP_ELEM: %s\n", strerror(errno));
		return 1;
	}
	printf("prog_fd=%d retval=%u map0=%u\n", prog_fd, retval, value);
	{
		char path[64], line[256];
		FILE *fp;
		snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", prog_fd);
		fp = fopen(path, "r");
		if (fp) {
			while (fgets(line, sizeof(line), fp))
				fputs(line, stdout);
			fclose(fp);
		}
	}
	close(prog_fd);
	close(map_fd);
	return (retval == XDP_DROP && value == 1) ? 0 : 1;
}
