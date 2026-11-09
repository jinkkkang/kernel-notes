# 7. 事件过滤与 trigger

**结论**：`filter` 文件里的表达式被编成一段谓词程序，`filter_match_preds` 对记录返回 1 或 0。`trigger` 文件注册的是 `event_command`：事件命中时执行 traceon、snapshot、stacktrace、enable_event，或把字段送进 histogram。hist 和 synth 这里只写到对象和注册函数。

> filter 在 `kernel/trace/trace_events_filter.c`。trigger 在 `kernel/trace/trace_events_trigger.c`。hist 在 `kernel/trace/trace_events_hist.c`，synth 在 `kernel/trace/trace_events_synth.c`。

---

## 7.1 filter

写 `events/<sys>/<name>/filter` 进 `apply_event_filter`（`trace_events_filter.c:2347`）。调用方要持有 `event_mutex`。字符串剥空白后等于 `"0"` 就 `filter_disable`（`:2356`），清掉过滤。

谓词跑在 `filter_match_preds`（`:1172`）。注释写明：返回 1 表示匹配，0 表示丢掉；`filter` 指针为空视为匹配。`prog` 用 `rcu_dereference_raw` 取出（`:1182`，注释写明调用方要在 SRCU 或 `preempt_disable` 之下）。循环里每条 `prog[i]` 有一个 `pred` 和 `when_to_branch`：`filter_pred_fn_call` 的结果等于 `when_to_branch` 时，`i` 改成 `target`（`:1186`）。循环结束后返回最后一条的 `target`，这个值就是 1 或 0。

`trace_trigger_soft_disabled`（`include/linux/trace_events.h:765`）在 reserve 之前被 `TRACE_EVENT` 展开调用（`include/trace/trace_events.h:392`）。`flags` 里没有 `TRIGGER_MODE`、`SOFT_DISABLED`、`PID_FILTER` 时直接返回 false，事件照常记录。有 trigger 且带 `TRIGGER_COND` 时也返回 false，把字段比较留到记录已经填好之后。否则进 `__trace_trigger_soft_disabled`。软关闭用来表达「事件没 enable，但 trigger 还要跑」：回调先执行，函数返回 true 时展开宏直接 return，不 `reserve`。

过滤发生在记录已经 reserve 之后还是之前，取决于调用点。软关闭在 reserve 之前返回。真正按字段丢掉一条已预留的记录时，走 [02](./02-ring-buffer.md) 的 `ring_buffer_discard_commit`。function tracer 那条路径在 `trace_function` 里叫 `call_filter_check_discard`（`trace.c:557`）。

字符串字段可以走几种正则。`filter_parse_regex`（`trace_events_filter.c:1084`）区分全匹配、前缀、后缀、中间和 glob，对应的 `regex_match_*` 装在同文件 `:1145` 一带。比较用的字段偏移来自该事件的 `format`。

`EVENT_FILE_FL_FILTERED`（`trace_events.h:644`）表示这份 file 上有过滤器。解析失败会置 `EVENT_FILE_FL_NO_SET_FILTER`，该过滤被忽略。`trace_array->n_err_log_entries` 和 `error_log` 文件记下 tracefs 命令的解析错误。

---

## 7.2 trigger 命令

写 `trigger` 进 `event_trigger_write`（`trace_events_trigger.c:331`）。命令表由 `register_event_command`（`:362`）在 init 时登记。v6.6.160 里这张表包括：

| 名字 | 定义 | 做什么 |
|------|------|--------|
| `traceon` / `traceoff` | `:1422`、`:1432` | 打开或关掉 `tracing_on` |
| `snapshot` | `:1511` | 把当前环和 `max_buffer` 交换。需要 `CONFIG_TRACER_SNAPSHOT` |
| `stacktrace` | `:1606` | 记一份栈 |
| `enable_event` / `disable_event` | `ENABLE_EVENT_STR`（`trace.h:1632`），命令在 `trace_events_trigger.c:1938` | 打开或关掉另一个事件 |
| `enable_hist` / `disable_hist` | `trace.h:1634` | 打开或关掉另一个事件上的 hist |
| `hist` | `trace_events_hist.c:6919` | 见下一节 |

一条 trigger 可以带计数（`:5` 表示再命中 5 次就卸掉）和自己的 filter。`event_trigger_data`（`trace.h:1614`）保存 `count`、`filter`、`cmd_ops`。`EVENT_FILE_FL_TRIGGER_MODE` 置上之后，事件即使没写入环，也会进入 trigger 回调；这就是 soft mode。`sm_ref` / `tm_ref` 在 `trace_event_file` 里计数（`trace_events.h:682`）。

`enable_event` 的目标是另一个 `trace_event_file`。它解决「平时不要记，某个稀有事件发生时再打开第二个事件」的需求，两个事件仍是各自的 `TRACE_EVENT`，没有新的记录格式。

---

## 7.3 hist 与 synth（薄）

`hist` 是一条 `event_command`（`trace_events_hist.c:6919`）：`name` 为 `"hist"`，`parse` 是 `event_hist_trigger_parse`（`:6754`），`reg` 是 `hist_register_trigger`（`:6523`），标志 `EVENT_CMD_FL_NEEDS_REC` 表示要拿到记录才能更新。

数据挂在 `struct hist_trigger_data`（`:549`）。开头几个成员是：

```c
struct hist_trigger_data {
	struct hist_field		*fields[HIST_FIELDS_MAX];
	unsigned int			n_vals;
	unsigned int			n_keys;
	unsigned int			n_fields;
	unsigned int			n_vars;
	unsigned int			n_var_str;
	unsigned int			key_size;
	struct tracing_map_sort_key	sort_keys[TRACING_MAP_SORT_KEYS_MAX];
	unsigned int			n_sort_keys;
	struct trace_event_file		*event_file;
	/* 后面还有 tracing_map、变量、动作 */
};
```

keys 决定桶，vals 是桶里的合计。结果放在 `tracing_map` 里，读 `hist` 文件时按 `sort_keys` 倒出来。`onmatch`、`onmax` 一类动作可以再去 `enable_event` 或记 snapshot。这些动作的语法是另一套小语言，本章不逐条列。

合成事件在 `kernel/trace/trace_events_synth.c`。用户在 `synthetic_events` 里声明字段，`register_synth_event`（`:898`）做出一个动态的 `trace_event_call`。`tracepoint_update_call` 对没有 `static_call_key` 的合成事件直接返回（`tracepoint.c:313`），因为它们没有编译进内核的调用点。内核里其它代码用 `synth_event_trace`（`trace_events_synth.c:1750`）按字段值写一条。字段布局以写进 `synthetic_events` 的那次声明为准。hist 动作怎样拼参数，本章不展开。

eprobe 挂在已有事件上再抽字段，标志是 `TRACE_EVENT_FL_EPROBE`（`trace_events.h:364`）。本章不展开。

---

## 7.4 和 BPF 过滤的差别

event filter 在记录形成之后用谓词决定留不留。BPF 挂在探针上，程序先跑，返回值由挂钩解释。kprobe 的 `trace_call_bpf` 返回 0 表示丢掉、不写环，这一点在 [ebpf/06](../ebpf/06-attach-and-link.md)。两边可以同时存在：filter 文件管 tracefs 这份 `trace_event_file`，BPF 程序挂在 perf event 或 raw tracepoint 上。

---

## 7.5 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `apply_event_filter` | `trace_events_filter.c:2347` | 解析并装上 filter |
| `filter_match_preds` | `:1172` | 1 保留，0 丢弃 |
| `filter_parse_regex` | `:1084` | 字符串比较的种类 |
| `event_trigger_write` | `trace_events_trigger.c:331` | 写 `trigger` 文件 |
| `register_event_command` | `:362` | 登记 traceon 等命令 |
| `event_hist_trigger_parse` | `trace_events_hist.c:6754` | 解析 `hist` |
| `register_synth_event` | `trace_events_synth.c:898` | 登记合成事件 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
