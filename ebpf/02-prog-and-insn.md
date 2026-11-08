# 2. 程序与指令

**结论**：一条 eBPF 程序在内核里是 `struct bpf_prog`：前面是元数据，后面是 `bpf_insn` 柔性数组。`bpf_prog_load` 拷贝用户态指令、跑 verifier，再由 `bpf_prog_select_runtime` 选解释器或 JIT。真正执行时 `bpf_prog_run` 调用 `prog->bpf_func`。

> 指令在 `include/uapi/linux/bpf.h:72`。`bpf_prog` 在 `include/linux/bpf.h:1518`，`bpf_prog_aux` 在 `:1424`。装载函数在 `kernel/bpf/syscall.c:2596`。解释器 `___bpf_prog_run` 在 `kernel/bpf/core.c:1743`。x86-64 JIT 是 `arch/x86/net/bpf_jit_comp.c:2809` 的 `bpf_int_jit_compile`。

---

## 2.1 `bpf_insn`

```c
/* include/uapi/linux/bpf.h:72 */
struct bpf_insn {
	__u8	code;		/* opcode */
	__u8	dst_reg:4;	/* dest register */
	__u8	src_reg:4;	/* source register */
	__s16	off;		/* signed offset */
	__s32	imm;		/* signed immediate constant */
};
```

`BPF_CLASS(code)` 取低 3 位（`bpf_common.h:6`）。`BPF_SIZE` 取 `0x18` 那两位：`BPF_W` 32 位、`BPF_H` 16 位、`BPF_B` 8 位，eBPF 的 `BPF_DW`（`0x18`，`bpf.h:21`）是 64 位。`BPF_MODE` 取高 3 位：`BPF_IMM`、`BPF_ABS`、`BPF_IND`、`BPF_MEM`，再加上 eBPF 的 `BPF_ATOMIC`（`0xc0`）和 `BPF_MEMSX`（`0x80`）。

ALU 的操作在 `BPF_OP`（高 4 位）：`BPF_ADD` 到 `BPF_XOR` 沿用经典编码（`bpf_common.h:31`），eBPF 增加 `BPF_MOV`（`0xb0`）和 `BPF_ARSH`（`0xc0`）。`BPF_SRC` 的 `BPF_K` 表示立即数，`BPF_X` 表示寄存器。`BPF_ALU` 是 32 位运算，`BPF_ALU64` 是 64 位。

跳转类 `BPF_JMP` / `BPF_JMP32` 把条件放在 `BPF_OP`：`BPF_JA`、`BPF_JEQ`、`BPF_JGT`、`BPF_JGE`、`BPF_JSET`，以及有符号的 `BPF_JSGT` / `BPF_JSGE` / `BPF_JSLT` / `BPF_JSLE`（`bpf.h:38`）。`off` 是相对下一条指令的条数。`BPF_CALL`（`0x80`）的 `imm` 是 helper id，`BPF_EXIT`（`0x90`）结束函数，返回值在 R0。

64 位立即数装不下 32 位的 `imm`，所以 `BPF_LD | BPF_IMM | BPF_DW` 占两条指令：第一条的 `imm` 是低 32 位，下一条的 `imm` 是高 32 位。`src_reg == BPF_PSEUDO_MAP_FD`（`bpf.h:1236`，值 1）时，这两条不是普通立即数，而是「这个 map 的 fd」。verifier 在 `resolve_pseudo_ldimm64` 里把它换成内核 map 指针。`BPF_PSEUDO_MAP_IDX` 是 5，给的是 fd 数组下标。

经典指令 `sock_filter`（`include/uapi/linux/filter.h:24`）只有累加器。`bpf_convert_filter`（`net/core/filter.c:569`）把它展开成上面的 `bpf_insn`。套接字过滤和 seccomp 进内核之后走的是译好的 eBPF。

---

## 2.2 寄存器

verifier 文件头写明调用约定（`kernel/bpf/verifier.c:65`）：

| 寄存器 | 约定 |
|--------|------|
| R0 | 返回值 |
| R1–R5 | 参数。程序入口时 R1 是 ctx，类型 `PTR_TO_CTX` |
| R6–R9 | 被调用者保存 |
| R10 | 帧指针，只读 |

栈向下生长。`MAX_BPF_STACK` 是 512（`include/linux/filter.h:91`）。访问栈要先把 R10 拷出来再加负偏移，verifier 把这种模式认成 `PTR_TO_STACK`（`verifier.c:74` 的注释）。helper 调用会弄脏 R1–R5，R6–R9 保持。`BPF_LD | BPF_ABS` 这一类从 skb 取数据的隐式调用同样弄脏 R1–R5，并把 R6 当作 ctx（`verifier.c:15161`）。

---

## 2.3 `bpf_prog` 与 `bpf_prog_aux`

`bpf_prog_alloc`（`kernel/bpf/core.c:132`）按页 `vmalloc` 出 `bpf_prog`，再 `kzalloc` 一块 `bpf_prog_aux`。`bpf_prog_alloc_no_stats`（`:92`）里把 `jit_requested` 设成 `ebpf_jit_enabled()` 的结果（`:118`），把 `blinding_requested` 设成 `bpf_jit_blinding_enabled`（`:119`）。

```c
/* include/linux/bpf.h:1518，字段按头文件顺序 */
struct bpf_prog {
	u16			pages;		/* 按页分配的页数 */
	u16			jited:1,	/* 已经 JIT */
				jit_requested:1,/* 分配时按 JIT 开关置位 */
				gpl_compatible:1, /* license 字符串被认成 GPL */
				cb_access:1,	/* 碰过 skb->cb */
				dst_needed:1,	/* 需要 skb 的 dst */
				blinding_requested:1,
				blinded:1,
				is_func:1,	/* 这是一个子函数，不是主程序 */
				kprobe_override:1,
				has_callchain_buf:1,
				enforce_expected_attach_type:1,
				call_get_stack:1,
				call_get_func_ip:1,
				tstamp_type_access:1;
	enum bpf_prog_type	type;
	enum bpf_attach_type	expected_attach_type; /* 部分类型在装载时就定死 */
	u32			len;		/* insn 条数 */
	u32			jited_len;
	u8			tag[BPF_TAG_SIZE];
	struct bpf_prog_stats __percpu *stats;
	int __percpu		*active;	/* 本 CPU 重入计数 */
	unsigned int		(*bpf_func)(const void *ctx,
					    const struct bpf_insn *insn);
	struct bpf_prog_aux	*aux;
	struct sock_fprog_kern	*orig_prog;	/* 从 cBPF 译过来时留着原程序 */
	union {
		DECLARE_FLEX_ARRAY(struct sock_filter, insns);
		DECLARE_FLEX_ARRAY(struct bpf_insn, insnsi);
	};
};
```

`bpf_prog_aux`（`:1424` 到 `:1516`）放引用计数、用过的 map、栈深、`attach_btf_id`、`sleepable`、`xdp_has_frags`、JIT 私有数据、行号信息和 BTF。和装载直接相关的是这几个：

```c
/* include/linux/bpf.h:1424 起的节选 */
struct bpf_prog_aux {
	atomic64_t refcnt;
	u32 used_map_cnt;
	u32 stack_depth;          /* verifier 看到的最大栈 */
	u32 id;
	u32 attach_btf_id;        /* fentry / raw tp 的目标 BTF id */
	struct btf *attach_btf;
	bool verifier_zext;       /* verifier 已经插入了 32 位零扩展 */
	bool sleepable;           /* BPF_F_SLEEPABLE */
	bool tail_call_reachable;
	bool xdp_has_frags;       /* BPF_F_XDP_HAS_FRAGS */
	const char *attach_func_name;
	struct bpf_prog **func;  /* 子程序 */
	void *jit_data;
	const struct bpf_prog_ops *ops;
	struct bpf_map **used_maps;
	u64 load_time;            /* ktime_get_boottime_ns */
	u32 verified_insns;       /* do_check 实际走过的条数 */
	char name[BPF_OBJ_NAME_LEN];
	struct bpf_prog *prog;
};
```

`bpf_prog_ops`（`:960`）目前主要是 `test_run`。verifier 用的是另一张表 `bpf_verifier_ops`（`:966`）：`get_func_proto` 和 `is_valid_access`。`find_prog_type`（`syscall.c:2057`）按 `bpf_prog_types[]` 把 `aux->ops` 指到对应类型；offload 程序改指向 `bpf_offload_prog_ops`。

---

## 2.4 `bpf_prog_load`

`BPF_PROG_LOAD` 在 `__sys_bpf` 的 switch 里落到 `bpf_prog_load`（`syscall.c:5497` 调用，函数本体 `:2596`）。顺序是固定的：

```text
CHECK_ATTR(BPF_PROG_LOAD)
拒绝未知的 prog_flags
  允许的位：STRICT_ALIGNMENT、ANY_ALIGNMENT、TEST_STATE_FREQ、
           SLEEPABLE、TEST_RND_HI32、XDP_HAS_FRAGS、XDP_DEV_BOUND_ONLY
           （:2607）
sysctl_unprivileged_bpf_disabled && !bpf_capable()  → -EPERM     :2628
insn_cnt == 0 或超过上限 → -E2BIG                                 :2631
  有 CAP_BPF：上限 BPF_COMPLEXITY_LIMIT_INSNS（1_000_000，bpf.h:1794）
  否则：BPF_MAXINSNS（4096，bpf_common.h:54）
类型不是 SOCKET_FILTER 也不是 CGROUP_SKB，且没有 CAP_BPF → -EPERM :2634
is_net_admin_prog_type 且没有 CAP_NET_ADMIN / CAP_SYS_ADMIN → -EPERM
is_perfmon_prog_type 且没有 perfmon_capable() → -EPERM
解析 attach_btf / attach_prog_fd
bpf_prog_alloc(bpf_prog_size(insn_cnt))
copy_from_bpfptr 指令
strncpy license，license_is_gpl_compatible → gpl_compatible
find_prog_type
bpf_check
bpf_prog_select_runtime
bpf_prog_alloc_id
bpf_prog_kallsyms_add、perf_event_bpf_event、bpf_audit_prog
bpf_prog_new_fd
```

`is_net_admin_prog_type`（`:2544`）覆盖 XDP、`SCHED_CLS` / `SCHED_ACT`、lwt、`SK_SKB`、`SK_MSG`、flow dissector、一批 cgroup 网络类型、`SOCK_OPS`、`NETFILTER`、`EXT`。`is_perfmon_prog_type`（`:2575`）覆盖 `KPROBE`、`TRACEPOINT`、`PERF_EVENT`、两种 raw tracepoint、`TRACING`、`LSM`、`STRUCT_OPS`、`EXT`。`bpf_capable()`（`include/linux/capability.h:200`）是 `CAP_BPF` 或 `CAP_SYS_ADMIN`。

id 分配成功之后程序已经能被 `BPF_PROG_GET_FD_BY_ID` 看见，所以后面的失败必须 `bpf_prog_put`，不能直接 `bpf_prog_free`（函数里 `:2764` 的注释）。

---

## 2.5 解释器

`bpf_prog_run`（`include/linux/filter.h:622`）调用 `__bpf_prog_run`（`:594`）。统计键 `bpf_stats_enabled_key` 打开时用 `sched_clock` 把耗时加进 per-cpu 的 `bpf_prog_stats`；否则直接调用 dispatcher。默认 dispatcher 是 `bpf_dispatcher_nop_func`，它调用 `prog->bpf_func(ctx, insnsi)`。

`bpf_prog_select_func`（`core.c:2425`）在 JIT 没有强制打开时，按 `aux->stack_depth` 向上取整到 32 字节，从 `interpreters[]` 里挑一个包装。这些包装最终进 `___bpf_prog_run`（`:1743`）。`CONFIG_BPF_JIT_ALWAYS_ON` 时这里改成 `__bpf_prog_ret0_warn`：没有 JIT 镜像就不能跑。

`___bpf_prog_run` 用一张 256 项的跳转表按 `insn->code` 分发（`:1747`）。`BPF_JMP | BPF_TAIL_CALL` 和 `BPF_JMP | BPF_CALL_ARGS` 是内核内部 opcode，用户态指令里写不出来，verifier 在 fixup 阶段才会把 tail call 改写成它们。尾调用的解释器实现在 `:2046`：下标越过 `max_entries`、或者 `tail_call_cnt` 已经到 `MAX_TAIL_CALL_CNT`（33，`bpf.h:1795`），就跳过；否则 `READ_ONCE` 取出 `bpf_array.ptrs[index]`，把 `insn` 指到那个程序的 `insnsi`，`goto select_insn`。

---

## 2.6 JIT

`bpf_prog_select_runtime`（`core.c:2447`）在 `bpf_func` 还空着的时候调用 `bpf_prog_select_func`，然后：

```text
非 offload：
  bpf_prog_alloc_jited_linfo
  fp = bpf_int_jit_compile(fp)          弱符号默认在 core.c:2979，原样返回
  若 !fp->jited 且 jit_needed：-ENOTSUPP
offload：
  bpf_prog_offload_compile
bpf_prog_lock_ro                         指令页改只读
bpf_check_tail_call                      :2402，尾调用两端必须同为 JIT 或同为解释
```

`jit_needed` 在 `CONFIG_BPF_JIT_ALWAYS_ON` 或程序里有 kfunc 调用时为真（`:2457`）。x86-64 的实现（`arch/x86/net/bpf_jit_comp.c:2809`）先看 `prog->jit_requested`，否则直接交还原程序（`:2826`）。需要 JIT 时调用 `bpf_jit_blind_constants`：加固失败必须退回解释器，不能交回一个半成品（`:2829` 的注释）。多趟把指令地址算稳，再把可执行页的入口写进 `bpf_func`，置 `jited`。

`bpf_jit_enable`（`core.c:564`）为 0 时 `ebpf_jit_enabled()` 为假，`jit_requested` 在分配时就是 0，x86 JIT 函数直接返回。为 2 时 sysctl 处理函数会打一条不要用于生产的警告（`net/core/sysctl_net_core.c:344`）。常量加固由 `bpf_jit_harden` 控制（`filter.h:1131`）：0 关闭；1 只加固没有 `CAP_BPF` 的程序；其他非 0 值对所有程序加固。

offload 程序不走这个 JIT。`bpf_prog_is_offloaded` 为真时 `bpf_prog_select_runtime` 调用 `bpf_prog_offload_compile`。设备绑定的初始化在 `bpf_prog_load` 里的 `bpf_prog_dev_bound_init`（`syscall.c:2728`）。

---

## 2.7 执行时看到的入口

挂钩拿到 `struct bpf_prog *` 之后调用的是类型自己的包装，里面最终是 `bpf_prog_run` 或 `bpf_prog_run_xdp`。例如 generic XDP 在 `bpf_prog_run_generic_xdp` 里调用 `bpf_prog_run_xdp`（`net/core/dev.c:5045`，包装在 `include/net/xdp.h:495`）。kprobe 的数组执行在 `trace_call_bpf`（`kernel/trace/bpf_trace.c:110`）。这些包装负责 RCU、重入计数 `prog->active`，以及把返回值翻译成 `XDP_*` 或「过滤掉这次事件」。指令机本身不再区分自己跑在哪一种挂钩上。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
