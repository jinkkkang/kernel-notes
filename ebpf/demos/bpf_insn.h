/* 手写 bpf_insn 的最小宏。编码对应 include/uapi/linux/bpf.h 的 BPF_ALU64 / BPF_MEM / BPF_CALL。
 * 不链接 libbpf。 */
#ifndef EBPF_DEMOS_BPF_INSN_H
#define EBPF_DEMOS_BPF_INSN_H

#include <linux/bpf.h>

#define INSN(CODE, DST, SRC, OFF, IMM) \
	((struct bpf_insn){ .code = (CODE), .dst_reg = (DST), .src_reg = (SRC), \
			     .off = (OFF), .imm = (IMM) })

#define BPF_MOV64_REG(DST, SRC) \
	INSN(BPF_ALU64 | BPF_MOV | BPF_X, DST, SRC, 0, 0)
#define BPF_ALU64_IMM(OP, DST, IMM) \
	INSN(BPF_ALU64 | (OP) | BPF_K, DST, 0, 0, IMM)
#define BPF_ALU32_IMM(OP, DST, IMM) \
	INSN(BPF_ALU | (OP) | BPF_K, DST, 0, 0, IMM)
#define BPF_LDX_MEM(SIZE, DST, SRC, OFF) \
	INSN(BPF_LDX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define BPF_STX_MEM(SIZE, DST, SRC, OFF) \
	INSN(BPF_STX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define BPF_ST_MEM(SIZE, DST, OFF, IMM) \
	INSN(BPF_ST | BPF_MEM | (SIZE), DST, 0, OFF, IMM)
#define BPF_MOV64_IMM(DST, IMM) \
	INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_JMP_IMM(OP, DST, IMM, OFF) \
	INSN(BPF_JMP | (OP) | BPF_K, DST, 0, OFF, IMM)
#define BPF_EXIT_INSN() INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)
#define BPF_EMIT_CALL(FUNC) INSN(BPF_JMP | BPF_CALL, 0, 0, 0, FUNC)

/* BPF_LD_IMM64 占两条。src_reg = BPF_PSEUDO_MAP_FD 时 imm 是 map fd。 */
#define BPF_LD_MAP_FD(DST, MAP_FD) \
	INSN(BPF_LD | BPF_IMM | BPF_DW, DST, BPF_PSEUDO_MAP_FD, 0, MAP_FD), \
	INSN(0, 0, 0, 0, 0)

#endif
