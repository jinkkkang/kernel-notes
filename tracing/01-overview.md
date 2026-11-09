# 1. 总览

**结论**：一次观测是「某个调用点把一条记录写进当前 CPU 的环，tracefs 再把环里的字节印出来」。function、tracepoint、kprobe 是三种不同的调用点，共用同一套 `trace_buffer` 和 `trace_array`。

> 环的类型在 v6.6.160 里叫 `struct trace_buffer`（`kernel/trace/ring_buffer.c:542`），分配入口是 `__ring_buffer_alloc`（`:1821`）。tracefs 上的一个目录对应 `struct trace_array`（`kernel/trace/trace.h:323`）。

---

## 1.1 三条生产路径，一条环

```text
函数入口的 NOP/call          ftrace_ops->func
TRACE_EVENT 的 trace_*()     trace_event_raw_event_*
kprobe 的 int3 / 优化跳转     kprobe->pre_handler
        │
        ▼
ring_buffer_lock_reserve → 填记录 → ring_buffer_unlock_commit
        │
        ▼
trace_array->array_buffer.buffer     每 CPU 一份
        │
        ▼
/sys/kernel/tracing/{trace,trace_pipe,events/...}
```

function tracer 的回调是 `function_trace_call`（`kernel/trace/trace_functions.c:172`），它调 `trace_function`（`kernel/trace/trace.c:3056`），里面 `__trace_buffer_lock_reserve`。`TRACE_EVENT` 展开出的 `do_trace_event_raw_event_*` 调 `trace_event_buffer_reserve`（`kernel/trace/trace_events.c:641`），底层仍是同一套 reserve。kprobe 事件在探针处理函数里走同一条 event 写入。断点本身在 [08](./08-kprobes-core.md)，事件文件在 [09](./09-kprobe-events.md)。

读的一侧有两种文件。`trace` 是缓冲区的静态快照，写这个文件用来清空。`trace_pipe` 由 `tracing_open_pipe`（`trace.c:6666`）打开，同一 CPU 上已经有读者时返回 `-EBUSY`（`trace_array` 的 `pipe_cpumask`，`trace.h:396`）。

---

## 1.2 和 perf、eBPF、各章观测的分工

| 角色 | 本章怎么对待 |
|------|----------------|
| tracefs / `current_tracer` / event 的 `enable` | 生产端的开关和文本输出。清单在 [11](./11-observability.md) |
| `perf` | 环和 tracepoint 的另一个消费者。`trace_event_reg` 的 `TRACE_REG_PERF_REGISTER` 把 perf 的探针挂到同一个 `struct tracepoint` 上（`trace_events.c:691`）。硬件计数器和 PEBS 不写 |
| eBPF | 消费端。kprobe / tracepoint / fentry 的程序类型、ctx、`bpf_link` 在 [ebpf/06](../ebpf/06-attach-and-link.md)。本章写到 `register_kprobe`、`tracepoint_probe_register`、`register_ftrace_direct` |
| bpftrace | 用户态前端，把脚本变成 BPF 再走 ebpf 章的装载。源码不在内核树里，本章不搬各章已有的 `.bt` |
| 各章 observability | 事件字段的含义留在原章。例如 `sched_switch` 的 `prev_state` 在 [scheduler/08](../scheduler/08-observability.md)。本章用它演示 `TRACE_EVENT` 怎么展开成记录 |

`sys_enter` 这个 tracepoint 的调用位置在 [syscalls/09](../syscalls/09-hooks.md)。本章说明 `trace_sys_enter` 怎样进环，不重写系统调用入口。

---

## 1.3 本章写到哪

写：环的 reserve/commit 与丢事件、`trace_array` 和 tracefs 节点、动态 ftrace 的改写与 direct call、function 与 function_graph、`TRACE_EVENT` 与探针链、event filter/trigger（hist 与 synth 只到对象）、`register_kprobe` 与优化 trampoline、`kprobe_events`、uprobe 的挂点对照、本机 tracefs 采样和开销配方。

不写：irqsoff / preemptoff / wakeup 的实现（只点名）、hwlat / osnoise / mmiotrace / bootconfig、perf 的硬件事件、BPF verifier 与 map、某个子系统事件的业务字段、用户态 ptrace 怎样单步。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
