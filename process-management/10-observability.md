# 10. 观测与调试清单

**结论**：`/proc/<pid>/` 是读 `task_struct` 的用户态窗口。先看身份、状态、命名空间、线程组，再考虑 bpftrace / ftrace。下面带输出的段落是 **sampled on 6.12** 的只读采样；源码行号以 **v6.6.160** 为准。没有在采样机上跑过的命令标成 **RECIPE**。

> **源码**：v6.6.160，对照 `/tmp/linux`。
> **采样**：`uname -r` = **6.12.94+**。采样时间 2026-10-22 14:17–14:18 Asia/Shanghai。这些数字不要当成 6.6.160 上重新量过。

## 10.1 `/proc` 与 ps

下面几条是 **RECIPE**，把 `<pid>` 换成目标。采样机上真正跑过的命令和输出在 §10.6。

```bash
# RECIPE
ps -eo pid,tid,ppid,tgid,stat,comm | head
cat /proc/<pid>/status | egrep 'Name|State|Tgid|Pid|PPid|NSpid|Threads'
ls /proc/<pid>/task
readlink /proc/<pid>/ns/pid
ps -eo pid,ppid,stat,comm | awk '$3 ~ /Z/'
```

`NSpid` 在 v6.6.160 的 `fs/proc/array.c:212`：从读文件的人所在的 pid ns，一直列到任务最深的那一层，对应 `struct pid` 的 `numbers[]`。见 [03-pid-namespaces.md](./03-pid-namespaces.md)。

| `/proc/<pid>/…` | 6.6.160 里谁实现 | 采样机上看到的（sampled on 6.12） |
|---|---|---|
| `status` | `proc_pid_status`，`fs/proc/array.c:439` | 可读。State / Tgid / NSpid / Threads 都在 |
| `comm` / `cmdline` | `fs/proc/base.c` 的 `comm`、`cmdline` 项 | cmdline 是 `\0` 分隔，要用 `tr` |
| `stat` | `do_task_stat`，`array.c:467` | 单行。`comm` 可能含空格和括号 |
| `wchan` | `proc_pid_wchan`，`base.c:412` | sleep 样本读到 `hrtimer_nanosleep` |
| `stack` | 内核栈回溯 | 非 root → Permission denied |
| `sched` | `CONFIG_SCHED_DEBUG` 才注册，`base.c:3295` | 文件不存在。采样机的配置是 `# CONFIG_SCHED_DEBUG is not set` |
| `schedstat` | `CONFIG_SCHED_INFO`，`base.c:508`。三个数是 `sum_exec_runtime`、`run_delay`、`pcount` | 主线程有三元组 |
| `ns/*` | `base.c:3286` 的 `ns` 目录 | 自己的进程能读 inode；PID 1 被拒绝 |
| `task/` | `proc_task_readdir`，`base.c:3895`，用 `first_tid` / `next_tid` 走线程组 | `Threads:` 和目录项对得上 |
| `cgroup` | cgroup 文件系统 | 采样机是 `0::/…` |
| `smaps_rollup` | `fs/proc/task_mmu.c` | 自己的进程可读 |

## 10.2 tracepoint（RECIPE）

采样机没装 bpftrace，下面没有跑过。

```bash
# RECIPE
bpftrace -lv 'tracepoint:sched:sched_process_*'
bpftrace -lv 'tracepoint:signal:*'
bpftrace -e 'tracepoint:sched:sched_process_fork { printf("parent %d -> child %d\n", args->parent_pid, args->child_pid); }'
bpftrace -e 'tracepoint:sched:sched_process_exit { printf("exit pid %d\n", args->pid); }'
```

v6.6.160 里这两个事件的字段（`include/trace/events/sched.h`）：

| 事件 | 行号 | 字段 |
|---|---|---|
| `sched_process_fork` | 372 | `parent_comm`、`parent_pid`、`child_comm`、`child_pid` |
| `sched_process_exit` | 333 | `comm`、`pid`、`prio`。调用点是 `exit.c:881` 的 `trace_sched_process_exit(tsk)` |

bpftrace 把字段暴露成 `args->...` 时，名字以 `-lv` 为准。上面的 RECIPE 用了源码里确实有的 `parent_pid`、`child_pid`、`pid`。

## 10.3 perf / ftrace（RECIPE）

```bash
# RECIPE。需要 root，function_graph 开销不小
echo function_graph > /sys/kernel/tracing/current_tracer
echo kernel_clone > /sys/kernel/tracing/set_graph_function
cat /sys/kernel/tracing/trace | head -100

perf probe -a 'kernel_clone'
perf record -e probe:kernel_clone -a -- sleep 5
```

sampled on 6.12：`bpftrace` 和 `perf` 都没装。没有装包，也没有开 ftrace。

## 10.4 信号（RECIPE）

```bash
# RECIPE
cat /proc/<pid>/status | egrep 'SigQ|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt'
```

`SigPnd` 是线程自己的 `pending`，`ShdPnd` 是 `shared_pending`。`SigBlk` 是 `blocked`，`SigIgn` / `SigCgt` 来自 `sighand->action[]`。这些名字在 `fs/proc/array.c` 的信号掩码输出里。

## 10.5 对照源码的姿势

1. 本章钉的是 **v6.6.160**。
2. 打开 [`fs/proc/array.c`](https://elixir.bootlin.com/linux/v6.6.160/source/fs/proc/array.c) 和 [`fs/proc/base.c`](https://elixir.bootlin.com/linux/v6.6.160/source/fs/proc/base.c)。
3. 观测只回答「发生了没有」。字段含义以源码为准。
4. §10.6 里贴出来的命令输出是 sampled on 6.12。解释句单独标了「解读」。

---

## 10.6 采样记录（sampled on 6.12）

**环境**（sampled on 6.12）：

```text
Linux grok-bot-vm-477963022 6.12.94+ #1 SMP PREEMPT_DYNAMIC Mon Oct 19 19:23:48 UTC 2026 x86_64 GNU/Linux
Kernel release: 6.12.94+
Timestamp (Asia/Shanghai): 2026-10-22 14:17:35 CST
Whoami: box uid=1000
```

**选样**：容器里 PID 1 是 `tini`。为了拿到完整可读的 `/proc`（含 `ns/`、`smaps_rollup`、`io`），主样本是本 uid 自己拉起的 `sleep 3600`（PID **24529**）。对照是 PID 1，以及多线程 `node`（PID **309**，11 个 tid）。

### 10.6.1 主样本：`sleep` PID 24529（sampled on 6.12）

```bash
sleep 3600 &    # → PID 24529，PPid=52
```

**身份与状态**（`cat /proc/24529/status` 摘录）：

```text
Name:	sleep
State:	S (sleeping)
Tgid:	24529
Pid:	24529
PPid:	52
Uid:	1000	1000	1000	1000
Gid:	1000	1000	1000	1000
NStgid:	24529
NSpid:	24529
Threads:	1
VmSize:	    3012 kB
VmRSS:	    1944 kB
voluntary_ctxt_switches:	1
nonvoluntary_ctxt_switches:	0
SigIgn:	0000000000000006
```

**解读**：

- `State: S`，后面的 `wchan=hrtimer_nanosleep`：自愿睡在高精度定时器上。
- `Tgid == Pid`、`Threads: 1`：单线程。线程组里只有自己。
- `NSpid` 只有一列 `24529`：从这个视角看，pid ns 只有一层编号。
- `voluntary_ctxt_switches: 1`、`nonvoluntary: 0`：几乎没被抢占，刚启动就睡了。
- `SigIgn: …0006`：bit1 和 bit2，忽略 `SIGHUP` / `SIGINT`。

**comm / cmdline**：

```text
$ cat /proc/24529/comm
sleep
$ tr '\0' ' ' < /proc/24529/cmdline; echo
sleep 3600
$ readlink /proc/24529/exe
/usr/bin/sleep
```

**命名空间 inode**：

```text
cgroup:[4026531835]
ipc:[4026532324]
mnt:[4026532322]
net:[4026531840]
pid:[4026532325]
pid_for_children -> pid:[4026532325]
time:[4026531834]
user:[4026531837]
uts:[4026532323]
```

和同一个 shell 的 `/proc/self/ns/` 里 pid、mnt、net、user、uts 的 inode 一致。`pid_for_children` 和 `pid` 相同，子进程还进同一层 pid ns。这和 `nsproxy->pid_ns_for_children` 的含义一致：它决定子进程用哪一层，不一定和自己所在的层不同。

**wchan / task / cgroup / ps**：

```text
$ cat /proc/24529/wchan; echo
hrtimer_nanosleep
$ ls /proc/24529/task/
24529
$ cat /proc/24529/cgroup
0::/agent
$ ps -o pid,tid,psr,stat,wchan:25,comm -L -p 24529
    PID     TID PSR STAT WCHAN                     COMMAND
  24529   24529   6 S    hrtimer_nanosleep         sleep
```

**解读**：`PSR=6` 是最近跑在 CPU 6（`Cpus_allowed_list: 0-7`）。`STAT=S` 和 status 一致。

**`stat` 关键字段**（`CLK_TCK=100`）：

```text
pid=24529 comm=sleep
  state=S  ppid=52  num_threads=1
  utime=0  stime=0
  vsize=3084288
  rss=442
```

**内存 rollup**（`smaps_rollup`）：

```text
Rss:                1944 kB
Pss:                 140 kB
Shared_Clean:       1832 kB
Private_Dirty:       112 kB
```

**解读**：RSS 里大半是共享文件页。PSS 摊薄到大约 140 kB。

**io**：

```text
rchar: 4148
wchar: 0
syscr: 8
syscw: 0
read_bytes: 0
write_bytes: 0
```

### 10.6.2 对照：PID 1 `tini`（sampled on 6.12）

```text
Name:	tini
State:	S (sleeping)
Tgid:	1
Pid:	1
PPid:	0
Uid:	0	0	0	0
NSpid:	1
Threads:	1
VmRSS:	     748 kB
voluntary_ctxt_switches:	2578
nonvoluntary_ctxt_switches:	5
cmdline: /tini -- /pod-daemon --ssh-auth-sock-path ... --ssh-auth-vsock-port 52
cgroup: 0::/system.slice/pod-…
```

uid=1000 读 PID 1 时的结果（不要把「读不到」理解成内核没有这个文件）：

| 路径 | 结果 |
|---|---|
| `status` / `comm` / `cmdline` / `stat` / `cgroup` / `limits` / `task/` | 可读 |
| `ns/` 目录、`readlink ns/*` | Permission denied / 空 |
| `stack` / `smaps_rollup` / `io` / `maps` / `exe` / `cwd` | Permission denied |
| `sched` | **No such file**。和权限无关：整机没开 `CONFIG_SCHED_DEBUG`。v6.6.160 里这个文件也是这道配置才注册的 |
| `wchan` | 字面量 `0`。没有解析出符号 |

**解读**：同一套 `/proc`，能看见什么取决于权限和 kconfig。

### 10.6.3 多线程：`node` PID 309（sampled on 6.12）

```text
Name:	node
State:	S (sleeping)
Tgid:	309
Pid:	309
PPid:	302
Threads:	11
VmRSS:	   90380 kB
voluntary_ctxt_switches:	4883
nonvoluntary_ctxt_switches:	924
wchan(主线程): ep_poll
cgroup: 0::/agent
```

`ls /proc/309/task/` 的 11 个 tid：`309 328 353 356 357 359 379 396 397 398 399`。

```text
$ ps -o pid,tid,psr,stat,wchan:25,comm -L -p 309
    PID     TID PSR STAT WCHAN                     COMMAND
    309     309   0 Sl   ep_poll                   node
    309     328   1 Sl   ep_poll                   DelayedTaskSche
    309     353   1 Sl   futex_wait_queue          node
    …
    309     396   1 Sl   futex_wait_queue          libuv-worker
    309     397   4 Sl   futex_wait_queue          libuv-worker
    309     398   4 Sl   futex_wait_queue          libuv-worker
    309     399   1 Sl   futex_wait_queue          libuv-worker
```

**解读**：

- 每行 PID 都是 309，TID 不同：同一个线程组。各有一份 `task_struct`，共享 `mm` 等（第 2、4 章）。
- 线程上的 `comm` 可以被 `pthread_setname_np` 改成 `libuv-worker` / `DelayedTaskSche`，`Tgid` 不变。`comm` 就是 `task_struct.comm`，`begin_new_exec` 和 `set_task_comm` 都会写它。
- 主线程在 `ep_poll`，工人在 `futex_wait_queue`。
- `nonvoluntary_ctxt_switches: 924` 比 sleep 样本的 0 高。这是采样点的快照，数字会继续涨。

`schedstat` 主线程（sampled on 6.12）。v6.6.160 的三个数依次是 on-CPU 纳秒、等待纳秒、被调度次数：

```text
4708772766 2161869294 5807
```

### 10.6.4 采样时没做的事（sampled on 6.12）

| 项 | 结果 |
|---|---|
| `bpftrace` / `perf` | 未安装。§10.2、§10.3 的命令是 RECIPE |
| `/proc/*/sched` | 全机没有这个文件（`CONFIG_SCHED_DEBUG` 关着） |
| `/proc/*/stack` | Permission denied（不是 root） |
| PID 1 的 `ns/`、`smaps_rollup`、`io` | Permission denied |
| ftrace / `perf probe` | 没跑 |

---

## 10.7 观测对回 v6.6.160

| 用户态看到的 | v6.6.160 |
|---|---|
| `status` 文本 | `fs/proc/array.c` 的 `proc_pid_status`（`:439`）。状态字符串来自 `get_task_state`（`:142`） |
| `NSpid` / `NStgid` | 同文件 `:209` 起，走 `task_pid_nr_ns` / `task_tgid_nr_ns` |
| `stat` 单行 | `do_task_stat`（`:467`） |
| `comm` | `fs/proc/base.c` 注册项，背后是 `task_struct.comm` |
| `ns/*` 的 inode | nsfs 的 `ns_common`。pid ns 见第 3 章 |
| `wchan` | `proc_pid_wchan`（`base.c:412`）调用 `get_wchan` |
| `task/` 目录 | `proc_task_readdir`（`base.c:3895`） |
| `schedstat` 三个数 | `base.c:514`：`se.sum_exec_runtime`、`sched_info.run_delay`、`sched_info.pcount` |

在线：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)

---

**导航**：[章节目录](./README.md) · [← 信号与进程](./09-signals.md)
