# 7. 与 cgroup cpu 的衔接

**结论**：cgroup **cpu** 控制器不另搞一套调度器，而是给 fair 挂上 **`task_group`**：权重变成 `shares`，带宽变成 `cfs_bandwidth`，限流时把组实体从父队列的红黑树上摘下来。接口细节见 [cgroup/05-cpu-cpuset.md](../cgroup/05-cpu-cpuset.md)；本章只钉调度器侧挂钩。

编译开关：`CONFIG_CGROUP_SCHED`（有 `task_group`）、`CONFIG_FAIR_GROUP_SCHED`（fair 的 `se` / `cfs_rq` / `shares`）、`CONFIG_CFS_BANDWIDTH`（quota / period / burst）。

---

## 7.1 对象怎么接上

```text
cgroup（cpu 控制器）
  └─ css ──container_of──► task_group          sched.h:364，css_tg() 在 core.c:10602
                              ├─ se[cpu]       每 CPU 一个组实体
                              ├─ cfs_rq[cpu]   每 CPU 一个组队列
                              ├─ shares        ← cpu.weight / cpu.shares
                              └─ cfs_bandwidth ← cpu.max（quota、period、burst）

task_struct.sched_task_group → 当前 tg
task_struct.se.parent        → tg->se[cpu]
task_struct.se.cfs_rq        → 父组的 cfs_rq[cpu]
```

根组 `root_task_group` 的 `shares` 在 `sched_init` 里设成 `ROOT_TASK_GROUP_LOAD`（`core.c:10015`），也就是 `NICE_0_LOAD`。

---

## 7.2 weight → shares

cgroup v2 的 `cpu.weight`（`core.c:11504`，`CFTYPE_NOT_ON_ROOT`，所以根 cgroup 上没有这个文件）：

- 合法范围 `CGROUP_WEIGHT_MIN .. CGROUP_WEIGHT_MAX`，默认 `CGROUP_WEIGHT_DFL` 是 100（`include/linux/cgroup.h:40`）
- 写入 `cpu_weight_write_u64`（`core.c:11391`）先把 weight 折成 `weight * 1024 / 100`，再 `sched_group_set_shares`
- 读回来走相反的折算，所以 100 对应内核里的 1024，和 nice 0 的权重对齐

cgroup v1 的 `cpu.shares` 走 `cpu_shares_write_u64`（`:10864`），直接把用户写的数 `scale_load` 后设进 `shares`。

`sched_group_set_shares`（`fair.c:13104`）最终到 `reweight_entity`（`:3851`）：实体还在队列上时，按新旧权重缩放 deadline 相对 `V` 的距离；不在队列上时，缩放 `se->vlag`（因为 lag 的定义里带权重，虚拟 lag 要跟着变）。

**权重是相对份额。** 同机竞争者变多，绝对 CPU 时间仍会掉。它限不住「这一组每 100 ms 最多跑 20 ms」——那是下一节的带宽。

另外还有 `cpu.weight.nice`（`:11510`）和 `cpu.idle`（`:11516`）。`cpu.idle` 写成非 0 时，组被当成 SCHED_IDLE 组，`shares` 收到 `WEIGHT_IDLEPRIO`（`fair.c:13183` 一带）。

---

## 7.3 max → bandwidth（quota / period / burst）

| 用户接口 | 内核 |
|---|---|
| v2 `cpu.max`：一行 `quota period`（`core.c:11524`） | `tg_set_cfs_bandwidth`（`:10890`）写入 `cfs_bandwidth.quota / period` |
| v2 `cpu.max.burst` | `cfs_bandwidth.burst` |
| v1 `cpu.cfs_quota_us` / `cpu.cfs_period_us` / `cpu.cfs_burst_us` | 同一套 `cfs_bandwidth`。v1 文件从 `cpu.cfs_quota_us`（`core.c:11285`）开始 |
| 周期到 | `sched_cfs_period_timer`（`fair.c:6347`）→ `do_sched_cfs_period_timer`（`:6108`） |
| 超额 | `throttle_cfs_rq`（`:5780`） |
| 恢复 | `unthrottle_cfs_rq`（`:5869`） |

`throttle_cfs_rq` 先向全局池再要 1 纳秒。要到了就不限流。要不着就把本 `cfs_rq` 挂上 `cfs_bandwidth.throttled_cfs_rq`，沿 `task_group` 把 `throttle_count` 加上，再对组实体 `dequeue_entity(..., DEQUEUE_SLEEP)`。组里的任务还在，只是父队列看不到这个组，`pick_eevdf` 不会选到它们。

**可见症状（排障用）：**

| 现象 | 解释 |
|---|---|
| 利用率卡在 quota 附近，再加压也不涨 | 很像 throttle |
| v2 `cpu.stat` 里 `nr_throttled` / `throttled_usec` 增长 | 确认节流。字段由 `cpu_extra_stat_show`（`core.c:11337`）打出，另外还有 `nr_periods`、`nr_bursts`、`burst_usec` |
| `cpu.stat.local` 里的 `throttled_usec` | 只算本 cgroup 自己被限的时间（`cpu_local_stat_show`，`:11363`），不含「因为祖先被限而跟着停」 |
| 延迟周期性毛刺，周期接近 `cpu.max` 的 period | 配额打满后的冻结窗口 |
| PSI `cpu.some` 升但主机空闲 | 压力在 **这个 cgroup 里面**，整机不一定缺 CPU |

v1 的 `cpu.stat` 是 `cpu_cfs_stat_show`（`:11179`）：`nr_periods`、`nr_throttled`、`throttled_time`（纳秒，不是微秒）、`nr_bursts`、`burst_time`。读的时候别和 v2 的 `throttled_usec` 混单位。

限流中的任务仍然可以是 `TASK_RUNNING` 且 `on_rq` 挂在子 `cfs_rq` 上，只是调度器不给这个组 CPU。和「堵在锁上」要用不同观测区分——见 [09](./09-perf-analysis.md)。

默认周期 `default_cfs_period`（`fair.c:5596`）是 100 ms（`100000000` 纳秒）。每次从全局池划给本地 `cfs_rq` 的一片由 `kernel.sched_cfs_bandwidth_slice_us` 控制，默认 5000 微秒（`fair.c:138`、sysctl 表 `:156`）。

---

## 7.4 cpuset 一眼

cpuset 改的是 **允许跑的 CPU / 内存节点**，不改 EEVDF 公式。集合过小的时候，岛内任务互相抢，看起来像调度不公平。`load_balance` 用的 `root_domain` 跟着独占 cpuset 走，任务不会被迁出这个集合。

细节回 [cgroup/05](../cgroup/05-cpu-cpuset.md)。

---

## 7.5 关键符号

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct task_group` | `sched.h:364` | 组对象 |
| `struct cfs_bandwidth` | `sched.h:337` | quota / period / burst 和统计 |
| `css_tg` | `core.c:10602` | css → tg |
| `cpu_weight_write_u64` | `core.c:11391` | v2 `cpu.weight` → shares |
| `cpu_shares_write_u64` | `:10864` | v1 `cpu.shares` → shares |
| `reweight_entity` | `fair.c:3851` | 改权重时缩放 lag 或 deadline |
| `sched_group_set_shares` | `:13104` | 写到每个 CPU 的组实体 |
| `throttle_cfs_rq` | `:5780` | 限流 |
| `unthrottle_cfs_rq` | `:5869` | 解除 |
| `sched_cfs_period_timer` | `:6347` | 周期定时器 |
| `tg_set_cfs_bandwidth` | `core.c:10890` | 写入 quota / period / burst |
| `cpu_extra_stat_show` | `:11337` | v2 `cpu.stat` |

---

**导航**：[← 入队与唤醒](./04-enqueue-wakeup.md) · [章节目录](./README.md) · [负载均衡 →](./05-load-balance.md)
