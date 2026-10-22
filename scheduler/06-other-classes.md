# 6. 其它调度类（薄）

**结论**：日常业务 99% 在 fair/EEVDF。碰到「关中断都救不了的延迟」或「必须在截止前跑完」才认真看 RT/DL。`stop`/`idle` 是内核脚手架；**sched_ext** 把选人逻辑交给 BPF——v7.3 已进主线，但配置与运维模型另开专题。

---

## 6.1 对照表

| 类 | 策略 / 入口 | 源码 | 何时用 |
|---|---|---|---|
| stop | 内部 stopper | `stop_task.c`，`DEFINE_SCHED_CLASS(stop)` `:99` | 停机、迁移，最高优先级 |
| deadline | `SCHED_DEADLINE` | `deadline.c`，`DEFINE_SCHED_CLASS(dl)` `:3647` | 周期任务，runtime/period/deadline |
| rt | `SCHED_FIFO` / `RR` | `rt.c`，`DEFINE_SCHED_CLASS(rt)` `:2602` | 固定优先级实时 |
| fair | `NORMAL`/`BATCH`/`IDLE` | `fair.c:15855` | 默认 |
| ext | sched_ext | `kernel/sched/ext/ext.c`，`DEFINE_SCHED_CLASS(ext)` `:5127` | BPF 调度器 |
| idle | idle 线程 | `idle.c:578` | CPU 没事做 |

优先级关系自检：`core.c:8987`–`:8990`（`stop>dl>rt>fair>idle`）。ext 与 fair 的相对位置另有 `BUG_ON(!sched_class_above(&fair_sched_class, &ext_sched_class))`（`:8992`）——**fair 仍高于 ext**；SCX 通过 `next_active_class` 在「接管全部 fair 任务」时跳过 fair。

---

## 6.2 RT / DL 一眼

- **RT**：同优先级 FIFO；`SCHED_RR` 有时间片。轻易不要给业务 FIFO 99——starve 普通任务是经典事故。  
- **DL**：声明 `runtime ≤ deadline ≤ period`；admission 控制拒掉超额。适合周期控制环，不适合「越快越好」的吞吐型服务。  
- 与 cgroup：`CONFIG_RT_GROUP_SCHED` 时 `task_group` 上也有 RT 带宽（`sched.h` 里 `rt_bandwidth`）。

---

## 6.3 sched_ext（只点位置）

目录：`kernel/sched/ext/`（`ext.c` 为主）。

| 点 | 说明 |
|---|---|
| Kconfig | `CONFIG_SCHED_CLASS_EXT`（本笔记对照的**运行中 box** 配置为未开启；**v7.3 源码树里有实现**） |
| 类符号 | `ext_sched_class` |
| 思路 | BPF 实现 enqueue/dispatch 等，内核类回调转到 BPF |
| 与本章关系 | **不展开**；会用 eBPF 章 / 专门笔记再写 |

---

## 6.4 和主线的关系

`__pick_next_task`（`core.c:6153`）按 active 类从高到低 `pick_task`：高类有可跑任务就轮不到 fair。所以「fair 调参无效」时，先看是不是 RT/DL/SCX 把 CPU 吃了。

---

**导航**：[← 负载均衡](./05-load-balance.md) · [章节目录](./README.md) · [性能分析 →](./09-perf-analysis.md)
