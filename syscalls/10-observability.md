# 10. 观测

**结论**：先看 `/proc/<pid>/maps` 里有没有 `[vdso]` / `[vvar]` / `[vsyscall]`，再用 `/proc/<pid>/syscall` 看一个堵在内核里的任务正在做哪一号。跟踪事件的名字以 v6.6.160 的头文件为准。本机没有 `strace`、`perf`、`bpftrace`、`ausyscall`，那些命令是 RECIPE。

> 采样内核 **6.12.113+**。结构与事件名对照 v6.6.160。和 [interrupts/12](../interrupts/12-observability.md)、[process-management/10](../process-management/10-observability.md) 一样：能读到的贴输出，读不到的不编数字。

---

## 10.1 `/proc/self/maps` 认三段

sampled on 6.12.113+：

```text
$ grep -E 'vdso|vvar|vsyscall' /proc/self/maps
7fa929818000-7fa92981c000 r--p 00000000 00:00 0                          [vvar]
7fa92981c000-7fa92981e000 r-xp 00000000 00:00 0                          [vdso]
ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0                  [vsyscall]
```

| 看到 | 说明 |
|---|---|
| `[vvar]` 且权限是 `r--` | `map_vdso` 装的数据页，`vvar_mapping.name`（`arch/x86/entry/vdso/vma.c:214`） |
| `[vdso]` 且权限是 `r-x` | 同一函数装的代码页（`:209`）。起始地址每次进程不同 |
| `[vsyscall]` 在 `ffffffffff600000`，权限 `--xp` | `VSYSCALL_ADDR`（`arch/x86/include/uapi/asm/vsyscall.h:11`）。这一页不随机 |

没有 `[vdso]` 时先看启动参数 `vdso=`。`vdso_setup`（`vma.c:416`）把 `vdso64_enabled` 设成这个值，`arch_setup_additional_pages` 在它为 0 时直接返回（`:369`）。

---

## 10.2 号对得上哪张表

内核表是 `arch/x86/entry/syscalls/syscall_64.tbl`，列格式在 [03](./03-dispatch-and-define.md)。本机用户态头 `/usr/include/x86_64-linux-gnu/asm/unistd_64.h` 里这几项和 tbl 一致：

| 宏 | 本机头文件里的值 | tbl |
|---|---|---|
| `__NR_read` | 0 | `0 common read` |
| `__NR_getpid` | 39 | `:50` |
| `__NR_gettid` | 186 | `:197` |
| `__NR_restart_syscall` | 219 | `:230` |
| `__NR_clock_gettime` | 228 | `:239` |

`ausyscall` 不在本环境里。有这个包时，`ausyscall --dump` 或 `ausyscall getpid` 读的是 audit 的号表，用来和 tbl 对名字，不要用它的行号代替内核行号。

```bash
# RECIPE：本环境没有 ausyscall
ausyscall getpid
ausyscall --dump | head
```

---

## 10.3 `/proc/<pid>/syscall`

`fs/proc/base.c:3306` 在 `CONFIG_HAVE_ARCH_TRACEHOOK` 下登记 `"syscall"`，模式 `S_IRUSR`，`show` 是 `proc_pid_syscall`（`:672`）。同一文件 `:3653` 还有一份给另一个 pid 目录模板。

`proc_pid_syscall` `lock_trace` 之后调用 `task_current_syscall`（`lib/syscall.c:69`）：

| 打印 | 条件 | 行号 |
|---|---|---|
| `running` | `task_current_syscall` 返回非 0。函数在任务没有保持堵住时返回 `-EAGAIN`（`lib/syscall.c:67` 的注释，`:79` 和 `:85` 返回） | `base.c:685` |
| `nr sp ip` | 返回 0 且 `info.data.nr < 0`：堵在缺页或异常里，注释写明不填 `args` | `:687` |
| `nr a0 a1 a2 a3 a4 a5 sp ip` | 返回 0 且号 ≥ 0：堵在某次系统调用里，六个参数来自 `syscall_get_arguments` | `:690` |

数字都按 `0x%llx` 打，除了号本身是 `%d`。

sampled on 6.12.113+：子进程堵在空管道的 `read` 上，父进程读 `/proc/<pid>/syscall`：

```text
0 0x3 0x7f9f36b02fc0 0x1 0x7f9f36e7b088 0x0 0x0 0x7ffc990ba468 0x7f9f36d1bd31
```

号 `0` 是 `read`。前三个参数是 fd `3`、缓冲区指针、长度 `1`，后面三个参数槽是 0。倒数第二个是用户 `sp`，最后一个是当时的 `ip`。指针每次运行都变。任务已经回到用户态时，这一行会变成 `running` 或负的号，不要拿一条样本推断它「总是」停在 `read`。

---

## 10.4 tracepoint 名单

| 事件 | 头文件 | 字段 |
|---|---|---|
| `raw_syscalls:sys_enter` | `include/trace/events/syscalls.h:18` | `id`，`args[6]` |
| `raw_syscalls:sys_exit` | `:44` | `id`，`ret` |
| `syscalls:sys_enter_<name>` | `SYSCALL_METADATA`，`include/linux/syscalls.h:173` | 按该调用的参数名 |
| `syscalls:sys_exit_<name>` | 同一宏 | 返回值 |

`TRACE_SYSTEM` 是 `raw_syscalls`，`TRACE_INCLUDE_FILE` 是 `syscalls`，所以不要在目录里找一个叫 `syscalls/sys_enter` 的 raw 事件。带名字的那组才在 `syscalls/` 下。

本机：

```text
$ ls /sys/kernel/tracing/events/raw_syscalls
ls: cannot access '/sys/kernel/tracing/events/raw_syscalls': Permission denied
```

非 root，不编「事件不存在」。root 下先看目录在不在，再打开：

```bash
# RECIPE
ls /sys/kernel/tracing/events/raw_syscalls
ls /sys/kernel/tracing/events/syscalls | head
echo 1 > /sys/kernel/tracing/events/raw_syscalls/sys_enter/enable
echo 1 > /sys/kernel/tracing/tracing_on
# 做一点会陷入的事，例如 demo 01 的 syscall 循环
cat /sys/kernel/tracing/trace | head
echo 0 > /sys/kernel/tracing/events/raw_syscalls/sys_enter/enable
```

`sys_enter` 的打印格式在头文件 `:34`：`NR %ld (%lx, %lx, %lx, %lx, %lx, %lx)`。`id` 是进入挂钩读到的号，可能已经被 ptrace 改过；被 seccomp 拒绝的调用到不了这个事件（[09](./09-hooks.md)）。

```bash
# RECIPE：本环境没有这些命令
strace -c -e trace=clock_gettime,getpid ./01-vdso-vs-syscall
perf trace -e raw_syscalls:sys_enter -- ./01-vdso-vs-syscall
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[args->id] = count(); }'
```

`strace` 走 ptrace，会把 [04](./04-enter-exit-work.md) 的进入/退出报告打开，测到的时间含跟踪器。`perf trace` / `bpftrace` 走 tracepoint。对比时把这个差别算进去，数字配方在 [11](./11-perf-analysis.md)。

---

## 10.5 和 CPU 特性有关的只读文件

sampled on 6.12.113+。这些行用来对照 [02](./02-x86-entry.md) 里哪些 `ALTERNATIVE` 在这台机器上会变成空操作：

```text
$ grep -m1 -o smap /proc/cpuinfo
smap
$ grep -m1 '^bugs' /proc/cpuinfo
bugs		: spectre_v1 spectre_v2 spec_store_bypass swapgs taa bhi ibpb_no_ret
$ cat /sys/devices/system/cpu/vulnerabilities/meltdown
Not affected
```

`lscpu` 同一台机器上的对应行是：Spectre v1 `Mitigation; usercopy/swapgs barriers and __user pointer sanitization`，Spectre v2 `Mitigation; Enhanced / Automatic IBRS; IBPB conditional; PBRSB-eIBRS Not affected; BHI BHI_DIS_S`。Meltdown 这一行是 `Not affected`。`SWITCH_TO_KERNEL_CR3` 是否执行 `mov %cr3` 取决于 `X86_FEATURE_PTI`（`calling.h:169`）。本机没有再读 CR3，也没有做 PTI 开/关计时，那一对比在 [11](./11-perf-analysis.md) 标成 RECIPE。

---

## 10.6 清单

| 想知道 | 先读 | 然后 |
|---|---|---|
| 进程有没有 vDSO | `/proc/<pid>/maps` | 没有就看 `vdso=` |
| 堵在哪次调用 | `/proc/<pid>/syscall` | 号对 tbl |
| 一次调用的参数和返回值 | `raw_syscalls:sys_enter/exit` | RECIPE，需跟踪权限 |
| 某个具名调用的频率 | `syscalls:sys_enter_<name>` | 同样是 RECIPE |
| 是不是被 ptrace 放大了 | 调用栈里有 `ptrace_report_syscall_*` | 见 [11](./11-perf-analysis.md) |
| 返回值是不是重启码 | 退出跟踪时的 `ax` | 512–516 在 [08](./08-restart.md)，用户态最终看不到 |

---

**导航**：[← 挂钩](./09-hooks.md) · [目录](./README.md) · [开销 →](./11-perf-analysis.md)
