# 3.5 释放与 buddy 合并

**结论**：用户拿到的页用 `free_pages` / `__free_pages` 还。引用没到 0 就只减计数。到 0 之后，order 0 到 3（以及 THP 的 pageblock 阶）先挂回 per-CPU 列表；列表达到 `high`，或者 PCP 锁拿不到，才在 `zone->lock` 下调用 `__free_one_page`，按伙伴地址一路合并，最高到 `MAX_ORDER`。

> `__free_one_page` 在 `mm/page_alloc.c:792`。`free_pages` 在 `:4594`，`__free_pages` 在 `:4581`。
> 内核版本：**v6.6.160**。
> 导航：[上一篇 PCP](./03-04-pcp-and-rmqueue.md) · [本目录](./03-00-page-alloc.md) · [下一篇 慢路径](./03-06-slowpath.md)

阶、伙伴 PFN、拆开的图在 [02 §2.5](./02-physical-layout.md#25-buddy-概念)。

---

## 从虚拟地址到空闲链

```text
free_pages(addr, order)                         # page_alloc.c:4594
  addr == 0：什么也不做
  virt_addr_valid 不过：VM_BUG_ON
  → __free_pages(virt_to_page(addr), order)     # :4581

__free_pages
  先看 PageHead
  put_page_testzero 成功：
    free_the_page(page, order)                  # :590
  引用还在，而且不是复合页的 head：
    while (order-- > 0)
      free_the_page(page + (1 << order), order)
```

`__free_pages` 的注释（`:4561-4579`）说得很直：它不检查传入的 order 是否和当初分配的一致。传大了会告警，传小了会漏。引用计数只放掉第一页时，非复合高阶页的尾页要在这个 `while` 里补放，否则漏掉。想靠引用计数决定何时释放，应该分配复合页，用 `put_page()`，不要用 `__free_pages()`。

注释里的上下文限制（`:4578-4579`）：可以在中断里、或者拿着普通自旋锁时调用，不能在 NMI 里，也不能拿着 raw 自旋锁。

`free_the_page`（`:590`）按阶分叉：

```c
if (pcp_allowed_order(order))
	free_unref_page(page, order);
else
	__free_pages_ok(page, order, FPI_NONE);
```

`pcp_allowed_order` 和分配侧是同一个函数：order ≤ `PAGE_ALLOC_COSTLY_ORDER`（3），或者开了 THP 且 order 等于 `pageblock_order`。

`free_pages_prepare`（`:1105`）在页真正进空闲链之前做检查：mapcount、mapping、引用、memcg、不能带着进 buddy 的 flag。坏页走 `page_bad_reason`（`:953`）打原因。order 0 的 hwpoison 页在这里直接返回 false，不进 PCP 也不进 buddy（`:1132-1141`）。

---

## 内部标志 `fpi_t`

`page_alloc.c:61` 起。只给不走 PCP 的释放用。

| 宏 | 值 | 作用 |
|---|---|---|
| `FPI_NONE` | 0 | 普通释放 |
| `FPI_SKIP_REPORT_NOTIFY` | bit 0 | 合并完不要通知 free page reporting。临时从空闲链摘下来再挂回去时用 |
| `FPI_TO_TAIL` | bit 1 | 强制挂到空闲链尾部，不参与 shuffle。内存上线、隔离归还用 |

`__free_pages_core`（`:1333`）把启动或热插上来的页直接 `__free_pages_ok(..., FPI_TO_TAIL)`，绕过 PCP。

---

## `__free_one_page`：伙伴合并

调用时已经拿着 `zone->lock`。参数是页、它的 pfn、zone、order、migratetype、`fpi_t` 标志。

```c
/* page_alloc.c:807 起 */
if (likely(!is_migrate_isolate(migratetype)))
	__mod_zone_freepage_state(zone, 1 << order, migratetype);

while (order < MAX_ORDER) {
	if (compaction_capture(capc, page, order, migratetype)) {
		__mod_zone_freepage_state(zone, -(1 << order), migratetype);
		return;                 /* 直接 compact 正等着这块，截走 */
	}
	buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
	if (!buddy)
		goto done_merging;
	if (unlikely(order >= pageblock_order)) {
		/* 不能合并的 migratetype 停下来 */
		...
	}
	从空闲链摘掉 buddy（guard 页则清 guard）
	pfn = buddy_pfn & pfn;          /* 合并后的块取对齐的低地址 */
	page 指到这块
	order++;
}
done_merging:
set_buddy_order(page, order);          /* order 写进 page->private，并设 PageBuddy */
按 to_tail 挂到 free_area[order] 的头或尾
page_reporting_notify_free(order);     /* 除非 FPI_SKIP_REPORT_NOTIFY */
```

`MAX_ORDER` 默认 10（`include/linux/mmzone.h:29`）。循环条件是 `order < MAX_ORDER`，所以合并结果最大就是 10。

伙伴地址在 `mm/internal.h:386` 的 `__find_buddy_pfn`：`page_pfn ^ (1 << order)`。order 0 时相邻的偶数/奇数 pfn 互为伙伴；每升一阶，异或的距离翻倍。合并后的块地址是 `buddy_pfn & pfn`，也就是 `internal.h:380-382` 那条「父块」公式 `P = B & ~(1 << O)`。

`page_is_buddy`（`internal.h:348`）要过这些关：伙伴不是洞（调用前已经保证）、在 buddy 里或是 debug guard、阶相同、同一个 zone。zone 比较放在最后，避免对根本合并不了的页先算 node/zone id。`find_buddy_page_pfn`（`:406`）把找地址和这套校验包在一起，校验失败返回 NULL。

`order >= pageblock_order` 时多一道 migratetype 检查（`page_alloc.c:824-836`）。两边类型不同，而且有一边 `migratetype_is_mergeable` 为假，就停止合并。`migratetype_is_mergeable`（`mmzone.h:94`）只在 `mt < MIGRATE_PCPTYPES` 时为真。CMA、ISOLATE、HIGHATOMIC 都不能和普通 pageblock 合成更大的块，否则隔离、CMA 记账、HIGHATOMIC 记账会错。

合并停下来的原因就这几样：到了 `MAX_ORDER`、伙伴不空闲、跨了不能合并的 pageblock、或者被 `compaction_capture` 截走。

挂回哪一头由 `to_tail` 决定（`:856-866`）：`FPI_TO_TAIL` 强制尾部；这一阶参与 shuffle 时随机；否则 `buddy_merge_likely` 看再升一阶的机会大不大，机会大就放尾部，让接下来的释放更容易撞上它。

合并后 `page_reporting_notify_free`（`mm/page_reporting.h:33`，调用点 `page_alloc.c:870`）在 order 达到 `page_reporting_order` 时通知空闲页上报。这是 [12](./12-balloon-fpr-and-overcommit.md) 里 FPR 的入口。没编 `CONFIG_PAGE_REPORTING` 时这个函数是空的。`FPI_SKIP_REPORT_NOTIFY` 时调用点直接跳过。

直接 compact 可以在合并环里把刚空出来的页截走：`compaction_capture`（`:652`，只在 `CONFIG_COMPACTION` 下做实事）。截走的条件是阶正好等于正在等的 `capc->cc->order`，而且不是 CMA / ISOLATE。阶小于 `pageblock_order` 时也不截 `MIGRATE_MOVABLE`，避免低阶分配弄脏可移动 pageblock。截走的页不再进空闲链，刚才加上的空闲计数减回去。`struct capture_control` 的成员在 [07](./03-07-high-order-and-compaction.md)。

---

## PCP 批量归还

`free_unref_page`（`:2451`）是进 PCP 的单页入口：

```text
free_unref_page(page, order)
  free_unref_page_prepare                 # :2361，内部 free_pages_prepare
  迁移类型 >= MIGRATE_PCPTYPES：
    ISOLATE：free_one_page 直接进 buddy
    HIGHATOMIC / CMA：当成 MOVABLE 放进 PCP，以后还能把这些区域还出来
  pcp_spin_trylock 成功：
    free_unref_page_commit                # :2421
      挂到 lists[order_to_pindex]
      count += 1 << order
      count >= high：free_pcppages_bulk
  锁拿不到：
    free_one_page                         # :1288，当场拿 zone->lock 合并
```

`free_unref_page_commit` 里，如果 `free_factor` 已经不是 0，而且这次是 order 1 到 3，就把 `free_high` 设上。`nr_pcp_high`（`:2403`）在 `free_high` 时返回 0，于是立刻倒。`nr_pcp_free`（`:2374`）在连续只释放、中间没有分配时把 `free_factor` 加一（上限 `CONFIG_PCP_BATCH_SCALE_MAX`），倒回的页数按 `batch << free_factor` 放大，但至少留一个 batch 在列表上。分配侧 `rmqueue_pcplist` 会把 `free_factor` 右移 1，所以一有分配，放大就收回来。

zone 上有 `ZONE_RECLAIM_ACTIVE` 时，`nr_pcp_high` 把 high 收成 `min(batch << 2, high)`，回收期间少在 PCP 里囤页。

```text
free_pcppages_bulk(zone, count, pcp, pindex)   # :1229
  count = min(pcp->count, count)
  pindex = pindex - 1                           # 先倒调用者指定的那条链
  拿 zone->lock
  轮转各条 PCP 链，空链跳过
    从链尾摘页
    count 和 pcp->count 减去 1 << order
    __free_one_page(..., FPI_NONE)
```

注释（`:1238-1241`）要求 count 不能比 `pcp->count` 大，否则下面的循环会在空链表上转。先倒调用者指定的那条链（`pindex - 1` 起，循环里先 `++pindex`），再轮转，避免某一类迁移页永远留在 PCP 里。

`drain_zone_pages`（`:2210`，只在 `CONFIG_NUMA` 下编译）是 vmstat 过期后排空远端 pageset，一次最多一个 batch。`drain_all_pages`（`:2356`）把各 CPU 的 PCP 都倒回去。直接回收拿到页失败后会调它，见 [06](./03-06-slowpath.md)。

`free_unref_page_list`（`:2493`）释放一串 order 0 的页。它按 zone 批量拿 PCP 锁，隔离页仍直接 `free_one_page`。LRU 上成批放掉的页走这里，和单页 `free_unref_page` 是同一类归还。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `free_pages` | `mm/page_alloc.c:4594` | 按虚拟地址释放。地址 0 直接返回 |
| `__free_pages` | `:4581` | 按 `struct page *` 释放。引用到 0 才进 `free_the_page` |
| `free_the_page` | `:590` | 能进 PCP 的走 `free_unref_page`，否则 `__free_pages_ok` |
| `free_pages_prepare` | `:1105` | 进空闲链之前的坏页、memcg、KASAN 检查 |
| `page_bad_reason` | `:953` | 坏页原因字符串 |
| `__free_pages_ok` | `:1304` | 准备好之后拿 `zone->lock` 调用 `__free_one_page` |
| `free_one_page` | `:1288` | PCP 锁失败或隔离页时的单页进 buddy |
| `__free_one_page` | `:792` | 持 `zone->lock` 合并伙伴并挂回 `free_area` |
| `set_buddy_order` | `:634` | `set_page_private(page, order)` 并 `__SetPageBuddy` |
| `find_buddy_page_pfn` | `mm/internal.h:406` | 找伙伴并校验 |
| `__find_buddy_pfn` | `internal.h:386` | `pfn ^ (1 << order)` |
| `page_is_buddy` | `internal.h:348` | 阶、zone、PageBuddy |
| `free_unref_page` | `page_alloc.c:2451` | 单页进 PCP，超额再批量还回 buddy |
| `free_unref_page_commit` | `:2421` | 挂上 PCP 链，按 high 决定倒多少 |
| `free_unref_page_list` | `:2493` | 一串 order 0 页批量进 PCP |
| `free_pcppages_bulk` | `:1229` | PCP 超额后在 `zone->lock` 里合并 |
| `nr_pcp_free` / `nr_pcp_high` | `:2374` / `:2403` | 连续释放放大倒回量；回收期间压低 high |
| `__free_pages_core` | `:1333` | 早期页和热插拔页，`FPI_TO_TAIL`，不进 PCP |
| `compaction_capture` | `:652` | 合并环里把页截给直接 compact |
| `page_reporting_notify_free` | `mm/page_reporting.h:33` | 合并结果够大时通知 FPR。调用点 `page_alloc.c:870` |

---

[上一篇 PCP](./03-04-pcp-and-rmqueue.md) · [本目录](./03-00-page-alloc.md) · [下一篇 慢路径](./03-06-slowpath.md)
