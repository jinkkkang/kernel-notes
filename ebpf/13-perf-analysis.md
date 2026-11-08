# 13. 性能与排障

**结论**：装不上先看 errno 和 verifier 日志，不要先看运行时间。跑起来之后，XDP 的丢包分清是程序返回了 `XDP_DROP` 还是 redirect 失败；map 的 `-EBUSY` 来自同一 CPU 重入桶锁；attach 的开销主要在挂钩把程序调用起来的那一层，不在解释器本身。本机没有 bpftool、bpftrace、perf，也没有 `ip`，下面除了已经在 [12](./12-observability.md) 读过的 sysctl，命令都是 RECIPE。

> 复杂度上限 `BPF_COMPLEXITY_LIMIT_INSNS` 在 `include/linux/bpf.h:1794`。hash 桶锁的 `-EBUSY` 在 `kernel/bpf/hashtab.c:159`。generic XDP 的丢弃在 `net/core/dev.c:5137`。tail call 上限 33 在 `bpf.h:1795`。

---

## 13.1 装载被拒绝

`bpf(BPF_PROG_LOAD)` 的返回值先按 errno 分：

| errno | 发生在 | 下一步 |
|-------|--------|--------|
| `-EPERM` | `unprivileged_bpf_disabled` 非 0 且没有 `CAP_BPF`；或类型要 `CAP_NET_ADMIN` / `perfmon_capable()` | 看 [10](./10-bpf-syscall-and-fs.md)。本机该 sysctl 是 2 |
| `-E2BIG` | `insn_cnt` 超过 4096（无 `CAP_BPF`）或 1_000_000；或 verifier 的 `insn_processed` 超过 1_000_000 | 日志里有 `BPF program is too large. Processed %d insn` 才是路径爆炸。静态条数很小也会走到这里 |
| `-EINVAL` | 未知 `prog_flags`、attach 类型和程序类型不配、helper 不被允许 | 打开 `BPF_LOG_LEVEL1` |
| `-ENOMEM` | `bpf_prog_alloc` 或 verifier 的 `explored_states` 失败 | 程序没问题，是内存 |
| `-EAGAIN` | verifier 里 `signal_pending`（`do_check`，`verifier.c:17433`） | 重试 |

helper 被拒的两句原文是 `invalid func %s#%d` 和 `program of this type cannot use helper %s#%d`（`check_helper_call`，`verifier.c:10204`）。第二句表示编号合法，但 `bpf_verifier_ops.get_func_proto` 没有把它交给这个类型。把一段 kprobe 程序原样声明成 `SCHED_CLS` 就会这样。

指针类错误要看日志里该指令的寄存器类型。`PTR_TO_MAP_VALUE_OR_NULL` 没和 0 比较就解引用，是 lookup 之后缺了判断。`PTR_TO_PACKET` 越过 `PTR_TO_PACKET_END`，是 XDP 或 tc 读包时 `data + off` 没有先和 `data_end` 比较。栈上没写过就读，类型还是 `NOT_INIT`。

`verified_insns` 远大于 `prog_len` 时，时间花在路径组合上。减分支、把 map 查找的结果收成标量再分支、避免在循环里制造新的指针 id，都能让 `is_state_visited` 更容易返回 1。`BPF_F_TEST_STATE_FREQ` 只用于对照剪枝是否掩盖了问题，它会让验证更慢。

RECIPE：把日志落到文件再装一次。本机没有 bpftool，用 [demos/](./demos/) 里的装载器时把 `log_buf` 打开即可。

```text
# RECIPE：bpftool 未安装，未跑
bpftool prog load obj.o /sys/fs/bpf/p
# 失败时 bpftool 会把 verifier 日志打到 stderr
```

---

## 13.2 map 争用

hash 的 `htab_lock_bucket` 在同一 CPU 上发现 `map_locked[hash]` 已经非 0 时返回 `-EBUSY`（`hashtab.c:159`），不自旋。这发生在程序更新 hash 的过程中又进入了同一张表，例如 tail call 进另一个也会更新它的程序，或者 helper 的调用链重入。调用者看到的是 update 的返回值，不是死锁。

数据路径上的计数用 `BPF_MAP_TYPE_PERCPU_ARRAY`：`array_map_lookup_elem` 没有桶锁，每 CPU 一个槽，用户态按 [04](./04-maps.md) 的缓冲长度把各 CPU 加起来。需要跨 CPU 一致的单值时再考虑 value 里的 `bpf_spin_lock`，并且记住 socket filter 和 tracing 程序用不了这把锁。

`BPF_F_NO_PREALLOC` 让 hash 在更新时分配元素。满负荷时这会在 RCU 读侧之外的分配路径上拉长更新时间，还会在删除时把释放推迟到 grace period。默认的预分配把失败模式收成 `-E2BIG`，延迟更稳。LRU hash 在满的时候淘汰，而不是返回 `-E2BIG`，计数类的 key 不适合放进去。

用户态批量 `BPF_MAP_UPDATE_ELEM` 与程序并发时，hash update 拿桶锁，lookup 走 RCU，读者可能看到旧 value 或新 value，不会看到半个指针。map-in-map 的替换在系统调用返回前 `synchronize_rcu`（`maybe_wait_bpf_programs`）。这个等待的时间是当前 RCU grace period，不要在包处理进程里同步地换内层 map。

RECIPE：对照 fdinfo 的 `run_cnt` 和自己在 per-cpu 数组里的计数。两者长期偏离时，先确认 `bpf_stats_enabled` 是 1，再怀疑有路径没走到程序。

---

## 13.3 XDP 丢包

generic 模式下，程序返回 `XDP_DROP`、`XDP_ABORTED` 或其他非法值时，`netif_receive_generic_xdp` 调用 `kfree_skb`（`dev.c:5143`）。非法值还会 `trace_xdp_exception`。redirect 失败走 `kfree_skb_reason(..., SKB_DROP_REASON_XDP)`（`do_xdp_generic` 的 `out_redir`，`:5208`）。

所以「包没进协议栈」有三种来源，计数不在同一个地方：

1. 程序主动 `XDP_DROP`。要在程序里用 per-cpu 数组自己记。内核不会按这个原因拆开 `rx_dropped`。
2. 返回值不是合法动作。看 `trace_xdp_exception`。
3. `XDP_REDIRECT` 但 map 槽是空的、设备 down、或者 cpumap 队列满。generic 路径的 drop reason 是 `SKB_DROP_REASON_XDP`。

`XDP_PASS` 之后的丢弃已经离开 XDP，属于 [networking/17](../networking/17-perf-analysis.md) 里的 softirq 和 qdisc。`tcpdump` 在 generic XDP 之后的 `ptype_all` 上，看不到第 1 种丢弃。

driver 模式把这三种决定做在 `ndo_bpf` 安装的程序里，发生在 `build_skb` 之前。virtio-net 的队列入口在 [networking/13](../networking/13-virtio-net-guest.md)。没有 `ip`、也没有第二块网卡时，本机用 `BPF_PROG_TEST_RUN` 看 R0，不把程序挂到设备上。步骤和当次结果在 [demos/README.md](./demos/README.md)。

RECIPE：有 veth 时把同一程序以 `XDP_FLAGS_SKB_MODE` 挂上，从对端发一帧，再读 map。挂载命令依赖 iproute2，本机没有 `ip`。

```text
# RECIPE：未跑。本机没有 iproute2
ip link add veth0 type veth peer name veth1
ip link set veth0 up
ip link set veth1 up
ip link set veth0 xdpgeneric obj drop.o sec xdp
# 对端发包后看 map，再 ip link set veth0 xdpgeneric off
```

`dev_xdp_install` 不允许这块设备上 generic 和 driver 同时存在。已经有 driver 模式时再挂 generic 会得到 `-EEXIST`，extack 字符串是 `Native and generic XDP can't be active at the same time`。

---

## 13.4 attach 的开销

kprobe 上的 BPF 每命中一次都要进 `trace_call_bpf`。这个函数在 `bpf_prog_active` 非 1 时直接返回，事件既不进 BPF 也不进环缓冲。高频符号上这既是开销也是静默丢数。`prog->active` 对 raw tracepoint 是另一计数，丢数在 fdinfo 的 `recursion_misses`。

fentry 避开了 kprobe 的断点，代价在 `bpf_trampoline_update` 生成的映像和 `register_ftrace_direct`。映像里若带 `BPF_TRAMP_F_CALL_ORIG`（有 fexit 或 modify_return），每次调用都要进原函数再回来。只要 fentry、没有 fexit 时标志是 `BPF_TRAMP_F_RESTORE_REGS`（`trampoline.c:436`）。一个函数上的 link 数量到 `BPF_MAX_TRAMP_LINKS` 就 `-E2BIG`。

XDP 的 per-packet 成本是一次 `bpf_prog_run_xdp` 加上程序自己的 map 访问。generic 模式还要把 skb 线性化、来回 push/pull MAC 头（`bpf_prog_run_generic_xdp`）。driver 模式省掉这次线性化，前提是驱动已经把帧放在 `xdp_buff` 里。把 generic 上测到的每包时间当成 driver 模式的时间，会把线性化算进程序头上。

tail call 把一次调用换成跳转，栈不增加，但每次都重新从目标程序入口执行，并且有 33 次的上限。用它拆分程序是为了绕开 verifier 的路径数，不是为了少几次函数调用。JIT 关闭时解释器的跳转表在 `___bpf_prog_run`；本机 `bpf_jit_enable` 是 1，正常装载会走 x86 JIT。对照解释器开销要把 sysctl 改成 0 再装，已经装上的程序不会因为事后修改 sysctl 而重新编译。

统计键打开之后，每次 `bpf_prog_run` 多一次 `sched_clock` 和 per-cpu 计数（`include/linux/filter.h:601`）。用 `run_time_ns / run_cnt` 估每包时间时，把这点算进误差，并且不要在生产上长期开着。本机该键是 0，fdinfo 里的 `run_time_ns` 保持 0，不能用来算耗时。

---

## 13.5 本机结果

| 项 | 状态 |
|----|------|
| `/proc/sys/kernel/unprivileged_bpf_disabled`、`bpf_jit_enable`、`bpf_stats_enabled` | 实测，见 [12](./12-observability.md) |
| root 读的 harden / kallsyms / limit | 实测，同节 |
| map 的 create / update / lookup | 实测。`04-map-user` 打印 `value=42` |
| XDP `BPF_PROG_TEST_RUN` | 实测。`03-xdp-testrun` 打印 `retval=1 map0=1`，没有挂设备 |
| kprobe `__x64_sys_getpid` | 实测。`01-kprobe-count` 打印 `hits=100` |
| 把 XDP 挂到 veth 或物理网卡 | RECIPE。没有 `ip`，也不写网卡环 |
| bpftrace 的 `sched_switch` | RECIPE。没有 bpftrace |
| `bpftool prog profile`、`perf` 采样 `bpf_prog_run` | RECIPE。两个工具都没装 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
