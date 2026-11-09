# 6. attach 与 bpf_link

**结论**：`BPF_LINK_CREATE` 按程序类型分到对应的 attach 函数，成功后交回一个 `bpf_link` 的 fd。kprobe 和普通 tracepoint 先有一个 perf event，再把程序设进去；raw tracepoint 按名字找到 `bpf_raw_event_map`；fentry 在装载时已经用 BTF id 定了目标函数，link 把它挂进 `bpf_trampoline`。断点怎样写入、ftrace 怎样改函数入口，本章停在 BPF 调用的那个函数名上。

> `struct bpf_link` 在 `include/linux/bpf.h:1560`，`bpf_link_ops` 在 `:1575`。`link_create` 在 `kernel/bpf/syscall.c:5038`。attach 类型枚举在 `include/uapi/linux/bpf.h:1010`，link 类型在 `:1065`。

---

## 6.1 `bpf_link`

```c
/* include/linux/bpf.h:1560 */
struct bpf_link {
	atomic64_t refcnt;
	u32 id;
	enum bpf_link_type type;
	const struct bpf_link_ops *ops;
	struct bpf_prog *prog;
	union {
		struct rcu_head rcu;
		struct work_struct work;
	};
};

struct bpf_link_ops {
	void (*release)(struct bpf_link *link);     /* 从挂钩上摘下 */
	void (*dealloc)(struct bpf_link *link);     /* 不等 RCU */
	void (*dealloc_deferred)(struct bpf_link *link);
	int (*detach)(struct bpf_link *link);       /* BPF_LINK_DETACH */
	int (*update_prog)(struct bpf_link *link, struct bpf_prog *new_prog,
			   struct bpf_prog *old_prog);
	void (*show_fdinfo)(const struct bpf_link *link, struct seq_file *seq);
	int (*fill_link_info)(const struct bpf_link *link,
			      struct bpf_link_info *info);
};
```

`bpf_link_init`（`syscall.c:2840`）把 `refcnt` 置 1，填 type、ops、prog。随后 `bpf_link_prime` 占一个 fd 和 id，attach 失败则 `bpf_link_cleanup`：它把 `prog` 清空，释放 id 和 fd，调用 `dealloc`，不调用 `release`（`:2861` 的注释）。成功则 `bpf_link_settle` 把 fd 交给用户态。

fd 最后一次关闭走 `release`，再按 ops 决定立刻 `dealloc` 还是等 RCU。睡眠程序还要多等一轮 tasks-trace RCU（`dealloc_deferred` 的注释，`:1581`）。`BPF_LINK_UPDATE` 在 `ops->update_prog` 存在时替换程序，`BPF_F_REPLACE` 要求用户态同时交出旧程序 fd（`link_update`，`:5161`）。

`BPF_PROG_ATTACH` 仍然存在（`:3931`），用于 cgroup 和一部分没有 link 的挂钩。它不返回长期的 link fd，摘除要再发 `BPF_PROG_DETACH`。同一类挂钩在 v6.6 里往往两条都能走，下面按 `link_create` 的 switch 列出 link 这一侧。

---

## 6.2 `link_create` 分发

`link_create`（`:5038`）先处理 `BPF_STRUCT_OPS`（这种 link 没有单独的 prog fd）。其余路径 `bpf_prog_get`，再 `bpf_prog_attach_check_attach_type`，然后按 `prog->type`：

| `prog->type` | attach | 函数 |
|--------------|--------|------|
| cgroup 的 skb / sock / sock_addr / device / sysctl / sockopt，以及 `SOCK_OPS` | 目标是 cgroup fd | `cgroup_bpf_link_attach` |
| `EXT` | `target_fd` + `target_btf_id` | `bpf_tracing_prog_attach`（`:3185`） |
| `TRACING` 且 `expected_attach_type == BPF_TRACE_RAW_TP` | BTF 名在装载时已经记下 | `bpf_raw_tp_link_attach(prog, NULL)` |
| `TRACING` 且 `BPF_TRACE_ITER` | | `bpf_iter_link_attach` |
| `TRACING` 的 fentry / fexit / modify_return | | `bpf_tracing_prog_attach` |
| `LSM` 且 `BPF_LSM_CGROUP` | cgroup fd | `cgroup_bpf_link_attach` |
| `LSM` 的 `BPF_LSM_MAC` | | `bpf_tracing_prog_attach` |
| `FLOW_DISSECTOR`、`SK_LOOKUP` | 网络命名空间 | `netns_bpf_link_create` |
| `XDP` | `target_ifindex` | `bpf_xdp_link_attach`（`net/core/dev.c:9676`） |
| `SCHED_CLS` | `BPF_TCX_INGRESS` / `EGRESS` | `tcx_link_attach` |
| `NETFILTER` | pf、hooknum、priority | `bpf_nf_link_attach` |
| `PERF_EVENT`、`TRACEPOINT` | perf event fd | `bpf_perf_link_attach`（`:3650`） |
| `KPROBE` 且 `BPF_PERF_EVENT` | perf event fd | `bpf_perf_link_attach` |
| `KPROBE` 且 `BPF_TRACE_KPROBE_MULTI` | 一批符号 | `bpf_kprobe_multi_link_attach`（`kernel/trace/bpf_trace.c:2890`） |
| `KPROBE` 且 `BPF_TRACE_UPROBE_MULTI` | | `bpf_uprobe_multi_link_attach` |

`TRACING` 要求 `link_create.attach_type` 等于装载时的 `expected_attach_type`，否则 `-EINVAL`（`:5076`）。

老的 `BPF_RAW_TRACEPOINT_OPEN`（`:3771`）仍然可用：它取出 prog，把用户态的名字指针交给 `bpf_raw_tp_link_attach`。

---

## 6.3 kprobe 与 tracepoint：BPF 侧

`BPF_PROG_TYPE_KPROBE` 的内核 ctx 是 `struct pt_regs`，用户态类型是 `bpf_user_pt_regs_t`（`bpf_types.h:40`）。x86 使用 `include/uapi/asm-generic/bpf_perf_event.h:7` 的 `typedef struct pt_regs bpf_user_pt_regs_t`。程序从寄存器读参数。返回值由 kprobe 处理函数解释：`trace_call_bpf`（`kernel/trace/bpf_trace.c:110`）的注释写明，0 表示这次事件被过滤掉，1 表示可以写入环缓冲，其他值目前按 1 处理。

`trace_call_bpf` 用 per-cpu 的 `bpf_prog_active` 挡重入。计数已经不是 1 就返回 0，不再调用程序，也不把事件送进环缓冲（`:116`）。这是 BPF 执行时的互斥，不是断点的实现。

挂到单个 kprobe 或单个 tracepoint 的 link 路径：

```text
用户态 perf_event_open          得到 perf event fd
BPF_LINK_CREATE
  bpf_perf_link_attach          syscall.c:3650
    perf_event_get(target_fd)
    bpf_link_init(..., BPF_LINK_TYPE_PERF_EVENT, ...)
    perf_event_set_bpf_prog(event, prog, bpf_cookie)    :3680
    bpf_link_settle
```

`flags` 非 0 则 `-EINVAL`（`:3658`）。`CONFIG_PERF_EVENTS` 关掉时这个函数返回 `-EOPNOTSUPP`（`:3695`）。

`perf_event_set_bpf_prog` 把程序挂到已经存在的 perf event 上。event 是 kprobe 还是 tracepoint，由 `perf_event_open` 的 `attr.type` 决定。kprobe PMU 和 tracepoint PMU 的注册、断点指令、`kprobe_events` 文件格式，在 [tracing/08](../tracing/08-kprobes-core.md) 和 [tracing/09](../tracing/09-kprobe-events.md)。本章只要求：fd 指向一个能接受 BPF 的 perf event，程序类型是 `KPROBE`、`TRACEPOINT` 或 `PERF_EVENT`。

`BPF_PROG_TYPE_TRACEPOINT` 的类型表把 ctx 记成 `__u64`（`bpf_types.h:41`）。perf 把该事件的记录交给程序，字段来自 `TP_STRUCT__entry`。`sched_switch` 的字段在 `include/trace/events/sched.h:231`：`prev_comm`、`prev_pid`、`prev_prio`、`prev_state`、`next_comm`、`next_pid`、`next_prio`。

一次挂多个 kprobe 走 `bpf_kprobe_multi_link_attach`（`bpf_trace.c:2890`），link 类型是 `BPF_LINK_TYPE_KPROBE_MULTI`（枚举值 8，`bpf.h:1074`）。符号解析和在每个地址上放入口的过程属于 kprobe 子系统，这里不展开。

没有 link 的老办法是对 perf fd 做 `ioctl(PERF_EVENT_IOC_SET_BPF)`（`include/uapi/linux/perf_event.h:555`）。程序的生命周期跟着 perf fd，而不是 link fd。

---

## 6.4 raw tracepoint

`BPF_PROG_TYPE_RAW_TRACEPOINT` 和 `RAW_TRACEPOINT_WRITABLE` 在 `bpf_raw_tp_link_attach`（`syscall.c:3701`）里从用户态拷贝名字（最多 127 个字符加 NUL）。`BPF_PROG_TYPE_TRACING` 且 attach 类型是 `BPF_TRACE_RAW_TP` 时不允许再传名字，用的是装载时记在 `aux->attach_func_name` 里的字符串（`:3720`）。

```text
bpf_get_raw_tracepoint(tp_name)     找不到 → -ENOENT
bpf_link_init(..., BPF_LINK_TYPE_RAW_TRACEPOINT, ...)
bpf_probe_register(btp, prog)
bpf_link_settle
```

运行时 `bpf_trace_runN` 把参数收成 `u64 args[]`，`__bpf_trace_run`（`bpf_trace.c:2314`）在 `prog->active` 为 1 时调用 `bpf_prog_run(prog, args)`。重入则 `bpf_prog_inc_misses_counter`，这次不跑。ctx 的形状是 `struct bpf_raw_tracepoint_args`（`bpf.h:6979`）。

`sys_enter` 这个名字对应 [syscalls/09](../syscalls/09-hooks.md) 里的 `trace_sys_enter`。参数是系统调用号和寄存器里的参数。BPF 程序在 tracepoint 被调用时才运行；谁把 `SYSCALL_WORK_SYSCALL_TRACEPOINT` 置上，仍是系统调用章的内容。

`bpf_probe_register`（`bpf_trace.c:2391`）转到 `__bpf_probe_register`（`:2373`）。这里还有两道 BPF 侧的检查，过了才碰 tracepoint：

```text
prog->aux->max_ctx_offset > btp->num_args * sizeof(u64)  → -EINVAL
prog->aux->max_tp_access  > btp->writable_size           → -EINVAL
tracepoint_probe_register_may_exist(tp, btp->bpf_func, prog)
```

`max_ctx_offset` 是 verifier 看到的对 ctx 的最大读取。程序读了 `args[3]`，而这个 tracepoint 只有两个参数，装载可以通过（ctx 类型是变长的 `args[0]`），attach 会在这里失败。`tracepoint_probe_register_may_exist` 把 `btp->bpf_func` 登记成该 tracepoint 的探针，探针再调用 `bpf_trace_runN`。探针链表怎么挂、tracepoint 的 `static_call` 怎么改，见 [tracing/06](../tracing/06-tracepoint-and-TRACE_EVENT.md)。摘除走 `bpf_probe_unregister`（`:2396`），里面是 `tracepoint_probe_unregister`。

可写变体 `RAW_TRACEPOINT_WRITABLE` 允许程序改某些事件提供的缓冲，verifier 用 `PTR_TO_TP_BUFFER` 跟踪这块内存。不是每个 raw tracepoint 都允许写。

---

## 6.5 fentry、fexit、modify_return

这三个 attach 类型属于 `BPF_PROG_TYPE_TRACING`（`bpf.h:1035` 起：`BPF_TRACE_FENTRY`、`BPF_TRACE_FEXIT`、`BPF_MODIFY_RETURN`）。装载时 `expected_attach_type` 必须是其中之一，并且 `attach_btf_id` 指向 vmlinux BTF 里的一个函数。`bpf_tracing_prog_attach`（`syscall.c:3185`）不接受别的 `expected_attach_type`。

```text
bpf_tracing_prog_attach
  目标：装载时保存在 aux->dst_trampoline，或按 attach_btf 重新算 key
  bpf_check_attach_target
  bpf_trampoline_get
  bpf_link_init(..., BPF_LINK_TYPE_TRACING, ...)
  bpf_trampoline_link_prog          trampoline.c:588
    __bpf_trampoline_link_prog      :536
      按 attach 类型放进 trampoline 的 hlist
      总数达到 BPF_MAX_TRAMP_LINKS → -E2BIG
      bpf_trampoline_update         :402
```

`bpf_trampoline_update` 调用 `arch_prepare_bpf_trampoline` 生成映像（`:450`）。fexit 或 modify_return 在场时置 `BPF_TRAMP_F_CALL_ORIG | BPF_TRAMP_F_SKIP_FRAME`，否则置 `BPF_TRAMP_F_RESTORE_REGS`（`:430`）。映像挂上去的函数是 `register_fentry`（`:205`）：

```text
ftrace_location(ip) 非 0：
  ftrace_set_filter_ip
  register_ftrace_direct(fops, image)
否则：
  bpf_arch_text_poke(ip, BPF_MOD_CALL, NULL, image)
```

`register_ftrace_direct` 和 `ftrace_set_filter_ip` 是 ftrace 的接口。direct call 怎样和函数图 tracer 共用一个入口、`ftrace_ops` 的标志位是什么意思，见 [tracing/04](../tracing/04-dynamic-ftrace.md)。本章的边界是：BPF link 已经拿着目标函数地址和一份 trampoline 映像，接下来要么登记 direct call，要么对没有 ftrace 位置的地址做一次 `BPF_MOD_CALL` 的 text poke。

`BPF_PROG_TYPE_EXT` 走同一函数，但 `bpf_attach_type_to_tramp` 得到 `BPF_TRAMP_REPLACE`。trampoline 上已经有 fentry/fexit 时返回 `-EBUSY`；反过来，extension 占着 `tr->extension_prog` 时也不能再挂 fentry（`__bpf_trampoline_link_prog`，`:546`）。替换用 `bpf_arch_text_poke(..., BPF_MOD_JUMP, ...)`，把目标开头改成跳进扩展程序。

装载成功后 `aux->dst_trampoline` 会被清掉（`bpf_tracing_prog_attach` 在 link 成功之后的赋值，`:3349` 一带）。再次 attach 必须还能从 `attach_btf` 找回目标，否则 `-EINVAL`。

fentry 的程序在目标函数入口运行，参数寄存器就是原函数的参数，类型来自 BTF 原型。它不经过 kprobe 的 `pt_regs` 打包。目标没有 ftrace 位置、text poke 也不接受这个地址时，`register_fentry` 返回错误，link 创建失败。这是 v6.6 上 fentry 能挂住的条件：BTF 里找得到函数，并且 `register_fentry` 的两条路有一条成功。

---

## 6.6 和程序类型的对应

一张程序 fd 不能换一种挂钩。`bpf_prog_attach_check_attach_type` 在 link 创建前就拒绝不匹配的 `attach_type`。`KPROBE` 程序不能拿去 `bpf_xdp_link_attach`，`XDP` 程序也不能放进 `bpf_perf_link_attach`。`expected_attach_type` 对 `TRACING`、`LSM`、`EXT` 是装载时的合同；对 XDP，`BPF_XDP_DEVMAP` 和 `BPF_XDP_CPUMAP` 表示这个程序只能挂在 map 的值里，不能挂到设备上（`dev_xdp_install` 的检查，见 [07](./07-xdp.md)）。

link 的 id 放在 `link_idr`（`syscall.c:59`）。`BPF_LINK_GET_FD_BY_ID` 和 `BPF_LINK_GET_NEXT_ID` 用来枚举。fdinfo 由 `ops->show_fdinfo` 打印，格式因类型而异。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
