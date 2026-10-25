# 8. 观测清单

**结论**：调度问题先分清「整机缺 CPU / cgroup 被掐 / 单任务排队 / 在睡锁或 IO」。内核给的接口看编译开关。下面分「通用接口」「配置门控」「样本」三块。**样本与源码分开写**：源码钉 v6.6.160；数字来自作者机器上的一次只读抓取，内核是 6.12.94+，标成 sampled on 6.12。

> 笔记对照源码：Linux **v6.6.160**  
> 抓观测的那台机器：**6.12.94+**（和笔记钉的版本不是同一个）。只拿它看「接口长什么样、当时机器闲不闲」。

---

## 8.1 `/proc` 与 debugfs

| 路径 | 依赖 | 能看什么 |
|---|---|---|
| `/proc/<pid>/sched` | `CONFIG_SCHED_DEBUG`（`fs/proc/base.c:3295`） | 该任务的 vruntime、slice、迁移、调度统计。实现是 `proc_sched_show_task`（`kernel/sched/debug.c:989`） |
| `/sys/kernel/debug/sched/debug` | 同上，`debug.c:375` | 全局 rq / cfs_rq 快照。`debug.c:11` 的注释写着 `/proc/sched_debug`，本树里这个名字没有 `proc_create`，读的是上面这个 debugfs 文件 |
| `/sys/kernel/debug/sched/features` | 同上，`:344` | 读写 `sched_feat`（`PLACE_LAG`、`RUN_TO_PARITY`、`NEXT_BUDDY` …） |
| `/sys/kernel/debug/sched/base_slice_ns` | 同上，`:350` | EEVDF 请求长度，单位纳秒，对应 `sysctl_sched_base_slice` |
| `/sys/kernel/debug/sched/migration_cost_ns`、`nr_migrate` | 同上且 `CONFIG_SMP`，`:357` | 迁移代价、一次均衡最多搬几个任务 |
| `/proc/schedstat` | `CONFIG_SCHEDSTATS`（`kernel/sched/build_utility.c` 在该开关下才 `#include "stats.c"`，创建点 `stats.c:218`） | 每 CPU / 每域的均衡计数 |
| `/proc/<pid>/schedstat` | `CONFIG_SCHED_INFO`（`fs/proc/base.c:3337`） | 三个数：`sum_exec_runtime`、`sched_info.run_delay`、`pcount`。关掉统计时打印 `0 0 0` |
| `/proc/<pid>/stat` | 默认 | `utime` / `stime`、`processor`（最近运行的 CPU）、nice |
| `/proc/<pid>/status` | 默认 | `Cpus_allowed`、`Cpus_allowed_list` |
| `/proc/stat` | 默认 | 汇总 user / system / iowait / idle |
| `/proc/loadavg` | 默认 | 负载平均（可跑 + 不可打断睡眠，不是纯 CPU 利用率） |
| `/proc/pressure/cpu` | `CONFIG_PSI` | some / full 压力 |
| `/sys/fs/cgroup/**/cpu.*` | cgroup v2 cpu | `weight` / `max` / `stat` / `pressure`。根 cgroup 上 `weight` 和 `max` 带 `CFTYPE_NOT_ON_ROOT`，文件本身不出现 |

`/proc/sys/kernel/` 里和 fair 相关、v6.6.160 仍然是 sysctl 的，主要是：

| 名字 | 位置 | 作用 |
|---|---|---|
| `sched_child_runs_first` | `fair.c:149` | fork 之后子进程是否先跑 |
| `sched_cfs_bandwidth_slice_us` | `fair.c:156` | 每次从组的全局带宽池划给本地 `cfs_rq` 的一片，默认 5000 微秒 |
| `sched_schedstats` | `core.c:4684` | 运行时开关 schedstat（还要编译进 `CONFIG_SCHEDSTATS`） |

`sched_info` 的记账函数在 `kernel/sched/stats.h`：`sched_info_dequeue`（`:210`）、`sched_info_arrive`（`:229`），整段包在 `CONFIG_SCHED_INFO`。它们进 `/proc/<pid>/sched` 仍要 `CONFIG_SCHED_DEBUG`；只开 `SCHED_INFO` 时，用户态能看的是 `/proc/<pid>/schedstat` 那三个数。

---

## 8.2 Tracepoint（ftrace / perf / bpf）

定义在 `include/trace/events/sched.h`。下面这些在 v6.6.160 里都有：

| 事件 | 行号 | 含义 | 分析用途 |
|---|---|---|---|
| `sched_waking` | 170 | ttwu 决定唤醒 | 延迟起点 A。字段：`comm`、`pid`、`prio`、`target_cpu` |
| `sched_wakeup` | 178 | 已写成 `TASK_RUNNING` | 起点 B。字段同上 |
| `sched_wakeup_new` | 185 | 唤醒一个新任务 | fork 之后第一次 |
| `sched_switch` | 222 | 上下文切换 | 上车点 C。字段：`prev_comm/pid/prio/state`、`next_comm/pid/prio` |
| `sched_migrate_task` | 274 | 迁移 | `orig_cpu`、`dest_cpu` |
| `sched_process_fork` | 372 | 生命周期 | 对照 |
| `sched_process_exit` / `sched_process_free` | 333 / 326 | 退出 / 释放 | 对照 |
| `sched_stat_wait` | 463 | 等 CPU 多久 | 要 `CONFIG_SCHEDSTATS`，否则是空事件 |
| `sched_stat_sleep` | 471 | 可打断睡眠 | 同上 |
| `sched_stat_iowait` | 479 | iowait | 同上 |
| `sched_stat_blocked` | 486 | 不可打断睡眠 | 同上 |
| `sched_stat_runtime` | 517 | 这段实际跑了多久 | 不包在 `SCHEDSTATS` 里，`update_curr_task` 会打 |

`sched_switch` 的 `prev_state` 是 `long`，由 `__trace_sched_switch_state`（`:190`）编出来：

- 任务仍是 `TASK_RUNNING` 且这次切换不是抢占：值为 0，打印成 `R`
- 抢占：带上 `TASK_REPORT_MAX`（`include/linux/sched.h:1685`，即 `0x100`），打印成 `R+`
- 其它位对应 `S` / `D` / `T` / `t` / `X` / `Z` / `P` / `I`（`:256` 的标志表）

挂载点一般在 `/sys/kernel/tracing/events/sched/`，debugfs 下也有同名目录。没有 root、或者 tracing 没挂上时，`ls` 会 Permission denied——样本里就是这样。

---

## 8.3 PSI 与 cpu.pressure

| 文件 | 含义 |
|---|---|
| `/proc/pressure/cpu` | 整机 CPU 压力 |
| `.../cgroup/.../cpu.pressure` | 该 cgroup 子树的压力 |

- `some`：至少有一个任务想跑、却在等 CPU 的时间占比  
- `full`：所有非 idle 任务都在等 CPU  

PSI 升高表示「有人 runnable 却没上 CPU」。它不分是 fair 在抢还是 cgroup 限流。限流要再看 `cpu.stat` 的 `nr_throttled` / `throttled_usec`（见 [07](./07-cgroup-cpu.md)）。

回链：[cgroup/08-psi-rstat.md](../cgroup/08-psi-rstat.md)。

---

## 8.4 样本（sampled on 6.12）

**以下数字是 MEASURED，抓取时间 2026-10-22 20:27:33 CST（Asia/Shanghai），内核 6.12.94+。** 没有在 v6.6.160 上重跑。原始记录：[`demos/box-sched-obs.txt`](./demos/box-sched-obs.txt)。

| 项 | 结果 |
|---|---|
| `uname -r` | `6.12.94+` |
| CPU | 8 逻辑 CPU，Xeon |
| PSI `/proc/pressure/cpu` | `some avg10=0.00 avg60=0.01 avg300=0.29`；`full` 全 0 |
| 根 `cpu.pressure` | 与上类似 |
| `/proc/loadavg` | `0.17 0.52 0.44 1/786 76145` |
| `/proc/stat` | 可读；idle 占绝对多数 |
| 根 cgroup | 有 `cpu.pressure`、`cpu.stat`、`cpu.stat.local`；没有 `cpu.weight`、`cpu.max` |

**以下在那台机器上 UNAVAILABLE（配置或权限）。** 原因来自当时的 `/proc/config.gz` 或访问结果，不是 v6.6.160 的 Kconfig 默认值。

| 项 | 原因 |
|---|---|
| `/proc/sched_debug` | 不存在。那台机器 `# CONFIG_SCHED_DEBUG is not set`。v6.6.160 的对应物是 debugfs `sched/debug`，样本没有去读它 |
| `/proc/schedstat` | `# CONFIG_SCHEDSTATS is not set` |
| `/proc/<pid>/sched` | 无文件（要 `SCHED_DEBUG`） |
| `perf` / `bpftrace` | 未安装 |
| `/sys/kernel/tracing/events/sched` 与 debugfs 同名目录 | Permission denied |
| `CONFIG_SCHED_INFO` | 那台机器是 `=y`。没有 `SCHED_DEBUG` 时，`/proc/<pid>/sched` 仍不会出现 |

读笔记的人：自己的机器若开了 `SCHED_DEBUG` / `SCHEDSTATS`，优先用 `/proc/<pid>/sched`、debugfs `sched/features` 和 `perf sched`。不要假设发行版默认把它们打开。

---

## 8.5 最小巡检顺序（实操）

1. `uptime` / `/proc/loadavg` / `/proc/stat` —— 整机是否忙  
2. `/proc/pressure/cpu` 与相关 `cpu.pressure` —— 压力在全局还是在某个 cgroup  
3. 目标进程：`Cpus_allowed_list`、`/proc/<pid>/stat` 的 `processor`、cgroup 路径  
4. 有权限再上：`perf sched record/latency` 或 `bpftrace` 直方图（[09](./09-perf-analysis.md)）  
5. 怀疑节流：该 cgroup 的 `cpu.max`，加上 `cpu.stat` 里的 `nr_throttled` 和 `throttled_usec`

---

## 8.6 关键内核符号（统计侧）

| 符号 | 位置 | 作用 |
|---|---|---|
| `schedstat_enabled` / `schedstat_inc` | `stats.h:37` / `:39` | schedstat 开关宏 |
| `sched_info_arrive` / `sched_info_dequeue` | `stats.h:229` / `:210` | 上 CPU / 离队时记等待 |
| `update_stats_enqueue_fair` | `fair.c:1292` | fair 入队统计 |
| `trace_sched_waking` / `trace_sched_wakeup` / `trace_sched_switch` | `core.c:4233`、`:3773`、`:6719` | 三个延迟锚点 |

---

**导航**：[← 负载均衡](./05-load-balance.md) · [章节目录](./README.md) · [性能分析 →](./09-perf-analysis.md)
