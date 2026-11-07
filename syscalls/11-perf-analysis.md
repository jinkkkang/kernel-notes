# 11. 开销与配方

**结论**：一次什么都不做的 `getpid` 仍要换栈、`swapgs`、搭 `pt_regs`，再从 `sysretq` 回去。`clock_gettime` 能走 vDSO 时，本机测到的周期大约是强制 `syscall` 指令的四分之一。`strace` 会把 ptrace 挂钩加进每一次调用。PTI 开/关的差值在这台机器上没有测，标成 RECIPE。

> 下面带 `cycles_per_call` 的数字 sampled on **6.12.113+**，程序是 [demos/01-vdso-vs-syscall.c](./demos/01-vdso-vs-syscall.c)，`gcc -O2`，每次循环 200000 次，用 `rdtsc` 包住整段再除。这是墙上周期，含流水线和中断，不是 v6.6.160 的指令条数。

---

## 11.1 一次最小调用里有什么

`syscall(__NR_getpid)` 仍走完 [02](./02-x86-entry.md) 和 [03](./03-dispatch-and-define.md)：

```text
swapgs
用户 RSP 存进 TSS.sp2，RSP ← pcpu_hot.top_of_stack
搭 pt_regs，ax 槽 = -ENOSYS
IBRS_ENTER / UNTRAIN_RET / CLEAR_BRANCH_HISTORY   特性位关闭时是空的 ALTERNATIVE
do_syscall_64
  add_random_kstack_offset                        静态键关闭时是空宏
  syscall_enter_from_user_mode                    没有 SYSCALL_WORK 时不进 syscall_trace_enter
  x64_sys_call → __x64_sys_getpid → task_tgid_vnr
  syscall_exit_to_user_mode                       没有 SYSCALL_WORK_EXIT 时不做 audit/ptrace
SYSRET 资格检查
蹦床栈上 sysretq
```

进入工作的空转仍然要读一次 `thread_info.syscall_work` 并和 `SYSCALL_WORK_ENTER` 相与（`kernel/entry/common.c:100`）。全零就返回。退出侧同样先读 `syscall_work`（`:269`）。`exit_to_user_mode_prepare` 还要读 TIF，没有 `NEED_RESCHED` / `SIGPENDING` 时不进循环。

这些 `ALTERNATIVE` 和静态键在不同 CPU、不同 config 上是不同的指令流。本机 Meltdown 不受影响，不要把 PTI 的 `mov %cr3` 算进下面的周期里，除非单独测过。

---

## 11.2 vDSO 与真陷入

两次运行，sampled on 6.12.113+：

```text
uname.release=6.12.113+ machine=x86_64
libc_clock_gettime n=200000 last_rc=0 cycles_per_call=64.9
syscall_clock_gettime n=200000 last_rc=0 errno=0 cycles_per_call=279.9
syscall_getpid n=200000 cycles_per_call=166.7

libc_clock_gettime n=200000 last_rc=0 cycles_per_call=64.9
syscall_clock_gettime n=200000 last_rc=0 errno=0 cycles_per_call=277.7
syscall_getpid n=200000 cycles_per_call=166.7
```

`libc_clock_gettime` 调的是 glibc 的 `clock_gettime(CLOCK_MONOTONIC)`。x86-64 上它进 [06](./06-vdso-and-vvar.md) 的 `__vdso_clock_gettime`，不执行 `syscall`。`syscall_clock_gettime` 是 `syscall(__NR_clock_gettime, ...)`，号 228，一定会陷入，并在内核里 `put_timespec64`。`syscall_getpid` 是 `syscall(__NR_getpid)`，号 39。程序没有用 libc 的 `getpid()`：glibc 可以在用户态记住 pid，那一次测量就不再是系统调用。

读这三行：

| 样本 | 大约周期/次 | 在干什么 |
|---|---|---|
| libc `clock_gettime` | 65 | vDSO：读 vvar、读 TSC、顺序锁 |
| `syscall` `getpid` | 167 | 陷入 + `task_tgid_vnr` + SYSRET |
| `syscall` `clock_gettime` | 278–280 | 陷入 + 读时钟 + 拷回用户缓冲 |

`clock_gettime` 的陷入比 `getpid` 贵，差值来自内核里的时钟读取和 `copy_to_user`，不是入口桩更长。两次运行的 `getpid` 都是 166.7，`clock_gettime` 的 libc 路径都是 64.9；`syscall` 的时钟路径差了约 2 个周期。这是两次样本，不是误差棒。

---

## 11.3 `strace` 放大，tracepoint 不改控制流

```bash
# RECIPE：本环境没有 strace / perf / bpftrace，下面没有输出
strace -c ./01-vdso-vs-syscall
perf trace -s -- ./01-vdso-vs-syscall
bpftrace -e 'tracepoint:raw_syscalls:sys_enter /pid == getpid()/ { @[args->id] = count(); }'
```

`strace` 默认 `PTRACE_SYSCALL`。被跟踪任务的 `SYSCALL_WORK_SYSCALL_TRACE` 被置上（`kernel/ptrace.c:853`），每次进入和退出都 `ptrace_report_syscall_*`，任务停下来等跟踪器读寄存器。`-c` 打出来的时间含这个来回。它适合回答「调用了哪些号、各多少次」，不适合回答「`getpid` 本身多少周期」。

`perf trace` 和 `bpftrace` 的 `raw_syscalls:sys_enter` 挂在 `trace_sys_enter`（[09](./09-hooks.md)）。要这个事件发生，任务还得有 `SYSCALL_WORK_SYSCALL_TRACEPOINT`。打开事件会给匹配的任务加上这一位，比 ptrace 停任务便宜，但已经不是「`syscall_work` 全零」的那条热路径。和 11.2 的 rdtsc 对比时分开写。

libc 的 `clock_gettime` 不产生 `raw_syscalls:sys_enter`。画像里看不到它，只说明没有陷入，不说明没有人在读时钟。要看 vDSO，用 11.2 这种用户态计时，或者对 `__vdso_clock_gettime` 做 uprobe（同样是 RECIPE，本环境没跑）。

---

## 11.4 seccomp、SMAP、PTI

seccomp 过滤器挂上之后，`SYSCALL_WORK_SECCOMP` 置位，每次进入都跑 `__secure_computing`（[containerization/10](../containerization/10-seccomp-caps-lsm.md)）。热路径多的是一次 BPF 解释或 JIT 调用，外加 [04](./04-enter-exit-work.md) 里「有任意进入位就走 `syscall_trace_enter`」的那条分支。本章不编「多一层 BPF 值多少纳秒」。容器章已经有过滤器对象，数字要在装了具体 filter 的进程上测。

SMAP 的 `stac`/`clac` 只包住 `copy_user_generic`（[05](./05-usercopy.md)）。`getpid` 不碰用户指针，没有这两条指令。不要把 SMAP 的成本摊进 11.2 的 `getpid` 数字。本机有 `smap`，但没有单独开关它再测一轮。

PTI：`SWITCH_TO_KERNEL_CR3` / `SWITCH_TO_USER_CR3_STACK` 在 `X86_FEATURE_PTI` 打开时各做一次 `mov %cr3`（`arch/x86/entry/calling.h:169`、`:214`）。本机 `/sys/devices/system/cpu/vulnerabilities/meltdown` 是 `Not affected`。这种 CPU 上该特性位通常关闭，入口宏被 `ALTERNATIVE` 换成跳过。

```bash
# RECIPE：换一台 Meltdown 受影响的机器，或在启动参数里显式打开/关闭 PTI 后再跑 demo 01
# 本机没有做这个对比，不要引用上面的 167 周期当作「含 PTI」或「不含 PTI」的证明
cat /sys/devices/system/cpu/vulnerabilities/meltdown
grep -o pti /proc/cpuinfo || true
```

`/proc/cpuinfo` 的 flags 里没有单独搜到可当开关用的结论时，以 vulnerabilities 文件和启动参数为准，不要猜 CR3 写了没有。

---

## 11.5 症状

**`%sys` 高，调用都很短。** `/proc/<pid>/syscall` 或 RECIPE 的 `raw_syscalls:sys_enter` 直方图会集中在 `read`/`write`/`futex`/`clock_gettime` 这类号上，单次 `ret` 很小。入口成本（11.1）被次数放大。先把能留在用户态的调用留在 vDSO，再谈批处理（`io_uring`、`sendmmsg` 属于别的章）。`strace` 开着的时候先关掉它，确认 `%sys` 不是跟踪器造成的。

**`clock_gettime` 比预期慢。** 对照 11.2：65 周期量级是 vDSO，280 周期量级是真陷入。慢的常见原因：

- 代码用了 `syscall` 指令或 `syscall()` 包装，绕过了 vDSO
- 时钟 id 不在 `VDSO_HRES` / `VDSO_COARSE` / `VDSO_RAW` 里，`__cvdso_clock_gettime_common` 返回 `-1`，落到 `clock_gettime_fallback`（`lib/vdso/gettimeofday.c:261`）
- 进程在 time namespace 里，`do_hres` 走 `do_hres_timens`（[06](./06-vdso-and-vvar.md)）

maps 里没有 `[vdso]` 时，libc 只能陷入。先看 `vdso=` 启动参数（[10](./10-observability.md)）。

**坏指针把调用变成失败，看起来像「系统调用很慢」。** `(void *)1` 这种地址在本机上立刻 `-EFAULT`（[05](./05-usercopy.md) 的 demo），慢的是缺页修复和异常表，不是 `x64_sys_call` 的 `switch`。先看 errno 是不是 `EFAULT`。

---

## 11.6 关键对照

| 文件 | 符号 | 行号 | 和成本的关系 |
|---|---|---|---|
| `arch/x86/entry/entry_64.S` | `entry_SYSCALL_64` | 87 | 换栈与 SYSRET 资格 |
| `kernel/entry/common.c` | `__syscall_enter_from_user_work` | 97 | `syscall_work` 全零则直接返回 |
| `arch/x86/entry/syscall_64.c` | `x64_sys_call` | 27 | 号到函数 |
| `lib/vdso/gettimeofday.c` | `__cvdso_clock_gettime` | 266 | 不陷入的时钟 |
| `lib/vdso/gettimeofday.c` | `__cvdso_clock_gettime_data` | 261 | 失败时调用 `clock_gettime_fallback` |
| `arch/x86/include/asm/vdso/gettimeofday.h` | `clock_gettime_fallback` | 71 | 64 位 vDSO 里的 `syscall` 指令 |
| `kernel/ptrace.c` | `ptrace_resume` | 853 | `strace` 置上的那一位 |

---

**导航**：[← 观测](./10-observability.md) · [目录](./README.md)
