# 7. ia32 与兼容入口

**结论**：64 位内核仍要接住 32 位二进制。它们用 `int $0x80`、`sysenter`，或兼容的 `syscall`。这些入口最后都进 `do_syscall_32_irqs_on`，由 `ia32_sys_call` 的 `switch` 分派，并在 `thread_info.status` 上置 `TS_COMPAT`。`syscall_get_arguments` 看到这个位就改读 `ebx`…`ebp`。

> 汇编在 `arch/x86/entry/entry_64_compat.S`。C 入口在 `arch/x86/entry/common.c` 后半。本章只点名，不展开整张 ia32 表，也不展开 x32 的参数细节。

64 位主线在 [02](./02-x86-entry.md) 和 [03](./03-dispatch-and-define.md)。

---

## 7.1 三条路

| 入口 | 位置 | 落到 |
|---|---|---|
| `entry_SYSCALL_compat` | `entry_64_compat.S:187` | `do_fast_syscall_32`（`:222`） |
| `entry_SYSENTER_compat` | `:51` | 同样进入 32 位快路径 |
| `int80_emulation` | `:296` | `CLEAR_BRANCH_HISTORY` 之后 `jmp do_int80_emulation` |

`syscall_init` 在 `CONFIG_IA32_EMULATION` 下把 `MSR_CSTAR` 设成 `entry_SYSCALL_compat`，并把 `MSR_IA32_SYSENTER_EIP` 设成 `entry_SYSENTER_compat`（`arch/x86/kernel/cpu/common.c:2204`）。没开模拟时 CSTAR 指向 `entry_SYSCALL32_ignore`（`entry_64.S:1523`），`eax = -ENOSYS` 后 `sysretl`。

IDT 里 `IA32_SYSCALL_VECTOR` 是 `0x80`（`arch/x86/include/asm/irq_vectors.h:38`）。`CONFIG_IA32_EMULATION` 下 `idt.c:120` 把它装成 `asm_int80_emulation`。

`do_int80_emulation`（`common.c:157`）确认来自用户态，`add_random_kstack_offset`，把 `regs->ax` 的低 32 位写入 `orig_ax`，再把 `ax` 设成 `-ENOSYS`，然后 `syscall_32_enter` → 开中断 → `syscall_enter_from_user_mode_work` → `do_syscall_32_irqs_on`。

`do_fast_syscall_32`（`:276`）先把 `regs->ip` 改成 32 位 vDSO 里的 `int $0x80` 着陆点。注释（`:288`）说明 SYSENTER 会丢掉 EIP，SYSCALL32 也需要这个着陆点，这样以后 `regs->ip -= 2` 才能落回一条 `int $0x80`。然后调用 `__do_fast_syscall_32`（`:228`）。返回值 1 表示可以 SYSRETL，0 表示调用方改走 IRET。

---

## 7.2 `TS_COMPAT` 与参数寄存器

```c
/* arch/x86/entry/common.c:87 */
static __always_inline int syscall_32_enter(struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_IA32_EMULATION))
		current_thread_info()->status |= TS_COMPAT;
	return (int)regs->orig_ax;
}
```

`TS_COMPAT` 是 `0x0002`（`arch/x86/include/asm/thread_info.h:217`）。`in_ia32_syscall()`（`:231`）就是看这个位。退出用户态时 `arch_exit_to_user_mode_prepare` 把它清掉，而且必须在信号处理之后（[04](./04-enter-exit-work.md)）。

```c
/* arch/x86/include/asm/syscall.h:100  CONFIG_X86_64 */
if (task->thread_info.status & TS_COMPAT) {
	args[0..5] = bx, cx, dx, si, di, bp;
} else {
	args[0..5] = di, si, dx, r10, r8, r9;
}
```

`ia32_sys_call`（`arch/x86/entry/syscall_32.c:36`）和 64 位一样是 `switch`，`default` 返回 `__ia32_sys_ni_syscall`。文件头注释（`:22`）同样写明 `sys_call_table[]` 只在 `CONFIG_X86_32` 下生成，给 tracing。64 位内核上的 ia32 热路径不靠那张表。

`do_syscall_32_irqs_on`（`common.c:102`）在 `unr < IA32_NR_syscalls` 时调用它，否则在号不是 `-1` 时调用 `__ia32_sys_ni_syscall`。`IA32_NR_syscalls` 是 `__NR_ia32_syscalls`（`asm/unistd.h:31`），由 `syscall_32.tbl` 生成。

重启码在 compat 下要换号：`get_nr_restart_syscall`（`arch/x86/kernel/signal.c:288`）若 `restart_block.arch_data & TS_COMPAT` 就返回 `__NR_ia32_restart_syscall`。32 位表里 `restart_syscall` 是 0 号（`arch/x86/entry/syscalls/syscall_32.tbl:14`），64 位表里是 219（[08](./08-restart.md)）。

---

## 7.3 x32 只点一句

`do_syscall_64` 在原生 64 位开关失败后还会试 `do_syscall_x32`（`common.c:76`）。号减去 `__X32_SYSCALL_BIT`（`0x40000000`）再进 `x32_sys_call`。参数顺序仍是 64 位那套 `r10`，桩用 `SC_X86_64_REGS_TO_ARGS`（`syscall_wrapper.h:174` 一带的 x32 compat 桩）。本章不展开哪些调用要 `compat_sys_*`。

---

## 7.4 关键符号

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/entry/entry_64_compat.S` | `entry_SYSCALL_compat` | 187 | 32 位 `syscall` |
| `arch/x86/entry/entry_64_compat.S` | `entry_SYSENTER_compat` | 51 | `sysenter` |
| `arch/x86/entry/entry_64_compat.S` | `int80_emulation` | 296 | `int $0x80` 的汇编头 |
| `arch/x86/entry/common.c` | `do_int80_emulation` | 157 | 建立 `orig_ax` 后走 32 位分派 |
| `arch/x86/entry/common.c` | `do_fast_syscall_32` | 276 | SYSENTER/SYSCALL32 的 C 入口 |
| `arch/x86/entry/syscall_32.c` | `ia32_sys_call` | 36 | 32 位 `switch` |
| `arch/x86/include/asm/thread_info.h` | `TS_COMPAT` | 217 | 32 位系统调用进行中 |

---

**导航**：[← vDSO](./06-vdso-and-vvar.md) · [目录](./README.md) · [重启 →](./08-restart.md)
