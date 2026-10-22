# 10. 观测与调试清单

**结论**：`/proc/<pid>/` 是读 `task_struct` 的用户态窗口；先把身份、状态、命名空间、线程组看清，再上 bpftrace / ftrace。下列命令默认**不在**读者机器上执行——§10.6 是本笔记作者在 **Grok Bot box** 上的**实测**（只读），用来把字段从「清单」落到「看见过的数」。

> **版本钉死**：笔记源码对照 **Linux v7.3**（`c03c1a14507d`）。  
> **实测环境**：`uname -r` = **6.12.94+**（与 v7.3 不同；字段语义大体兼容，行号/结构体以 v7.3 为准）。  
> **实测时间**：2026-10-22 14:17–14:18 Asia/Shanghai（CST, UTC+8）。

## 10.1 `/proc` 与 ps

```bash
# 状态字母、tgid、pid、ppid
ps -eo pid,tid,ppid,tgid,stat,comm | head

# 单任务
cat /proc/<pid>/status | egrep 'Name|State|Tgid|Pid|PPid|NSpid|Threads'
cat /proc/<pid>/sched          # 需 CONFIG_SCHED_DEBUG；本机实测未启用 → 文件不存在
ls /proc/<pid>/task            # 线程组内所有 tid
readlink /proc/<pid>/ns/pid    # inode 形式：pid:[inum]

# 僵尸
ps -eo pid,ppid,stat,comm | awk '$3 ~ /Z/'
```

`NSpid` 字段可看到多层 pid ns 中的号（对应 `struct pid->numbers[]`，见 [03-pid-namespaces.md](./03-pid-namespaces.md)）。

| `/proc/<pid>/…` | 主要反映 | 备注 |
|---|---|---|
| `status` | 可读摘要：State / Tgid / NSpid / Threads / ctxt_switches / 信号掩码 / Vm* | 首选入口 |
| `comm` / `cmdline` | `task->comm`；完整 argv（`\0` 分隔） | cmdline 用 `tr '\0' ' '` |
| `stat` | 单行、字段密；解析时注意 `comm` 可能含空格/括号 | 见 §10.6.2 |
| `wchan` | 阻塞内核符号（若可读） | 与 `ps -o wchan` 同源思路 |
| `stack` | 内核栈回溯 | 通常要 `CAP_SYS_ADMIN`；本机非 root → Permission denied |
| `sched` | 调度实体调试 | `CONFIG_SCHED_DEBUG`；本机 `# CONFIG_SCHED_DEBUG is not set` |
| `schedstat` | 三元组：on-cpu / wait / 切片次数 | 本机有 |
| `ns/*` | 各 namespace inode | 对他人进程可能列目录失败 |
| `task/` | 同线程组 tid 目录 | `Threads:` 应对上 |
| `cgroup` | cgroup v2 路径 | 本机统一 `0::/…` |
| `smaps_rollup` | RSS/PSS 汇总 | 自己的进程可读 |

## 10.2 tracepoint（sched / task / signal）

```bash
# 列出（本机未装 bpftrace，下列为模板）
bpftrace -lv 'tracepoint:sched:sched_process_*'
bpftrace -lv 'tracepoint:signal:*'

# fork / exit（字段名以 -lv 为准）
bpftrace -e 'tracepoint:sched:sched_process_fork { printf("parent %d -> child %d\n", args->parent_pid, args->child_pid); }'
bpftrace -e 'tracepoint:sched:sched_process_exit { printf("exit pid %d\n", args->pid); }'
```

已核实：内核里 `trace_sched_process_exit(tsk, group_dead)` 在 `exit.c:957`（v7.3）。**bpftrace 导出的字段名可能与内核函数参数不完全同名**，以 `-lv` 为准。

## 10.3 perf / ftrace 模板

```bash
# 函数图（需 root，注意开销）
echo function_graph > /sys/kernel/tracing/current_tracer
echo kernel_clone > /sys/kernel/tracing/set_graph_function
# … 复现后
cat /sys/kernel/tracing/trace | head -100

perf probe -a 'kernel_clone'
perf record -e probe:kernel_clone -a -- sleep 5
```

本机实测：`bpftrace` / `perf` **均未安装**；未强行装包、未开 ftrace（避免无 root 权限与挂死风险）。

## 10.4 信号观测

```bash
cat /proc/<pid>/status | egrep 'SigQ|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt'
# kill -0 测存在；strace -e signal,kill -p <pid>
```

## 10.5 对照源码的建议姿势

1. 先定版本（本章钉死 **v7.3**）
2. elixir 打开文件，搜索函数名：[`fs/proc/array.c`](https://elixir.bootlin.com/linux/v7.3/source/fs/proc/array.c)（`status`/`stat`）、[`fs/proc/base.c`](https://elixir.bootlin.com/linux/v7.3/source/fs/proc/base.c)
3. 观测只验证「发生了没有」，细节仍以源码为准
4. **区分实测与推测**：下面 §10.6 凡贴命令输出的都是实测；解释性句子标「解读」

---

## 10.6 本机实测（Grok Bot box）

**环境**（实测）：

```text
Linux grok-bot-vm-477963022 6.12.94+ #1 SMP PREEMPT_DYNAMIC Mon Oct 19 19:23:48 UTC 2026 x86_64 GNU/Linux
Kernel release: 6.12.94+
Timestamp (Asia/Shanghai): 2026-10-22 14:17:35 CST
Whoami: box uid=1000
```

**选样**：容器里 PID 1 是 `tini`（不是 systemd）。为拿到完整可读的 `/proc`（含 `ns/`、`smaps_rollup`、`io`），主样本用**本 uid 自spawn** 的 `sleep 3600`（PID **24529**）；对照 PID 1（权限边界）与多线程 `node`（PID **309**，11 个 tid）。

### 10.6.1 主样本：`sleep` PID 24529（实测）

```bash
sleep 3600 &    # → PID 24529，PPid=52（sand-exit-watch 一侧）
```

**身份与状态**（`cat /proc/24529/status` 摘录，实测）：

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

**解读（标注）**：

- `State: S` + 后面 `wchan=hrtimer_nanosleep` → 自愿睡在高精度定时器上（`sleep(1)` 路径），不是 D 盘等待。
- `Tgid == Pid`、`Threads: 1` → 单线程进程；线程组里只有自己。
- `NSpid` 只有一列 `24529` → **当前可见 pid ns 只有一层编号**（容器内看自己；没有「宿主机第二列」）。这与「嵌套 pid ns 会看到多列 NSpid」的理论不矛盾——本 box 视角下就是单层。
- `voluntary_ctxt_switches: 1`、`nonvoluntary: 0` → 几乎没被抢占；刚启动就睡下去，符合预期。
- `SigIgn: …0006` → bit1+bit2 → 忽略 `SIGHUP`/`SIGINT`（`sleep` 常见）。

**comm / cmdline**（实测）：

```text
$ cat /proc/24529/comm
sleep
$ tr '\0' ' ' < /proc/24529/cmdline; echo
sleep 3600
$ readlink /proc/24529/exe
/usr/bin/sleep
```

**命名空间 inode**（实测，`ls -l` + `readlink`）：

```text
cgroup:[4026531835]
ipc:[4026532324]
mnt:[4026532322]
net:[4026531840]
pid:[4026532325]
pid_for_children -> pid:[4026532325]   # 与当前 pid ns 相同
time:[4026531834]
user:[4026531837]
uts:[4026532323]
```

与同 shell 的 `/proc/self/ns/pid|mnt|net|user|uts` **inode 一致**（同容器）。`pid_for_children` 与 `pid` 相同 → 子进程仍进同一 pid ns（未做嵌套）。

**wchan / task / cgroup / ps**（实测）：

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

**解读**：`PSR=6` → 最近跑在 CPU 6（8 逻辑 CPU：`Cpus_allowed_list: 0-7`）。`STAT=S` 与 status 一致。

**`stat` 关键字段**（实测解析，`CLK_TCK=100`）：

```text
pid=24529 comm=sleep
  state=S  ppid=52  num_threads=1
  utime=0  stime=0          # 几乎无用户/系统 tick
  vsize=3084288             # 字节；≈ status 的 VmSize 3012 kB
  rss=442                   # 页；×4k ≈ 1768 kB，与 VmRSS 1944 kB 同量级（计法略有差）
```

**内存 rollup**（实测 `smaps_rollup`）：

```text
Rss:                1944 kB
Pss:                 140 kB
Shared_Clean:       1832 kB
Private_Dirty:       112 kB
```

**解读**：RSS 里大半是共享文件页（libc 等）；PSS 被摊薄到 ~140 kB，更接近「这个 sleep 自己占的锅」。

**io**（实测）：

```text
rchar: 4148
wchar: 0
syscr: 8
syscw: 0
read_bytes: 0
write_bytes: 0
```

### 10.6.2 对照：PID 1 `tini`（权限边界，实测）

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

**uid=1000 读 PID 1 时失败项**（实测，勿当内核没有该文件）：

| 路径 | 结果 |
|---|---|
| `status` / `comm` / `cmdline` / `stat` / `cgroup` / `limits` / `task/` | 可读 |
| `ns/` 目录、`readlink ns/*` | Permission denied / 空 |
| `stack` / `smaps_rollup` / `io` / `maps` / `exe` / `cwd` | Permission denied |
| `sched` | **No such file**（与权限无关：整机未开 `CONFIG_SCHED_DEBUG`） |
| `wchan` | 读到字面量 `0`（未解析出符号；root 进程上常见受限表现） |

**解读**：同一份 `/proc` 接口，**能看见什么取决于权限与 kconfig**，不是「文件不存在就代表内核没实现」。写笔记时把这两类失败分开写。

### 10.6.3 多线程：`node` PID 309（实测）

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

`ls /proc/309/task/` → 11 个 tid：`309 328 353 356 357 359 379 396 397 398 399`。

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

**解读（实测支撑的理论）**：

- 所有行 **PID 同为 309、TID 不同** → 同线程组；`task_struct` 各一份，共享 `mm` 等（见第 2、4 章）。
- `comm` 在线程上可被 `pthread_setname_np` 改成 `libuv-worker` / `DelayedTaskSche`，**不影响** `Tgid`。
- 主线程 `ep_poll`、工人 `futex_wait_queue` → 典型事件循环 + 线程池等待。
- `nonvoluntary_ctxt_switches: 924` 远高于 `sleep` 的 0 → 活进程会被抢占；数字本身随时间涨，此处是采样点快照。

`schedstat` 主线程（实测三元组：`on-cpu-ns wait-ns timeslices`）：

```text
4708772766 2161869294 5807
```

### 10.6.4 本机未做 / 失败清单（诚实记录）

| 项 | 结果 |
|---|---|
| `bpftrace` / `perf` | 未安装；未执行 one-liner |
| `/proc/*/sched` | 全机无此文件（`# CONFIG_SCHED_DEBUG is not set`） |
| `/proc/*/stack` | Permission denied（非 root） |
| PID 1 的 `ns/`、`smaps_rollup`、`io` | Permission denied |
| ftrace / `perf probe` | 未跑（需特权，且易拖慢 box） |

---

## 10.7 把观测绑回 v7.3 源码（指针）

| 用户态看到的 | v7.3 附近 |
|---|---|
| `status` 文本 | `fs/proc/array.c` → `proc_pid_status` / `task_state` 等 |
| `stat` 单行 | `fs/proc/array.c` → `do_task_stat` |
| `comm` | `fs/proc/base.c` + `task_struct::comm` |
| `ns/*` inode | `nsfs` + `ns_common::inum`；pid ns 见第 3 章 `struct pid` |
| `wchan` | 栈上调度点符号；依赖配置与权限 |
| `task/` 目录 | 遍历线程组（`while_each_thread` 一类） |

在线：[elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)

---

**导航**：[章节目录](./README.md) · [← 信号与进程](./09-signals.md)
