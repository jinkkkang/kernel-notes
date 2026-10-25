# 3.7 高阶失败与 compaction

**结论**：页的总量够，不代表有连续的 2^order 页。贵的分配（order ≥ 4）或不可移动的高阶，slowpath 先 compact：从 zone 两头对向扫，把可移动页挪开，把空闲页留在另一头，直到中间出现一块够大的空闲。compact 做不成，才回到回收或失败。

> `compact_zone` 在 `mm/compaction.c:2381`。`struct compact_control` 在 `mm/internal.h:545`。
> 内核版本：**v6.6.160**。
> 导航：[上一篇 慢路径](./03-06-slowpath.md) · [本目录](./03-00-page-alloc.md) · [下一篇 NUMA](./03-08-numa-policy.md)

---

## 高阶分配为什么失败

1. **外碎片**。不可移动页钉在 MOVABLE pageblock 里，buddy 合并不上去。`/proc/buddyinfo` 高阶列是 0 就是这个样子。在 6.12 上采样，Normal zone 的 order 10 是 0，order 9 还有 11 块（[13](./13-observability.md)）。4K 页时 order 10 是 4MB，列数是 0 到 `MAX_ORDER`（默认 10，`include/linux/mmzone.h:29`）。
2. **要的 migratetype 没有整块，steal 被抬高了门槛**。`ALLOC_NOFRAGMENT` 且 `order < pageblock_order` 时，`__rmqueue_fallback` 只从 `pageblock_order` 往上找（`page_alloc.c:2065`）。小块先不偷。
3. **order 超过 `MAX_ORDER`**。`__alloc_pages` 开头 `WARN_ON_ONCE_GFP(order > MAX_ORDER)` 直接返回 NULL（`page_alloc.c:4477`）。再大的连续物理页用 `alloc_contig_range`（`:6198`）或 CMA 的 `cma_alloc`。虚拟连续用 vmalloc，见 [05](./05-vmalloc-and-kernel-va.md)。
4. **水位 + lowmem_reserve**。DMA32 看起来有空闲，但 `lowmem_reserve[highest_zoneidx]` 留给更低 zone 的分配，高 zone 的请求进不去。
5. **cpuset / `__GFP_THISNODE`**。别的节点有连续页，这条 zonelist 不让用。`ZONELIST_NOFALLBACK` 只含本节点。
6. **HIGHATOMIC 保留占着 pageblock**。`GFP_ATOMIC` 的高阶分配会把大约 1% 的 zone 划成 `MIGRATE_HIGHATOMIC`（`reserve_highatomic_pageblock`，`:1926`）。压力大时 `unreserve_highatomic_pageblock`（`:1967`）会吐出来。
7. **CMA / ISOLATE pageblock** 不能和普通类型合并。`migratetype_is_mergeable`（`mmzone.h:94`）只对小于 `MIGRATE_PCPTYPES` 的类型为真。见 [05](./03-05-buddy-free.md) 和 [09](./03-09-cma.md)。
8. **直接 compact 没做成**。计数是 `compact_stall` / `compact_fail` / `compact_success`（`mm/vmstat.c:1324-1326`）。在 6.12 上采样，这三个都是 0，但 `compact_daemon_wake` 是 72：kcompactd 在后台扫，同步压缩没有发生。daemon 扫了多少看 `compact_daemon_migrate_scanned` / `compact_daemon_free_scanned`，不要用 `compact_success` 判断后台没干活。

`/proc/pagetypeinfo` 能看见每种 migratetype 的 buddy 分布。在 6.12 上采样，读它是 `Permission denied`。没有这份数就不要猜哪种类型碎了。

贵的分配还会被 `vm.extfrag_threshold` 挡住。`compaction_suitable`（`compaction.c:2307`）在 order 大于 costly 时算 fragmentation index，index 落在 0 和这个阈值之间就认为失败是因为内存少，不值得压。阈值默认 500（`:1813`），sysctl 名 `extfrag_threshold`（`:3208`），范围 0 到 1000。

---

## `struct compact_control`（`mm/internal.h:545`）

`CONFIG_COMPACTION` 或 `CONFIG_CMA` 才有这份结构。注释（`:539-543`）把扫描方向说死了：`free_pfn` 从 zone 尾部开始，`migrate_pfn` 从 zone 头部开始，可移动页被挪到 zone 尾部，两者相遇（`free_pfn <= migrate_pfn`）这次 compact 结束。

```c
#if defined CONFIG_COMPACTION || defined CONFIG_CMA
struct compact_control {
	struct list_head freepages;	/* 隔离出来、准备当迁移目的地的空闲页 */
	struct list_head migratepages;	/* 隔离出来、准备搬走的页 */
	unsigned int nr_freepages;	/* 已隔离的空闲页数 */
	unsigned int nr_migratepages;	/* 待迁移页数 */
	unsigned long free_pfn;		/* 空闲扫描的当前位置 */
	/*
	 * isolate_migratepages 拿它当搜索起点。
	 * isolate_migratepages_block 把它更新成最后一页隔离之后的下一个 pfn。
	 */
	unsigned long migrate_pfn;
	unsigned long fast_start_pfn;	/* 线性扫描可以改从这里开始 */
	struct zone *zone;
	unsigned long total_migrate_scanned;
	unsigned long total_free_scanned;
	unsigned short fast_search_fail;/* 走空闲链表快速搜索失败的次数 */
	short search_order;		/* 快速搜索从哪一阶开始 */
	const gfp_t gfp_mask;		/* 直接 compact 的调用者 gfp */
	int order;			/* 直接 compact 需要的阶。整区压时可以是 -1 */
	int migratetype;		/* 由 gfp_migratetype 填 */
	const unsigned int alloc_flags;
	const int highest_zoneidx;
	/*
	 * MIGRATE_ASYNC / SYNC_LIGHT / SYNC / SYNC_NO_COPY。
	 * 见 include/linux/migrate_mode.h:15。
	 */
	enum migrate_mode mode;
	bool ignore_skip_hint;		/* 就算 pageblock 标了 skip 也扫 */
	bool no_set_skip_hint;		/* 扫完不要给 block 打 skip */
	bool ignore_block_suitable;	/* 扫那些被认为不合适的 block */
	bool direct_compaction;		/* 直接 compact 为真。kcompactd 或 /proc 为假 */
	bool proactive_compaction;	/* kcompactd 的主动压缩 */
	bool whole_zone;		/* 要求扫完整 zone，或已经扫完 */
	bool contended;			/* 锁竞争，调用者可以收手 */
	/*
	 * 把当前 pageblock 扫完。隔离或迁移暂时失败时用来保证向前走。
	 */
	bool finish_pageblock;
	bool alloc_contig;		/* 这次是 alloc_contig_range */
};
#endif
```

`freepages` 是一条链，不是按阶分成数组。

`struct capture_control`（同文件 `:589`）给直接 compact 用。合并刚做出一块阶数正好的空闲页时，`__free_one_page` 可以把它截给正在等的分配，不必先挂上 buddy 再被别人拿走。

```c
#if defined CONFIG_COMPACTION || defined CONFIG_CMA
struct capture_control {
	struct compact_control *cc;	/* 正在进行的这次压缩 */
	struct page *page;		/* 截到的页。没截到是 NULL */
};
#endif
```

要的阶、zone 都在 `cc` 里。`compaction_capture`（`page_alloc.c:652`）要求 `order == capc->cc->order`，并且拒绝 CMA、ISOLATE，以及「阶小于 pageblock 的 MOVABLE」。截到的页记在 `capc->page`。

直接 compact 把这份结构挂在 `current->capture_control` 上（`compaction.c:2649`）。中断里的释放能看见它，但 `task_capc`（`page_alloc.c:641`）拒绝 `PF_KTHREAD`，并且 zone 必须是 `capc->cc->zone`。

---

## `compact_zone`（`mm/compaction.c:2381`）

```text
compact_zone(cc, capc)                              # compaction.c:2381
  清扫描计数，INIT_LIST_HEAD freepages 和 migratepages
  migratetype = gfp_migratetype(cc->gfp_mask)
  不是 compact_memory 那种整区压（order 不是 -1）：
    当前水位已经够：返回 COMPACT_SUCCESS
    compaction_suitable 为假：返回 COMPACT_SKIPPED   # :2307
  compaction_restarting：__reset_isolation_suitable  # 上次延期之后重来，清 skip
  whole_zone：migrate_pfn = zone 头，free_pfn = zone 尾的 pageblock
  否则用 zone->compact_cached_migrate_pfn[sync] 和 compact_cached_free_pfn
  trace_mm_compaction_begin
  lru_add_drain()                                   # 先把本 CPU 的 LRU 批量刷进链表
  while compact_finished() == COMPACT_CONTINUE：    # :2266
    isolate_migratepages(cc)                        # :1967
      ISOLATE_ABORT → COMPACT_CONTENDED，页放回，结束
      ISOLATE_NONE  → 看要不要排空，继续
      ISOLATE_SUCCESS →
        migrate_pages(..., compaction_alloc, compaction_free,
                      MR_COMPACTION)                # migrate.c:1903
    迁移失败的 putback_movable_pages
    capc->page 已经截到：COMPACT_SUCCESS，跳出
  把没用来的 freepages 还回去，更新 compact_cached_free_pfn
  trace_mm_compaction_end
```

`migrate_pages` 的模式、返回值和其它调用者在 [10-10-migration.md](./10-10-migration.md)。这里的 reason 是 `MR_COMPACTION`（`migrate_mode.h` 里这个枚举的第一项），模式是 `cc->mode`。调用点在 `compaction.c:2514`：

```c
err = migrate_pages(&cc->migratepages, compaction_alloc,
		compaction_free, (unsigned long)cc, cc->mode,
		MR_COMPACTION, &nr_succeeded);
```

`compaction_alloc` / `compaction_free` 从 `cc->freepages` 上拿目的页、把没用完的还回去。

异步和同步的 migrate pfn 缓存是分开的（`:2456-2465` 注释）：异步迁移会失败，失败过的位置同步再试仍可能成功。但在找到第一个真正有隔离候选的 pageblock 之前，两条缓存保持同步，避免反复踩同一块空 block。`ISOLATE_NONE` 且 `update_cached` 仍为真时，把同步缓存写成和异步一样（`:2498-2500`）。

`compaction_suitable` 先调 `__compaction_suitable`（`:2278`）。order 0 的水位必须够，否则没有迁移目的页。贵的分配要求 LOW 水位，便宜的要求 MIN，并且都加上 `compact_gap(order)`。判断时用 `ALLOC_CMA`，因为 CMA pageblock 里的空闲页可以当迁移目的地。`highest_zoneidx` 用来跳过那些就算压出高阶页、也会被 `lowmem_reserve` 挡住的 zone。

### 谁调用它

直接 compact 从 `__alloc_pages_direct_compact`（`page_alloc.c:3417`）进来，先 `memalloc_noreclaim_save`，再 `try_to_compact_pages`（`compaction.c:2683`）。后者对 zonelist 上每个 zone 调 `compact_zone_order`（`:2618`）。`compact_zone_order` 在栈上放 `capture_control`，`direct_compaction = true`，优先级是异步时 `mode = MIGRATE_ASYNC`，否则 `MIGRATE_SYNC_LIGHT`（`:2629-2630`），然后 `compact_zone(&cc, &capc)`（`:2651`）。

kcompactd 走 `compact_zone(&cc, NULL)`（`kcompactd_do_work` 里，`:2974`），没有截页，模式是 `MIGRATE_SYNC_LIGHT`。主动压缩 `proactive_compact_node`（`:2755`）也是 `compact_zone(&cc, NULL)`（`:2775`），`order = -1`，`proactive_compaction = true`，`whole_zone = true`。`/proc` 触发的 `compact_node`（`:2785`）同样 `order = -1`，模式是 `MIGRATE_SYNC`。

order 0 的 `__alloc_pages_direct_compact` 直接返回 NULL（`page_alloc.c:3425`）。单页不需要连续。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `compact_zone` | `mm/compaction.c:2381` | 一个 zone 的对向扫描和迁移 |
| `compaction_suitable` | `:2307` | 阶、水位、碎片指数不合适就跳过 |
| `__compaction_suitable` | `:2278` | order 0 水位加 `compact_gap`，并用 `ALLOC_CMA` |
| `compact_finished` | `:2266` | 扫描相遇、或已经有够大的空闲块，就停 |
| `isolate_migratepages` | `:1967` | 从低地址隔离可移动页 |
| `migrate_pages` | `mm/migrate.c:1903` | 把隔离页搬到空闲页上。调用点 `compaction.c:2514` |
| `try_to_compact_pages` | `compaction.c:2683` | 直接压缩的入口，沿 zonelist 走 |
| `compact_zone_order` | `:2618` | 为一个分配请求压一个 zone，并挂上 `capture_control` |
| `__alloc_pages_direct_compact` | `mm/page_alloc.c:3417` | slowpath 上的同步压缩 |
| `should_compact_retry` | `page_alloc.c:3476` | 失败后要不要再压一次 |
| `proactive_compact_node` | `compaction.c:2755` | kcompactd 的主动压缩，`order = -1` |
| `kcompactd_do_work` | `compaction.c:2932`，其中 `compact_zone` 在 `:2974` | 后台按 `kcompactd_max_order` 压，不截页 |
| `reserve_highatomic_pageblock` | `page_alloc.c:1926` | 高阶原子保留，占着就不容易合并 |
| `unreserve_highatomic_pageblock` | `:1967` | 压力下归还 |
| `alloc_contig_range` | `page_alloc.c:6198` | 指定 PFN 区间的连续分配。CMA 用它 |

---

[上一篇 慢路径](./03-06-slowpath.md) · [本目录](./03-00-page-alloc.md) · [下一篇 NUMA 策略](./03-08-numa-policy.md)
