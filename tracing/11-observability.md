# 11. 观测

**结论**：先看顶层 `current_tracer`、`tracing_on`、`buffer_size_kb` 和 `per_cpu/cpuN/stats`，再决定要开 function、某个 event，还是一根 kprobe。下面的数字都是 sampled on 6.12.113+（4 核，root）。文件节点能在 v6.6.160 对上的，行号写在旁边；6.12 多出来的节点单独标出。

> 非 root 的 capability 集是空的。`/sys/kernel/tracing` 模式是 `700`，不经 root 列目录得到 `Permission denied`。tracefs 自己的说明在该目录的 `README`：默认去掉 other 的权限位，挂载时可以用 `gid=` 把一个组加回去。

---

## 11.1 顶层当时的状态

```text
current_tracer=nop
available_tracers=function_graph function nop
tracing_on=1
buffer_size_kb=7 (expanded: 1408)
buffer_total_size_kb=28 (expanded: 5632)
trace_clock=[local] global counter uptime perf mono mono_raw boot tai x86-tsc
set_ftrace_filter=#### all functions enabled ####
set_ftrace_notrace=#### no functions disabled ####
enabled_functions 行数=0
kprobe_events 空
uprobe_events 空
events/enable=0
events/sched/sched_switch/enable=0
instances/ 空
```

`available_tracers` 只有 function、function_graph、nop。irqsoff / preemptoff / wakeup 没有出现在这份构建里。v6.6.160 里它们的代码在 `kernel/trace/trace_irqsoff.c`，要对应的 `CONFIG_*` 才会注册。

`buffer_size_kb` 的 `7 (expanded: 1408)` 是 `tracing_entries_read` 在环还没展开时的格式（`trace.c:7101`）。1408 等于 `TRACE_BUF_SIZE_DEFAULT`（1441792）除以 1024（`trace.c:861`）。四个 CPU 的合计是 28 与 5632。第一次 `echo function > current_tracer` 会把环扩开，见 [03](./03-tracefs-and-array.md)。这次采样没有切顶层 tracer，所以顶层仍是未展开的 7。

`trace_clock` 的名字表在 `trace.c:1600`，`ARCH_TRACE_CLOCKS` 在 x86 上加上 `x86-tsc`。方括号是当前值。`local` 的 `in_ns` 为 1。

`set_ftrace_filter` 的那行文字由 `ftrace.c:3856` 在 filter hash 为空时打印，含义是全部函数都算匹配（`hash_contains_ip`，`:1490`）。

---

## 11.2 目录里有什么

v6.6.160 的 `ftrace_init_dyn_tracefs`（`ftrace.c:6500`）在顶层创建：

| 文件 | 作用 |
|------|------|
| `available_filter_functions` | 能被 filter 的函数，一行一个 |
| `available_filter_functions_addrs` | 同上，带地址（`:6505`） |
| `enabled_functions` | 当前真正挂上回调的函数 |
| `set_ftrace_filter` / `set_ftrace_notrace` | 顶层 ops 的两张 hash |
| `set_ftrace_pid` / `set_ftrace_notrace_pid` | 按 pid 收窄 function tracer |
| `set_graph_function` / `set_graph_notrace` | function_graph 的子树（`ftrace.c:6517`）。只在顶层 |
| `touched_functions` | 启动后被回调碰过的函数 |
| `dyn_ftrace_total_info` | `dyn_ftrace` 记录的规模 |
| `kprobe_events` / `kprobe_profile` | [09](./09-kprobe-events.md) |
| `uprobe_events` / `uprobe_profile` | [10](./10-uprobe.md) |

本机读到：

```text
available_filter_functions  50524 行
available_events            2580 行
events 下 enable 文件       2683 个
dyn_ftrace_total_info       50524 pages:208 groups: 3
```

enable 文件比 `available_events` 多，是因为有的事件带 `hist` 等附加节点，或 enable 出现在 subsystem 目录上（`events/enable`、`events/sched/enable`）。不要把 2683 当成事件个数。50524 与 filter 函数行数相同，是这份内核的 ftrace 调用点数量，不是 v6.6.160 的常量。

`available_filter_functions` 的前几行是 `__traceiter_initcall_level`、`__probestub_initcall_level` 一类，对应 [06](./06-tracepoint-and-TRACE_EVENT.md) 里宏生成的迭代器和空 stub。`do_sys_openat2`、`vfs_read`、`schedule` 在表里。`__x64_sys_getpid` 当时不在这张表里，所以 function tracer 的 filter 写它会失败；kprobe 仍可以按符号挂，[09](./09-kprobe-events.md) 的实测就是这样。

`events/sched/sched_switch/` 下有 `enable`、`filter`、`format`、`id`、`trigger`。`format` 的公共头和 `prev_comm` 等字段与 `include/trace/events/sched.h:222` 的 `TP_STRUCT__entry` 一致，`prev_state` 是 `long`、8 字节。当次 `id` 是 373，这是本机分配的类型号，换一次启动会变。

`per_cpu/cpu0/` 有 `buffer_size_kb`、`stats`、`trace`、`trace_pipe`、`trace_pipe_raw`。`stats` 由 `tracing_stats_read`（`trace.c:8511`）打印。当时 cpu0：

```text
entries: 0
overrun: 0
commit overrun: 0
bytes: 0
oldest event ts:     0.000000
now ts:   222.743259
dropped events: 0
read events: 0
```

`overrun` 对应 overwrite 模式下盖掉的页，`dropped events` 对应非 overwrite 时丢掉的新事件，`commit overrun` 对应写者绕过了还没提交的页。三个计数的来源在 [02](./02-ring-buffer.md)。`now ts` 只说明当时 trace clock 已经在走，环里没有事件。

`trace_options` 里当时包括 `overwrite`、`function-trace`（无 `no` 前缀表示打开）。`options/` 目录下有同名文件，其中能看到 `funcgraph-abstime`、`funcgraph-cpu`。没有 `snapshot` 文件。

本机 6.12 还有 `buffer_subbuf_size_kb`。v6.6.160 的 `kernel/trace/trace.c` 没有这个节点，不要把它写进 6.6 的操作步骤。`buffer_size_kb` 的括号格式在 6.6 已经有了，见上面的 `:7101`。

---

## 11.3 打开过的三件事

三次都在 `instances/kn-demo` 里做，做完 `rmdir`。顶层 `current_tracer` 始终是 `nop`，顶层 `buffer_size_kb` 仍是 `7 (expanded: 1408)`。instance 刚创建时 `buffer_size_kb` 读到 1410，这是 6.12 上这份 instance 的数，不是 `TRACE_BUF_SIZE_DEFAULT` 的另一种写法。

function，filter 只有 `do_sys_openat2`：

```text
enabled_functions（顶层，instance 的 function tracer 开着时）:
do_sys_openat2 (1)    tramp: 0xffffffffc0203000 (function_trace_call+0x0/0x110) ->function_trace_call+0x0/0x110
```

引用计数 1，回调符号是 `function_trace_call`，和 `trace_functions.c:172` 的函数名一致。trampoline 地址是当次内核的运行时地址。instance 目录里没有 `enabled_functions`，这份清单只在顶层，和 `ftrace_init_dyn_tracefs` 只调用一次相符。

`trace` 里摘的几行（sampled on 6.12.113+）：

```text
cat-3802    [003] .....   250.031103: do_sys_openat2 <-__x64_sys_openat
head-3803   [002] .....   250.031279: do_sys_openat2 <-__x64_sys_openat
```

父函数是 `__x64_sys_openat`，对应 `ftrace_entry.parent_ip`。pid 是当次的。

`sched_switch` 只在 instance 里 enable，睡 0.05 秒后的几行：

```text
bash-3799  [001] d..2.  250.182904: sched_switch: prev_comm=bash prev_pid=3799 prev_prio=120 prev_state=S ==> next_comm=swapper/1 next_pid=0 next_prio=120
sleep-3808 [003] d..2.  250.183478: sched_switch: prev_comm=sleep prev_pid=3808 prev_prio=120 prev_state=S ==> next_comm=swapper/3 next_pid=0 next_prio=120
```

字段顺序与 `TP_printk` 一致。`prev_state` 的 `S` / `R` 编码见 [scheduler/08](../scheduler/08-observability.md)。

kprobe 写在顶层 `kprobe_events`（instance 没有这个文件），enable 写在 instance 的 `events/kprobes/myprobe/enable`：

```text
p:kprobes/myprobe __x64_sys_getpid
python3-3811 [000] .....  250.309356: myprobe: (__do_sys_getpid+0x0/0x30)
```

测完 `echo '-:myprobe' >> kprobe_events`，文件长度回到 0。

完整命令在 [demos](./demos/README.md)。

---

## 11.4 读取顺序

1. `current_tracer` 和 `tracing_on`。tracer 是 nop 时，function 行不会出现；`tracing_on` 为 0 时，event 即使 enable 也不写入。
2. `per_cpu/*/stats` 的 `dropped events` 和 `overrun`。先确认环没在丢，再读 `trace`。
3. `set_ftrace_filter`。空 hash 的那行文字表示全开，不要在这个状态下切 `function`。
4. `events/<sys>/<name>/enable` 与 `format`。`id` 只在本机、本次启动有效。
5. `kprobe_events` 与 `kprobe_profile`。有行不一定已经 arm，还要看 `events/kprobes/<name>/enable`。
6. `error_log`。filter、trigger、kprobe 解析失败写在这里。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
