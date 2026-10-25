# 6. 其它调度类（薄）

**结论**：日常业务几乎都在 fair/EEVDF。碰到「关中断都救不了的延迟」或「必须在截止前跑完」才认真看 RT/DL。`stop` / `idle` 是内核自己的两端：一个用来做停机和迁移，一个在没人可跑时占用 CPU。

---

## 6.1 对照表

| 类 | 策略 / 入口 | 源码 | 何时用 |
|---|---|---|---|
| stop | 内部 stopper | `kernel/sched/stop_task.c`，`DEFINE_SCHED_CLASS(stop)` `:106` | 停机、迁移，最高优先级。只在 `CONFIG_SMP` 下编进镜像 |
| deadline | `SCHED_DEADLINE` | `kernel/sched/deadline.c`，`DEFINE_SCHED_CLASS(dl)` `:2757` | 周期任务，带 runtime / deadline / period |
| rt | `SCHED_FIFO` / `SCHED_RR` | `kernel/sched/rt.c`，`DEFINE_SCHED_CLASS(rt)` `:2701` | 固定优先级实时 |
| fair | `SCHED_NORMAL` / `BATCH` / `IDLE` | `kernel/sched/fair.c:13225` | 默认 |
| idle | idle 线程 | `kernel/sched/idle.c`，`DEFINE_SCHED_CLASS(idle)` `:505` | CPU 没事做 |

优先级自检：`kernel/sched/core.c:9990`。`idle` 必须紧挨在 `fair` 后面，`fair` 紧挨 `rt`，`rt` 紧挨 `dl`；SMP 上 `dl` 再紧挨 `stop`。链接脚本 `include/asm-generic/vmlinux.lds.h:129` 把段排成 `stop, dl, rt, fair, idle`。

---

## 6.2 RT / DL 一眼

- **RT**：同一优先级按 FIFO。`SCHED_RR` 有时间片（`sched_rt_entity.time_slice`）。轻易不要给业务 `SCHED_FIFO` 最高档——它排在 fair 前面，普通任务会饿死。  
- **DL**：`__checkparam_dl`（`kernel/sched/deadline.c:2987`）要求 `runtime ≤ deadline ≤ period`（period 为 0 时用 deadline 充当 period），另外还有最小 runtime 和 period 上下限。准入控制会拒绝超额。适合周期控制环，不适合「越快越好」的吞吐型服务。任务自己的带宽定时器是 `sched_dl_entity.dl_timer`（`include/linux/sched.h:662`）。  
- 与 cgroup：`CONFIG_RT_GROUP_SCHED` 时 `task_group` 上有 `rt_bandwidth`（`sched.h:391`），接口是 cgroup v1 的 `cpu.rt_runtime_us` / `cpu.rt_period_us`（`core.c:11310`）。

---

## 6.3 和主线的关系

`__pick_next_task`（`core.c:6004`）的慢路径 `for_each_class` 从高到低调用 `class->pick_next_task`。高类有可跑任务就轮不到 fair。快路径还加了一道保险：前一个任务的类高于 fair，或者 `rq->nr_running != rq->cfs.h_nr_running`（队列里不只有 fair），都走慢路径。

所以「fair 怎么调都没用」时，先看是不是 RT/DL 把 CPU 占了：`ps -eLo cls,pri,comm` 里 `cls` 为 `FF` / `RR` / `DL` 的那些。

---

**导航**：[← 负载均衡](./05-load-balance.md) · [章节目录](./README.md) · [性能分析 →](./09-perf-analysis.md)
