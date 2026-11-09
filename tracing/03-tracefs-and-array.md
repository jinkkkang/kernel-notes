# 3. tracefs 与 trace_array

**结论**：tracefs 上的一个目录是一份 `trace_array`。顶层 `/sys/kernel/tracing` 是带 `TRACE_ARRAY_FL_GLOBAL` 的那份；`instances/<name>` 是 `instance_mkdir` 再造的一份，各自有环、`current_trace` 和 event 开关。`current_tracer` 切换的是 `struct tracer`，`tracing_on` 暂停的是写入。

> `struct trace_array` 在 `kernel/trace/trace.h:323`。`struct tracer` 在 `:561`。`tracing_set_tracer` 在 `kernel/trace/trace.c:6417`。`trace_array_create` 在 `:9612`，`instance_mkdir` 在 `:9617`。

---

## 3.1 `trace_array`

```c
/* kernel/trace/trace.h:323
 * 单个 tracer 打交道的最高层对象。
 */
struct trace_array {
	struct list_head	list;
	char			*name;
	struct array_buffer	array_buffer;   /* 正在写的环 */
#ifdef CONFIG_TRACER_MAX_TRACE
	struct array_buffer	max_buffer;     /* snapshot / 最大延迟 */
	bool			allocated_snapshot;
	unsigned long		max_latency;
#endif
	struct trace_pid_list	__rcu *filtered_pids;
	struct trace_pid_list	__rcu *filtered_no_pids;
	arch_spinlock_t		max_lock;       /* 交换 max_buffer 时用 */
	int			buffer_disabled;
	int			clock_id;
	struct tracer		*current_trace;
	unsigned int		trace_flags;    /* trace_options 的位 */
	unsigned int		flags;          /* TRACE_ARRAY_FL_GLOBAL 等 */
	struct dentry		*dir;
	struct eventfs_inode	*event_dir;
	struct list_head	systems;
	struct list_head	events;
	cpumask_var_t		tracing_cpumask;
	cpumask_var_t		pipe_cpumask;   /* 每 CPU 的 trace_pipe 只能一个读者 */
	int			ref;
#ifdef CONFIG_FUNCTION_TRACER
	struct ftrace_ops	*ops;
	struct trace_pid_list	__rcu *function_pids;
	int			function_enabled;
#endif
	/* 其余：syscall 文件表、error_log、hist_vars、last_func_repeats */
};
```

`array_buffer`（`:213`）里真正的环是 `struct trace_buffer *buffer`，旁边有 per-CPU 的 `trace_array_cpu`。`TRACE_ARRAY_FL_GLOBAL`（`:421`）标顶层那一份。`top_trace_array`（`:443`）取 `ftrace_trace_arrays` 链表尾并检查这个标志。

`max_buffer` 在 `CONFIG_TRACER_MAX_TRACE` 下存在。snapshot 或延迟 tracer 达到最大值时，`max_lock` 保护两块缓冲的交换，注释在 `:352`。本机 6.12 的顶层目录没有 `snapshot` 文件，见 [11](./11-observability.md)，说明当次内核没编进这份缓冲。

---

## 3.2 `struct tracer` 与 `current_tracer`

```c
/* kernel/trace/trace.h:561 */
struct tracer {
	const char		*name;          /* available_tracers 里的名字 */
	int			(*init)(struct trace_array *tr);
	void			(*reset)(struct trace_array *tr);
	void			(*start)(struct trace_array *tr);  /* tracing_on = 1 */
	void			(*stop)(struct trace_array *tr);   /* tracing_on = 0 */
	int			(*update_thresh)(struct trace_array *tr);
	void			(*open)(struct trace_iterator *iter);
	void			(*pipe_open)(struct trace_iterator *iter);
	enum print_line_t	(*print_line)(struct trace_iterator *iter);
	int			(*set_flag)(struct trace_array *tr,
					    u32 old_flags, u32 bit, int set);
	struct tracer		*next;
	struct tracer_flags	*flags;
	int			enabled;
	bool			allow_instances;
#ifdef CONFIG_TRACER_MAX_TRACE
	bool			use_max_tr;
#endif
	bool			noboot;
};
```

`tracing_set_tracer`（`trace.c:6417`）持 `trace_types_lock`，按名字在 `trace_types` 链表里找。第一次从 nop 切走、环还没展开时，调用 `__tracing_resize_ring_buffer(tr, trace_buf_size, RING_BUFFER_ALL_CPUS)`（`:6428`）。`trace_buf_size` 的初值是 `TRACE_BUF_SIZE_DEFAULT`，1441792 字节（`trace.c:861`，注释写 `16384 * 88`）。除以 1024 是 1408，和 `buffer_size_kb` 在未展开时打印的 `expanded:` 是同一个数。

`buffer_size_kb` 的读函数是 `tracing_entries_read`（`:7070`）。所有 CPU 大小一样且环还没展开时，格式是 `"%lu (expanded: %lu)\n"`（`:7101`）：前面是当前每 CPU 的 KB，括号里是展开后的 KB。展开之后只印一个数（`:7105`）。写函数是 `tracing_entries_write`（`:7118`），`kstrtoul_from_user` 读十进制 KB。

function tracer 的 `allow_instances` 是 true（`trace_functions.c:434`）。所以 instance 里可以 `echo function > current_tracer`。irqsoff、preemptoff、wakeup 的实现在 `kernel/trace/trace_irqsoff.c`，它们要 `CONFIG_IRQSOFF_TRACER` 一类选项才会出现在 `available_tracers`。本章不展开这三条延迟 tracer，命令留在 [12](./12-perf-analysis.md)。

---

## 3.3 instances

`instance_mkdir`（`trace.c:9617`）调用 `trace_array_create`（`:9612`）。新的 `trace_array` 挂进 `ftrace_trace_arrays`，有自己的 `array_buffer`、`current_trace`（初始 nop）和 `events/`。`trace_array_get_by_name`（`:9656`）按名字查找。

顶层和 instance 不共享「现在用哪个 tracer」。它们共享的是动态 ftrace 的调用点改写：函数入口只有一份 NOP/call，每个 `ftrace_ops` 自己决定要不要跟。顶层的 ops 是 `global_ops`（`ftrace_init_global_array_ops`，`ftrace.c:7465`）。instance 在 `ftrace_allocate_ftrace_ops`（`trace_functions.c:52`）里单独分配，`ops->private` 指回那份 `trace_array`。

`enabled_functions`、`available_filter_functions` 由 `ftrace_init_dyn_tracefs`（`ftrace.c:6500`）创建一次，挂在初始化时传入的顶层目录上（调用点 `:8023`）。instance 目录里有 `set_ftrace_filter` 和 `current_tracer`，没有那两份全局清单。本机 6.12 的 instance 同样没有 `enabled_functions`，和这段初始化一致。

`kprobe_events` 也只建在顶层：`trace_kprobe.c:1960` 的 `trace_create_file("kprobe_events", ...)`。探针造出来之后，每个 instance 的 `events/kprobes/<name>/enable` 可以单独打开。

---

## 3.4 `trace`、`trace_pipe`、`tracing_on`、options

| 文件 | 作用 |
|------|------|
| `trace` | 当前环的文本快照。写入清空 |
| `trace_pipe` | `tracing_open_pipe`（`trace.c:6666`）。同一 CPU 已有读者则 `-EBUSY` |
| `tracing_on` | `tracing_on()` 在 `trace.c:1026`。0 走 `ring_buffer_record_off` 一类开关，暂停写入，不卸 tracer |
| `current_tracer` | 写名字调用 `tracing_set_tracer` |
| `available_tracers` | 已注册且允许出现在这个目录里的 `tracer->name` |
| `trace_clock` | `tracing_set_clock`（`trace.h:435` 声明）。`local` 是每 CPU 时钟 |
| `trace_options` / `options/` | `trace_array->trace_flags` 的位。`tracer_opt`（`trace.h:515`）是某个 tracer 私有的选项 |
| `buffer_size_kb` | 每 CPU 环的 KB，见上一节 |
| `buffer_total_size_kb` | 各 CPU 加总 |
| `tracing_cpumask` | 只在这些 CPU 上记 |
| `set_ftrace_pid` | 只跟这些 pid。对应 `ftrace_ops` 的 `FTRACE_OPS_FL_PID` |
| `events/` | 每个 event 一个目录：`enable`、`filter`、`format`、`trigger`、`id` |

`tracing_on` 和 `current_tracer` 是两层开关。tracer 仍是 function、`tracing_on` 为 0 时，回调还在，记录函数看到环被关掉就返回。切回 `nop` 才会 `tracer->reset`，function tracer 的 reset 是 `function_trace_reset`（`trace_functions.c:159`），里面 `unregister_ftrace_function`。

选项位的用户可见名字在 `trace_options`。function tracer 自己的 `func_stack_trace` 和 `func-no-repeats` 在 `trace_functions.c:365`。`overwrite` 这一位对应环的 `RB_FL_OVERWRITE`。

---

## 3.5 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `trace_array_create` | `trace.c:9612` | 造一份 `trace_array` |
| `instance_mkdir` | `:9617` | tracefs 的 `instances/` 写入 |
| `tracing_set_tracer` | `:6417` | 切换 `current_trace`，必要时把环扩到 `trace_buf_size` |
| `tracing_entries_read` | `:7070` | 打印 `buffer_size_kb` |
| `tracing_open_pipe` | `:6666` | 打开 `trace_pipe` |
| `tracing_stats_read` | `:8511` | `per_cpu/cpuN/stats` |
| `tracer_init` | `:6217` | 调用 `tracer->init` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
