# 3. EEVDF：选谁跑

**结论**：v6.6.160 的 fair 类是 **EEVDF（Earliest Eligible Virtual Deadline First）**。可跑集合里先筛 **eligible（内核欠它服务，lag ≥ 0）**，再在合格者中取 **虚拟 deadline 最早** 的那个。红黑树仍按 **`vruntime`** 排序；每个节点额外存 `min_deadline`（自己和左右子树里最早的 deadline），所以找最早合格 deadline 是 O(log n)。

> 主文件：`kernel/sched/fair.c`  
> 关键符号：`avg_vruntime` `:671`、`entity_eligible` `:744`、`__pick_eevdf` `:881`、`pick_eevdf` `:975`、`update_deadline` `:1025`、`update_curr` `:1201`、`place_entity` `:5194`

---

## 3.1 四个量（先把词记住）

实体 `sched_entity` 上：

| 量 | 含义 |
|---|---|
| `vruntime`（ve） | 虚拟已执行进度。权重越大，同样的墙上时间加得越少 |
| `deadline`（vd） | 虚拟截止。请求没吃完时 `vd = ve + r/w` |
| `slice`（r） | 这次「请求」的墙上长度。变量 `sysctl_sched_base_slice`，默认 700000 纳秒（0.7 ms），启动时按在线 CPU 数放大 |
| `vlag` | 出队时记下的虚拟 lag（`V - v_i`）。入队 `place_entity` 用它把实体放回欠账的位置 |

权重怎么把墙上时间变成虚拟时间：`calc_delta_fair`（`fair.c:311`）。nice 0 的权重是 `NICE_0_LOAD` 时，虚拟增量就等于墙上增量；更重的任务虚拟时间涨得更慢。

`slice` 的用户态旋钮不在 `/proc/sys`，而在 debugfs：`/sys/kernel/debug/sched/base_slice_ns`（`kernel/sched/debug.c:350`，要 `CONFIG_SCHED_DEBUG`）。变量名仍叫 `sysctl_sched_base_slice`（`fair.c:78`）。`sched_init_granularity`（`:244`）用 `1 + ilog2(在线 CPU 数)`（最多按 8 个 CPU）乘上 700000。

和选人有关的 `sched_feat` 在 `kernel/sched/features.h`，运行时看 `/sys/kernel/debug/sched/features`：

| 特性 | 默认 | 干什么 |
|---|---|---|
| `PLACE_LAG` | 开 | 睡眠再醒时保住 lag，别一入队就被平均吃掉 |
| `PLACE_DEADLINE_INITIAL` | 开 | 新任务第一次入队只给半片 slice，好插进已经跑到一半的队伍 |
| `RUN_TO_PARITY` | 开 | 选中之后尽量跑完这一片，直到不再 eligible 或领了新 slice |
| `NEXT_BUDDY` | **关** | 醒了却没抢到 CPU 的任务，下次优先看看它。动的是延迟，不是长期公平 |
| `WAKEUP_PREEMPTION` | 开 | 唤醒时允许抢占当前任务 |

---

## 3.2 平均虚拟时间 `avg_vruntime`

理想公平调度器里，每个实体的 lag 是 `lag_i = w_i * (V - v_i)`。`V` 是全体 `vruntime` 的加权平均。源码把 `V` 拆成零点加偏移（`fair.c:624` 附近的注释）：

```text
v0                 = cfs_rq->min_vruntime
Σ (v_i - v0) * w_i = cfs_rq->avg_vruntime
Σ w_i              = cfs_rq->avg_load
```

正在跑的 `curr` 不在红黑树上，所以这两个和里没有它。函数现算时再加回去：

```c
/* fair.c:671 */
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {
		unsigned long weight = scale_load_down(curr->load.weight);

		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	if (load) {
		/* 负数时先减 (load - 1)，让除法朝 -∞ 取整，避免 V 偏大 */
		if (avg < 0)
			avg -= (load - 1);
		avg = div_s64(avg, load);
	}

	return cfs_rq->min_vruntime + avg; /* 这就是 V */
}
```

`entity_key`（`:572`）就是 `se->vruntime - cfs_rq->min_vruntime`，用有符号差避免 u64 直接相乘溢出。

---

## 3.3 合格性 `entity_eligible`

注释里的定义（`fair.c:727`）：**lag ≥ 0 就是 V ≥ v_i**。平均虚拟时间已经走到你前面（或持平），说明系统还欠你服务，你 eligible。一个 `vruntime` 很大、跑在平均前面的任务（负 lag）不合格，CPU 让给落后的。

除法会丢精度，所以比较时不先除，两边同乘权重（`:744`）：

```c
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {
		unsigned long weight = scale_load_down(curr->load.weight);

		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	return avg >= entity_key(cfs_rq, se) * load;
}
```

出队前把 lag 夹紧再存进 `se->vlag`（`:710`、`:720`）。夹紧范围是 `±calc_delta_fair(max(2*slice, TICK_NSEC), se)`，避免加减任务把 `V` 晃得太厉害，让一个人的 lag 越积越大。

---

## 3.4 更新 deadline 与当前任务记账

### `update_deadline`（`:1025`）

请求还没吃完（`vruntime` 仍小于 `deadline`）就直接返回。吃完了就领一片新的，并把 deadline 推到 `vruntime + 虚拟 slice`。队列里不只有自己时，调用 `resched_curr`，让当前任务尽快再参加一轮挑选。

```c
static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if ((s64)(se->vruntime - se->deadline) < 0)
		return;

	/*
	 * 虚拟时间的斜率由权重决定；
	 * 请求长度 r 来自 sysctl_sched_base_slice。
	 */
	se->slice = sysctl_sched_base_slice;

	/* vd_i = ve_i + r_i / w_i */
	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);

	if (cfs_rq->nr_running > 1) {
		resched_curr(rq_of(cfs_rq));
		clear_buddies(cfs_rq, se);
	}
}
```

### `update_curr`（`:1201`）

tick 和调度点都走这里，给正在跑的实体记一笔：

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = update_curr_se(rq_of(cfs_rq), curr); /* 墙上跑了多久 */
	if (unlikely(delta_exec <= 0))
		return;

	curr->vruntime += calc_delta_fair(delta_exec, curr);
	update_deadline(cfs_rq, curr);
	update_min_vruntime(cfs_rq);

	if (entity_is_task(curr))
		update_curr_task(task_of(curr), delta_exec); /* trace_sched_stat_runtime 等 */

	account_cfs_rq_runtime(cfs_rq, delta_exec); /* 带宽配额 */
}
```

`update_curr_fair`（`:1223`）只对 `rq->curr->se` 所在的那一个 `cfs_rq` 做这件事。组层次上每一层的 `curr` 在 `pick_next_task_fair` 往下走的时候各自更新。

`update_min_vruntime`（`:774`）取「当前实体」和「树里最左（vruntime 最小）」的较小值，并且只往前走。零点往前挪时，`avg_vruntime_update` 把已经记在 `avg_vruntime` 里的偏移一起减掉，平均结果不变。

---

## 3.5 红黑树：按 vruntime 排，用 min_deadline 当堆

`__entity_less`（`:800`）比较的是 `entity_before`，也就是 `a->vruntime < b->vruntime`。所以 `tasks_timeline` 的最左节点是 **vruntime 最小** 的实体，不是 deadline 最早的。

插入时（`__enqueue_entity`，`:837`）：

1. `avg_vruntime_add`：把 `(vruntime - min_vruntime) * weight` 加进 `avg_vruntime`，权重加进 `avg_load`
2. `se->min_deadline = se->deadline`
3. `rb_add_augmented_cached`，增强回调 `min_deadline_update`（`:819`）让每个节点的 `min_deadline` 等于自己和左右孩子的最小 deadline

正在跑的 `curr` 会在 `set_next_entity`（`:5440`）里从树上摘下来。它仍算 on_rq，只是不占树节点。算 `V` 和 eligible 时要单独加回去。

---

## 3.6 `pick_eevdf`（选实体）

`pick_next_entity`（`:5489`）先看 buddy：`NEXT_BUDDY` 开着、`cfs_rq->next` 存在且 eligible，就直接返回它。默认 `NEXT_BUDDY` 是关的，于是落到 `pick_eevdf`（`:975`）。`pick_eevdf` 只是给 `__pick_eevdf` 包一层：万一没选出来，打一行错误并退回最左节点。

`__pick_eevdf`（`:881`）的两段搜索：

```c
static struct sched_entity *__pick_eevdf(struct cfs_rq *cfs_rq)
{
	struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
	struct sched_entity *curr = cfs_rq->curr;
	struct sched_entity *best = NULL;
	struct sched_entity *best_left = NULL;

	/* curr 不在 rq 上，或已经不再 eligible，就不当候选人 */
	if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
		curr = NULL;
	best = curr;

	/*
	 * RUN_TO_PARITY：set_next_entity 把「选中那一刻的 deadline」塞进了 vlag。
	 * vlag 还等于 deadline，说明这一片还没被 update_deadline 换掉，继续跑它。
	 */
	if (sched_feat(RUN_TO_PARITY) && curr && curr->vlag == curr->deadline)
		return curr;

	while (node) {
		struct sched_entity *se = __node_2_se(node);

		/* 这个节点自己不合格，合格的只可能在 vruntime 更小的左子树 */
		if (!entity_eligible(cfs_rq, se)) {
			node = node->rb_left;
			continue;
		}

		/* 合格者里留 deadline 更早的 */
		if (!best || deadline_gt(deadline, best, se))
			best = se;

		/*
		 * 左子树里每个节点 vruntime 更小，所以如果本节点合格，
		 * 左子树全都合格。记下左子树里 min_deadline 最好的那支。
		 */
		if (node->rb_left) {
			struct sched_entity *left = __node_2_se(node->rb_left);

			if (!best_left || deadline_gt(min_deadline, best_left, left))
				best_left = left;

			/* 本节点的 min_deadline 就在左子树里，不用再看右边 */
			if (left->min_deadline == se->min_deadline)
				break;
		}

		/* min_deadline 就是自己，不用往下 */
		if (se->deadline == se->min_deadline)
			break;

		/* 否则最早 deadline 在右子树 */
		node = node->rb_right;
	}

	/* 没有更好的左支，或者 best 的 deadline 比那支的 min_deadline 还早 */
	if (!best_left || (s64)(best_left->min_deadline - best->deadline) > 0)
		return best;

	/*
	 * best_left 整棵子树都合格，只要沿着 min_deadline 往下走到
	 * deadline == min_deadline 的那个节点。
	 */
	node = &best_left->run_node;
	while (node) {
		struct sched_entity *se = __node_2_se(node);

		if (se->deadline == se->min_deadline)
			return se;

		if (node->rb_left &&
		    __node_2_se(node->rb_left)->min_deadline == se->min_deadline) {
			node = node->rb_left;
			continue;
		}

		node = node->rb_right;
	}
	return NULL;
}
```

`set_next_entity`（`:5455`）里的 HACK：实体还在树上时，先把它摘下来，再执行 `se->vlag = se->deadline`。`vlag` 要到出队才重新写成真正的 lag，所以这一段时间里 `vlag == deadline` 就表示「这一片还没换过」。这就是 `RUN_TO_PARITY` 的判据。

---

## 3.7 放置 `place_entity`（入队时摆 vruntime / deadline）

`fair.c:5194`。它由 `enqueue_entity` 调用，不由 `enqueue_task_fair` 单独先调一次。当前正在跑的实体在 `update_curr` 之前摆；其它实体在改完权重之后摆（见 [04](./04-enqueue-wakeup.md)）。

```c
static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 vslice, vruntime = avg_vruntime(cfs_rq);
	s64 lag = 0;

	se->slice = sysctl_sched_base_slice;
	vslice = calc_delta_fair(se->slice, se);

	/*
	 * PLACE_LAG 且队列里已经有人：
	 * 新实体自己会把 V 往后拉，lag 会缩水。
	 * 先按 vl_i = (W + w_i) * vl'_i / W 把 lag 放大，
	 * 放进去之后剩下的才是出队时记下的那份。
	 */
	if (sched_feat(PLACE_LAG) && cfs_rq->nr_running) {
		struct sched_entity *curr = cfs_rq->curr;
		unsigned long load;

		lag = se->vlag;

		load = cfs_rq->avg_load;
		if (curr && curr->on_rq)
			load += scale_load_down(curr->load.weight);

		lag *= load + scale_load_down(se->load.weight);
		if (WARN_ON_ONCE(!load))
			load = 1;
		lag = div_s64(lag, load);
	}

	se->vruntime = vruntime - lag;

	/* 新任务：别人平均已经跑了半片，自己也从半片开始，免得一上来 deadline 特别近 */
	if (sched_feat(PLACE_DEADLINE_INITIAL) && (flags & ENQUEUE_INITIAL))
		vslice /= 2;

	/* vd_i = ve_i + r_i / w_i */
	se->deadline = se->vruntime + vslice;
}
```

正的 `vlag` 让 `vruntime = V - lag` 落在平均后面，deadline 也更近，醒来更容易被选中。这是睡眠公平和唤醒延迟之间的桥。出队时 `dequeue_entity` 先 `update_entity_lag` 再把 `on_rq` 清掉。

---

## 3.8 延迟和吞吐，各自靠什么

| 你想要的 | 机制上发生什么 |
|---|---|
| 交互任务快响 | 睡醒时 lag 为正 → vruntime 落在平均后面，deadline 更早；`WAKEUP_PREEMPTION` 允许抢占 |
| 少一点无谓切换 | `RUN_TO_PARITY` 让当前这片跑完再换人 |
| 批处理别老被切 | nice 更优先（权重更大）→ 虚拟时间涨得慢。`SCHED_BATCH` 在 `check_preempt_wakeup_fair`（`:8382`）里不抢占 `SCHED_NORMAL` |
| 尾延迟毛刺 | 查：是否 lag 为负被判不合格、是否迁走、是否 cgroup 限流、是否 RT 抢占 |

---

## 3.9 关键函数表

| 函数 | 行号 | 作用 |
|---|---|---|
| `calc_delta_fair` | `fair.c:311` | 墙上时间 → 虚拟时间 |
| `entity_key` | `:572` | `vruntime - min_vruntime` |
| `avg_vruntime_add` / `avg_vruntime_sub` | `:639` / `:649` | 维护 `avg_vruntime` 与 `avg_load` |
| `avg_vruntime` | `:671` | 算出 V |
| `entity_lag` | `:710` | 计算并夹紧虚拟 lag |
| `update_entity_lag` | `:720` | 出队前写入 `se->vlag` |
| `entity_eligible` | `:744` | lag ≥ 0 的整数比较 |
| `update_min_vruntime` | `:774` | 推进零点 v0 |
| `__enqueue_entity` / `__dequeue_entity` | `:837` / `:845` | 红黑树插入 / 删除 |
| `__pick_first_entity` | `:852` | 最左，vruntime 最小 |
| `__pick_eevdf` | `:881` | EEVDF 搜索 |
| `pick_eevdf` | `:975` | 搜索失败时退回最左 |
| `update_deadline` | `:1025` | 请求吃完后重算 vd，并 `resched_curr` |
| `update_curr` | `:1201` | 推进当前实体的 vruntime |
| `update_curr_fair` | `:1223` | `sched_class.update_curr`，只更新当前任务那一层 |
| `reweight_entity` | `:3851` | 改权重时按比例改 lag 或 deadline |
| `place_entity` | `:5194` | 入队放置 ve / vd |
| `enqueue_entity` / `dequeue_entity` | `:5300` / `:5387` | 入出队 |
| `set_next_entity` | `:5440` | 选中：摘下树，`vlag = deadline` |
| `pick_next_entity` | `:5489` | 先看 `NEXT_BUDDY`，再 `pick_eevdf` |
| `put_prev_entity` | `:5504` | 把当前实体放回树 |

---

**导航**：[← 核心对象](./02-core-objects.md) · [章节目录](./README.md) · [入队与唤醒 →](./04-enqueue-wakeup.md)
