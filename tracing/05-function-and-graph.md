# 5. function 与 function_graph

**结论**：`echo function > current_tracer` 注册一份 `ftrace_ops`，回调 `function_trace_call` 把 `ip` 和 `parent_ip` 写进环。`function_graph` 另外在返回地址上做手脚：`prepare_ftrace_return` 把原返回地址推进当前任务的 `ret_stack`，返回时进 `ftrace_return_to_handler`，用 `calltime` 和 `rettime` 算这一帧的耗时。v6.6.160 同时只允许一个 graph tracer 注册。

> function tracer 在 `kernel/trace/trace_functions.c`。graph 的 shadow stack 在 `kernel/trace/fgraph.c`，打印和 `fgraph_ops` 在 `kernel/trace/trace_functions_graph.c`。记录结构在 `include/linux/ftrace.h:1013` 起。

---

## 5.1 function tracer

`struct tracer function_trace`（`trace_functions.c:426`）的名字是 `"function"`，`allow_instances` 为 true。`init` 是 `function_trace_init`（`:132`）：

```text
select_trace_function(func_flags.val)     默认 function_trace_call
ftrace_init_array_ops(tr, func)           顶层改的是 global_ops
tracing_start_function_trace
  tr->function_enabled = 0
  register_ftrace_function(tr->ops)
  tr->function_enabled = 1
```

先把 `function_enabled` 清 0 再注册，避免注册过程中的调用写进环。`ftrace_init_array_ops`（`ftrace.c:7472`）对全局 array 有一个检查：当时的 `ops->func` 必须还是 `ftrace_stub`。

默认回调：

```text
function_trace_call                         trace_functions.c:172
  tr->function_enabled 为 0 则返回
  ftrace_test_recursion_trylock(ip, parent_ip)
  tracing_gen_ctx_dec()
  本 CPU 的 trace_array_cpu->disabled 为 0
    trace_function(tr, ip, parent_ip, trace_ctx)     trace.c:3056
  ftrace_test_recursion_unlock
```

`trace_function` 预留一条 `TRACE_FN` 记录，类型是 `ftrace_entry`（`kernel/trace/trace_entries.h:59`）：

```c
/* trace_entries.h:59 ，FTRACE_ENTRY_REG 展开后的字段 */
__field_fn(unsigned long, ip)
__field_fn(unsigned long, parent_ip)
/* 打印：%ps <-- %ps */
```

`trace` 文件里看到的是 `被调用函数 <- 父函数`。父函数来自 `parent_ip`，不是栈回溯。

`trace_array_cpu->disabled`（`trace.h:187`）是另一层嵌套计数。带栈的变体 `function_stack_trace_call`（`trace_functions.c:217`）在进回调时 `local_irq_save`，`atomic_inc_return(&data->disabled) == 1` 才记录，并 `__trace_stack`。选项位是 `TRACE_FUNC_OPT_STACK`（`:43`），tracefs 上的名字是 `func_stack_trace`（`:367`）。`func-no-repeats` 对应 `TRACE_FUNC_OPT_NO_REPEATS`，连续相同的 `ip/parent_ip` 合成 `trace_func_repeats`（`trace.h:311`）。

`reset`（`:159`）把 `function_enabled` 清 0，`unregister_ftrace_function`，再 `ftrace_reset_array_ops` 把 `func` 设回 `ftrace_stub`。

filter 过宽时，每个匹配函数的每次调用都进 `function_trace_call` 再尝试 reserve。怎么收窄见 [12](./12-perf-analysis.md)。本机一次只跟 `do_sys_openat2` 的采样在 [demos](./demos/README.md)。

---

## 5.2 function_graph 的两个记录

```c
/* include/linux/ftrace.h:1013 */
struct ftrace_graph_ent {
	unsigned long func; /* 当前函数 */
	int depth;
} __packed;

/* :1023
 * CONFIG_FUNCTION_GRAPH_RETVAL 才有 retval。
 */
struct ftrace_graph_ret {
	unsigned long func;
#ifdef CONFIG_FUNCTION_GRAPH_RETVAL
	unsigned long retval;
#endif
	int depth;
	unsigned int overrun;          /* 超过深度限制的次数 */
	unsigned long long calltime;
	unsigned long long rettime;
} __packed;

/* :1043 */
struct fgraph_ops {
	trace_func_graph_ent_t		entryfunc;  /* 返回非 0 才保留这一帧 */
	trace_func_graph_ret_t		retfunc;
};
```

环上的包装是 `ftrace_graph_ent_entry` / `ftrace_graph_ret_entry`（`trace_entries.h:75` 起），类型号 `TRACE_GRAPH_ENT` 和 `TRACE_GRAPH_RET`。`trace_assign_type` 能认出这两份（`trace.h:501`）。

function_graph tracer 准备的 ops（`trace_functions_graph.c:294`）：

```c
static struct fgraph_ops funcgraph_ops = {
	.entryfunc = &trace_graph_entry,
	.retfunc = &trace_graph_return,
};
```

`tracing_thresh` 非 0 时改用 `funcgraph_thresh_ops`，返回回调是 `trace_graph_thresh_return`：耗时小于阈值就不再 `trace_graph_return`（`:287`）。`graph_trace_init`（`:304`）按这个条件 `register_ftrace_graph`。

---

## 5.3 shadow stack

每个任务一块 `ftrace_ret_stack` 数组，深度上限 `FTRACE_RETFUNC_DEPTH` 是 50（`ftrace.h:1092`）。

```c
/* include/linux/ftrace.h:1053 */
struct ftrace_ret_stack {
	unsigned long ret;             /* 原返回地址 */
	unsigned long func;
	unsigned long long calltime;
#ifdef CONFIG_FUNCTION_PROFILER
	unsigned long long subtime;
#endif
#ifdef HAVE_FUNCTION_GRAPH_FP_TEST
	unsigned long fp;
#endif
#ifdef HAVE_FUNCTION_GRAPH_RET_ADDR_PTR
	unsigned long *retp;           /* 栈上存放返回地址的那个槽 */
#endif
};
```

x86 入口是 `prepare_ftrace_return`（`arch/x86/kernel/ftrace.c:621`）。它拿到 `return_to_handler` 的地址，做几道拒绝：栈指针看起来不像内核虚拟地址（从 suspend 的实模式回来时）、`ftrace_graph_is_dead()`、`current->tracing_graph_pause`、递归锁失败。通过后调用 `function_graph_enter(*parent, ip, frame_pointer, parent)`（`:649`）。

`function_graph_enter`（`fgraph.c:123`）：

```text
trace.func = func
trace.depth = ++current->curr_ret_depth
没有 DYNAMIC_FTRACE_WITH_ARGS 且返回地址属于 direct trampoline
  → -EBUSY，不记录
ftrace_push_return_trace(ret, func, frame_pointer, retp)    :69
  ret_stack 为空或 curr_ret_stack 已到 DEPTH-1 → -EBUSY
  满了则 atomic_inc(&current->trace_overrun)
  否则写下 ret / func / calltime（trace_clock_local）
ftrace_graph_entry(&trace) 返回 0
  → 弹出刚推进去的一帧，返回 -EBUSY
成功则调用方把 *parent 换成 return_to_handler
```

`ftrace_graph_entry` 默认是 `ftrace_graph_entry_stub`（`fgraph.c:401`），注册之后换成 `ftrace_graph_entry_test`（`:664`），测试通过再调用户的 `entryfunc`。`entryfunc` 返回 0 表示「这一帧不要」。

返回路径进 `ftrace_return_to_handler`。v6.6.160 有两个原型，由 `CONFIG_HAVE_FUNCTION_GRAPH_RETVAL` 选择（`fgraph.c:281`）：有 retval 时参数是 `struct fgraph_ret_regs *`，否则是 `unsigned long frame_pointer`。两者都进 `__ftrace_return_to_handler`，弹出 `ret_stack`，填 `rettime`，调用 `ftrace_graph_return`（注册时是 `gops->retfunc`），最后返回保存的原返回地址。

`overrun` 字段记的是深度被顶满的次数，和环缓冲的 `overrun` 不是同一个计数。环那边的丢事件看 `per_cpu/cpuN/stats`。

---

## 5.4 注册时的限制

`register_ftrace_graph`（`fgraph.c:633`）持 `ftrace_lock`。`ftrace_graph_active` 已经非 0 就 `-EBUSY`（`:640`），注释写明当前只允许一个 graph tracer。随后 `start_graph_tracing` 给任务分配 `ret_stack`，并 `register_trace_sched_switch` 一份探针，以便切换任务时处理 shadow stack（`:623`）。

成功路径：

```text
ftrace_graph_return     = gops->retfunc
__ftrace_graph_entry    = gops->entryfunc
ftrace_graph_entry      = ftrace_graph_entry_test
ftrace_startup(&graph_ops, FTRACE_START_FUNC_RET)
```

`FTRACE_START_FUNC_RET` 让 ftrace 改写 graph 用的调用点（`ftrace.h:615`）。`unregister_ftrace_graph`（`fgraph.c:673`）把两个函数指针设回 stub。

`set_graph_function` 把记录收成一棵子树：只跟踪列出的函数及其被调用者。这个文件由 `ftrace_init_dyn_tracefs` 建在顶层目录（`ftrace.c:6517`），instance 里没有。`function_graph` 仍可以写进 instance 的 `current_tracer`，输出进那份 `trace_array`（`graph_trace_init` 调 `set_graph_array`），函数名单读的是顶层这份文件。

本章没有在本机打开 function_graph。命令在 [demos/04-function-graph.sh](./demos/04-function-graph.sh)，标为 RECIPE。

---

## 5.5 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `function_trace_init` | `trace_functions.c:132` | 打开 function tracer |
| `function_trace_call` | `:172` | 默认回调，写 `ftrace_entry` |
| `trace_function` | `trace.c:3056` | reserve `TRACE_FN` |
| `prepare_ftrace_return` | `arch/x86/kernel/ftrace.c:621` | 改返回地址 |
| `function_graph_enter` | `fgraph.c:123` | 推进 shadow stack |
| `ftrace_push_return_trace` | `:69` | 写入 `ret_stack[index]` |
| `ftrace_return_to_handler` | `:282` 或 `:288` | 弹出并调用 retfunc |
| `register_ftrace_graph` | `:633` | 全局只允许一份 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
