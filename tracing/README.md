# 追踪（ftrace / tracepoint / kprobe）

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.113+**（`uname -r`，4 核）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：探针在函数入口、tracepoint 或断点上被叫到，记录写进每 CPU 的 `trace_buffer`，用户从 tracefs 读出来。本章写这条环、动态 ftrace、`TRACE_EVENT` 和 kprobe 基建。

## 阅读顺序

```text
01 总览
  → 02 环缓冲
  → 03 tracefs 与 trace_array
  → 04 动态 ftrace
  → 05 function 与 function_graph
  → 06 tracepoint 与 TRACE_EVENT
  → 07 事件过滤与 trigger
  → 08 kprobe 核心
  → 09 kprobe 事件
  → 10 uprobe（薄）
  → 11 观测
  → 12 性能配方
```

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | probe → 环 → tracefs；与 perf / eBPF / 各章 observability 的分工 | 中 |
| [02-ring-buffer.md](./02-ring-buffer.md) | `trace_buffer`、per-CPU 页、reserve/commit、reader 页交换、事件头、overwrite 与 discard | **厚** |
| [03-tracefs-and-array.md](./03-tracefs-and-array.md) | `trace_array`、instances、`current_tracer`、`trace` / `trace_pipe`、`buffer_size_kb`、options | 中 |
| [04-dynamic-ftrace.md](./04-dynamic-ftrace.md) | `__mcount_loc`、NOP/call、`ftrace_ops`、filter hash、trampoline、`register_ftrace_direct` | **厚** |
| [05-function-and-graph.md](./05-function-and-graph.md) | function tracer；function_graph 入口/返回、shadow stack、`ftrace_graph_ent` / `ret` | **厚** |
| [06-tracepoint-and-TRACE_EVENT.md](./06-tracepoint-and-TRACE_EVENT.md) | `TRACE_EVENT` 展开、`struct tracepoint`、`static_call`、探针链、event 文件 | **厚** |
| [07-event-filter-trigger.md](./07-event-filter-trigger.md) | filter 表达式、trigger；hist / synth 只写到机制 | 中 |
| [08-kprobes-core.md](./08-kprobes-core.md) | `register_kprobe` / kretprobe、int3 与优化 trampoline、黑名单、`kprobe_busy` | **厚** |
| [09-kprobe-events.md](./09-kprobe-events.md) | `kprobe_events`、`trace_kprobe`、参数抓取；停在交给 perf / BPF | 中 |
| [10-uprobe.md](./10-uprobe.md) | `uprobe_events`、inode+offset、uretprobe；对照 kprobe | 较薄 |
| [11-observability.md](./11-observability.md) | `/sys/kernel/tracing` 清单与本机采样 | **厚** |
| [12-perf-analysis.md](./12-perf-analysis.md) | function / graph / event 的开销、丢事件、filter 过宽、选型 | **厚** |
| [demos/](./demos/) | tracefs 原语：function filter、sched_switch、动态 kprobe、function_graph | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| 环缓冲、tracefs、动态 ftrace、function / function_graph | — |
| `TRACE_EVENT` 基建与 event 文件节点 | 各子系统事件字段语义留在各章 observability。`sched_switch` 的 `prev_state` 编码见 [scheduler/08](../scheduler/08-observability.md) |
| `register_kprobe`、优化 trampoline、`kprobe_events` | BPF 的 kprobe / tracepoint / raw tp / fentry attach 与 ctx 在 [ebpf/06](../ebpf/06-attach-and-link.md)。该章在 `register_fentry` 调用 `register_ftrace_direct` 处停下，direct call 的 ftrace 侧在 [04](./04-dynamic-ftrace.md) |
| uprobe 挂点（inode + offset） | 用户态调试器语义不展开 |
| hist / synth / eprobe | 点到机制。不当第二套查询语言教程 |
| irqsoff / preemptoff / wakeup | 不单开厚节。名字在 [03](./03-tracefs-and-array.md)，用法留 [12](./12-perf-analysis.md) 的 RECIPE |
| hwlat / osnoise / mmiotrace / bootconfig | 不写 |
| perf 硬件计数器、PEBS | 不写。perf 只作为环和 event 的消费者之一 |
| softirq、调度、系统调用路径本体 | [interrupts/](../interrupts/README.md)、[scheduler/](../scheduler/README.md)、[syscalls/](../syscalls/README.md) |

## 源码地图

| 路径 | 角色 |
|------|------|
| `kernel/trace/ring_buffer.c`、`include/linux/ring_buffer.h` | 环缓冲。v6.6.160 里对象类型是 `struct trace_buffer` |
| `kernel/trace/trace.c`、`kernel/trace/trace.h` | `trace_array`、tracefs 文件 |
| `kernel/trace/ftrace.c`、`include/linux/ftrace.h` | 动态 ftrace、`ftrace_ops`、direct |
| `kernel/trace/trace_functions.c`、`trace_functions_graph.c`、`fgraph.c` | function / graph |
| `include/linux/tracepoint.h`、`include/linux/tracepoint-defs.h`、`kernel/tracepoint.c` | tracepoint 注册 |
| `include/trace/define_trace.h`、`include/trace/trace_events.h`、`kernel/trace/trace_events.c` | `TRACE_EVENT` 与 event |
| `kernel/trace/trace_events_filter.c`、`trace_events_trigger.c` | filter / trigger |
| `kernel/kprobes.c`、`include/linux/kprobes.h` | kprobe 核心 |
| `kernel/trace/trace_kprobe.c`、`trace_probe.c` | kprobe events |
| `kernel/trace/trace_uprobe.c`、`kernel/events/uprobes.c` | uprobe events 与挂点 |
| `arch/x86/kernel/ftrace.c`、`ftrace_64.S`、`arch/x86/kernel/kprobes/` | x86 改写与断点 |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.113+，root）：[11](./11-observability.md) 的 tracefs 清单。[demos/](./demos/) 的 `01-function-filter`、`02-sched-switch-event`、`03-kprobe-events` 在 instance `kn-demo` 上跑过，stdout 贴在 demos README。顶层 `current_tracer` 保持 `nop`。
- **RECIPE**：`04-function-graph`（开销大，本章没有打开）。没有 `perf`、没有 `trace-cmd`、没有 `bpftrace`。irqsoff / preemptoff 的命令在 [12](./12-perf-analysis.md)，本机 `available_tracers` 里也没有这两项。

## 未覆盖

- irqsoff / preemptoff / wakeup 的延迟 tracer 实现
- hwlat、osnoise、mmiotrace、bootconfig
- perf 硬件计数器与 PEBS
- BPF 程序类型、ctx、`bpf_link` 的 attach 表
- 各子系统事件字段的业务含义
- 用户态调试器如何解释 uprobe 断点

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
