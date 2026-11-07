# 4. 进出用户态的工作

**结论**：`syscall_enter_from_user_mode` 先建立内核上下文并打开中断，再按 `thread_info.syscall_work` 上的 `SYSCALL_WORK_*` 跑挂钩。这些位和 `thread_info.flags` 上的 `_TIF_*` 是两个字段。退出时先做系统调用自己的 audit / ptrace / tracepoint，再进 `exit_to_user_mode_loop` 处理调度、信号和 `NOTIFY_RESUME`。中断回到用户态不跑前一半。

> 掩码在 `include/linux/entry-common.h:39`。实现在 `kernel/entry/common.c`。x86 的字段在 `arch/x86/include/asm/thread_info.h:56`。

---

## 4.1 两套位

```c
/* arch/x86/include/asm/thread_info.h:56 */
struct thread_info {
	unsigned long flags;         /* _TIF_* ，按位原子改 */
	unsigned long syscall_work;  /* SYSCALL_WORK_* */
	u32           status;        /* TS_COMPAT 等，只有本线程写 */
#ifdef CONFIG_SMP
	u32           cpu;
#endif
};
```

`SYSCALL_WORK_*` 的枚举在 `include/linux/thread_info.h:41`，包在 `CONFIG_GENERIC_ENTRY` 里。x86-64 走这套。位从 0 开始数，`BIT(枚举值)` 就是掩码：

| 宏 | 枚举 | 进入掩码 | 退出掩码 |
|---|---|---|---|
| `SYSCALL_WORK_SECCOMP` | 0 | 在 `SYSCALL_WORK_ENTER` | |
| `SYSCALL_WORK_SYSCALL_TRACEPOINT` | 1 | 在 | 在 `SYSCALL_WORK_EXIT` |
| `SYSCALL_WORK_SYSCALL_TRACE` | 2 | 在 | 在 |
| `SYSCALL_WORK_SYSCALL_EMU` | 3 | 在 | |
| `SYSCALL_WORK_SYSCALL_AUDIT` | 4 | 在 | 在 |
| `SYSCALL_WORK_SYSCALL_USER_DISPATCH` | 5 | 在 | 在 |
| `SYSCALL_WORK_SYSCALL_EXIT_TRAP` | 6 | | 在 |

`SYSCALL_WORK_ENTER` / `EXIT` 的按位或在 `entry-common.h:39` 和 `:46`，末尾还可以或上架构自己的 `ARCH_SYSCALL_WORK_*`（默认 0）。`set_syscall_work`（`thread_info.h:149`）对 `current_thread_info()->syscall_work` 做 `set_bit`。

`_TIF_*` 仍在 `flags` 上。和「回到用户态之前必须做完」有关的一撮合成 `EXIT_TO_USER_MODE_WORK`（`entry-common.h:60`）：

```c
_TIF_SIGPENDING | _TIF_NOTIFY_RESUME | _TIF_UPROBE |
_TIF_NEED_RESCHED | _TIF_PATCH_PENDING | _TIF_NOTIFY_SIGNAL
```

x86 上这几个位的定义在 `asm/thread_info.h:81` 起，例如 `TIF_SIGPENDING` 是 2，`TIF_NEED_RESCHED` 是 3，`TIF_NOTIFY_SIGNAL` 是 17。信号待处理看的是这组 TIF，看的是 `flags`。

---

## 4.2 进入

```c
/* kernel/entry/common.c:113 */
noinstr long syscall_enter_from_user_mode(struct pt_regs *regs, long syscall)
{
	long ret;

	__enter_from_user_mode(regs);     /* :19  lockdep、RCU user_exit、kmsan */
	instrumentation_begin();
	local_irq_enable();
	ret = __syscall_enter_from_user_work(regs, syscall);
	instrumentation_end();
	return ret;
}
```

`__enter_from_user_mode` 在中断仍关闭时做完。`entry_64.S` 的注释已经说明 `SYSCALL` 用 `SYSCALL_MASK` 清掉了 `IF`。打开中断之后才跑可能睡眠的挂钩（ptrace、seccomp）。

`__syscall_enter_from_user_work`（`:97`）读 `syscall_work`，和 `SYSCALL_WORK_ENTER` 相与。全零就直接返回原号，热路径上 `getpid` 就是这样。有任何一位，进 `syscall_trace_enter`（`:48`）。顺序是源码里的书写顺序：

```text
1. SYSCALL_WORK_SYSCALL_USER_DISPATCH
     syscall_user_dispatch(regs) 返回真 → 整个函数返回 -1
2. SYSCALL_WORK_SYSCALL_TRACE 或 SYSCALL_WORK_SYSCALL_EMU
     ptrace_report_syscall_entry(regs)
     返回非 0，或 EMU 置位 → 返回 -1
     然后重新读 syscall_work（ptrace 可能改了位）
3. SYSCALL_WORK_SECCOMP
     __secure_computing(NULL) == -1 → 返回 -1
4. 重新 syscall_get_nr（上面两步可能改了号）
5. SYSCALL_WORK_SYSCALL_TRACEPOINT
     trace_sys_enter(regs, syscall)
     再读一次号（探针可能改号）
6. syscall_enter_audit
     audit_context() 非空才 audit_syscall_entry
```

注释（`:54`）要求 user dispatch 最先做，因为被拦下的调用其 ABI 对其它挂钩没有意义。注释（`:73`）要求 seccomp 在 ptrace 之后，这样跟踪器改过的号和参数才会被过滤器看见。

返回 `-1` 时 `do_syscall_64` 跳过 `x64_sys_call`，`ax` 保持入口写下的 `-ENOSYS`，除非挂钩自己已经改过 `ax`。user dispatch 会 `syscall_rollback` 把 `ax` 设回 `orig_ax`，见 [09](./09-hooks.md)。

seccomp 过滤器怎么跑在 [containerization/10](../containerization/10-seccomp-caps-lsm.md)。本章只保留「这一拍被叫到」。

---

## 4.3 退出：先系统调用，再 TIF

```c
/* kernel/entry/common.c:302 */
__visible noinstr void syscall_exit_to_user_mode(struct pt_regs *regs)
{
	instrumentation_begin();
	__syscall_exit_to_user_mode_work(regs);   /* :290 */
	instrumentation_end();
	__exit_to_user_mode();                    /* :136  RCU 回到用户、lockdep */
}
```

`__syscall_exit_to_user_mode_work` 是两步：`syscall_exit_to_user_mode_prepare`（中断仍开着，`:267`），然后关中断，再 `exit_to_user_mode_prepare`（`:202`）。

准备函数先 `rseq_syscall(regs)`（`:279`），再在 `work & SYSCALL_WORK_EXIT` 时调用 `syscall_exit_work`（`:236`）：

```text
SYSCALL_WORK_SYSCALL_USER_DISPATCH 且 current->syscall_dispatch.on_dispatch
    清掉 on_dispatch，直接返回（入口没跑过的跟踪器，退出也不跑）
audit_syscall_exit(regs)
SYSCALL_WORK_SYSCALL_TRACEPOINT → trace_sys_exit
report_single_step：
    EMU 置位则不报告单步
    否则看 SYSCALL_WORK_SYSCALL_EXIT_TRAP
单步或 SYSCALL_WORK_SYSCALL_TRACE
    → ptrace_report_syscall_exit(regs, step)
```

`audit_syscall_exit`（`include/linux/audit.h:352`）自己再看 `audit_context()`。

然后 `exit_to_user_mode_prepare` 在中断关闭时读 `read_thread_flags()`。和 `EXIT_TO_USER_MODE_WORK` 有交集就进循环（`:156`）：

```text
while (ti_work & EXIT_TO_USER_MODE_WORK) {
    开中断
    _TIF_NEED_RESCHED      → schedule()
    _TIF_UPROBE            → uprobe_notify_resume(regs)
    _TIF_PATCH_PENDING     → klp_update_patch_state(current)
    _TIF_SIGPENDING 或 _TIF_NOTIFY_SIGNAL
                           → arch_do_signal_or_restart(regs)
    _TIF_NOTIFY_RESUME     → resume_user_mode_work(regs)
    arch_exit_to_user_mode_work(regs, ti_work)
    关中断，tick_nohz_user_enter_prepare()
    ti_work = read_thread_flags()          /* 上面可能又挂上新工作 */
}
arch_exit_to_user_mode_prepare(regs, ti_work)
```

`arch_do_signal_or_restart` 的 x86 实现在 `arch/x86/kernel/signal.c:306`。弱定义（`kernel/entry/common.c:154`）是空函数，x86 提供了强符号。信号怎么选、处理函数怎么上栈在 [process-management/09](../process-management/09-signals.md)；返回值怎么变成重做或 `-EINTR` 在 [08](./08-restart.md)。

循环退出后，`arch_exit_to_user_mode_prepare`（`arch/x86/include/asm/entry-common.h:47`）还做这几件和系统调用、中断共用的事：`TIF_USER_RETURN_NOTIFY` 时触发 user-return notifier，`TIF_IO_BITMAP` 时更新 IO 位图，需要的话 `switch_fpu_return()`，清掉 `TS_COMPAT`，并 `choose_random_kstack_offset(rdtsc())`。清 `TS_COMPAT` 必须在信号处理之后，注释（`:64`）写明重启的 compat 修正还要读这个位。

---

## 4.4 和中断出口并排

```c
/* kernel/entry/common.c:315 */
noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	instrumentation_begin();
	exit_to_user_mode_prepare(regs);    /* 只有 TIF 循环 */
	instrumentation_end();
	__exit_to_user_mode();
}
```

中断出口没有 `syscall_exit_work`，也就没有 `audit_syscall_exit`、`trace_sys_exit`、`ptrace_report_syscall_exit`。`NEED_RESCHED` 和 `SIGPENDING` 两条公共：从中断返回用户态同样会 `schedule` 和 `arch_do_signal_or_restart`。设备中断把 `orig_ax` 设成 `-1` 之后，重启逻辑里的 `syscall_get_nr() != -1` 不成立，不会把一次中断的 `ip` 减 2。

---

## 4.5 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/thread_info.h` | `enum syscall_work_bit` | 41 | `SYSCALL_WORK_*` 的位号 |
| `include/linux/entry-common.h` | `SYSCALL_WORK_ENTER` | 39 | 进入时要看的掩码 |
| `include/linux/entry-common.h` | `EXIT_TO_USER_MODE_WORK` | 60 | TIF 循环的掩码 |
| `kernel/entry/common.c` | `syscall_trace_enter` | 48 | user dispatch → ptrace → seccomp → tracepoint → audit |
| `kernel/entry/common.c` | `syscall_enter_from_user_mode` | 113 | 上下文 + 开中断 + 进入工作 |
| `kernel/entry/common.c` | `syscall_exit_work` | 236 | audit / trace_sys_exit / ptrace 退出 |
| `kernel/entry/common.c` | `exit_to_user_mode_loop` | 156 | 调度、信号、livepatch、resume |
| `kernel/entry/common.c` | `irqentry_exit_to_user_mode` | 315 | 中断回用户态，无 syscall_exit_work |
| `arch/x86/include/asm/entry-common.h` | `arch_exit_to_user_mode_prepare` | 47 | 清 `TS_COMPAT`，选下一次栈偏移 |

---

**导航**：[← 分派](./03-dispatch-and-define.md) · [目录](./README.md) · [usercopy →](./05-usercopy.md)
