# 3.7 高阶失败与 compaction

**结论**：页的总量够，不代表有连续的 2^order 页。贵的分配（order ≥ 4）或不可移动的高阶，slowpath 先 compact：从 zone 两头对向扫，把可移动页挪到高地址，把空闲页留在低地址，直到中间出现一块够大的空闲。compact 做不成，才回到回收或失败。

> 失败原因是旧 3.6 的原文。`compact_zone` 在 `mm/compaction.c:2562`。
> 导航：[上一篇 慢路径](./06-slowpath.md) · [本目录](./README.md) · [下一篇 NUMA](./08-numa-policy.md)

---

## 高阶分配为什么失败

1. **外碎片**。不可移动页（UNMOVABLE）钉在 MOVABLE pageblock 里，buddy 合并不上去。`/proc/buddyinfo` 高阶列是 0 就是这个样子。本机 Normal zone 的 order 10 是 0，order 9 还有 11 块（[13](../13-observability.md)）。
2. **要的 migratetype 没有整块，steal 被禁**。`ALLOC_NOFRAGMENT` 时 `__rmqueue` 不做 STEAL（`:2519`）。
3. **order 超过 `MAX_PAGE_ORDER`（默认 10，4MB）**。`alloc_order_allowed` 直接拒绝。再大用 vmalloc 或 CMA / `alloc_contig_pages`。
4. **水位 + lowmem_reserve**。DMA32 看起来有空闲，但 reserve 留给更低 zone 的分配，高 zone 的请求进不去。
5. **cpuset / `__GFP_THISNODE`**。别的节点有连续页，这条 zonelist 不让用。
6. **HIGHATOMIC 保留占着 pageblock**。原子高阶分配会把大约 1% 的 zone 划成不可随便合并的保留（`reserve_highatomic_pageblock`，`:3447`）。压力大时 `unreserve_highatomic_pageblock`（`:3496`）会吐出来。
7. **CMA / ISOLATE pageblock** 不能和普通类型合并。见 [05](./05-buddy-free.md) 合并停下来的条件，以及 [09](./09-cma.md)。
8. **直接 compact 没做成**。`compact_stall` / `compact_fail` / `compact_success`（`vmstat.c:1389` 一带）。本机直接 compact 的 stall/fail/success 都是 0，但 `compact_daemon_wake` 72：是 kcompactd 在后台扫，不是分配路径上的同步压缩。

`/proc/pagetypeinfo` 能看见每种 migratetype 的 buddy 分布。本机快照读它是 `Permission denied`（通常要 root 或放宽 `kptr_restrict` 一类限制，具体看发行版）。没有这份数就不要猜哪种类型碎了。

---

## `struct compact_control`（`mm/internal.h:823`）

`CONFIG_COMPACTION` 或 `CONFIG_CMA` 才有这份结构。注释（`:817-821`）把扫描方向说死了：`free_pfn` 从 zone 尾部开始，`migrate_pfn` 从 zone 头部开始，可移动页被挪到 zone 尾部，两者相遇（`free_pfn <= migrate_pfn`）这次 compact 结束。

| 成员 | 中文 |
|---|---|
| `struct list_head freepages[NR_PAGE_ORDERS]` | 隔离出来、准备当迁移目的地的空闲页，按阶分链 |
| `struct list_head migratepages` | 隔离出来、准备搬走的页 |
| `unsigned int nr_freepages` | 已隔离的空闲页数 |
| `unsigned int nr_migratepages` | 待迁移页数 |
| `unsigned long free_pfn` | 空闲扫描的当前位置 |
| `unsigned long migrate_pfn` | 迁移扫描的当前位置。`isolate_migratepages` 拿它当搜索起点，`isolate_migratepages_block` 把它更新成最后一页隔离之后的下一个 pfn |
| `unsigned long fast_start_pfn` | 线性扫描可以改从这里开始 |
| `struct zone *zone` | 正在压的 zone |
| `unsigned long total_migrate_scanned` | 这次迁移扫描看过的页 |
| `unsigned long total_free_scanned` | 这次空闲扫描看过的页 |
| `unsigned short fast_search_fail` | 走空闲链表快速搜索失败的次数 |
| `short search_order` | 快速搜索从哪一阶开始 |
| `const gfp_t gfp_mask` | 直接 compact 的调用者 gfp |
| `int order` | 直接 compact 需要的阶 |
| `int migratetype` | 直接 compact 需要的迁移类型 |
| `const unsigned int alloc_flags` | 直接 compact 的分配标志 |
| `const int highest_zoneidx` | 直接 compact 的最高 zone |
| `enum migrate_mode mode` | `MIGRATE_ASYNC` / `SYNC_LIGHT` / `SYNC`。模式含义在 [10-reclaim/10](../10-reclaim/10-migration.md) |
| `bool ignore_skip_hint` | 就算 pageblock 标了 skip 也扫 |
| `bool no_set_skip_hint` | 扫完不要给 block 打 skip |
| `bool ignore_block_suitable` | 扫那些被认为不合适的 block |
| `bool direct_compaction` | 直接 compact 为真。kcompactd 或 `/proc` 触发的为假 |
| `bool proactive_compaction` | kcompactd 的主动压缩 |
| `bool whole_zone` | 要求扫完整 zone，或已经扫完 |
| `bool contended` | 锁竞争，调用者可以收手 |
| `bool finish_pageblock` | 把当前 pageblock 扫完。隔离或迁移暂时失败时用来保证向前走 |
| `bool alloc_contig` | 这次是 `alloc_contig_range`，不是普通高阶分配 |

`struct capture_control`（同文件 `:867`）给直接 compact 用：合并刚做出一块够大的空闲页时，`__free_one_page` 可以把它截给正在等的分配，不必先挂上 buddy 再被别人拿走。

| 成员 | 中文 |
|---|---|
| `struct zone *zone` | 截页的 zone |
| `int migratetype` | 要的迁移类型 |
| `int order` | 分配请求的阶。注释写明可以和 compact 的阶不同：defrag 模式会把小块分配提升成 pageblock 阶的压缩，截页仍按原始分配阶匹配，好让 `prep_new_page()` 一致 |
| `struct page *page` | 截到的页。没截到是 NULL |

---

## `compact_zone`（`mm/compaction.c:2562`）

```text
compact_zone(cc, capc)                              # compaction.c:2562
  清扫描计数，按阶初始化 freepages 链，清空 migratepages
  migratetype = gfp_migratetype(cc->gfp_mask)
  不是 compact_memory 整区压：
    compaction_suit_allocation_order                # 水位不够、阶不合适 → COMPACT_SKIPPED
  compaction_restarting：__reset_isolation_suitable # 上次延期之后重来，清 skip
  whole_zone：migrate_pfn = zone 头，free_pfn = zone 尾的 pageblock
  否则用 zone->compact_cached_migrate_pfn[sync] 和 compact_cached_free_pfn
  trace_mm_compaction_begin
  lru_add_drain()                                   # 先把 per-CPU LRU 批量刷进链表，否则迁的是过期视图
  while compact_finished() == COMPACT_CONTINUE：
    isolate_migratepages(cc)
      ISOLATE_ABORT → COMPACT_CONTENDED，页放回，结束
      ISOLATE_NONE  → 这段没有可搬的，看要不要排空，继续
      ISOLATE_SUCCESS → migrate_pages(..., compaction_alloc, compaction_free,
                                       MR_COMPACTION)   # migrate.c:2100
    迁移失败的 putback_movable_pages
  更新 compact_cached_*_pfn，trace_mm_compaction_end
```

`migrate_pages` 的模式、返回值和其它调用者在 [10-reclaim/10](../10-reclaim/10-migration.md)。这里的 reason 是 `MR_COMPACTION`，模式是 `cc->mode`（调用点 `compaction.c:2698`）。

异步和同步的 migrate pfn 缓存是分开的（`:2634` 注释）：异步迁移会失败，失败过的位置同步再试仍可能成功。但在找到第一个真正有隔离候选的 pageblock 之前，两条缓存保持同步，避免反复踩同一块空 block（`:2641`）。

`ISOLATE_NONE` 之后如果 `update_cached` 仍为真，把同步缓存写成和异步一样（`:2675-2678`），表示「到这里为止两种模式都没东西可搬」。

直接 compact 从 `__alloc_pages_direct_compact`（`page_alloc.c:4139`）进来，`direct_compaction` 为真，`capc` 指向栈上的 `capture_control`。kcompactd 走 `compact_zone(&cc, NULL)`（`compaction.c:2945`、`:3127`），没有截页。

水位判断里有一条 CMA 特例（`:2540-2552`）：order 大于 costly、异步、并且这次分配不许用 CMA 时，如果非 CMA 空闲页连 order 0 的水位都过不了，直接 `COMPACT_SKIPPED`。注释写明否则压缩可能在 CMA pageblock 里做出高阶页，对这次分配没有帮助，还可能把页从非 CMA 迁进 CMA。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `compact_zone` | `mm/compaction.c:2562` | 一个 zone 的对向扫描和迁移 |
| `compaction_suit_allocation_order` | 调用点 `:2588`；函数在同文件前部 | 阶和水位不合适就跳过 |
| `isolate_migratepages` | 调用点 `:2668` | 从低地址隔离可移动页 |
| `migrate_pages` | `mm/migrate.c:2100` | 把隔离页搬到高地址空闲页 |
| `__alloc_pages_direct_compact` | `mm/page_alloc.c:4139` | 分配慢路径上的同步压缩 |
| `should_compact_retry` | `page_alloc.c:4248` | 失败后要不要再压一次 |
| `reserve_highatomic_pageblock` | `page_alloc.c:3447` | 高阶原子保留，占着就不容易合并 |
| `unreserve_highatomic_pageblock` | `:3496` | 压力下归还 |

---

[上一篇 慢路径](./06-slowpath.md) · [本目录](./README.md) · [下一篇 NUMA 策略](./08-numa-policy.md)
