# 4. 动态 ftrace

**结论**：编译期每个被跟踪函数的入口地址收进 `__mcount_loc`。`ftrace_init` 把它变成 `dyn_ftrace` 记录。x86 上这条指令是 5 字节，启动时改成 NOP；有人 `register_ftrace_function` 之后，`text_poke_queue` 把 NOP 改回 `call`。`ftrace_ops` 上的 filter hash 决定哪些地址跟这个回调。`register_ftrace_direct` 在同一条入口上再登记一个直接跳转目标，BPF 的 fentry 从这里接上。

> `struct ftrace_ops` 在 `include/linux/ftrace.h:327`。`struct dyn_ftrace` 在 `:592`。`ftrace_init` 在 `kernel/trace/ftrace.c:7369`。`register_ftrace_function` 在 `:8200`。`register_ftrace_direct` 在 `:5460`。x86 的改写在 `arch/x86/kernel/ftrace.c`，`MCOUNT_INSN_SIZE` 是 5（`arch/x86/include/asm/ftrace.h:10`）。

---

## 4.1 从 `__mcount_loc` 到 NOP

`ftrace_init`（`:7369`）读 `__start_mcount_loc` 到 `__stop_mcount_loc` 的地址个数，交给 `ftrace_process_locs`（`:6561`）。失败则把 `ftrace_disabled` 置 1。成功后 `ftrace_enabled = 1`（`:7399`）。

头文件里调用点所在的段名（`ftrace.h:997`）：定义了 `CC_USING_PATCHABLE_FUNCTION_ENTRY` 时是 `__patchable_function_entries`，否则是 `__mcount_loc`。`ftrace_init` 使用的符号名仍是 `__start_mcount_loc` / `__stop_mcount_loc`。链接脚本把对应段收成这两个符号。本章不把某一次编译的段名说成唯一形式。

每条记录：

```c
/* include/linux/ftrace.h:592 */
struct dyn_ftrace {
	unsigned long		ip; /* mcount / fentry 调用点地址 */
	unsigned long		flags;
	struct dyn_arch_ftrace	arch;
};
```

x86 的 `dyn_arch_ftrace` 是空结构（`arch/x86/include/asm/ftrace.h:98`）。`flags` 的高位是状态，低 19 位是引用计数：`FTRACE_REF_MAX_SHIFT` 为 19（`ftrace.h:587`），`ftrace_rec_count` 取低位。高位里和本章有关的是 `FTRACE_FL_ENABLED`、`FTRACE_FL_REGS`、`FTRACE_FL_TRAMP`、`FTRACE_FL_IPMODIFY`、`FTRACE_FL_DIRECT` 以及对应的 `_EN`（`:571`）。

启动时把 `MCOUNT_ADDR` 改成 NOP 走 `ftrace_make_nop`（`arch/x86/kernel/ftrace.c:127`）。`addr == MCOUNT_ADDR` 时用 `ftrace_modify_code_direct`（`:111`）：开机或模块刚加载、指令还不会被执行，直接 `text_poke_early` 或 `text_poke_queue`。注释写明这条路径不会再变回 `MCOUNT_ADDR`。其它 `addr` 调用 `ftrace_make_nop` 会 `WARN_ONCE` 并返回 `-EINVAL`（`:150`），因为 x86 覆盖了 `ftrace_replace_code`，运行中的改写不走这个函数。

---

## 4.2 `ftrace_ops` 与注册

```c
/* include/linux/ftrace.h:327
 * 回调类型 ftrace_func_t（:173）：
 *   void (*)(ip, parent_ip, ops, fregs)
 */
struct ftrace_ops {
	ftrace_func_t			func;
	struct ftrace_ops __rcu		*next;
	unsigned long			flags;
	void				*private;
	ftrace_func_t			saved_func;
#ifdef CONFIG_DYNAMIC_FTRACE
	struct ftrace_ops_hash		local_hash;
	struct ftrace_ops_hash		*func_hash;
	struct ftrace_ops_hash		old_hash;
	unsigned long			trampoline;
	unsigned long			trampoline_size;
	struct list_head		list;
	ftrace_ops_func_t		ops_func;
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
	unsigned long			direct_call;
#endif
#endif
};
```

`ftrace_ops_hash`（`:300`）有两张表：`filter_hash` 和 `notrace_hash`，外加 `regex_lock`。`hash_contains_ip`（`ftrace.c:1490`）的规则写在注释里：filter 为空视为全部匹配；notrace 为空视为没有排除；地址在 filter 里且不在 notrace 里才算命中。`set_ftrace_filter` 读出来是 `#### all functions enabled ####` 时，打印点在 `:3856`，对应空的 filter hash。

标志位 `FTRACE_OPS_FL_*`（`ftrace.h:234`）里本章用到的：

| 位 | 含义 |
|----|------|
| `ENABLED` | 已注册 |
| `DYNAMIC` | 动态分配，注销时要特别处理 |
| `SAVE_REGS` / `SAVE_REGS_IF_SUPPORTED` | 回调要 `pt_regs`。架构不支持且没设 IF_SUPPORTED，注册失败 |
| `RECURSION` | 基础设施负责防递归。不设则回调自己处理 |
| `PID` | 受 `set_ftrace_pid` 影响 |
| `IPMODIFY` | 允许改 IP。必须同时 `SAVE_REGS`。同一地址上不能有第二个 IPMODIFY |
| `DIRECT` | direct helper 使用，注释写明内部用 |
| `ALLOC_TRAMP` | 核心代码分配过 trampoline |
| `PERMANENT` | 不受 `ftrace_enabled` 关掉的影响 |
| `RCU` | 只在 RCU 正在观察时调用 |

顶层 function tracer 用的 `global_ops`（`ftrace.c:1070`）初始 `func` 是 `ftrace_stub`，两张 hash 都是 `EMPTY_HASH`，标志是 `INITIALIZED | PID`。`register_ftrace_function`（`:8200`）走到 `__register_ftrace_function`（`:339`），再按记录上的计数把 NOP 改成 call。

多个 ops 挂在 `ftrace_ops_list` 上。`do_for_each_ftrace_op`（`ftrace.h:359`）用 `rcu_dereference_raw_check` 遍历。只有一个 ops 时，入口可以直接跳该 ops 的 trampoline，不必进列表函数。列表函数是 `__ftrace_ops_list_func`（`ftrace.c:7490`）。

---

## 4.3 x86 上的改写与 trampoline

运行中批量改写是 `ftrace_replace_code`（`arch/x86/kernel/ftrace.c:197`）。它扫 `ftrace_rec_iter`，用 `ftrace_test_record` 得到四种结果（`ftrace.h:631`）：

| 结果 | 做什么 |
|------|--------|
| `FTRACE_UPDATE_IGNORE` | 已经是目标形态 |
| `FTRACE_UPDATE_MAKE_CALL` | NOP → call，旧字节是 NOP |
| `FTRACE_UPDATE_MODIFY_CALL` | 换成另一个目标（例如要保存寄存器） |
| `FTRACE_UPDATE_MAKE_NOP` | call → NOP |

第一遍 `ftrace_verify_code` 核对当前字节，第二遍 `text_poke_queue` 写入 `MCOUNT_INSN_SIZE`（5）个字节（`:249`）。改全局回调 `ftrace_call` / `ftrace_regs_call` 的目标用 `text_poke_bp`（`:188`、`:192`）。

`arch_ftrace_update_trampoline`（`arch/x86/kernel/ftrace.c:474`）为 ops 准备 trampoline。`FTRACE_FL_REGS` 的记录进保存 `pt_regs` 的 `ftrace_regs_caller`（`arch/x86/kernel/ftrace_64.S:199`），否则走普通的 `ftrace_caller`。

`ftrace_set_filter_ip`（`ftrace.c:5719`）按地址改一张 ops 的 filter。`ftrace_set_filter`（`:5788`）吃 tracefs 写进来的符号或 glob。`remove` 与 `reset` 两个整数参数决定是追加、删除还是先清空。BPF 侧在挂 fentry 之前调用的就是 `ftrace_set_filter_ip`，见 [ebpf/06](../ebpf/06-attach-and-link.md)。

---

## 4.4 `register_ftrace_direct`

BPF 的 `register_fentry` 在 ftrace 位置非 0 时调用这个函数。本章从这里写 ftrace 侧。

`register_ftrace_direct(ops, addr)`（`ftrace.c:5460`）的注释（`:5440`）要求：`addr` 指向的 trampoline 能接受一次 direct call，保存被跟踪函数的参数，返回前恢复或按需要注入新参数。

前置条件，不满足直接 `-EINVAL`：

- `ops->func` 或 `ops->trampoline` 已经非 0（`:5466`）
- 没有 `FTRACE_OPS_FL_INITIALIZED`（`:5468`）
- 已经 `FTRACE_OPS_FL_ENABLED`（`:5470`）
- `filter_hash` 为空（`:5474`）。direct 不能「跟踪一切」，必须先 `ftrace_set_filter_ip`

然后持 `direct_mutex`。filter 里任一地址已经在 `direct_functions` 里（`ftrace_find_rec_direct`）就 `-EBUSY`（`:5483`）：同一 ip 上只能有一个 direct 目标。通过后把旧的 direct 项和新地址拷进一张新 hash，`entry->direct = addr`（`:5518`），`rcu_assign_pointer` 换上 `direct_functions`。

接着改 ops 本身（`:5527`）：

```text
ops->func        = call_direct_funcs
ops->flags       = MULTI_FLAGS
ops->trampoline  = FTRACE_REGS_ADDR
ops->direct_call = addr
register_ftrace_function_nolock(ops)
```

`MULTI_FLAGS`（`:5396`）：有 `CONFIG_HAVE_DYNAMIC_FTRACE_WITH_REGS` 时是 `FTRACE_OPS_FL_DIRECT | FTRACE_OPS_FL_SAVE_REGS`，否则是 `DIRECT | SAVE_ARGS`。x86 走前一种，所以 direct 从 `ftrace_regs_caller` 出去。

`call_direct_funcs`（`:2582`）读 `ops->direct_call`，调用 `arch_ftrace_set_direct_caller`。x86 的实现把地址写进 `pt_regs->orig_ax`（`arch/x86/include/asm/ftrace.h:87`）。`ftrace_regs_caller` 末尾检查 `ORIG_RAX`（`ftrace_64.S:265`）：非 0 则把它当成 direct 目标，用注释里写的 PUSH/RET 形式尾调用（`:295`）。

失败时 `reset_direct`（`:5534`）。换下的旧 hash 在 `synchronize_rcu_tasks` 之后释放（`:5540`）。

和函数图 tracer 的关系：`function_graph_enter`（`kernel/trace/fgraph.c:123`）在没有 `CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS` 时，若返回地址落在 direct trampoline 上就返回 `-EBUSY`，不把这一帧推进 shadow stack。注释说明 BPF trampoline 可能跳过帧。direct 与 graph 共用入口时的这条限制在 v6.6.160 的这段 `#ifndef` 里。

`FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_*`（`ftrace.h:283`）用来让一个 DIRECT ops 和一个带 `IPMODIFY` 的 ops 出现在同一函数上。协商失败则注册返回错误。BPF 侧的 trampoline 映像和 `BPF_TRAMP_F_*` 标志不在本章，见 ebpf/06。

---

## 4.5 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `ftrace_init` | `ftrace.c:7369` | 消费 `__mcount_loc` |
| `ftrace_process_locs` | `:6561` | 建成 `dyn_ftrace` 页 |
| `ftrace_location` | `:1628` | 地址是否是 ftrace 调用点。BPF 用它决定走 direct 还是 text poke |
| `ftrace_set_filter_ip` | `:5719` | 按 ip 改 filter hash |
| `hash_contains_ip` | `:1490` | 空 filter 等于全匹配 |
| `register_ftrace_function` | `:8200` | 注册 ops 并改写调用点 |
| `ftrace_replace_code` | `arch/x86/kernel/ftrace.c:197` | NOP 与 call 的批量替换 |
| `ftrace_make_nop` | 同文件 `:127` | 仅用于把 `MCOUNT_ADDR` 改成 NOP |
| `register_ftrace_direct` | `ftrace.c:5460` | 把 `addr` 登记成这些 ip 的 direct 目标 |
| `call_direct_funcs` | `:2582` | 把 `direct_call` 写入 `orig_ax` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
