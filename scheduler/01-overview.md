# 1. 总览与边界

**结论**：调度器回答三件事——**谁可以跑（策略类）**、**在哪个 CPU 的队列里等（`rq`）**、**这一次选谁上（`sched_class->pick_task`）**。绝大多数业务线程走 **fair / EEVDF**；RT/DL 是「插队权」；`stop`/`idle` 是机器自己用的两端。进程章已经讲清「怎么进 `__schedule`、怎么切上下文」——本章从 **选人** 开始。

---

## 1.1 调度类链条（v7.3）

链接顺序由链接脚本把各类放进 `__sched_class_highest` … `__sched_class_lowest` 段；比较用 `sched_class_above(a,b) := (a < b)`（地址越小优先级越高），见 `kernel/sched/sched.h:2864`。启动时自检（`core.c:8987` 一带）：

```text
stop  >  dl  >  rt  >  fair  >  idle
                 \—— 若启用 CONFIG_SCHED_CLASS_EXT：ext 插在 fair 与 idle 之间（见下）
```

| 类 | 符号 | 典型用途 |
|---|---|---|
| stop | `stop_sched_class` | 停机/迁移 stopper，最高 |
| deadline | `dl_sched_class` | `SCHED_DEADLINE` |
| rt | `rt_sched_class` | `SCHED_FIFO` / `SCHED_RR` |
| fair | `fair_sched_class` | `SCHED_NORMAL` / `BATCH` / `IDLE`（EEVDF） |
| ext | `ext_sched_class` | **sched_ext**：eBPF 可插拔（`kernel/sched/ext/`） |
| idle | `idle_sched_class` | 每 CPU idle 线程 |

`for_each_active_class`（`sched.h:2861`）会按配置跳过：SCX 接管全部 fair 任务时跳过 fair；SCX 未启用时跳过 ext（`next_active_class`，`:2840`）。

本笔记主线：**fair/EEVDF**。其它类见 [06](./06-other-classes.md)。

---

## 1.2 与进程管理章的分界

| 在 process-management/08 | 在本章 |
|---|---|
| 谁调用 `__schedule`（阻塞 / 抢占 / yield / exit） | `__schedule` 里 `pick_next_task` 之后 |
| `context_switch`：mm / `switch_to` | `put_prev_task` / `set_next_task`、fair 记账 |
| 不展开「选谁」 | EEVDF、`sched_entity`、唤醒入队、负载均衡 |

入口锚点（已在进程章核对）：`__schedule` → `kernel/sched/core.c:7111`；`pick_next_task`（非 `CONFIG_SCHED_CORE`）→ `:6721` 一带调 `__pick_next_task`（`:6153`）。

---

## 1.3 一张热路径图

```text
阻塞 / TIF_NEED_RESCHED / yield
        │
        ▼
   __schedule(sched_mode)          core.c:7111
        │  rq_lock; update_rq_clock
        │  可能 deactivate prev（睡眠）
        ▼
   pick_next_task → __pick_next_task
        │  按 active sched_class 从上往下 pick_task
        ▼
   fair: pick_eevdf(cfs_rq)        fair.c:1187
        │
        ▼
   context_switch(prev, next)      （进程章）
```

唤醒反向路径：`try_to_wake_up`（`core.c:4274`）→ 选 CPU → `ttwu_do_activate` / enqueue → 可能 `resched_curr`。见 [04](./04-enqueue-wakeup.md)。

---

## 1.4 性能视角预习（后面各节回扣）

读调度代码时，眼里要同时盯四类数：

| 问题 | 直觉指标 | 本章落点 |
|---|---|---|
| **唤醒延迟** | wakeup → 首次 running | [04](./04-enqueue-wakeup.md)、[08](./08-observability.md)、[09](./09-perf-analysis.md) |
| **抢占 / 调度延迟** | 可跑却没上 CPU 的等待 | `sched_info` / `sched_stat_wait`、perf sched |
| **CPU 利用率 vs 饱和** | 忙但不干活 vs 真算满 | `/proc/stat`、PSI、火焰图误判 |
| **负载不均** | 有核闲、有核打满 | [05](./05-load-balance.md)、绑核/cpuset |

**先定性再下钻**：是「睡太多（锁/IO）」还是「排队太久（竞争 CPU）」还是「被 cgroup 节流」——三者源码路径完全不同。

---

## 1.5 本章覆盖 / 不覆盖

| 覆盖 | 不覆盖（另章） |
|---|---|
| EEVDF 选人与实体记账 | 完整 mm 切换、缺页 |
| 入队/唤醒、NEED_RESCHED | 完整中断/softirq 入口 |
| 负载均衡与亲和概要 | 完整拓扑 / energy / cpufreq |
| cgroup cpu 衔接 | memory/io 控制器深挖 |
| 观测 + 性能配方 | 通用 perf 百科（可再开 performance/） |

---

## 1.6 关键函数表（总览级）

| 文件 | 符号 | 作用 |
|---|---|---|
| `kernel/sched/core.c` | `__schedule` | 调度主入口 |
| | `__pick_next_task` / `pick_next_task` | 按类选下一个 task |
| | `try_to_wake_up` | 唤醒 |
| `kernel/sched/fair.c` | `pick_eevdf` | EEVDF 选实体 |
| | `enqueue_entity` / `dequeue_entity` | fair 入队/出队 |
| | `sched_balance_rq` | 域内负载均衡（v7.3 名；旧称 load_balance） |
| `kernel/sched/sched.h` | `struct rq` / `cfs_rq` / `sched_class` | 核心对象 |

---

**导航**：[章节目录](./README.md) · [核心对象 →](./02-core-objects.md)
