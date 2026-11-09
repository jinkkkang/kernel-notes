# 8. kprobe 核心

**结论**：`register_kprobe` 解析地址、查黑名单、在可执行槽里放好原指令，然后把目标第一字节改成 `int3`（`0xCC`）。命中后 `kprobe_int3_handler` 跑 `pre_handler`，再单步原指令。能优化时 `arch_prepare_optimized_kprobe` 做一条越界跳进 trampoline，trampoline 调 `optimized_callback` 后跳回后续指令。同一地址上的第二根探针收成 aggr probe。

> `struct kprobe` 在 `include/linux/kprobes.h:60`。`register_kprobe` 在 `kernel/kprobes.c:1634`。x86 的断点在 `arch/x86/kernel/kprobes/core.c`，优化在 `opt.c`。BPF 怎样把程序挂到已经存在的 kprobe perf event 上，见 [ebpf/06](../ebpf/06-attach-and-link.md)。本章停在注册、断点和优化。

---

## 8.1 `struct kprobe` 与 `kretprobe`

```c
/* include/linux/kprobes.h:60 */
struct kprobe {
	struct hlist_node hlist;          /* 按地址挂进 kprobe_table */
	struct list_head list;            /* 同一地址多探针时的链 */
	unsigned long nmissed;            /* 暂时卸下或重入时加一 */
	kprobe_opcode_t *addr;
	const char *symbol_name;
	unsigned int offset;
	kprobe_pre_handler_t pre_handler; /* 返回非 0 则跳过单步 */
	kprobe_post_handler_t post_handler;
	kprobe_opcode_t opcode;           /* 被 int3 替换掉的原字节 */
	struct arch_specific_insn ainsn;  /* 拷出去的原指令 */
	u32 flags;
};
```

标志（`:98`）：`KPROBE_FLAG_GONE`、`DISABLED`、`OPTIMIZED`、`FTRACE`、`ON_FUNC_ENTRY`。用户传给 `register_kprobe` 的 flags 只保留 `DISABLED`（`kprobes.c:1653`），其余由核心设置。`kprobe_ftrace()` 为真时不走 int3，改走 ftrace 入口，`prepare_kprobe`（`:1161`）调用 `arch_prepare_kprobe_ftrace`。

```c
/* include/linux/kprobes.h:147 */
struct kretprobe {
	struct kprobe kp;
	kretprobe_handler_t handler;        /* 返回时 */
	kretprobe_handler_t entry_handler;  /* 进入时，可选 */
	int maxactive;                      /* 同时在飞的实例上限 */
	int nmissed;
	size_t data_size;                   /* 每实例 data[] 的长度，上限 4096 */
#ifdef CONFIG_KRETPROBE_ON_RETHOOK
	struct rethook *rh;
#else
	struct freelist_head freelist;
	struct kretprobe_holder *rph;
#endif
};
```

`KRETPROBE_MAX_DATA_SIZE` 是 4096（`:162`）。`maxactive <= 0` 时 `register_kretprobe` 改成 `max(10, 2 * num_possible_cpus())`（`kprobes.c:2248`）。实例耗尽则 `nmissed` 增加，这次返回不再回调。

---

## 8.2 `register_kprobe`

```text
register_kprobe                         kernel/kprobes.c:1634
  _kprobe_addr(addr, symbol_name, offset)     解析并记下是否在函数入口
  warn_kprobe_rereg
  flags 只留 DISABLED，nmissed = 0
  check_kprobe_address_safe
    黑名单、不在可探测的指令边界、模块 __init 已释放 → 错误
  kprobe_mutex
  入口则置 KPROBE_FLAG_ON_FUNC_ENTRY
  该地址已有探针 → register_aggr_kprobe
  否则 prepare_kprobe
    ftrace 入口 → arch_prepare_kprobe_ftrace
    否则 arch_prepare_kprobe
  hlist_add_head_rcu 进 kprobe_table
  未全局卸下且未 DISABLED → arm_kprobe
  try_to_optimize_kprobe
```

`check_kprobe_address_safe` 会 `try_module_get`。目标在模块的 `.init.text` 且模块已经不是 `MODULE_STATE_COMING` 时返回 `-ENOENT`（`:1620`）。

同一地址的第二根探针走 `register_aggr_kprobe`。新的带 `post_handler` 时，`add_new_kprobe`（`:1268`）先 `unoptimize_kprobe`：优化 trampoline 不跑 post_handler 那条单步，有 post 就退回 int3。

`kprobe_on_func_entry`（`:2200`）给 kretprobe 和 fprobe 用。偏移不在入口返回 `-EINVAL`。

---

## 8.3 x86：int3 与单步

`arch_prepare_kprobe`（`arch/x86/kernel/kprobes/core.c:751`）：

- 地址落在 alternatives 保留区 → `-EINVAL`
- `can_probe` 失败 → `-EILSEQ`（不在指令边界，或这条指令不能安全地单步）
- `get_insn_slot` 从可执行页拿一块缓冲，`arch_copy_kprobe` 把原指令写进去

`arch_arm_kprobe`（`:777`）把 1 字节 `INT3_INSN_OPCODE`（`0xCC`，`arch/x86/include/asm/text-patching.h:57`）`text_poke` 到 `p->addr`，再 `text_poke_sync`。原字节留在 `p->opcode`。

`arm_kprobe`（`kprobes.c:1170`）在非 ftrace 路径上持 `cpus_read_lock` 和 `text_mutex` 调 `__arm_kprobe`。

命中后的入口是 `kprobe_int3_handler`（`core.c:968`）。`user_mode` 直接返回 0，交给其它 int3。内核态把 `regs->ip - 1` 当作探针地址。注释（`:979`）写明：int3 和 debug 陷阱关掉中断，单步时还清 IF，整段处理不能被抢占。

```text
get_kprobe(addr) 命中
  已经有 current_kprobe → reenter_kprobe
  否则 set_current_kprobe，状态 KPROBE_HIT_ACTIVE
  没有 pre_handler，或 pre_handler 返回 0
    → setup_singlestep
  pre_handler 返回非 0
    → reset_current_kprobe，不再单步
```

单步在拷出去的那条指令上做。指令自己又触发 fault 时，`kprobe_fault_handler`（`:1023`）把 `ip` 指回原地址。处理函数标了 `NOKPROBE_SYMBOL`，避免探针打在探针上。

`pre_handler` 的返回值约定就在 `:995` 的注释里：非 0 表示处理函数已经把寄存器安排成从别的指令继续，核心不再单步。BPF 的 kprobe 程序返回值是另一套（0 过滤、1 记录），那是 `trace_call_bpf` 的约定，不是这里的 `pre_handler` 返回值。

---

## 8.4 优化 trampoline

`try_to_optimize_kprobe` 在 arm 之后调用（`register_kprobe` 的 `:1696`）。`arch_prepare_optimized_kprobe`（`arch/x86/kernel/kprobes/opt.c:391`）：

```text
can_optimize 失败 → -EILSEQ
get_optinsn_slot
槽和探针地址的相对偏移超出 ±2GB → -ERANGE
  （用的是 rel32 跳转）
从 optprobe_template_entry 拷模板
copy_optimized_instructions 把探针处的若干指令搬进槽
synthesize_relcall → optimized_callback
槽尾 synthesize_reljump 回到 addr + optinsn.size
text_poke 写进只读执行页
```

优化成功后，入口从 int3 换成一条跳进槽的跳转，`KPROBE_FLAG_OPTIMIZED` 置上。`optimized_callback`（`opt.c:177`）在 `kprobe_running()` 已经为真时只 `kprobes_inc_nmissed_count` 并返回；否则 `preempt_disable`，修正 `regs->sp`（加回一个 long，`:189`），再走和 int3 类似的 pre_handler。它不单步被拷走的那几条指令，那些指令放在槽里、回调之后执行，最后跳回原函数后续。

有 `post_handler` 的探针不会留在优化路径上，见 `add_new_kprobe`。

---

## 8.5 黑名单、`kprobe_busy`、抢占

`within_kprobe_blacklist`（`kprobes.c:1413`）先看 `kprobe_blacklist` 上的 `[start, end)`，再对带后缀的符号（`name.xxx`）剥掉后缀重查。`kprobe_blacklist_entry`（`kprobes.h:185`）是这张表的节点。x86 在 `arch_populate_kprobe_blacklist`（`core.c:1057`）里把 `__entry_text_start` 到 `__entry_text_end` 加进去。kretprobe 另有 `kretprobe_blacklist`，`register_kretprobe` 命中则 `-EINVAL`（`kprobes.c:2229`）。

`kprobe_busy`（`kprobes.c:1247`）是一根假探针，地址指向 `get_kprobe`。`kprobe_busy_begin`（`:1251`）`preempt_disable`，把本 CPU 的 `current_kprobe` 设成它，状态 `KPROBE_HIT_ACTIVE`。这段区间里 `kprobe_running()` 为真，真正的探针走重入路径并增加 `nmissed`。`kprobe_busy_end`（`:1262`）清掉 `current_kprobe` 并 `preempt_enable`。核心在自己读探针表、不能再嵌进用户处理函数时用这一对。

int3 路径的约束写在 `kprobe_int3_handler` 的注释里：处理过程不可抢占，中断已关。优化回调自己 `preempt_disable`（`opt.c:183`）。因此 `pre_handler` 不能睡眠。要睡眠的观测用 tracepoint 或 kprobe 事件的之后阶段，不在这个处理函数里。

ftrace 入口上的 kprobe 不改 int3。`kprobe_ftrace_handler` 在 `arch/x86/kernel/kprobes/ftrace.c:16`。它要求探针带 `KPROBE_FLAG_FTRACE`，并且地址就是函数入口。这条路径和 `register_ftrace_direct` 共用 ftrace 的调用点改写，direct call 的实现在 [04](./04-dynamic-ftrace.md)。

---

## 8.6 `register_kretprobe`

```text
register_kretprobe                      kprobes.c:2214
  kprobe_on_func_entry                  不在入口 → 错误
  kretprobe_blacklist
  data_size > 4096 → -E2BIG
  kp.pre_handler = pre_handler_kretprobe
  kp.post_handler = NULL
  maxactive 默认 max(10, 2 * nr_cpu_ids 的可能 CPU 数)
  CONFIG_KRETPROBE_ON_RETHOOK：
    rethook_alloc + 预分配 maxactive 个 instance
  否则 freelist 预分配
  register_kprobe(&rp->kp)
```

入口探针命中时从池里取一个 `kretprobe_instance`，把返回地址换成 trampoline。返回进 `handler`。池空则 `nmissed++`，这次不跟踪返回。`CONFIG_KRETPROBE_ON_RETHOOK` 时实例挂在 `rethook` 上，`get_kretprobe` 从 `ri->node.rethook->data` 拿回 `kretprobe`（`kprobes.h:204`）。没开这个选项时用 `freelist`，架构要提供 `arch_prepare_kretprobe` 和 `__kretprobe_trampoline`。

---

## 8.7 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `register_kprobe` | `kprobes.c:1634` | 解析、准备、arm、尝试优化 |
| `register_kretprobe` | `:2214` | 入口探针加返回 trampoline |
| `arm_kprobe` | `:1170` | int3 或 ftrace |
| `within_kprobe_blacklist` | `:1413` | 拒绝入口区等地址 |
| `kprobe_busy_begin` | `:1251` | 占住 `current_kprobe` |
| `arch_prepare_kprobe` | `arch/x86/kernel/kprobes/core.c:751` | 拷贝原指令 |
| `arch_arm_kprobe` | `:777` | 写入 `0xCC` |
| `kprobe_int3_handler` | `:968` | pre_handler 与单步 |
| `arch_prepare_optimized_kprobe` | `opt.c:391` | 准备优化槽 |
| `optimized_callback` | `:177` | 优化路径的回调 |
| `kprobe_ftrace_handler` | `ftrace.c:16`（kprobes 目录） | 函数入口走 ftrace |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
