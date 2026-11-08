# 系统调用（x86-64）

> 内核版本：**v6.6.160**（stable 6.6 LTS）。下文路径、函数、结构体字段与行号对照该版本核对（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.113+**（`uname -r`）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：用户态执行 `syscall` 指令进内核。x86-64 上 `MSR_LSTAR` 指向 `entry_SYSCALL_64`，经 `do_syscall_64` / `x64_sys_call` 落到 `__x64_sys_*`；能在用户态算完的时钟走 vDSO，不进这条路径。

面向 **C 语言新手**：系统调用号和 6 个参数都放在寄存器里。内核在自己的栈上搭一份 `struct pt_regs`，后面的 C 代码只读这份快照。返回值写回 `pt_regs->ax`，`SYSRET` 时它回到用户态的 `rax`。

## 阅读顺序

```text
01 总览与 ABI
  → 02 entry_SYSCALL_64 与 SYSRET
  → 03 x64_sys_call 与 SYSCALL_DEFINE
  → 04 进入/退出工作（SYSCALL_WORK 与 TIF）
  → 05 copy_*_user 与 SMAP
  → 06 vDSO / vvar
  → 07 ia32 兼容入口（薄）
  → 08 -ERESTART* 与 restart_block
  → 09 seccomp / ptrace / tracepoint 挂钩面
  → 10 观测 → 11 开销与配方
```

热路径记一句：`do_syscall_x64` 调用 `x64_sys_call(regs, unr)`。`sys_call_table[]` 仍在，`arch/x86/entry/syscall_64.c:14` 的注释写明它留给 `kernel/trace/trace_syscalls.c` 取函数地址。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | ABI、`pt_regs`、总图、与邻章的边界 | 中 |
| [02-x86-entry.md](./02-x86-entry.md) | `syscall_init`、`entry_SYSCALL_64`、`pcpu_hot.top_of_stack`、SYSRET 资格 | **厚** |
| [03-dispatch-and-define.md](./03-dispatch-and-define.md) | `do_syscall_64`、`x64_sys_call` 开关、tbl、`__x64_sys_*` | **厚** |
| [04-enter-exit-work.md](./04-enter-exit-work.md) | `SYSCALL_WORK_*`、`syscall_trace_enter` 顺序、与 IRQ 出口对照 | **厚** |
| [05-usercopy.md](./05-usercopy.md) | `access_ok`、`copy_from_user`、`stac`/`clac`、extable | 中 |
| [06-vdso-and-vvar.md](./06-vdso-and-vvar.md) | `map_vdso`、`struct vdso_data`、`__vdso_clock_gettime` | **厚** |
| [07-compat.md](./07-compat.md) | `entry_SYSCALL_compat`、`int $0x80`、`TS_COMPAT` | **薄** |
| [08-restart.md](./08-restart.md) | 四种 `-ERESTART*`、`arch_do_signal_or_restart`、`restart_block` | 中偏厚 |
| [09-hooks.md](./09-hooks.md) | seccomp / ptrace / tracepoint / audit / user dispatch 的入口面 | 中 |
| [10-observability.md](./10-observability.md) | maps、`/proc/<pid>/syscall`、tracepoint 名单 | **厚** |
| [11-perf-analysis.md](./11-perf-analysis.md) | vDSO 与真陷入的采样、strace/perf/PTI 配方 | **厚** |
| [demos/](./demos/) | 三个用户态 C 程序 | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `entry_SYSCALL_64` 怎么换栈、怎么 `SYSRET` | IDT、`common_interrupt`、中断栈、`irqentry_enter`：[interrupts/02-x86-entry.md](../interrupts/02-x86-entry.md) |
| 返回用户态时谁调用 `arch_do_signal_or_restart`，四种 `-ERESTART*` 怎么改 `ax`/`ip` | 信号怎么投递、`get_signal`、`sighand`：[process-management/09-signals.md](../process-management/09-signals.md) |
| `SYSCALL_WORK_SECCOMP` 置位后调用 `__secure_computing(NULL)` | 过滤器对象、`RET_*`、装载：[containerization/10-seccomp-caps-lsm.md](../containerization/10-seccomp-caps-lsm.md) |
| `copy_from_user` / `copy_to_user` 在系统调用里搬字节，`stac` 打开 SMAP | 缺页本身：[memory-management/07-page-fault.md](../memory-management/07-page-fault.md)。要拿到 `struct page` 并钉住：[memory-management/07b-gup-and-pin.md](../memory-management/07b-gup-and-pin.md) |
| `restart_block.futex` 里记下的等待参数 | futex 的 WAIT/WAKE：[locks-and-ds/09-futex.md](../locks-and-ds/09-futex.md) |
| time namespace 让 vvar 缺页走到 `find_timens_vvar_page` | 偏移怎么写：[containerization/04-pid-user-uts-ipc-time-ns.md](../containerization/04-pid-user-uts-ipc-time-ns.md) |
| 用户态怎样进到 `sys_read` 的桩 | `vfs_read` 往下：[vfs/05-read-write.md](../vfs/05-read-write.md) |
| `epoll_*`、`pipe2`、`eventfd2` 从 `x64_sys_call` 落到 `fs/eventpoll.c` 等 | 对象与唤醒：[ipc/](../ipc/README.md) |

本章是用户态进内核的封口。具体业务系统调用（VFS、网络、futex 哈希桶）从对应章节接着读。ptrace 只写到 `PTRACE_SYSCALL` 置上 `SYSCALL_WORK_SYSCALL_TRACE`。arm64 的 `svc` 入口不在这里展开。

## 源码地图

| 路径 | 角色 |
|------|------|
| `arch/x86/entry/entry_64.S` | `entry_SYSCALL_64`、SYSRET/IRET 返回 |
| `arch/x86/entry/calling.h` | `PUSH_AND_CLEAR_REGS`、`SWITCH_TO_*_CR3` |
| `arch/x86/entry/common.c` | `do_syscall_64`、32 位入口 |
| `arch/x86/entry/syscall_64.c` | `x64_sys_call`、`sys_call_table` |
| `arch/x86/entry/syscalls/syscall_64.tbl` | 号与符号 |
| `arch/x86/include/asm/syscall_wrapper.h` | `__x64_sys_*` 从 `pt_regs` 解参数 |
| `arch/x86/kernel/cpu/common.c` | `syscall_init`（`MSR_STAR` / `LSTAR` / `SYSCALL_MASK`） |
| `arch/x86/include/asm/current.h` | `struct pcpu_hot` |
| `arch/x86/kernel/asm-offsets.c` | `X86_top_of_stack` → `pcpu_hot.top_of_stack` |
| `include/linux/entry-common.h`、`kernel/entry/common.c` | 进出用户态公共层 |
| `include/linux/thread_info.h` | `SYSCALL_WORK_*` |
| `arch/x86/include/asm/thread_info.h` | `thread_info.syscall_work` 与 `_TIF_*` |
| `include/linux/syscalls.h` | `SYSCALL_DEFINE*` |
| `arch/x86/include/asm/uaccess.h`、`uaccess_64.h`、`arch/x86/lib/copy_user_64.S` | usercopy / SMAP / extable |
| `arch/x86/entry/vdso/`、`include/vdso/datapage.h` | vDSO / vvar |
| `include/linux/restart_block.h`、`arch/x86/kernel/signal.c` | 重启 |
| `include/trace/events/syscalls.h` | `raw_syscalls:sys_enter` / `sys_exit` |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.113+，KVM 客户机，4 核，`smap` 在 flags 里，`/sys/devices/system/cpu/vulnerabilities/meltdown` 为 `Not affected`）：[demos/](./demos/) 三个程序的 stdout 贴在 [06](./06-vdso-and-vvar.md)、[05](./05-usercopy.md)、[08](./08-restart.md)、[11](./11-perf-analysis.md)。`/proc/self/maps` 的 `[vvar]` / `[vdso]` / `[vsyscall]` 和一次阻塞 `read` 的 `/proc/<pid>/syscall` 贴在 [10](./10-observability.md)。
- **RECIPE**：`strace -c`、`perf trace`、`bpftrace` 的 `raw_syscalls:sys_enter`、PTI 开/关对比。本环境没有 `strace` / `perf` / `bpftrace`，非 root 读 `/sys/kernel/tracing/events/raw_syscalls/` 是 `Permission denied`。本机 Meltdown 不受影响，`SWITCH_TO_KERNEL_CR3` 在这种 CPU 上是 `ALTERNATIVE` 直接跳过，这里不编一份 PTI 成本数字。

## 未覆盖

- 完整 ptrace 协议、完整 audit 子系统、完整 LSM 策略语言
- 怎样给主线新增一个系统调用（tbl 的列格式在 [03](./03-dispatch-and-define.md) 点到）
- 完整 ia32 系统调用表、x32 ABI 的参数细节
- arm64 / 其它架构的陷入指令

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
