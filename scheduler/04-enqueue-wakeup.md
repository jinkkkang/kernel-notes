# 4. 入队、出队与唤醒

**结论**：任务要跑，必须出现在某 CPU `rq` 的对应类队列里。fair 路径上，**`enqueue_entity` / `dequeue_entity`** 维护 `cfs_rq` 账本与树；睡眠出队、唤醒再入队走 **`try_to_wake_up`（ttwu）**。性能上的「唤醒延迟」几乎都从 ttwu 成功把任务变成 runnable 开始算，到第一次 `sched_switch` 上车结束。

---

## 4.1 fair 入队：`enqueue_entity`

`fair.c:6534`（骨架加注）：

```c
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH); /* 中文：PELT 同步 */
	se_update_runnable(se);
	update_cfs_group(se);           /* 中文：组权重/份额 */
	account_entity_enqueue(cfs_rq, se); /* 中文：sum_weight、挂 RB 等 */

	if (flags & ENQUEUE_MIGRATED)
		se->exec_start = 0;     /* 中文：迁移后不再视为 cache-hot */

	update_stats_enqueue_fair(cfs_rq, se, flags); /* 中文：schedstat/调度统计 */
	se->on_rq = 1;

	if (cfs_rq->nr_queued == 1) {
		check_enqueue_throttle(cfs_rq); /* 中文：带宽节流检查 */
		list_add_leaf_cfs_rq(cfs_rq);   /* 中文：叶子 cfs_rq 参与负载均衡 */
	}
}
```

通常在更上层 `enqueue_task_fair` 里会先 **`place_entity`**（摆 vruntime/deadline），再 `enqueue_entity`。`ENQUEUE_WAKEUP` / `ENQUEUE_MIGRATED` 等 flags 影响统计与放置策略。

---

## 4.2 fair 出队：`dequeue_entity`

`fair.c:6655`：

- `update_load_avg`（睡眠路径可带 `UPDATE_UTIL_EST`）  
- `update_stats_dequeue_fair`  
- `se->on_rq = 0`；`account_entity_dequeue`  
- `return_cfs_rq_runtime`（带宽剩余归还）  
- 队列空时可能 `list_del_leaf_cfs_rq` / 记 throttle 时钟  

**延迟出队（`sched_delayed`）**：v7.3 里为减少锁竞争，允许某些路径「逻辑上要睡、实体稍后再从树上摘干净」——读 `dequeue` flags 里的 `DEQUEUE_DELAYED` 时不要和「任务已经 TASK_RUNNING」搞混。

---

## 4.3 唤醒主路径：`try_to_wake_up`

`kernel/sched/core.c:4274`

```text
try_to_wake_up(p, state, wake_flags)
  ├─ p == current？快路径 ttwu_do_wakeup
  ├─ 锁 p->pi_lock；检查 p->__state 是否匹配
  ├─ trace_sched_waking(p)          ← 观测：唤醒意图
  ├─ 若已在 rq 上 runnable：ttwu_runnable → 可能立刻抢占
  └─ 否则：
        select_task_rq(p, ...)      ← 选 CPU（亲和、负载、缓存）
        ttwu_queue / ttwu_queue_wakelist
          → ttwu_do_activate        ← enqueue + 激活
          → wakeup_preempt / resched_curr
        trace_sched_wakeup(p)       ← 观测：已入队可跑
```

`ttwu_do_activate`：`core.c:3828`。远程 CPU 上常用 **wakelist**（`TTWU_QUEUE`）把激活丢给目标 CPU，减锁持有时间——这是唤醒延迟的常见来源之一（「唤醒了但对方还没处理队列」）。

---

## 4.4 `NEED_RESCHED` 与抢占

| API | 位置 | 作用 |
|---|---|---|
| `resched_curr(rq)` | core.c:1241 | 给当前任务打上需重调度（`TIF_NEED_RESCHED`） |
| `resched_curr_lazy` | `:1267` | 懒抢占位（`TIF_NEED_RESCHED_LAZY`），降低无谓切换 |
| `set_nr_and_not_polling` | `:1058` 一带 | 对 idle 轮询路径的优化 |

谁在置位：tick 里 `update_curr` 发现 deadline 耗尽、唤醒路径 `wakeup_preempt` 认为新任务应抢当前、负载均衡迁走当前等。

真正切换发生在：返回用户态 / `preempt_enable` / 显式 `schedule()` ——见进程章 08。

---

## 4.5 唤醒延迟：从哪段代码开始算

**建议统一口径（分析时写清楚）：**

| 锚点 | trace / 统计 | 含义 |
|---|---|---|
| A. 唤醒意图 | `sched:sched_waking` | ttwu 已决定要醒它 |
| B. 可运行 | `sched:sched_wakeup` | 已 enqueue，状态 runnable |
| C. 上 CPU | `sched:sched_switch`（next=该任务） | 真正开始跑 |

- **队列延迟** ≈ C − B（可跑却在等 CPU）  
- **激活延迟** ≈ B − A（跨 CPU wakelist / 选核）  
- **端到端唤醒延迟** ≈ C − A  

`sched_info` / `schedstat`（若开）在 enqueue/arrive 路径记账，见 [08](./08-observability.md)。`bpftrace` 直方图配方见 [09](./09-perf-analysis.md) 与 [demos/](./demos/)。

---

## 4.6 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `place_entity` | fair.c:6383 | 入队前放置 ve/vd |
| `enqueue_entity` | `:6534` | fair 实体入队 |
| `dequeue_entity` | `:6655` | fair 实体出队 |
| `try_to_wake_up` | core.c:4274 | 唤醒 |
| `ttwu_do_activate` | `:3828` | 激活并入队 |
| `ttwu_queue_wakelist` | `:4079` | 远程队列优化 |
| `resched_curr` | `:1241` | 标记重调度 |
| `select_task_rq` | 各类实现 | 选目标 CPU |

---

**导航**：[← EEVDF](./03-eevdf.md) · [章节目录](./README.md) · [cgroup cpu 衔接 →](./07-cgroup-cpu.md)（阅读顺序）· [负载均衡 →](./05-load-balance.md)
