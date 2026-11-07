# 9. 拦截挂钩面

**结论**：观察或拦截发生在 `syscall_trace_enter` / `syscall_exit_work` 里，顺序是源码写死的。本章只写「哪一位被置上、调用哪个函数」。seccomp 的 BPF 与 `RET_*` 在容器章；信号递送在进程章；`-ERESTART*` 在 [08](./08-restart.md)。

> 顺序的实现是 `kernel/entry/common.c:48`。进入掩码见 [04](./04-enter-exit-work.md)。

---

## 9.1 时间线

```text
用户 syscall
  entry_SYSCALL_64          中断关，ax = -ENOSYS
  syscall_enter_from_user_mode
    开中断
    1 user dispatch          可能 rollback 并返回 -1
    2 ptrace 进入            TRACE 或 EMU
    3 __secure_computing     SECCOMP
    4 trace_sys_enter        TRACEPOINT
    5 audit_syscall_entry    有 audit context 才真正记录
  x64_sys_call               返回 -1 则跳过
  syscall_exit_work          中断开
    audit_syscall_exit
    trace_sys_exit
    ptrace 退出
  exit_to_user_mode_loop     TIF：调度、信号、NOTIFY_RESUME
  SYSRET 或 IRET
```

user dispatch 若决定拦下，入口返回 `-1`，`syscall_exit_work` 看到 `on_dispatch` 就不再调用后面的 audit / tracepoint / ptrace（`common.c:246`）。

---

## 9.2 seccomp

`SYSCALL_WORK_SECCOMP` 置位时，`syscall_trace_enter` 调用 `__secure_computing(NULL)`（`kernel/entry/common.c:75`）。声明在 `include/linux/seccomp.h:44`，包在 `CONFIG_HAVE_ARCH_SECCOMP_FILTER` 里。`secure_computing()`（`:45`）是「先 `test_syscall_work(SECCOMP)` 再调用」的包装；热路径不走这个包装，而是已经看过位之后直接调 `__secure_computing`。

返回 `-1` 则这次系统调用不做。过滤器链、`SECCOMP_RET_*`、runc 怎样装上过滤器，见 [containerization/10-seccomp-caps-lsm.md](../containerization/10-seccomp-caps-lsm.md)。

---

## 9.3 ptrace

`PTRACE_SYSCALL` 是 24（`include/uapi/linux/ptrace.h:25`）。`ptrace_resume` 在请求等于它时 `set_task_syscall_work(child, SYSCALL_TRACE)`，其它恢复请求则清掉这一位（`kernel/ptrace.c:853`）。

`PTRACE_SYSEMU` 是 31，`PTRACE_SYSEMU_SINGLESTEP` 是 32（`arch/x86/include/uapi/asm/ptrace-abi.h:85`）。这两个请求置 `SYSCALL_EMU`（`ptrace.c:860`）。

进入时 `ptrace_report_syscall_entry`（`include/linux/ptrace.h:449`）调用 `ptrace_report_syscall(PTRACE_EVENTMSG_SYSCALL_ENTRY)`。跟踪器可以改 `pt_regs` 里的号和参数。返回非 0，或者 `SYSCALL_EMU` 仍置位，`syscall_trace_enter` 就返回 `-1`，系统调用体不执行。EMU 的含义是：报告一次进入，然后当作这次调用没发生。

退出时 `ptrace_report_syscall_exit`（`:472`）在 `step` 非 0 时改走 `user_single_step_report`，否则报告 `PTRACE_EVENTMSG_SYSCALL_EXIT`。`report_single_step`（`common.c:228`）在 EMU 置位时直接返回 false：进入已经报告过，退出不再为了单步重复一次，除非没有 EMU 且 `SYSCALL_WORK_SYSCALL_EXIT_TRAP` 置位。

完整的 `PTRACE_PEEK` / `POKE`、停止通知和信号注入不在本章。

---

## 9.4 tracepoint

`include/trace/events/syscalls.h` 把跟踪系统定成 `raw_syscalls`（`:3`），文件名宏是 `syscalls`（`:5`），所以调试目录里的名字是 `raw_syscalls/sys_enter` 和 `sys_exit`。

`TRACE_EVENT_FN(sys_enter)`（`:18`）的字段是 `id` 和 6 个 `args`。赋值时调用 `syscall_get_arguments`。打印格式是 `NR %ld (%lx, %lx, %lx, %lx, %lx, %lx)`。`sys_exit`（`:44`）的字段是 `id`（再读 `syscall_get_nr`）和 `ret`。两个事件都标了 `TRACE_EVENT_FL_CAP_ANY`（`:42`、`:66`）。

每个 `SYSCALL_DEFINE*` 还会通过 `SYSCALL_METADATA`（`include/linux/syscalls.h:173`）生成 `sys_enter_<name>` / `sys_exit_<name>`。那是另一组事件，名字里带系统调用；`raw_syscalls` 是所有号共用的两个点。`trace_sys_enter` 在 seccomp 之后，所以被 seccomp 拒绝、函数返回 `-1` 的调用不会进这个 tracepoint。

本环境非 root 列 `/sys/kernel/tracing/events/raw_syscalls/` 得到 `Permission denied`。打开方式写在 [10](./10-observability.md)，标成 RECIPE。

---

## 9.5 audit

`syscall_enter_audit`（`kernel/entry/common.c:38`）在 `audit_context()` 非空时取出 6 个参数，但 `audit_syscall_entry` 只记录前 4 个（`:44`）。`audit_syscall_entry`（`include/linux/audit.h:345`）内部还会再看一次 `audit_context()`。退出侧 `audit_syscall_exit(regs)`（`common.c:253`）用 `is_syscall_success` 和 `regs_return_value` 交给 `__audit_syscall_exit`（`audit.h:352`）。

`SYSCALL_WORK_SYSCALL_AUDIT` 在进入和退出掩码里都有。只要任意进入位把 `syscall_trace_enter` 拉进来，函数末尾都会尝试 audit；真正写记录仍取决于有没有 audit context。auditd 的规则语言不在本章。

---

## 9.6 syscall user dispatch

`struct syscall_user_dispatch`（`include/linux/syscall_user_dispatch.h:12`）挂在 `task_struct.syscall_dispatch`（`include/linux/sched.h:1133`）：

```c
struct syscall_user_dispatch {
	char __user  *selector;   /* 用户内存里的一个字节 */
	unsigned long offset;     /* 放行区间的起点 */
	unsigned long len;
	bool          on_dispatch;
};
```

`syscall_user_dispatch`（`kernel/entry/syscall_user_dispatch.c:35`）：

```text
指令指针落在 [offset, offset+len) → 返回 false，系统调用照常做
arch_syscall_is_vdso_sigreturn → 返回 false
selector 存在：
    __get_user 失败 → SIGSEGV，返回 true
    字节 == SYSCALL_DISPATCH_FILTER_ALLOW → 返回 false
    字节不是 BLOCK → SIGSYS，返回 true
on_dispatch = true
syscall_rollback（ax = orig_ax）
trigger_sigsys（SIGSYS / SYS_USER_DISPATCH）
返回 true
```

返回 true 时 `syscall_trace_enter` 返回 `-1`。这是给一部分用户代码「禁止直接 syscall、改走模拟器」用的，Wine 一类会用。选择器的装载接口是 `set_syscall_user_dispatch`（`:121`），本章不展开 prctl 参数。

---

## 9.7 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `kernel/entry/common.c` | `syscall_trace_enter` | 48 | 五步进入顺序 |
| `kernel/entry/common.c` | `syscall_exit_work` | 236 | audit、trace、ptrace 退出 |
| `include/linux/seccomp.h` | `__secure_computing` | 44 | seccomp 入口声明 |
| `kernel/ptrace.c` | `ptrace_resume` | 853 | `PTRACE_SYSCALL` 置 TRACE |
| `include/linux/ptrace.h` | `ptrace_report_syscall_entry` | 449 | 进入报告 |
| `include/trace/events/syscalls.h` | `sys_enter` / `sys_exit` | 18 / 44 | `raw_syscalls` |
| `kernel/entry/syscall_user_dispatch.c` | `syscall_user_dispatch` | 35 | 按区间和选择器拦截 |

---

**导航**：[← 重启](./08-restart.md) · [目录](./README.md) · [观测 →](./10-observability.md)
