# 6. tracepoint 与 TRACE_EVENT

**结论**：`TRACE_EVENT` 展开出三样东西：一个 `struct tracepoint`、一个 `trace_<name>()` 内联函数、一个 `struct trace_event_call`。调用点先看 `static_key`，打开后 `__DO_TRACE` 用 `static_call` 进迭代器，迭代器再跑 `funcs` 数组里的探针。event 子系统把 `trace_event_raw_event_*` 登记成探针，探针里 `trace_event_buffer_reserve` 把 `TP_STRUCT__entry` 写进环。

> `struct tracepoint` 在 `include/linux/tracepoint-defs.h:32`。声明宏在 `include/linux/tracepoint.h`。`TRACE_EVENT` 的多段展开在 `include/trace/trace_events.h`，由 `include/trace/define_trace.h` 包含。注册函数 `tracepoint_probe_register` 在 `kernel/tracepoint.c:531`。

---

## 6.1 `struct tracepoint`

```c
/* include/linux/tracepoint-defs.h:26 */
struct tracepoint_func {
	void *func;
	void *data;
	int prio;
};

/* :32 */
struct tracepoint {
	const char *name;
	struct static_key key;                 /* 调用点的 static_key_false */
	struct static_call_key *static_call_key;
	void *static_call_tramp;
	void *iterator;                        /* __traceiter_<name> */
	void *probestub;                       /* 空的 __probestub_<name> */
	int (*regfunc)(void);                  /* 第一个探针挂上时 */
	void (*unregfunc)(void);               /* 最后一个探针摘掉时 */
	struct tracepoint_func __rcu *funcs;   /* 以 NULL func 结尾的数组 */
};
```

`DEFINE_TRACE_FN`（`tracepoint.h:301`）把对象放进 `__tracepoints` 段，名字字符串放进 `__tracepoints_strings`。`key` 初始是 `STATIC_KEY_INIT_FALSE`。`static_call` 的键是 `tp_func_<name>`，初始目标是 `__traceiter_<name>`。`__TRACEPOINT_ENTRY` 再往 `__tracepoints_ptrs` 放一个指针，遍历用它，避免结构体对齐把段撑出空洞（`:297` 的注释）。

`bpf_raw_event_map`（同文件 `:50`）是 raw tracepoint 的另一张表：`tp`、`bpf_func`、`num_args`、`writable_size`。BPF 按这张表找名字。`bpf_probe_register` 之后怎样跑程序在 [ebpf/06](../ebpf/06-attach-and-link.md)，本章停在 `tracepoint_probe_register`。

---

## 6.2 `trace_*()` 怎么进探针

`__DECLARE_TRACE`（`tracepoint.h:251`）生成 `trace_<name>()`：

```text
static_key_false(&__tracepoint_<name>.key) 为假 → 什么都不做
为真 → __DO_TRACE(..., rcuidle=0)
CONFIG_LOCKDEP 且条件为真时，另外 WARN_ON_ONCE(!rcu_is_watching())
```

`__DO_TRACE`（`:195`）先看 `TP_CONDITION`。然后 `preempt_disable_notrace`。`rcuidle` 变体（`trace_<name>_rcuidle`）改拿 `tracepoint_srcu` 并 `ct_irq_enter_irqson`，因为 idle 路径上普通 RCU 不在观察。普通 `trace_*()` 传的 `rcuidle` 是 0。

`__DO_TRACE_CALL`（`:165`，`CONFIG_HAVE_STATIC_CALL`）从 `funcs` 取出第一个探针的 `data`，然后 `static_call(tp_func_<name>)(data, args)`。没有 static call 时直接调 `__traceiter_<name>`。

迭代器由 `DEFINE_TRACE_FN` 生成（`:319`）：

```text
it_func_ptr = rcu_dereference_raw(tp->funcs)
while (it_func_ptr->func) {
    调用 func(data, args)
    it_func_ptr++
}
```

只有一个探针时，`tracepoint_update_call`（`kernel/tracepoint.c:313` 一带）把 static call 的目标从迭代器换成那一个 `func`。`nr_func_state == TP_FUNC_1` 时 `func = tp_funcs[0].func`（`:315`）。探针数不是 1 就指回 `tp->iterator`。合成事件没有 static call 站点，`static_call_key` 为空则直接返回（`:313`）。

`trace_<name>_enabled()` 只读同一把 `static_key`（`tracepoint.h:291`）。头文件里要用 `tracepoint_enabled(name)`（`tracepoint-defs.h:85`），避免在头文件里展开出 `trace_*_enabled`。

---

## 6.3 注册探针

```text
tracepoint_probe_register(tp, probe, data)          tracepoint.c:531
  tracepoint_probe_register_prio(..., TRACEPOINT_DEFAULT_PRIO)
```

`TRACEPOINT_DEFAULT_PRIO` 是 10（`tracepoint.h:34`）。`tracepoint_add_func`（`tracepoint.c:323`）在 key 还没打开且 `tp->regfunc` 存在时先调 `regfunc`。然后 `func_add` 按 `prio` 插入，`rcu_assign_pointer` 换上新的 `funcs` 数组。注释（`:347`）说明这次 store-release 保证数组写完之后调用点才看得到。

摘除是 `tracepoint_probe_unregister`（`:545`）。调用方如果探针属于模块，要在模块消失前摘掉，函数注释写了这一点。`tracepoint_synchronize_unregister`（`tracepoint.h:91`）等 RCU。

event 子系统挂上来的探针不是用户写的函数，而是宏生成的 `trace_event_raw_event_<call>`。`trace_event_reg`（`trace_events.c:673`）在 `TRACE_REG_REGISTER` 时：

```text
tracepoint_probe_register(call->tp, call->class->probe, file)
```

`file` 是 `struct trace_event_file *`，进探针后变成 `__data`。`TRACE_REG_PERF_REGISTER` 挂的是 `call->class->perf_probe`，data 是 `call` 本身（`:694`）。同一 tracepoint 上可以同时有 tracefs 的探针和 perf 的探针，外加 BPF 的 `bpf_func`。它们都在 `funcs` 数组里，按 prio 排序。

---

## 6.4 `TRACE_EVENT` 展开成什么

头文件里的写法：

```c
TRACE_EVENT(name,
    TP_PROTO(...),
    TP_ARGS(...),
    TP_STRUCT__entry(...),
    TP_fast_assign(...),
    TP_printk(...)
);
```

`include/trace/define_trace.h:27` 把 `TRACE_EVENT` 定义成 `DECLARE_EVENT_CLASS` 加 `DEFINE_EVENT`。`CREATE_TRACE_POINTS` 的那一次包含会把 `trace_events.h` 按阶段重定义这些宏。和环有关的阶段是 stage 6（`trace_events.h:382`）：

```text
do_trace_event_raw_event_<call>(__data, args)
  trace_file = __data
  trace_trigger_soft_disabled(trace_file) → return
  trace_event_get_offsets_<call>          动态数组字段的长度
  entry = trace_event_buffer_reserve(&fbuffer, trace_file,
                                     sizeof(*entry) + __data_size)
  失败 → return
  { assign; }                             TP_fast_assign
  trace_event_buffer_commit(&fbuffer)
```

`DECLARE_EVENT_CLASS` 再生成一个薄包装 `trace_event_raw_event_<call>`，它只调用上面的 `do_` 函数（`:414`）。系统调用类的包装多一对 `preempt_disable_notrace` / `preempt_enable_notrace`（`:424`）。

下一阶段生成 `struct trace_event_class` 和 `struct trace_event_call`（`:452`、`:466`）：

```c
/* include/trace/trace_events.h:466 ，DEFINE_EVENT 的展开 */
static struct trace_event_call event_<call> = {
	.class			= &event_class_<template>,
	{ .tp			= &__tracepoint_<call>, },
	.event.funcs		= &trace_event_type_funcs_<template>,
	.print_fmt		= print_fmt_<template>,
	.flags			= TRACE_EVENT_FL_TRACEPOINT,
};
/* 指针放进 _ftrace_events 段 */
```

`class->probe` 就是 `trace_event_raw_event_<call>`（`:460`）。`class->reg` 是 `trace_event_reg`。

`struct trace_event_call` 本体（`include/linux/trace_events.h:372`）：

```c
struct trace_event_call {
	struct list_head	list;
	struct trace_event_class *class;
	union {
		const char		*name;
		struct tracepoint	*tp;   /* 置了 TRACE_EVENT_FL_TRACEPOINT */
	};
	struct trace_event	event;         /* type 号 + 打印函数 */
	char			*print_fmt;
	struct event_filter	*filter;
	union {
		void			*module;
		atomic_t		refcnt;    /* 动态事件用引用计数 */
	};
	void			*data;
	int			flags;             /* TRACE_EVENT_FL_* */
#ifdef CONFIG_PERF_EVENTS
	int				perf_refcount;
	struct hlist_head __percpu	*perf_events;
	struct bpf_prog_array __rcu	*prog_array;
#endif
};
```

`TRACE_EVENT_FL_*`（`:355`）里：`TRACEPOINT`、`DYNAMIC`、`KPROBE`、`UPROBE`、`EPROBE`、`FPROBE`、`FILTERED`。kprobe 事件走 `DYNAMIC | KPROBE`，不走上面的 `DEFINE_EVENT` 段。

每条记录前面都有 `struct trace_entry`（`trace_events.h:79`）：

```c
struct trace_entry {
	unsigned short		type;
	unsigned char		flags;          /* TRACE_FLAG_* */
	unsigned char		preempt_count;
	int			pid;
};
```

`format` 文件里这四项印成 `common_type`、`common_flags`、`common_preempt_count`、`common_pid`。`tracing_generic_entry_update`（`:167`）填写它们。`flags` 的位在 `enum trace_flag_type`（`:179`）：IRQ 关、need-resched、hardirq、softirq、NMI、bh 关。`trace` 文件开头的 `irqs-off / need-resched / hardirq / preempt-depth / migrate-disable` 图例由 `print_lat_help_header` 打印（`trace.c:4199`）。

`trace_event_buffer_reserve`（`trace_events.c:641`）若文件带 `EVENT_FILE_FL_PID_FILTER` 且本 CPU 的 `ignore_pid` 为真，直接返回 NULL。否则 `tracing_gen_ctx_dec`：tracepoint 自己已经 `preempt_disable_notrace`，抢占计数多了 1，这里减回去，注释在 `:652`。然后 `trace_event_buffer_lock_reserve`，类型号是 `event_call->event.type`。

---

## 6.5 用 `sched_switch` 对一下

`include/trace/events/sched.h:222`：

```c
TRACE_EVENT(sched_switch,
	TP_PROTO(bool preempt, struct task_struct *prev,
		 struct task_struct *next, unsigned int prev_state),
	TP_ARGS(preempt, prev, next, prev_state),
	TP_STRUCT__entry(
		__array(char, prev_comm, TASK_COMM_LEN)
		__field(pid_t, prev_pid)
		__field(int, prev_prio)
		__field(long, prev_state)
		__array(char, next_comm, TASK_COMM_LEN)
		__field(pid_t, next_pid)
		__field(int, next_prio)
	),
	/* TP_fast_assign / TP_printk 接在后面 */
);
```

`prev_state` 在记录里是 `long`。它由 `__trace_sched_switch_state`（同文件 `:190`）填，编码留在调度章。调用点是调度器里的 `trace_sched_switch`，位置见 [scheduler/04](../scheduler/04-enqueue-wakeup.md)。本章只确认：这个宏走上面的展开，event 目录是 `events/sched/sched_switch/`，里面有 `enable`、`filter`、`format`、`id`、`trigger`。

打开 `enable` 走到 `event_enable_write`（`trace_events.c:1564`），再 `__ftrace_event_enable_disable`（`:757`），最后 `trace_event_reg(TRACE_REG_REGISTER)` 把探针挂上，`static_key` 被打开，之后的 `trace_sched_switch()` 才进环。

---

## 6.6 event 文件从哪来

静态事件的 `trace_event_call` 指针在 `_ftrace_events` 段里。启动时 `trace_create_new_event`（`trace_events.c:3204`）为每个 call 在所属的 `trace_array` 上做一份 `trace_event_file`（`trace_events.h:655`）：`event_call`、`filter`、`tr`、`triggers`、`flags`。`events/<system>/<name>/` 下的 `enable` 改的是这份 file 的 `EVENT_FILE_FL_ENABLED`，不是全局唯一的 call。所以 instance 可以单独打开同一个 tracepoint，探针的 `data` 指向那份 file，记录进那份 `trace_array` 的环。

---

## 6.7 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `trace_<name>` | `tracepoint.h:255` 宏 | static key 通过后 `__DO_TRACE` |
| `__traceiter_<name>` | `tracepoint.h:319` 宏 | 遍历 `funcs` |
| `tracepoint_probe_register` | `tracepoint.c:531` | 默认优先级 10 |
| `tracepoint_add_func` | `:323` | 插入并更新 static call |
| `trace_event_reg` | `trace_events.c:673` | event/perf 的注册入口 |
| `trace_event_buffer_reserve` | `:641` | 为一条 event 预留环空间 |
| `trace_event_buffer_commit` | 声明在 `trace_events.h:321`，定义 `trace.c:2993` | 提交 |
| `event_enable_write` | `trace_events.c:1564` | 写 `enable` 文件 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
