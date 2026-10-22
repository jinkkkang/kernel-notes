# 7. 与 cgroup cpu 的衔接

**结论**：cgroup **cpu** 控制器不另搞一套调度器，而是给 fair 挂上 **`task_group`**：权重变成 `shares`，带宽变成 `cfs_bandwidth`，节流时任务从 runnable 集合里暂时消失。接口细节见 [cgroup/05-cpu-cpuset.md](../cgroup/05-cpu-cpuset.md)；本章只钉调度器侧挂钩。

---

## 7.1 对象怎么接上

```text
cgroup (cpu 控制器启用)
  └─ css ──container_of──► task_group
                              ├─ shares          ← cpu.weight
                              ├─ cfs_bandwidth   ← cpu.max (quota/period)
                              └─ cfs_rq__percpu[] ← 每 CPU 一组实体队列

task_struct.sched_task_group → 当前 tg
task_struct.se.parent / cfs_rq → 嵌在对应 tg 的层次里
```

`css_tg()`：`sched.h:551`。组调度编译开关：`CONFIG_FAIR_GROUP_SCHED`、`CONFIG_CFS_BANDWIDTH`（带宽）。

---

## 7.2 weight → shares

- 用户写 `cpu.weight`（v2，默认 100）→ 内核映到 `task_group->shares`。  
- `update_cfs_group` / `reweight_entity`（`fair.c:4960`）在入队出队与配额变化时重算实体权重。  
- **权重是相对份额**，不是「保证百分之多少 CPU」——同机竞争者变多，绝对 CPU 时间仍会掉。

---

## 7.3 max → bandwidth（quota/period）

| 用户接口 | 内核 |
|---|---|
| `cpu.max`：`quota period` | `cfs_bandwidth` + 每 CPU `cfs_rq.runtime_*` |
| 周期到 | `sched_cfs_period_timer`（约 `fair.c:7711`） |
| 超额 | `throttle_cfs_rq`（`:7210`）——组被节流 |
| 恢复 | `unthrottle_cfs_rq`（`:7275`） |

**可见症状（实测口径，排障用）：**

| 现象 | 解释 |
|---|---|
| 利用率卡在 quota 附近，再加压也不涨 | 很像 throttle |
| `cpu.stat` 里 `throttled` / `throttled_usec` 增长 | 确认节流（cgroup v2） |
| 延迟周期性毛刺，周期≈`period` | 配额打满后的冻结窗口 |
| PSI `cpu.some` 升但主机空闲 | 可能是 **cgroup 内**挤兑，不是整机缺 CPU |

节流中的任务：**不是**永远 TASK_UNINTERRUPTIBLE 睡死，而是调度器侧不给跑；和「堵在锁上」要用不同观测区分——见 [09](./09-perf-analysis.md) 场景册。

---

## 7.4 cpuset 一眼

cpuset 改的是 **允许跑的 CPU/内存节点集合**，不直接改 EEVDF 公式。但：

- 集合过小 → 看起来像「调度不公平」，其实是岛内互卷；  
- 与 `sched_balance_*`：均衡不会把任务迁出 cpuset。  

细节回 [cgroup/05](../cgroup/05-cpu-cpuset.md)。

---

## 7.5 关键符号

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct task_group` | sched.h:478 | 组对象 |
| `css_tg` | `:551` | css→tg |
| `reweight_entity` | fair.c:4960 | 重加权 |
| `throttle_cfs_rq` / `unthrottle_cfs_rq` | `:7210` / `:7275` | 节流/解除 |
| `sched_cfs_period_timer` | `:7711` | 周期配额 |

---

**导航**：[← 入队与唤醒](./04-enqueue-wakeup.md) · [章节目录](./README.md) · [负载均衡 →](./05-load-balance.md)
