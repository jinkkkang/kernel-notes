# 3.4 PCP 与 `rmqueue`

**结论**：order 0 到 3，以及开了透明大页时等于 `pageblock_order` 的那一档，走 per-CPU 页。热路径先拿 PCP 自己的锁，不碰 `zone->lock`。列表空了才在一把 zone 锁里批量 `__rmqueue`。PCP 锁拿不到，或者这一阶根本不进 PCP，就直接 `rmqueue_buddy`。

> `rmqueue` 在 `mm/page_alloc.c:2817`。`struct per_cpu_pages` 在 `include/linux/mmzone.h:684`。
> 内核版本：**v6.6.160**。
> 导航：[上一篇 水位](./03-03-fastpath-freelist.md) · [本目录](./03-00-page-alloc.md) · [下一篇 释放](./03-05-buddy-free.md)

---

## `struct per_cpu_pages`（`mmzone.h:684`）

每个 CPU、每个 zone 一份，挂在 `zone->per_cpu_pageset`（`mmzone.h:839`）。结构体带 `____cacheline_aligned_in_smp`，避免和旁边的计数抢同一条缓存线。

```c
struct per_cpu_pages {
	spinlock_t lock;	/* 保护 lists。这是 PCP 自己的锁，不是 zone->lock */
	int count;		/* 列表里现在有多少页 */
	int high;		/* 高水位。达到就要倒回 buddy */
	int batch;		/* 一次向 buddy 批量申请或归还的大小 */
	short free_factor;	/* 连续只释放、不分配时，放大一次倒回的页数 */
#ifdef CONFIG_NUMA
	/*
	 * 减到 0 时，vmstat 更新会排空这个 CPU 在远端 node 上的 pageset。
	 * 本地 CPU 的页留着。
	 */
	short expire;
#endif
	/* 按迁移类型和阶分链。条数是 NR_PCP_LISTS */
	struct list_head lists[NR_PCP_LISTS];
} ____cacheline_aligned_in_smp;
```

链的条数（`mmzone.h:671-677`）：

```c
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define NR_PCP_THP 2
#else
#define NR_PCP_THP 0
#endif
#define NR_LOWORDER_PCP_LISTS (MIGRATE_PCPTYPES * (PAGE_ALLOC_COSTLY_ORDER + 1))
#define NR_PCP_LISTS (NR_LOWORDER_PCP_LISTS + NR_PCP_THP)
```

`PAGE_ALLOC_COSTLY_ORDER` 是 3（`mmzone.h:45`）。`MIGRATE_PCPTYPES` 是 3：UNMOVABLE、MOVABLE、RECLAIMABLE（`:47-51`）。低阶 PCP 是 3 种类型乘（0 到 3 共 4 阶），12 条。开了 THP 再加 2 条：一条给 `MIGRATE_MOVABLE`，一条给 UNMOVABLE 和 RECLAIMABLE 共用（`order_to_pindex`，`page_alloc.c:546-562`）。THP 那两条只接受 `order == pageblock_order`。

`pcp_allowed_order`（`:579`）：`order <= 3` 返回真；开了 THP 且 `order == pageblock_order` 也返回真；其它高阶返回假，直接走 buddy。

zone 上还有一份公共的 `pageset_high` 和 `pageset_batch`（`mmzone.h:845-846`），初始化时拷进每个 CPU 的 `high` / `batch`。sysctl `percpu_pagelist_high_fraction` 为 0 时用 `zone_highsize`（`page_alloc.c:5339`）算。

---

## `rmqueue` 怎么选路

```text
rmqueue                                     # page_alloc.c:2817
  order > 1 且带 __GFP_NOFAIL：WARN_ON_ONCE
  若 pcp_allowed_order(order)：
    rmqueue_pcplist                         # :2770
      pcp_spin_trylock 失败：返回 NULL，下面改走 buddy
      free_factor >>= 1                     # 有分配，就把「只释放」的放大因子收一档
      __rmqueue_pcplist                     # :2730
        对应 lists[] 空：
          rmqueue_bulk                      # :2168，batch 页，只拿一次 zone->lock
            __rmqueue                       # :2131
        从 lists[] 摘一页，count 减去 1 << order
  PCP 没拿到页（锁失败、列表补不上，或这一阶不进 PCP）：
    rmqueue_buddy                           # :2688
      已拿 zone->lock
      ALLOC_HIGHATOMIC：先 __rmqueue_smallest(..., MIGRATE_HIGHATOMIC)
      否则 __rmqueue
      仍失败且是 ALLOC_OOM 或 ALLOC_NON_BLOCK：再试 HIGHATOMIC
  若 alloc_flags 有 ALLOC_KSWAPD，并且 zone 上有 ZONE_BOOSTED_WATERMARK：
    清掉这个位，wakeup_kswapd               # vmscan.c:7817
```

`rmqueue` 标了 `__no_sanitize_memory`（`:2815`）。注释说它可能经 `set_pfnblock_flags_mask` 进 KMSAN 的 alloca 毒化，KMSAN 再分配页会递归进 `rmqueue`，所以这条路径不做 KMSAN 插桩。

### `__rmqueue`：已经拿着 `zone->lock`

```text
__rmqueue(zone, order, migratetype, alloc_flags)   # :2131
  编了 CONFIG_CMA，而且 ALLOC_CMA，
  并且 NR_FREE_CMA_PAGES > NR_FREE_PAGES / 2：
    __rmqueue_cma_fallback                          # :1650，就是 __rmqueue_smallest(..., MIGRATE_CMA)
    拿到就返回。免得 CMA 区囤着不用、普通区先碎掉
retry:
  __rmqueue_smallest(zone, order, migratetype)      # :1611
  没有，且 ALLOC_CMA：再试一次 CMA
  还没有：__rmqueue_fallback                        # :2050
    它把别的类型的空闲块搬到目标类型上，返回 true
    然后 goto retry，再走一遍 smallest
```

`__rmqueue_smallest` 从目标 order 往 `NR_PAGE_ORDERS - 1` 找。找到就 `del_page_from_free_list`，再 `expand`（`:1435`）把多出来的高阶一半一半挂回 `free_area`。`set_buddy_order` 把阶写进 `page->private`，并设上 `PageBuddy`。这就是 [02 物理布局](./02-physical-layout.md) 里 `private` 在 `PageBuddy` 时的含义。阶和拆分的图在 [02 §2.5](./02-physical-layout.md#25-buddy-概念)。

`__rmqueue_fallback` 不直接把页交给调用者。它在别的 migratetype 的空闲链上找一块，调用 `steal_suitable_fallback`（`:1813`）把 pageblock 的类型改过来、把空闲页搬到目标链，然后返回 true。`__rmqueue` 再 `goto retry`，用 `__rmqueue_smallest` 把刚搬过来的块摘走。

fallback 的顺序在 `fallbacks[]`（`:1643`）：

| 想要的类型 | 先试 | 再试 |
|---|---|---|
| `MIGRATE_UNMOVABLE` | RECLAIMABLE | MOVABLE |
| `MIGRATE_MOVABLE` | RECLAIMABLE | UNMOVABLE |
| `MIGRATE_RECLAIMABLE` | UNMOVABLE | MOVABLE |

`ALLOC_NOFRAGMENT` 且 `order < pageblock_order` 时，fallback 的最低阶被抬到 `pageblock_order`（`:2065-2066`）。不够一个 pageblock 的碎块先不偷。本地 zone 都没有时，快路径会清掉 `ALLOC_NOFRAGMENT` 再扫一遍，那时才允许偷小块。

`can_steal_fallback`（`:1747`）决定要不要把整个 pageblock 的类型改掉。阶已经不低于 pageblock、阶不低于 pageblock 的一半、或者请求本身是 RECLAIMABLE / UNMOVABLE，就整块认领。可移动的小块如果不能整块认领，就改去偷最小的那块（`:2089-2091`），避免一块可移动分配把大块不可移动空闲切碎。

偷页的时候如果 `boost_watermark` 成功且带 `ALLOC_KSWAPD`，只把 `ZONE_BOOSTED_WATERMARK` 置上（`:1840-1841`）。这时还拿着 `zone->lock`，不在这里叫醒 kswapd。`rmqueue` 放完锁再看这个位。

CMA 区本身见 [09](./03-09-cma.md)。归还时的合并在 [05](./03-05-buddy-free.md) 的 `__free_one_page`。

### buddy 上的空闲链

```c
struct free_area {
	struct list_head free_list[MIGRATE_TYPES]; /* 每种迁移类型一条 */
	unsigned long nr_free;			/* 这一阶有多少块，不是多少页 */
};
```

`zone->free_area[NR_PAGE_ORDERS]`（`mmzone.h:933`）。`NR_PAGE_ORDERS` 是 `MAX_ORDER + 1`。`MAX_ORDER` 默认 10（`mmzone.h:29`）。

### 高阶原子保留

`reserve_highatomic_pageblock`（`:1926`）在 `get_page_from_freelist` 拿到页之后调用，条件是这次 `alloc_flags` 带了 `ALLOC_HIGHATOMIC`（`page_alloc.c:3227`）。它把当前 pageblock 划成 `MIGRATE_HIGHATOMIC`。上限大约是 zone managed 页数的 1%，再加一个 pageblock（`:1935`）。只有 `migratetype_is_mergeable` 的普通 pageblock 才会被划走，CMA 和 ISOLATE 不动。

`gfp_to_alloc_flags` 在「不能直接回收、没有 `__GFP_NOMEMALLOC`、order > 0、而且有 `ALLOC_MIN_RESERVE`」时加上这个标志（`:3771-3772`）。也就是 `GFP_ATOMIC` 的高阶分配。

压力大时 `unreserve_highatomic_pageblock`（`:1967`）会把保留吐回 `ac->migratetype`。`force` 为假时每个 zone 至少留一个 pageblock。直接回收失败后会以 `force == false` 调它（`:3711`），准备 OOM 前 `should_reclaim_retry` 会以 `force == true` 再试（`:3909`）。这块保留是高阶失败的原因之一，见 [07](./03-07-high-order-and-compaction.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `rmqueue` | `mm/page_alloc.c:2817` | PCP 或 buddy。boost 过的 zone 在这里叫醒 kswapd |
| `pcp_allowed_order` | `:579` | order ≤ 3，或 THP 的 pageblock 阶 |
| `order_to_pindex` | `:546` | 迁移类型和阶合成 PCP 链下标 |
| `rmqueue_pcplist` | `:2770` | 拿 PCP 锁，失败就让调用者改走 buddy |
| `__rmqueue_pcplist` | `:2730` | 空了就 `rmqueue_bulk` 补一批 |
| `rmqueue_bulk` | `:2168` | 持 `zone->lock` 循环 `__rmqueue` |
| `rmqueue_buddy` | `:2688` | 不走 PCP。原子分配可以先碰 HIGHATOMIC |
| `__rmqueue` | `:2131` | smallest，CMA 超过一半空闲时优先 CMA，失败再 fallback |
| `__rmqueue_smallest` | `:1611` | 从目标阶往高阶找，`expand` 拆开 |
| `expand` | `:1435` | 高阶块对半切，另一半挂回 `free_area` |
| `__rmqueue_cma_fallback` | `:1650`；没编 CMA 时 `:1656` 返回 NULL | `__rmqueue_smallest(..., MIGRATE_CMA)` |
| `__rmqueue_fallback` | `:2050` | 按 `fallbacks[]` 找别的类型，再 `steal_suitable_fallback` |
| `steal_suitable_fallback` | `:1813` | 改 pageblock 类型，搬走空闲页，必要时 boost 水位 |
| `can_steal_fallback` | `:1747` | 这一阶够不够把整个 pageblock 认领过来 |
| `find_suitable_fallback` | `:1894` | 在一个 `free_area` 里按 fallback 表找非空链 |
| `reserve_highatomic_pageblock` | `:1926` | 高阶原子保留，大约 1% 的 zone |
| `unreserve_highatomic_pageblock` | `:1967` | 压力下把保留归还成请求的迁移类型 |
| `drain_zone_pages` | `:2210`（`CONFIG_NUMA`） | 远端 pageset 过期后倒回，最多一个 batch |
| `drain_all_pages` | `:2356` | 把各 CPU 的 PCP 倒回 buddy。直接回收失败后会调 |
| `free_factor` 的放大 | `nr_pcp_free` `:2374` | 连续释放把 `free_factor` 加一，倒回页数按它左移 |

---

[上一篇 水位](./03-03-fastpath-freelist.md) · [本目录](./03-00-page-alloc.md) · [下一篇 释放与合并](./03-05-buddy-free.md)
