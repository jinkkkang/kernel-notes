# 调度（EEVDF）

> 内核版本：**v6.6.160**（stable 6.6 LTS）。下文路径、函数、行号对照 `/tmp/linux` 的该版本核对（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。

**结论**：进程章 [`08-context-switch`](../process-management/08-context-switch.md) 停在「何时进 `__schedule`、怎么切 mm/寄存器」；本章从 **`pick_next_task` → 各 `sched_class` → fair/EEVDF 选谁跑** 接着讲，并把 **观测面与性能分析**写厚。

## 阅读顺序

`01 → 02 → 03 → 04 → 07 → 05 → 08 → 09 → 06`

先把 fair 主链和观测跑通，再扫其它调度类。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 总览、调度类链、与进程章边界、性能视角预习 | 中 |
| [02-core-objects.md](./02-core-objects.md) | `rq` / `cfs_rq` / `sched_entity` / `sched_class` / `task_group` | 中 |
| [03-eevdf.md](./03-eevdf.md) | EEVDF：`vruntime` / `deadline` / `vlag` / eligible / `pick_eevdf` | **厚** |
| [04-enqueue-wakeup.md](./04-enqueue-wakeup.md) | 入队出队、`try_to_wake_up`、唤醒延迟挂钩 | 中 |
| [05-load-balance.md](./05-load-balance.md) | PELT、`load_balance`、亲和与 cpuset | 中 |
| [06-other-classes.md](./06-other-classes.md) | RT / DL / idle / stop（点到） | **薄** |
| [07-cgroup-cpu.md](./07-cgroup-cpu.md) | 与 cgroup cpu / `task_group` 衔接 | 中 |
| [08-observability.md](./08-observability.md) | `/proc`、debugfs、schedstat、tracepoint、PSI、本机样本 | **厚** |
| [09-perf-analysis.md](./09-perf-analysis.md) | 方法论、perf/bpftrace 配方、场景与误判 | **厚** |

可选配方脚本：[demos/](./demos/)（`wakeup_latency.bt`、`sched_switch_hist.bt`、`capture-sched.sh`）。

## 边界（本章不覆盖）

| 留给别章 | 原因 |
|----------|------|
| 完整上下文切换 mm/寄存器细节 | 已在 process-management/08 |
| 完整拓扑 / energy / cpufreq 深挖 | 电源与拓扑另开 |
| DL/RT 算法细节 | 06 只给边界与选用场景 |
| 完整 MM / 缺页 | memory-management |

## 源码地图（先认门牌）

| 路径 | 角色 |
|------|------|
| `kernel/sched/core.c` | `__schedule`、`try_to_wake_up`、`pick_next_task` |
| `kernel/sched/fair.c` | EEVDF、`enqueue/dequeue_entity`、`load_balance` |
| `kernel/sched/sched.h` | `rq` / `cfs_rq` / `task_group` / `sched_class` |
| `include/linux/sched.h` | `sched_entity`、`task_struct` 调度字段 |
| `kernel/sched/pelt.c` | PELT 负载跟踪 |
| `kernel/sched/rt.c` / `deadline.c` / `idle.c` / `stop_task.c` | 其它调度类 |
| `kernel/sched/features.h` | `sched_feat()` 开关及默认值 |
| `kernel/sched/debug.c` | debugfs `sched/`（`CONFIG_SCHED_DEBUG`） |
| `kernel/sched/stats.h` | sched_info / schedstat 宏 |
| `include/trace/events/sched.h` | `sched_*` tracepoint |

*笔记生成对照源码：Linux v6.6.160。*
