# 9. helper、RCU 与 tail call

**结论**：helper 是一条 `BPF_CALL`，`imm` 为 `bpf_func_id`。能不能调用由程序类型的 `get_func_proto` 决定，基表在 `bpf_base_func_proto`。map 的查找要求 RCU 读侧；hash 的更新拿桶上的 `raw_spinlock`，同一 CPU 重入得到 `-EBUSY`。`bpf_tail_call` 跳进 `BPF_MAP_TYPE_PROG_ARRAY`，计数到 33 停止。

> id 表由 `___BPF_FUNC_MAPPER` 生成，宏从 `include/uapi/linux/bpf.h:5675` 开始，枚举在 `:5900`。基表函数在 `kernel/bpf/helpers.c:1825`。tail call 的 proto 在 `kernel/bpf/core.c:2963`，解释器实现在 `:2046`。

---

## 9.1 调用约定与白名单

`BPF_JMP | BPF_CALL` 的 `imm` 是 helper 编号。R1–R5 是参数，返回值在 R0，R1–R5 作废，R6–R9 保留。这和 [02](./02-prog-and-insn.md) 的寄存器约定相同。`check_helper_call` 的拒绝日志见 [03](./03-verifier.md)。

编号的前一段（`bpf.h:5676`）：

| id | 名字 | 典型用途 |
|----|------|----------|
| 1–3 | `map_lookup_elem` / `update` / `delete` | 所有能访问 map 的类型 |
| 5 | `ktime_get_ns` | 时钟 |
| 7 | `get_prandom_u32` | 标量，verifier 把它当未知数 |
| 8 | `get_smp_processor_id` | 当前 CPU |
| 12 | `tail_call` | 跳进 prog array |
| 14 | `get_current_pid_tgid` | 高 32 位 tgid，低 32 位 pid |
| 23 | `redirect` | 改向设备 |
| 25 | `perf_event_output` | 写到 perf event array |

`bpf_base_func_proto` 的 switch 从 `BPF_FUNC_map_lookup_elem` 开始（`helpers.c:1828`），包含 push/pop/peek、`ktime_get_ns` / `ktime_get_boot_ns` / `ktime_get_tai_ns`、`tail_call`，以及 ringbuf 的 reserve / submit / discard / query（`:1856`）。类型自己的 `get_func_proto` 可以在调用基表之前先返回更专门的 proto，例如网络程序的 `bpf_skb_load_bytes`、`bpf_xdp_adjust_head`、`bpf_redirect_map`。

`gpl_only` 为真的 proto 要求 `prog->gpl_compatible`。装载时的 license 字符串由 `license_is_gpl_compatible` 判断（`bpf_prog_load`，`syscall.c:2721`）。

`BPF_F_SLEEPABLE` 置位后 `aux->sleepable` 为真。这种程序可以走到会睡眠的 helper。不会睡眠的挂钩（NAPI 里的 XDP、`trace_call_bpf` 里的 `cant_sleep()`）拒绝装载睡眠程序。装载标志的允许集合在 `bpf_prog_load` 的 `:2607`。

---

## 9.2 自旋锁与 RCU

hash 查找的第一行检查是 `WARN_ON_ONCE(!bpf_rcu_lock_held())`（`hashtab.c:684`）。挂钩在调用 `bpf_prog_run` 之前要处在 RCU 读侧：XDP 的 `bpf_prog_run_xdp` 注释写明驱动在 NAPI poll 里、`local_bh_disable` 之下调用（`include/net/xdp.h:498`）；raw tracepoint 的 `__bpf_trace_run` 自己 `rcu_read_lock`（`bpf_trace.c:2321`）。value 指针只在这次 RCU 读侧内有效。想把指针塞进 map 留给以后的程序用，要用 `kptr` 那套 BTF 字段，而不是保存 lookup 的返回值。

`bpf_spin_lock` 是嵌在 map value 里的字段，位置必须是编译期常数。verifier 同时只允许持有一把（`verifier.c:7667`，日志 `Locking two bpf_spin_locks are not allowed`）。socket filter 和 tracing 程序被明确拒绝（`:17918`、`:17923`）。持锁区间里不能做 `BPF_LD_ABS` / `BPF_LD_IND`（`:15216`）。

hash 更新的另一把锁是桶上的 `raw_spinlock`，见 [04](./04-maps.md)。同一 CPU 上 `map_locked[]` 已经非 0 就返回 `-EBUSY`，避免程序在 helper 里再进同一张表。per-cpu array 和 per-cpu hash 让每个 CPU 写自己的槽，数据路径上可以不拿这把桶锁。

`BPF_MAP_TYPE_ARRAY` 的 lookup 不加锁，value 和 map 一起活到释放。并发的 `BPF_STX` 是普通存储，不是原子的，除非指令用了 `BPF_ATOMIC`。计数若不能丢，用 per-cpu 槽再在用户态求和，或者对 value 里的 `bpf_spin_lock` 配对使用 `BPF_F_LOCK`。

---

## 9.3 tail call

`bpf_tail_call` 的 proto（`core.c:2963`）三个参数：ctx、`CONST_PTR_TO_MAP`、下标。`ret_type` 是 `RET_VOID`：调用成功时当前程序不再返回，失败时执行落到下一条指令。

解释器（`:2046`）把 R2 当成 `bpf_map *`，转成 `bpf_array`，用 R3 当下标：

```text
index >= max_entries          → 继续下一条
tail_call_cnt >= 33           → 继续下一条     MAX_TAIL_CALL_CNT，bpf.h:1795
ptrs[index] 为 NULL           → 继续下一条
否则 insn = prog->insnsi，tail_call_cnt++，从头执行那个程序
```

33 次是运行时的上限，不是 map 的 `max_entries`。每次跳转都重新从目标程序的第一条指令开始，栈不叠加，所以不受 `MAX_BPF_STACK` 的多层累加；R6–R9 也不会按函数调用那样保存。ctx 必须仍是原来的那块，verifier 用 `ARG_PTR_TO_CTX` 保证这一点（`:2064` 的注释）。

`bpf_check_tail_call`（`core.c:2402`）在 JIT 选定之后跑：prog array 里的程序必须全是 JIT 或全是解释，不能混。`aux->tail_call_reachable` 让 JIT 为可能被尾调用的程序保留入口约定。

尾调用的目标在装载时就要放进 `BPF_MAP_TYPE_PROG_ARRAY`。map 槽后来可以换程序，但新程序仍要能和原来的 ctx 类型、JIT 形态对上。换槽使用的是 fd 数组的更新路径（`bpf_map_update_value` 里的 `bpf_fd_array_map_update_elem`）。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
