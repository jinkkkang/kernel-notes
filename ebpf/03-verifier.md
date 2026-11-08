# 3. verifier

**结论**：`bpf_check` 先做 CFG，再让 `do_check` 沿每条路径模拟寄存器类型。同一条指令上，如果当前状态被以前保存的状态覆盖，搜索在这里剪掉。helper 是否允许由该程序类型的 `get_func_proto` 决定。模拟通过之后才改写指令：ctx 访问换成真实加载，map 查找换成直连调用。

> 入口 `bpf_check` 在 `kernel/bpf/verifier.c:20757`。主循环 `do_check` 在 `:17371`，由 `do_check_common`（`:20058`）准备好 R1 = `PTR_TO_CTX` 之后调用。环境是 `struct bpf_verifier_env`（`include/linux/bpf_verifier.h:636`）。寄存器类型枚举在 `include/linux/bpf.h:876`。

---

## 3.1 `bpf_check` 的顺序

```text
kvzalloc bpf_verifier_env
为每条 insn 分配 bpf_insn_aux_data，orig_idx = 下标
env->ops = bpf_verifier_ops[prog->type]
非特权：mutex_lock(&bpf_verifier_lock)          :20794
bpf_vlog_init(log_level, log_buf, log_size)
严格对齐：BPF_F_STRICT_ALIGNMENT，或架构不能高效非对齐
         BPF_F_ANY_ALIGNMENT 且调用者 bpf_capable 时关掉
add_subprog_and_kfunc
check_subprogs
check_btf_info
resolve_pseudo_ldimm64                         把 BPF_PSEUDO_MAP_FD 换成 map
check_cfg                                      :15721
check_attach_btf_id
do_check_subprogs
do_check_main                                  → do_check_common → do_check
check_max_stack_depth                          :6133
特权：opt_hard_wire_dead_code_branches、opt_remove_dead_code、opt_remove_nops
非特权：sanitize_dead_code
convert_ctx_accesses                           :18655
do_misc_fixups                                 :19280
opt_subreg_zext_lo32_rnd_hi32                  32 位高位清零，供需要的 JIT
fixup_call_args
print_verification_stats
把 env->insn_processed 写入 aux->verified_insns
bpf_vlog_finalize，必要时把 log_true_size 拷回用户态
```

offload 程序在 `check_cfg` 之前还有 `bpf_prog_offload_verifier_prep`，模拟通过后 `bpf_prog_offload_finalize`（`:20853`、`:20870`）。内核 BTF 损坏时日志写 `in-kernel BTF is malformed` 并跳过完整检查（`:20809`）。

`BPF_PROG_TYPE_EXT` 在验证期间借用目标程序的 attach type，返回前把 `expected_attach_type` 清回 0（`:20983`）。

非特权路径拿着 `bpf_verifier_lock`，所以并发的非特权装载是串行的。特权路径不拿这把锁。

---

## 3.2 环境与寄存器类型

```c
/* include/linux/bpf_verifier.h:636 的前半，后面还有 subprog、log、回溯 */
struct bpf_verifier_env {
	u32 insn_idx;
	u32 prev_insn_idx;
	struct bpf_prog *prog;
	const struct bpf_verifier_ops *ops;
	struct bpf_verifier_stack_elem *head; /* 还没走完的分支 */
	int stack_size;
	bool strict_alignment;
	bool test_state_freq;       /* BPF_F_TEST_STATE_FREQ，仅特权 */
	struct bpf_verifier_state *cur_state;
	struct bpf_verifier_state_list **explored_states; /* 剪枝表 */
	struct bpf_map *used_maps[MAX_USED_MAPS];
	u32 used_map_cnt;
	u32 id_gen;                 /* 指针 id，用来判断两个寄存器是不是同一次查找 */
	bool allow_ptr_leaks;
	bool allow_uninit_stack;
	bool bpf_capable;
	bool bypass_spec_v1;
	bool bypass_spec_v4;
	struct bpf_insn_aux_data *insn_aux_data;
	struct bpf_verifier_log log;
	u32 pass_cnt;               /* do_check 被调用的次数 */
};
```

`enum bpf_reg_type`（`bpf.h:876`）里和数据通路直接相关的是：

```c
enum bpf_reg_type {
	NOT_INIT = 0,            /* 没写过 */
	SCALAR_VALUE,            /* 数，不是指针 */
	PTR_TO_CTX,              /* bpf_context */
	CONST_PTR_TO_MAP,        /* 地图本身，由 BPF_LD_MAP_FD 得到 */
	PTR_TO_MAP_VALUE,        /* 元素 value */
	PTR_TO_MAP_KEY,
	PTR_TO_STACK,            /* 帧指针加已知偏移 */
	PTR_TO_PACKET_META,
	PTR_TO_PACKET,           /* skb->data */
	PTR_TO_PACKET_END,
	PTR_TO_FLOW_KEYS,
	PTR_TO_SOCKET,
	PTR_TO_SOCK_COMMON,
	PTR_TO_TCP_SOCK,
	PTR_TO_TP_BUFFER,        /* 可写的 raw tracepoint 缓冲 */
	PTR_TO_XDP_SOCK,
	PTR_TO_BTF_ID,           /* 内核结构，读之前仍可能是 NULL */
	PTR_TO_MEM,
	PTR_TO_BUF,
	PTR_TO_FUNC,
	CONST_PTR_TO_DYNPTR,
	/* OR_NULL 变体 = PTR_MAYBE_NULL | 基类型 */
};
```

`PTR_TO_MAP_VALUE_OR_NULL` 是 `bpf_map_lookup_elem` 的典型返回。没和 0 比较过就解引用，`check_mem_access`（`:6900`）拒绝。比较之后，走非 NULL 的那个分支变成 `PTR_TO_MAP_VALUE`。`PTR_TO_BTF_ID` 的注释写明：这个类型表示「程序自己会处理空指针」，verifier 做分支分析时不假设它非空（`:893`）。

标量带范围：有符号最小最大、无符号最小最大。条件跳转在 `check_cond_jmp_op`（`:14797`）里把范围拆到两个后继上。指针算术若超出 verifier 认识的对象边界，寄存器退化成 `SCALAR_VALUE`，之后不能再当指针用。

---

## 3.3 `do_check`

`do_check_common` 分配 `bpf_verifier_state`，`branches = 1`。主程序把 R1 设成 `PTR_TO_CTX` 并 `mark_reg_known_zero`（`:20105`）。子程序和 `BPF_PROG_TYPE_EXT` 走 `btf_prepare_func_args`。然后调用 `do_check`。

主循环（`:17381`）对当前 `insn_idx`：

```text
insn_idx 越界 → "invalid insn idx %d insn_cnt %d"，-EFAULT
++insn_processed > BPF_COMPLEXITY_LIMIT_INSNS
    → "BPF program is too large. Processed %d insn"，-E2BIG
    上限 1_000_000（include/linux/bpf.h:1794）
若该指令是剪枝点：
    is_state_visited
      返回 1：日志打 "%d: safe"，这条路径结束
      返回 0：继续模拟，并可能把当前状态存进 explored_states
signal_pending → -EAGAIN
need_resched → cond_resched
按 BPF_CLASS 分发：
    BPF_ALU / BPF_ALU64 → check_alu_op
    BPF_LDX             → check_mem_access(..., BPF_READ)
    BPF_STX             → BPF_ATOMIC 走 check_atomic，否则检查写
    BPF_ST              → 立即数写入
    BPF_JMP / BPF_JMP32 → 条件跳转压栈另一侧，或 BPF_CALL / BPF_EXIT
```

`BPF_LOG_LEVEL` 打开时，每条指令用 `print_bpf_insn` 打出来，分支点再打 `print_verifier_state`。`BPF_LOG_LEVEL2` 让状态在每次跳转都打印。级别宏在 `bpf_verifier.h:580`：`BPF_LOG_LEVEL1 = 1`，`BPF_LOG_LEVEL2 = 2`，`BPF_LOG_STATS = 4`，`BPF_LOG_FIXED = 8`。`BPF_LOG_LEVEL` 是前两个的或。

循环不是只走一遍直线。条件跳转把另一侧压进 `env->head`，当前侧走完（`BPF_EXIT` 或剪枝）再 `pop_stack`（`:2067`）。`insn_processed` 把重复走到的指令也算进去，所以 4096 条静态指令照样可以因为路径组合碰到 100 万的上限。

---

## 3.4 剪枝

`is_state_visited`（`:16962`）在剪枝点把当前状态和 `explored_states[insn_idx]` 链上的旧状态比较。

启发式写在 `:16974`：普通程序大约每 4 条指令一个剪枝点。若从上次保存以来跳转少于 2 次、指令少于 8 条，默认不把新状态放进表，用来压 `total_states`。`BPF_F_TEST_STATE_FREQ`、强制检查点、或者跳转历史超过 40 条时，`force_new_state` 为真，这条启发式失效。

比较函数是 `states_equal`（`:16672`）。帧数不同、当前持有的 `bpf_spin_lock` 不同、RCU 锁状态不同，都不算相等。旧状态来自 speculative 模拟、当前状态不是，也不能剪（`:16684` 的注释：speculative 状态永远不能把非 speculative 的搜索剪掉）。`exact` 为真时读写标记和精度标记必须一致；循环里的 `bpf_iter_*` 走这条精确比较，避免把还没标精度的寄存器错剪掉（`:17015` 附近的注释给了一段会漏掉越界读的例子）。

返回 1 表示当前路径上的寄存器范围已经被旧状态覆盖，后面的指令不必再模拟。返回 0 表示要继续，并把当前状态链进去，供以后的路径使用。

---

## 3.5 helper 白名单

`BPF_CALL` 的 `imm` 是 `enum bpf_func_id`。`check_helper_call`（`:10187`）调用 `get_helper_proto`（`:10174`）：

```text
func_id 不在 [0, __BPF_FUNC_MAX_ID) → -ERANGE
    日志：invalid func %s#%d
env->ops->get_func_proto 为空，或返回的 proto 没有 func
    日志：program of this type cannot use helper %s#%d
```

`bpf_verifier_ops.get_func_proto`（`include/linux/bpf.h:968`）按程序类型实现。多数类型先问自己的表，没有再落到 `bpf_base_func_proto`（`kernel/bpf/helpers.c:1825`）。map 查找、`ktime_get_ns`、`tail_call`、ringbuf 在这张基表里。`bpf_probe_read`、`bpf_override_return` 这一类只出现在 tracing 类型的 proto 函数里。

proto 上的 `gpl_only` 为真时，程序的 `gpl_compatible` 必须为 1，否则拒绝。参数类型（`ARG_PTR_TO_CTX`、`ARG_CONST_MAP_PTR`、`ARG_PTR_TO_MEM`）用来检查 R1–R5 的 `bpf_reg_type`。返回类型写回 R0：`RET_PTR_TO_MAP_VALUE_OR_NULL` 就是 lookup 的那个或空指针。

同一个 helper id 在不同类型上可以是不同的 proto。读日志时先看程序类型，再看 id 的名字。

---

## 3.6 拒绝日志怎么读

用户态在 `union bpf_attr` 里提供 `log_level`、`log_buf`、`log_size`（`bpf.h:1440` 一带）。`BPF_LOG_FIXED` 要求缓冲区够大，否则装载失败即使程序本身合法。没有这个位时，内核可以截断日志，程序仍然装上；`log_true_size` 告诉用户态完整日志有多长。

常见行（字符串都在 `verifier.c` 里）：

| 日志 | 含义 |
|------|------|
| `invalid func ...` / `cannot use helper` | id 越界，或这个 `bpf_prog_type` 的 ops 不提供它 |
| `BPF program is too large. Processed N insn` | 路径模拟超过 1_000_000，静态条数可以远小于这个数 |
| `R%d doesn't have constant offset. bpf_spin_lock has to be at the constant offset` | 锁必须在 map value 或对象里的编译期偏移上（`:7637`） |
| `Locking two bpf_spin_locks are not allowed` | 同时只能持有一把（`:7667`） |
| `socket filter progs cannot use bpf_spin_lock yet` | `BPF_PROG_TYPE_SOCKET_FILTER` 明确拒绝（`:17918`） |
| `tracing progs cannot use bpf_spin_lock yet` | tracing 类型同样拒绝（`:17923`） |
| `from A to B: safe` | 剪枝成功，B 处状态已被覆盖 |
| `in-kernel BTF is malformed` | 内核 BTF 不可用，后续依赖 BTF 的检查做不了 |

`BPF_LOG_STATS` 让 `print_verification_stats` 打出处理条数和状态数。`aux->verified_insns` 就是这次的 `insn_processed`，之后能在 fdinfo 里看到（[12](./12-observability.md)）。

特权与非特权的差别不只是 helper 多少。非特权还把死代码改成安全的跳转（`sanitize_dead_code`），并且 `allow_ptr_leaks` 等开关走 `bpf_allow_ptr_leaks()` 的结果（`bpf_check` 里 `:20821`）。读一份拒绝日志时先确认装载进程有没有 `CAP_BPF`。

---

## 3.7 通过之后的改写

模拟只证明安全，不产生最终机器码。`convert_ctx_accesses` 把 `*(u32 *)(ctx + off)` 换成该类型 `bpf_verifier_ops.convert_ctx_access` 生成的指令：对 XDP，`xdp_md.data` 这种 UAPI 字段变成对内核 `xdp_buff` 的加载。`do_misc_fixups` 把一部分 helper 调用内联成 map 的 `map_gen_lookup` 指令序列，array 的实现在 `array_map_gen_lookup`（`kernel/bpf/arraymap.c:208`），注释写明它等价于 `array_map_lookup_elem`。tail call 也在这一步变成解释器或 JIT 认识的内部跳转。

这些改写发生在 `bpf_prog_select_runtime` 之前。JIT 看到的是改写后的 `insnsi`。行号表 `aux->linfo` 记的是改写后的偏移（`bpf_prog_aux` 里 `:1487` 的注释）。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
