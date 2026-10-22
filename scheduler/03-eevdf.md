# 3. EEVDF：选谁跑

**结论**：v7.3 的 fair 类已是 **EEVDF（Earliest Eligible Virtual Deadline First）**，不再是「单纯比 `vruntime` 谁小」。可跑集合里先筛 **eligible（内核欠它服务）**，再在合格者中取 **虚拟 deadline 最早** 的那个。树按 deadline 排序，并用子树 `min_vruntime` 做合格性剪枝，所以是 O(log n)。

> 主文件：`kernel/sched/fair.c`  
> 关键符号：`avg_vruntime` `:749`、`entity_eligible` `:967`、`pick_eevdf` `:1187`、`update_deadline` `:1289`、`update_curr` `:2223`、`place_entity` `:6383`

---

## 3.1 从 CFS 到 EEVDF（直觉）

| 旧 CFS 口头禅 | EEVDF 口头禅 |
|---|---|
| 谁 `vruntime` 小谁优先 | 谁 **既 eligible 又 deadline 最早** 谁优先 |
| 用 min_vruntime 做基准 | 用加权平均虚拟时间 **V**（`avg_vruntime`）做基准 |
| nice → 权重 → vruntime 斜率 | 仍然：权重影响虚拟时间斜率；**slice** 影响 deadline 远近 |

核心量（实体 `sched_entity`）：

| 量 | 含义 |
|---|---|
| `vruntime` (ve) | 虚拟已执行进度 |
| `deadline` (vd) | 虚拟截止：`vd ≈ ve + r/w`（见 `update_deadline`） |
| `slice` (r) | 本次「请求」时长，默认 `sysctl_sched_base_slice` |
| `vlag` | 出队时记住的 virtual lag，入队 `place_entity` 用来摆位置 |
| `vprot` | 保护量子，避免刚上 CPU 立刻被抢（`protect_slice`） |

---

## 3.2 平均虚拟时间 `avg_vruntime`

`fair.c:749`（节选加注）：

```c
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	long weight = cfs_rq->sum_weight;
	s64 delta = 0;
	/* ... 把 curr 若 on_rq 也并入加权和 ... */
	if (weight) {
		s64 runtime = cfs_rq->sum_w_vruntime;
		/* 中文：delta ≈ Σ w_i*(v_i - v0) / Σ w_i */
		delta = div64_long(runtime, weight);
	} else if (curr) {
		delta = curr->vruntime - cfs_rq->zero_vruntime;
	}
	update_zero_vruntime(cfs_rq, delta);
	return cfs_rq->zero_vruntime; /* 中文：返回更新后的 V（经 zero_vruntime） */
}
```

`cfs_rq` 上缓存 `sum_w_vruntime` / `sum_weight` / `zero_vruntime`，避免每次从头扫树。

---

## 3.3 合格性 `entity_eligible`

注释里的定义（约 `:910`）：**lag ≥ 0 ⇒ V ≥ v_i** —— 平均虚拟时间已经走到你前面（或持平），说明系统还「欠」你服务，你 eligible。

```c
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return vruntime_eligible(cfs_rq, se->vruntime); /* :967 */
}
```

`vruntime_eligible`（`:922`）用整数乘法比较，避免先做除法丢精度；64-bit 上还要小心 `__int128` / overflow。

**读代码时别写反**：eligible ≠「vruntime 最小」；一个 vruntime 很大的任务若远远超前（负 lag），会被判不合格，把 CPU 让给落后的。

---

## 3.4 更新 deadline 与当前任务记账

### `update_deadline`（`:1289`）

```c
static bool update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (vruntime_cmp(se->vruntime, "<", se->deadline))
		return false; /* 中文：还没吃完这一份请求 */

	if (!se->custom_slice)
		se->slice = sysctl_sched_base_slice;

	/* 中文：vd_i = ve_i + r_i/w_i （calc_delta_fair 把墙时钟 slice 映到虚拟域） */
	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
	avg_vruntime(cfs_rq);
	return true; /* 中文：请求耗尽 → 调用方应考虑 resched */
}
```

### `update_curr`（`:2223`）

每次 tick / 调度点推进当前实体：

1. `delta_exec = update_se(...)` 算实际跑了多久  
2. `curr->vruntime += calc_delta_fair(delta_exec, curr)`  
3. `resched = update_deadline(...)`  
4. 若该 resched 或不再受 `protect_slice` 保护 → `resched_curr_lazy(rq)`

组调度时 `update_curr_fair` 会 `for_each_sched_entity` 从叶子向上逐层 `update_curr`。

---

## 3.5 `pick_eevdf`（选实体）

`fair.c:1187` —— 建议整函数对照源码读一遍。逻辑骨架：

```c
static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq, bool protect)
{
	struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
	struct sched_entity *se = __pick_first_entity(cfs_rq); /* 中文：deadline 最左 */
	struct sched_entity *curr = cfs_rq->curr;
	struct sched_entity *best = NULL;

	if (cfs_rq->h_nr_queued == 1) /* 中文：单实体快路径 */
		return curr && curr->on_rq ? curr : se;

	/* 中文：buddy（next）在保护模式下可优先，换延迟不换公平 */
	if (sched_feat(PICK_BUDDY) && protect &&
	    cfs_rq->next && entity_eligible(cfs_rq, cfs_rq->next))
		return cfs_rq->next;

	if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
		curr = NULL;

	if (curr && protect && protect_slice(curr))
		return curr; /* 中文：最小量子保护，刚跑别立刻抢走 */

	if (se && entity_eligible(cfs_rq, se)) {
		best = se;
		goto found;
	}

	/* 中文：在 RB 上堆式搜索：左子树若有 eligible 则必更优（deadline 更早） */
	while (node) {
		struct rb_node *left = node->rb_left;
		if (left && vruntime_eligible(cfs_rq,
					__node_2_se(left)->min_vruntime)) {
			node = left;
			continue;
		}
		se = __node_2_se(node);
		if (entity_eligible(cfs_rq, se)) {
			best = se;
			break;
		}
		node = node->rb_right;
	}
found:
	if (!best || (curr && entity_before(curr, best)))
		best = curr;
	return best;
}
```

**延迟 vs 吞吐**：`protect=true` 时更护着当前量子（降切换、护尾延迟）；强制选人时可以 `protect=false`。buddy（`cfs_rq->next`）是「刚唤醒的相关任务」提示，**动的是延迟，不是长期公平**——源码注释写得很直白。

---

## 3.6 放置 `place_entity`（入队时摆 vruntime/deadline）

`fair.c:6383` 起。入队时：

1. 取 `vruntime = avg_vruntime(cfs_rq)` 作基准  
2. 默认 `se->slice = sysctl_sched_base_slice`  
3. 若 `PLACE_LAG` 且有历史 `vlag`：按注释里的公式 **放大 lag** 再放入，避免「一入队 lag 被平均掉」  
4. 由此设置 `se->vruntime`，并推 `deadline`

出队前会把 lag 记进 `se->vlag`，睡眠再醒才能「接着欠的账」——这是 wakeup 延迟与公平之间的桥梁。细节与 `enqueue_entity` 一起看 [04](./04-enqueue-wakeup.md)。

---

## 3.7 和「延迟友好 / 吞吐」怎么对照

| 你想要的 | 机制上发生什么 |
|---|---|
| 交互任务快响 | 短 slice / 正 lag → eligible + 早 deadline；buddy；保护量子别抖 |
| 批处理别老被切 | 更大权重（nice 低）→ 虚拟时间涨得慢；或 `SCHED_BATCH` |
| 尾延迟毛刺 | 查：是否不 eligible 被压、是否迁移、是否 cgroup throttle、是否 RT 抢占 |

**不要**再拿「CFS 红黑树按 vruntime 排序」的旧图去硬套 v7.3——树键是 **deadline**，eligible 靠 `min_vruntime` 堆属性剪枝。

---

## 3.8 关键函数表

| 函数 | 行号（约） | 作用 |
|---|---|---|
| `avg_vruntime` | fair.c:749 | 计算/更新 V |
| `entity_lag` / 更新 `vlag` | `:865` 一带 | lag 记账 |
| `vruntime_eligible` | `:922` | 合格性核心比较 |
| `entity_eligible` | `:967` | 对 se 的包装 |
| `pick_eevdf` | `:1187` | 选下一个实体 |
| `update_deadline` | `:1289` | 耗尽请求后重算 vd |
| `update_curr` | `:2223` | 推进当前实体账本 |
| `place_entity` | `:6383` | 入队放置 |
| `enqueue_entity` / `dequeue_entity` | `:6534` / `:6655` | 入出队 |

---

**导航**：[← 核心对象](./02-core-objects.md) · [章节目录](./README.md) · [入队与唤醒 →](./04-enqueue-wakeup.md)
