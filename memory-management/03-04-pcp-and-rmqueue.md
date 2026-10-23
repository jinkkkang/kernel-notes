# 3.4 PCP 与 `rmqueue`

**结论**：order 0 和「便宜」的高阶走 per-CPU 页。热路径不碰 `zone->lock`。列表空了才在一把 zone 锁里批量 `__rmqueue`。PCP 没有，或者 order 不允许，就直接 `rmqueue_buddy`。

> `rmqueue` 在 `mm/page_alloc.c:3413`。`struct per_cpu_pages` 在 `include/linux/mmzone.h:844`。
> 导航：[上一篇 水位](./03-03-fastpath-freelist.md) · [本目录](./03-00-page-alloc.md) · [下一篇 释放](./03-05-buddy-free.md)

---

## `struct per_cpu_pages`（`mmzone.h:844`）

每个 CPU、每个 zone 一份。`____cacheline_aligned_in_smp`，避免和旁边的计数抢同一条缓存线。

```c
struct per_cpu_pages {
	/*
	 * 保护下面的 lists。
	 * 这是 PCP 自己的锁，不是 zone->lock。
	 */
	spinlock_t lock;
	int count;		/* 列表里现在有多少页 */
	int high;		/* 高水位。超过就要倒回 buddy */
	int high_min;		/* high 允许落到的下限 */
	/*
	 * high 允许涨到的上限。分配忙时 alloc_factor 可以把 high 往上抬，
	 * 闲时 decay_pcp_high 再把它降下来。
	 */
	int high_max;
	int batch;		/* 一次向 buddy 批量申请或归还的大小 */
	/*
	 * 由 pcp->lock 保护。
	 * PCPF_PREV_FREE_HIGH_ORDER（mmzone.h:841）：上一次释放是高阶，
	 * 避免一次偶然的高阶释放就把 PCP 倒空。
	 * PCPF_FREE_HIGH_BATCH（:842）：连续高阶释放、中间没有分配时，
	 * 先留住 batch 页再倒，减少 zone 锁竞争，也让缓存热的页还能被复用。
	 */
	u8 flags;
	u8 alloc_factor;	/* 分配时放大 batch 的因子 */
#ifdef CONFIG_NUMA
	/*
	 * 减到 0 时，远程 CPU 的 pageset 被排空。
	 * 本地 CPU 的页留着，远端滞留的页还回。
	 */
	u8 expire;
#endif
	/*
	 * 连续释放计数。用来识别「只还不要」的模式，
	 * 配合上面的高阶标志决定要不要提前倒。
	 */
	short free_count;

	/*
	 * 按迁移类型分链。低阶是
	 * MIGRATE_PCPTYPES * (PAGE_ALLOC_COSTLY_ORDER + 1)
	 * （mmzone.h:826）。CONFIG_TRANSPARENT_HUGEPAGE 时再加两条
	 * THP 链（:821-827）：一条给 GFP_MOVABLE，一条给
	 * GFP_UNMOVABLE 和 GFP_RECLAIMABLE。
	 */
	struct list_head lists[NR_PCP_LISTS];
} ____cacheline_aligned_in_smp;
```

`PAGE_ALLOC_COSTLY_ORDER` 是 3。低阶 PCP 覆盖 order 0 到 3。

---

## `rmqueue` 怎么选路

```text
rmqueue                               # page_alloc.c:3413
  若 pcp_allowed_order(order)：
    rmqueue_pcplist                   # :3371
      __rmqueue_pcplist               # :3326
        列表空 → rmqueue_bulk         # :2535，一次拿 batch 页，只拿一次 zone->lock
          __rmqueue                   # :2466
  PCP 没有（或 order 不允许）：
    rmqueue_buddy                     # :3221
      __rmqueue
  若 zone 被 boost 过水位：
    wakeup_kswapd                     # vmscan.c:7573
```

`pcp_allowed_order`：order 0，以及「便宜」的高阶（含 THP 用的 PCP 链）。

`__rmqueue` 在 **已经拿着 `zone->lock`** 时做真正的摘链。模式（`*mode` 会被记住，因为 `rmqueue_bulk` 在同一把锁里循环）：

| 模式 | 做什么 |
|---|---|
| `RMQUEUE_NORMAL` | `__rmqueue_smallest`（`:1901`）：从目标 order 往高阶找，拆 buddy |
| `RMQUEUE_CMA` | 调用者允许 `ALLOC_CMA` 时，`__rmqueue_cma_fallback` |
| `RMQUEUE_CLAIM` | `__rmqueue_claim`（`:2373`）：按 fallback 把别的 migratetype 的空 pageblock 整块认领过来 |
| `RMQUEUE_STEAL` | 没有 `ALLOC_NOFRAGMENT` 时 `__rmqueue_steal`（`:2428`）：从别的类型偷页，这会制造外碎片 |

另外，CMA 页超过该 zone 空闲页的一半时，可移动分配会先从 CMA 拿（`:2477`），免得 CMA 区囤着不用、普通区先碎掉。CMA 区本身见 [09](./03-09-cma.md)。

拆页在 `__rmqueue_smallest` 里完成：高阶块一切二，另一半挂回 `free_area[order]`。归还时的合并在 [05](./03-05-buddy-free.md) 的 `__free_one_page`。

`set_buddy_order` 把 order 写进 `page->private`。这就是 [02 物理布局](./02-physical-layout.md) 里 `private` 在 `PageBuddy` 时的含义。阶和拆分的图在 [02 §2.5](./02-physical-layout.md#25-buddy-概念)。

`reserve_highatomic_pageblock`（`:3447`）给原子高阶分配留 pageblock，大约占 zone 的 1%。压力大时 `unreserve_highatomic_pageblock`（`:3496`）会吐出来。这块保留是高阶失败的原因之一，见 [07](./03-07-high-order-and-compaction.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `rmqueue` | `mm/page_alloc.c:3413` | PCP 或 buddy |
| `rmqueue_pcplist` | `:3371` | 每 CPU 列表 |
| `__rmqueue_pcplist` | `:3326` | 空了就批量补 |
| `rmqueue_bulk` | `:2535` | 持锁批量 `__rmqueue` |
| `rmqueue_buddy` | `:3221` | 不走 PCP |
| `__rmqueue` | `:2466` | smallest / cma / claim / steal |
| `__rmqueue_smallest` | `:1901` | 拆高阶 |
| `__rmqueue_claim` | `:2373` | 认领 pageblock |
| `__rmqueue_steal` | `:2428` | 偷页 |
| `decay_pcp_high` | `:2576` | 闲时降低 PCP high |
| `drain_zone_pages` | `:2615` | 把一个 PCP 倒回 zone |
| `reserve_highatomic_pageblock` | `:3447` | 高阶原子保留 |
| `unreserve_highatomic_pageblock` | `:3496` | 压力下归还保留 |

---

[上一篇 水位](./03-03-fastpath-freelist.md) · [本目录](./03-00-page-alloc.md) · [下一篇 释放与合并](./03-05-buddy-free.md)
