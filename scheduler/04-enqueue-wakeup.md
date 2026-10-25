# 4. 入队、出队与唤醒

**结论**：任务要跑，必须出现在某 CPU `rq` 的对应类队列里。fair 路径上，**`enqueue_entity` / `dequeue_entity`** 维护 `cfs_rq` 账本与红黑树；睡眠出队、唤醒再入队走 **`try_to_wake_up`（ttwu）**。性能上的「唤醒延迟」从 ttwu 打出 `sched_waking` 开始算，到第一次 `sched_switch` 把该任务选为 next 结束。

---

## 4.1 fair 入队：`enqueue_task_fair` → `enqueue_entity`

`enqueue_task_fair`（`fair.c:6750`）从任务的 `se` 出发，`for_each_sched_entity` 向上。某一层已经 `on_rq` 就停；否则 `enqueue_entity`。遇到被限流的 `cfs_rq` 也停，不再把祖先算进 `rq->nr_running`。

`place_entity` 在 **`enqueue_entity` 里面**，不在它前面单独打一次。正在跑的实体要先摆位再 `update_curr`（否则账记到旧的 vruntime 上）；其它实体先改权重，再摆位。

```c
/* fair.c:5300，骨架与源码调用顺序一致 */
static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool curr = cfs_rq->curr == se;

	if (curr)
		place_entity(cfs_rq, se, flags);	/* 当前实体：先摆再记账 */

	update_curr(cfs_rq);				/* 把正在跑的人的账补到现在 */

	update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH); /* PELT 对齐到现在 */
	se_update_runnable(se);			/* 组实体：刷新 runnable_weight */
	update_cfs_group(se);				/* 按 shares 重算组实体权重 */

	if (!curr)
		place_entity(cfs_rq, se, flags);	/* 非当前：权重量好后再摆 ve/vd */

	account_entity_enqueue(cfs_rq, se);		/* load 加权重，nr_running++，任务挂上 cfs_tasks */

	if (flags & ENQUEUE_MIGRATED)
		se->exec_start = 0;			/* 换过 CPU，不再当成 cache-hot */

	check_schedstat_required();
	update_stats_enqueue_fair(cfs_rq, se, flags);	/* schedstat：开始等 CPU / 结束睡眠 */
	if (!curr)
		__enqueue_entity(cfs_rq, se);		/* 插进按 vruntime 排序的红黑树 */
	se->on_rq = 1;

	if (cfs_rq->nr_running == 1) {
		check_enqueue_throttle(cfs_rq);		/* 带宽：这一层该不该马上限流 */
		if (!throttled_hierarchy(cfs_rq))
			list_add_leaf_cfs_rq(cfs_rq);	/* 叶子队列参与负载均衡 */
		/* 已经在限流层次里：记下 throttled_clock / throttled_clock_self */
	}
}
```

`account_entity_enqueue`（`:3642`）做的是 `cfs_rq->load`、`nr_running`、以及 SMP 上 `rq->cfs_tasks`。红黑树和 `avg_vruntime` 在 `__enqueue_entity`。当前实体本来就不在树上，所以 `curr` 为真时不调用它。

`ENQUEUE_WAKEUP` 让 `update_stats_enqueue_fair` 去结清睡眠时间。`ENQUEUE_MIGRATED` 清 `exec_start`。`ENQUEUE_INITIAL` 让 `place_entity` 把第一片 slice 砍半（`PLACE_DEADLINE_INITIAL`）。

---

## 4.2 fair 出队：`dequeue_entity`

`dequeue_task_fair`（`:6843`）对每一层调用 `dequeue_entity`（`:5387`），顺序是：

1. `update_curr`：先把当前账补上  
2. `update_load_avg`。任务正在迁移（`task_on_rq_migrating`）时带 `DO_DETACH`  
3. `update_stats_dequeue_fair`：结束「等 CPU」；`DEQUEUE_SLEEP` 时按 `__state` 记下 sleep 或 block 起点  
4. `clear_buddies`：它若是 `cfs_rq->next`，把 buddy 清掉  
5. `update_entity_lag`：此时还 `on_rq`，把夹紧后的 `V - vruntime` 写入 `se->vlag`  
6. 不是 `curr` 的话 `__dequeue_entity` 从树上摘下  
7. `se->on_rq = 0`，`account_entity_dequeue`  
8. `return_cfs_rq_runtime`：把没用完的带宽额度还回组的全局池  
9. `update_cfs_group`  
10. 除非这次是 `DEQUEUE_SAVE` 且没有 `DEQUEUE_MOVE`（马上要原样放回），否则 `update_min_vruntime`  
11. 队列空了：`update_idle_cfs_rq_clock_pelt`，PELT 时钟停在 idle  

睡眠时如果父实体还在，`dequeue_task_fair`（`:6875`）可能 `set_next_buddy`：希望下一次挑选留在同一个 cgroup。`NEXT_BUDDY` 默认关，这个提示先记着，特性打开才生效。

---

## 4.3 唤醒主路径：`try_to_wake_up`

`kernel/sched/core.c:4213`。返回值表示这次有没有真的把状态改成可跑。

```text
try_to_wake_up(p, state, wake_flags)          core.c:4213
  ├─ p == current
  │     ttwu_state_match 成功
  │     trace_sched_waking(p)                 ← 观测点 A
  │     ttwu_do_wakeup(p)                     ← 置 TASK_RUNNING，trace_sched_wakeup（点 B）
  ├─ 锁 p->pi_lock
  │     ttwu_state_match 失败则什么都不做
  │     trace_sched_waking(p)                 ← 点 A
  │     已经 on_rq：ttwu_runnable            ← 可能 wakeup_preempt，然后点 B
  └─ 否则（SMP）：
        p->__state = TASK_WAKING
        对方 CPU 还在 schedule 里用着 p，且 ttwu_queue_wakelist 成功
            → 挂到目标 CPU 的 wake_list，本函数返回
            → 目标 CPU 上 sched_ttwu_pending（:3880）再 ttwu_do_activate
        否则等 p->on_cpu 落下
        select_task_rq（:3636）               ← 选 CPU
        换了 CPU：set_task_cpu，wake_flags 带上 WF_MIGRATED
        ttwu_queue（:4029）
            ├─ 再次尝试 ttwu_queue_wakelist（:4009，特性 TTWU_QUEUE）
            └─ 否则锁目标 rq，ttwu_do_activate（:3777）
                  activate_task → enqueue（ENQUEUE_WAKEUP）
                  wakeup_preempt → fair 里是 check_preempt_wakeup_fair
                  ttwu_do_wakeup → 点 B：trace_sched_wakeup
```

`ttwu_do_wakeup`（`:3770`）本身只做两件事：`p->__state = TASK_RUNNING`，然后 `trace_sched_wakeup(p)`。入队发生在它之前的 `activate_task`。

远程 wakelist 受 `sched_feat(TTWU_QUEUE)` 控制。`features.h:38`：`CONFIG_PREEMPT_RT` 下默认关，否则默认开。开着时，唤醒方把任务挂到目标 CPU，自己少持有对方的 `rq` 锁；代价是目标 CPU 还没处理队列之前，任务已经「决定要醒」但还没进红黑树。这是激活延迟（下面的 B − A）的常见来源。

`check_preempt_wakeup_fair`（`fair.c:8323`）在 `WAKEUP_PREEMPTION` 开着时决定要不要 `resched_curr`。当前实体是 `SCHED_IDLE`（或 idle 组）、醒来的不是，就直接抢。两边都不是 idle 时，只有 `SCHED_NORMAL` 会继续用 `pick_eevdf` 比较；`SCHED_BATCH` 到这里就返回，不抢占 `SCHED_NORMAL`。

---

## 4.4 `NEED_RESCHED` 与抢占

| API | 位置 | 作用 |
|---|---|---|
| `resched_curr(rq)` | `core.c:1055` | 本 CPU：`set_tsk_need_resched` + `set_preempt_need_resched`。远程 CPU：`set_nr_and_not_polling`（`:918`）成功后发 reschedule IPI；失败则 `trace_sched_wake_idle_without_ipi` |
| `set_nr_and_not_polling` | `core.c:918` / `:947` | `:918` 在 `CONFIG_SMP` 且有 `TIF_POLLING_NRFLAG` 时用 `cmpxchg` 置位并看对方是否在轮询；`:947` 的 `#else` 只置 `TIF_NEED_RESCHED` |

谁在置位：`update_deadline` 发现请求吃完且 `nr_running > 1`、唤醒路径 `wakeup_preempt` 认为新任务该上、负载均衡把当前任务迁走。

真正切换发生在：返回用户态、`preempt_enable`、显式 `schedule()`。见进程章 08。`__schedule`（`core.c:6600`）开头持 `rq` 锁、`update_rq_clock`，若前一个任务的状态不是 `TASK_RUNNING` 且这次不是抢占，就 `deactivate_task(..., DEQUEUE_SLEEP)`，然后 `pick_next_task`。

---

## 4.5 唤醒延迟：从哪段代码开始算

分析时把三个锚点写清楚：

| 锚点 | trace / 统计 | 含义 |
|---|---|---|
| A. 唤醒意图 | `sched:sched_waking` | ttwu 已决定要醒它（`core.c:4233` / `:4249`） |
| B. 可运行 | `sched:sched_wakeup` | `ttwu_do_wakeup` 已把状态写成 `TASK_RUNNING`（`:3773`） |
| C. 上 CPU | `sched:sched_switch` 的 next 是该任务 | `__schedule` 里 `trace_sched_switch`（`:6719`） |

- **队列延迟** ≈ C − B（可跑却在等 CPU）  
- **激活延迟** ≈ B − A（选核、跨 CPU wakelist）  
- **端到端唤醒延迟** ≈ C − A  

走 wakelist 时，B 发生在目标 CPU 的 `sched_ttwu_pending`，不是唤醒方的调用栈里。`sched_info` / schedstat（若编译并打开）在入队和上 CPU 时记账，见 [08](./08-observability.md)。直方图配方见 [09](./09-perf-analysis.md) 与 [demos/](./demos/)。

---

## 4.6 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `place_entity` | `fair.c:5194` | 入队时放置 ve/vd |
| `enqueue_entity` | `:5300` | fair 实体入队 |
| `dequeue_entity` | `:5387` | fair 实体出队，并写下 `vlag` |
| `enqueue_task_fair` | `:6750` | 从叶子向上入队 |
| `dequeue_task_fair` | `:6843` | 从叶子向上出队 |
| `account_entity_enqueue` | `:3642` | `load` / `nr_running` / `cfs_tasks` |
| `__enqueue_entity` | `:837` | 插入红黑树并更新 `avg_vruntime` |
| `check_preempt_wakeup_fair` | `:8323` | 唤醒后是否 `resched_curr` |
| `try_to_wake_up` | `core.c:4213` | 唤醒 |
| `ttwu_do_wakeup` | `:3770` | 置 `TASK_RUNNING`，打 `sched_wakeup` |
| `ttwu_do_activate` | `:3777` | enqueue + `wakeup_preempt` + 上面的 wakeup |
| `ttwu_queue` | `:4029` | 直接激活，或丢进 wakelist |
| `ttwu_queue_wakelist` | `:4009` | `TTWU_QUEUE` 打开时的远程队列 |
| `sched_ttwu_pending` | `:3880` | 目标 CPU 上把 wakelist 真正激活 |
| `select_task_rq` | `:3636` | 调类的 `select_task_rq` |
| `resched_curr` | `:1055` | 标记重调度 |
| `__schedule` | `:6600` | 睡眠出队 + `pick_next_task` |

---

**导航**：[← EEVDF](./03-eevdf.md) · [章节目录](./README.md) · [cgroup cpu 衔接 →](./07-cgroup-cpu.md)（阅读顺序）· [负载均衡 →](./05-load-balance.md)
