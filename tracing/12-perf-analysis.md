# 12. 性能与配方

**结论**：function 的开销按「命中的函数次数 × 一次 reserve」涨；function_graph 再加一次 shadow stack 和返回钩子；单个 tracepoint 只在那个调用点付费。环满了看 `dropped events` 还是 `overrun`。filter 写成空 hash 等于跟踪全部 `dyn_ftrace` 记录。要脚本时用 bpftrace 或 perf，本章不重写各章已有的 `.bt`。

> 机制在 [02](./02-ring-buffer.md) 到 [09](./09-kprobe-events.md)。这里只给「现象 → 看什么 → 改什么」。本机没有 `perf`、没有 `trace-cmd`、没有 `bpftrace`。带命令的条目是 RECIPE，实测栏留空，除非 [11](./11-observability.md) 已经贴过当次输出。

---

## 12.1 function 把 CPU 打满

现象：一切 `function` 之后某一颗核的系统时间升高，`trace` 里每秒的行数远大于业务本身的调用。

看什么：`set_ftrace_filter` 是不是 `#### all functions enabled ####`。那表示 filter hash 为空，`hash_contains_ip` 对每个调用点返回真（`ftrace.c:1499`）。本机这张表有 50524 行（sampled on 6.12）。`enabled_functions` 里每个符号后面的引用计数和 trampoline 会告诉你是哪一个 `ftrace_ops`。

改什么：先写 filter，再切 tracer。只要一个函数时：

```text
echo do_sys_openat2 > /sys/kernel/tracing/set_ftrace_filter
echo function > /sys/kernel/tracing/current_tracer
```

instance 上的同组命令在 [demos/01-function-filter.sh](./demos/01-function-filter.sh)，当次输出在 [11](./11-observability.md)。看完 `echo nop > current_tracer`。只把 `tracing_on` 设成 0 会停写，不会把 call 改回 NOP。

`func_stack_trace` 让每次命中再走 `__trace_stack`（`trace_functions.c:357`）。栈的开销大于一行 `ip <- parent_ip`。默认 `func_flags.val` 是 `TRACE_FUNC_NO_OPTS`（`:374`），不要为了看调用关系先打开它；调用关系用 parent_ip，或者改 function_graph。

---

## 12.2 function_graph 的额外开销

现象：打开 graph 之后延迟明显上升，或者 `trace` 里 `overrun` 字段（记录里的那个，不是 stats 里的）增加。

看什么：`FTRACE_RETFUNC_DEPTH` 是 50（`ftrace.h:1092`）。超出时 `ftrace_push_return_trace` 给 `current->trace_overrun` 加一（`fgraph.c:89`），这一帧不再记录。`register_ftrace_graph` 全局只允许一份（`:640`），第二个注册得到 `-EBUSY`。

改什么：用 `set_graph_function` 收成一棵子树，或设 `tracing_thresh`，让低于阈值的返回不写记录（`trace_functions_graph.c:287`）。不要对空 filter 开 graph。

**RECIPE-G1**

目的：只看一个函数的进入和返回耗时。
命令：见 [demos/04-function-graph.sh](./demos/04-function-graph.sh)。默认不执行。
期望看什么：每个进入行有配对的返回行，返回行带耗时；`per_cpu/*/stats` 的 `dropped events` 仍是 0。
实测（待补）：本章没有在这台机器上打开 function_graph。

---

## 12.3 事件比 function 便宜，仍然会丢

现象：`events/sched/sched_switch/enable` 打开后 `trace` 被调度填满，业务事件看不见了。

看什么：`per_cpu/cpuN/stats`。

| stats 字段 | 计数 | 含义 |
|------------|------|------|
| `overrun` | `ring_buffer_overrun_cpu` | `RB_FL_OVERWRITE` 打开，旧页被盖掉 |
| `dropped events` | `ring_buffer_dropped_events_cpu` | 没开 overwrite，新事件被丢 |
| `commit overrun` | `ring_buffer_commit_overrun_cpu` | 写者绕回了尚未提交的页 |

打印点在 `tracing_stats_read`（`trace.c:8532`、`:8560`、`:8535`）。顶层 `trace_options` 里有 `overwrite` 时走第一行。盖掉和丢掉都是丢数据，只是丢掉的是旧的还是新的。

改什么：给事件加 filter，或换一个 instance，让调度事件不要和你要的事件写进同一块环。`buffer_size_kb` 加大只推迟满的时刻。`trace_pipe` 要有人持续读，否则和 `trace` 一样堆在环里。

sched 的字段含义不在这里重复，见 [scheduler/08](../scheduler/08-observability.md)。本机 instance 里的几行在 [11](./11-observability.md)。

---

## 12.4 filter 过宽与 trigger

现象：写了 filter 仍然每条都在，或 `error_log` 里有解析错误。

看什么：`filter` 文件读回来的字符串。写成 `0` 是关掉过滤（`apply_event_filter`，`trace_events_filter.c:2356`）。解析失败时 `EVENT_FILE_FL_NO_SET_FILTER` 让这份过滤被忽略，事件等于没有过滤。

改什么：先对 `format` 里的字段名写一条能读回来的表达式，再 enable。trigger 的 soft mode 会在事件没写入环时也跑回调；`stacktrace` 和 `snapshot` 比多记一条事件贵。hist 的 `tracing_map` 按 key 的基数占内存，key 用了指针或 comm 以外的高基数字段时桶会铺开。hist 的语法见 [07](./07-event-filter-trigger.md)，这里不作为查询教程。

---

## 12.5 kprobe 比 fentry 贵在断点

现象：对一个热函数 `echo 'p:...' >> kprobe_events` 并 enable 之后，该函数的调用变慢，`kprobe_profile` 的 miss 增加。

看什么：探针是否带 `post_handler`。有 post 就不能留在优化 trampoline 上（`add_new_kprobe`，`kprobes.c:1271`）。优化路径是一次跳转加 `optimized_callback`；未优化是 int3 加单步（[08](./08-kprobes-core.md)）。`nmissed` 增加说明命中时 `kprobe_running()` 已经为真，或 kretprobe 的 `maxactive` 不够。

改什么：只要函数入口、且能接受 ftrace 调用点时，BPF fentry 走 `register_ftrace_direct`，不放 int3。那条路径的 ftrace 侧在 [04](./04-dynamic-ftrace.md)，BPF 侧在 [ebpf/06](../ebpf/06-attach-and-link.md) 和 [ebpf/13](../ebpf/13-perf-analysis.md)。kprobe 事件适合「符号加偏移」和抓 `$stack` / `%REG`。抓完就 `echo 0 > enable`，再从 `kprobe_events` 删掉，避免断点留在热路径上。

---

## 12.6 选型

| 想看的 | 用 |
|--------|----|
| 一个函数被谁调用 | 先写 `set_ftrace_filter`，再切 function |
| 这个函数花了多久、调用了谁 | function_graph，先写 `set_graph_function` |
| 切换、唤醒、系统调用这类已有语义点 | 对应的 `events/.../enable` |
| 没有 tracepoint 的内核指令 | `kprobe_events` 的 `KSYM+offset` |
| 用户态二进制里的一条指令 | `uprobe_events` 的 `PATH:OFFSET` |
| 命中时跑一段程序、结果进 map | BPF，见 [ebpf/06](../ebpf/06-attach-and-link.md) |

tracefs 适合确认「事件在不在、格式长什么样」。统计分布、按 pid 聚合，用 perf 或 bpftrace 更合适，它们消费同一批 tracepoint，不把文本行打进 `trace`。

**RECIPE-P1**

目的：在有 `perf` 的机器上数 `sched_switch`，而不把文本写进 ftrace 环。
命令：`perf stat -e sched:sched_switch -a -- sleep 2`；需要栈时再 `perf record -e sched:sched_switch -a -g -- sleep 2`。
期望看什么：计数在涨，`/sys/kernel/tracing/per_cpu/cpu0/stats` 的 `entries` 不因为这次 perf 而涨。perf 用自己的环，`TRACE_REG_PERF_REGISTER` 挂的是另一根探针（`trace_events.c:691`）。
实测（待补）：本机没有 `perf`。

**RECIPE-P2**

目的：确认 irqsoff 或 preemptoff 是否编进内核。
命令：`cat /sys/kernel/tracing/available_tracers`。若有 `irqsoff`，再 `echo irqsoff > current_tracer`，读 `tracing_max_latency`，看完 `echo nop > current_tracer`。
期望看什么：本机 6.12 的列表是 `function_graph function nop`，没有 irqsoff。有该 tracer 的机器上，`tracing_max_latency` 是 `trace_array->max_latency`（`trace.h:343`，需要 `CONFIG_TRACER_MAX_TRACE`）。
实测（待补）：本章不跑延迟 tracer。

**RECIPE-P3**

目的：bpftrace 按 `sched_switch` 聚合。脚本留在调度章，不搬过来。
命令：见 [scheduler/demos/sched_switch_hist.bt](../scheduler/demos/sched_switch_hist.bt)。
期望看什么：按 `prev_state` 分桶。
实测（待补）：本机没有 bpftrace。

---

## 12.7 测完要恢复的状态

1. `echo nop > current_tracer`（顶层和用过的 instance）。
2. `echo > set_ftrace_filter`，让 hash 回到空，文件重新显示 all functions enabled。这只是恢复默认匹配，不是「关掉 ftrace」。
3. 每个打开过的 `events/.../enable` 写 0。
4. `kprobe_events` / `uprobe_events` 里本次加的行删掉。
5. `rmdir instances/<name>`。目录非空时 rmdir 失败，先把该 instance 的 tracer 设回 nop。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
