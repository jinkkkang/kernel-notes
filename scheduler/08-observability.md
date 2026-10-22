# 8. 观测清单

**结论**：调度问题先分清「整机缺 CPU / cgroup 被掐 / 单任务排队 / 在睡锁或 IO」。内核给的接口参差不齐——**依赖 Kconfig**。下面分「通用接口」「配置门控」「本机实测」三块；**实测与推测分开写**。

> 笔记对照源码：Linux **v7.3**（`c03c1a14507d` / 树 `VERSION=7.3.0`）  
> 本机跑观测的 box 内核：**6.12.94+**（≠ 笔记钉版本）——只当「接口长什么样」的样例。

---

## 8.1 `/proc` 与调度相关文件

| 路径 | 依赖 | 能看什么 |
|---|---|---|
| `/proc/<pid>/sched` | 通常需 `CONFIG_SCHED_DEBUG` | 该任务 se 统计、vruntime、迁移等（调试神器） |
| `/proc/sched_debug` | `CONFIG_SCHED_DEBUG` | 全局 rq / cfs_rq 快照 |
| `/proc/schedstat` | `CONFIG_SCHEDSTATS` | 每 CPU / 域均衡统计；开销略高 |
| `/proc/<pid>/stat` | 默认 | `utime/stime`、`processor`（最近运行 CPU）、nice 等 |
| `/proc/<pid>/status` | 默认 | `Cpus_allowed`、`Cpus_allowed_list` |
| `/proc/stat` | 默认 | 汇总 user/system/iowait/idle… |
| `/proc/loadavg` | 默认 | 负载平均（可跑+不可打断，**不是**纯 CPU 利用率） |
| `/proc/pressure/cpu` | PSI | some/full 压力（见下） |
| `/sys/fs/cgroup/**/cpu.*` | cgroup v2 cpu | weight/max/stat/pressure |

`sched_info` 路径（`kernel/sched/stats.h`）在 `CONFIG_SCHED_INFO` 下给 arrive/dequeue 延迟记账；**导出到 `/proc/<pid>/sched` 仍常绑在 SCHED_DEBUG 上**——只有 `SCHED_INFO=y` 不等于你一定能 cat 到文件。

---

## 8.2 Tracepoint（ftrace / perf / bpf）

常用（`sched:` 子系统）：

| 事件 | 含义 | 分析用途 |
|---|---|---|
| `sched_waking` | ttwu 决定唤醒 | 延迟起点 A |
| `sched_wakeup` / `sched_wakeup_new` | 已可跑 | 起点 B |
| `sched_switch` | 上下文切换 | 上车点 C；prev/next 状态 |
| `sched_migrate_task` | 迁移 | 抖动、跨核 |
| `sched_process_fork` / `exit` / `free` | 生命周期 | 对照 |
| `sched_stat_wait` / `sleep` / `iowait` / `blocked` / `runtime` | 统计类（需 schedstat/相关配置） | 量化等待类型 |

挂载点一般在：`/sys/kernel/tracing/events/sched/`（或 `debugfs` 下同名）。**没有 root / 未挂 tracing 时会 Permission denied**——本机实测即如此。

---

## 8.3 PSI 与 cpu.pressure

| 文件 | 含义 |
|---|---|
| `/proc/pressure/cpu` | 整机 CPU 压力 |
| `.../cgroup/.../cpu.pressure` | **该 cgroup 子树**压力 |

- `some`：有任务想跑却等 CPU 的时间占比  
- `full`：所有非空闲任务都在等 CPU（更「堵死」）  

**和调度延迟的关系**：PSI 升 ≈ 「runnable 排队」的压力表；它**不区分**是 fair 竞争还是 cgroup throttle——要结合 `cpu.stat` 的 throttled 字段（见 [07](./07-cgroup-cpu.md)）。

回链：[cgroup/08-psi-rstat.md](../cgroup/08-psi-rstat.md)。

---

## 8.4 本机只读实测（box）

**标记：以下为 MEASURED（2026-10-22，Asia/Shanghai）。**

| 项 | 结果 |
|---|---|
| 时间 | `2026-10-22 20:27:33 CST`（Asia/Shanghai） |
| `uname -r` | `6.12.94+`（**不是**笔记 v7.3） |
| CPU | 8 逻辑 CPU，Xeon |
| PSI `/proc/pressure/cpu` | `some avg10=0.00 avg60=0.01 avg300≈0.28`；`full` 全 0 |
| 根 `cpu.pressure` | 与上类似 |
| `/proc/loadavg` | `0.17 0.52 0.44 ...`（当时空闲） |
| `/proc/stat` | 可读；idle 占绝对多数 |

**标记：以下为 UNAVAILABLE on this box（配置/权限）。**

| 项 | 原因（来自 `/proc/config.gz` 或访问结果） |
|---|---|
| `/proc/sched_debug` | `# CONFIG_SCHED_DEBUG is not set` |
| `/proc/schedstat` | `# CONFIG_SCHEDSTATS is not set` |
| `/proc/<pid>/sched` | 无文件（同上，DEBUG 未开） |
| `perf` / `bpftrace` | 未安装 |
| `tracing/events/sched` | Permission denied |
| `CONFIG_SCHED_CLASS_EXT` | 本机 kernel **未开**；源码树 v7.3 **有**实现 |
| `CONFIG_SCHED_INFO` | `=y`（但缺 DEBUG 导出时对用户态帮助有限） |

原始抓取：[`demos/box-sched-obs.txt`](./demos/box-sched-obs.txt)。

**读笔记的人**：你自己机器若开了 `SCHED_DEBUG`/`SCHEDSTATS`，优先用 `/proc/<pid>/sched` 和 `perf sched`；不要假设所有发行版默认开启。

---

## 8.5 最小巡检顺序（实操）

1. `uptime` / `/proc/loadavg` / `/proc/stat` —— 整机是否忙  
2. `/proc/pressure/cpu` 与相关 `cpu.pressure` —— 压力在全局还是在某 cgroup  
3. 目标进程：`Cpus_allowed_list`、`psr`（运行核）、cgroup 路径  
4. 若有权限：`perf sched record/lat` 或 `bpftrace` 直方图（[09](./09-perf-analysis.md)）  
5. 若怀疑节流：看该 cgroup `cpu.max` + `cpu.stat`  

---

## 8.6 关键内核符号（统计侧）

| 符号 | 位置 | 作用 |
|---|---|---|
| `schedstat_inc` / `schedstat_enabled` | stats.h:37 | schedstat 开关宏 |
| `sched_info_arrive` / `dequeue` / `switch` | stats.h | 到达/离队/切换记账 |
| `update_stats_enqueue_fair` | fair 侧 | fair 入队统计 |
| `trace_sched_*` | tracepoint | 见上表 |

---

**导航**：[← 负载均衡](./05-load-balance.md) · [章节目录](./README.md) · [性能分析 →](./09-perf-analysis.md)
