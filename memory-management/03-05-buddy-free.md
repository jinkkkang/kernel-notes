# 3.5 释放与 buddy 合并

**结论**：用户拿到的页用 `free_pages` / `__free_pages` 还。引用没到 0 就只减计数。到 0 之后，order 0 和能进 PCP 的页先挂回 per-CPU 列表；列表超过 `high`，或者调用者要求立刻进 buddy，才在 `zone->lock` 下调用 `__free_one_page`，按伙伴地址一路合并。

> `__free_one_page` 在 `mm/page_alloc.c:940`。`free_pages` 在 `:5589`，`__free_pages` 在 `:5565`。
> 导航：[上一篇 PCP](./03-04-pcp-and-rmqueue.md) · [本目录](./03-00-page-alloc.md) · [下一篇 慢路径](./03-06-slowpath.md)

旧笔记只在 `rmqueue` 那节提了一句合并。这里把释放路径补全。阶、伙伴 PFN、拆开的图在 [02 §2.5](./02-physical-layout.md#25-buddy-概念)。

---

## 从虚拟地址到 frozen

```text
free_pages(addr, order)                    # page_alloc.c:5589
  addr == 0：什么也不做
  virt_addr_valid 不过：VM_BUG_ON
  → __free_pages(virt_to_page(addr), order) # :5565
       → ___free_pages                      # :5520，fpi_flags = FPI_NONE

___free_pages
  put_page_testzero 成功：
    __free_frozen_pages(page, order, fpi)  # 引用到 0，真正还
  引用还在，而且不是复合页的 head：
    把非复合高阶页的后续尾页逐个 __free_frozen_pages
    注释（:5533-5541）写明：投机引用只放掉第一页，尾页要在这里补放，否则漏
```

`__free_pages` 的注释（`:5545-5563`）说得很直：它不检查传入的 order 是否和当初分配的一致，传大了会告警，传小了会漏。想靠引用计数决定何时释放，应该分配复合页，用 `put_page()`，不要用 `__free_pages()`。

`free_pages_nolock`（`:5575`）走同一条 `___free_pages`，但 `fpi_flags` 是 `FPI_NOLOCK`。注释允许在 raw 自旋锁、IRQ、NMI 里对任意页调用，不只是 `alloc_pages_nolock` 出来的页。

`free_pages_prepare`（`:1461`）在页真正进空闲链之前做检查：mapcount、mapping、引用、memcg、page_pool、不能带着进 buddy 的 flag。坏页走 `page_bad_reason`（`:1051`）打原因，不编造成「已经合并」。

---

## `__free_one_page`：伙伴合并

调用时已经拿着 `zone->lock`。参数是页、它的 pfn、zone、order、migratetype、`fpi_t` 标志。

```c
/* page_alloc.c:960 起，按源码写意，不是逐字抄 */
account_freepages(zone, 1 << order, migratetype);
while (order < MAX_PAGE_ORDER) {
    if (compaction_capture(capc, page, order, migratetype))
        return;                    /* 直接 compact 正等着这块，截走，不再合并 */
    buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
    if (!buddy)
        break;                     /* 伙伴不空闲、不在本 zone、或阶不一样 */
    /* order >= pageblock_order 时，不可合并的 migratetype 停止 */
    从空闲链摘掉 buddy（guard 页则清 guard）
    若 buddy 的 migratetype 不同：change_pageblock_range 改成这边的类型
    pfn = buddy_pfn & pfn;         /* 合并后的块取对齐的低地址 */
    page 指到这块
    order++;
}
set_buddy_order(page, order);     /* order 写进 page->private */
__add_to_free_list(...);          /* 挂回 free_area[order] */
page_reporting_notify_free(order); /* 除非 FPI_SKIP_REPORT_NOTIFY */
```

伙伴地址的算法在 `page_alloc.h:176` 的 `__find_buddy_pfn`：`page_pfn ^ (1 << order)`。order 0 时相邻的偶数/奇数 pfn 互为伙伴；每升一阶，异或的距离翻倍。合并后的块地址是 `buddy_pfn & pfn`，也就是把这一位清掉（`page_alloc.h:170` 那条「父块」公式）。

`page_is_buddy`（`page_alloc.h:138`）四条都要过：伙伴不是洞、在 buddy 里（或是 debug guard）、阶相同、同一个 zone。zone 比较放在最后，避免对根本合并不了的页先算 node/zone id。

`order >= pageblock_order` 时多一道 migratetype 检查（`:972-984`）。注释写明：没有 fallback 的 pageblock（CMA、ISOLATE、HIGHATOMIC 一类）不能和普通 pageblock 合并，否则隔离、CMA 记账、HIGHATOMIC 记账会错。

合并停下来的原因就这几样：到了 `MAX_PAGE_ORDER`（默认 10）、伙伴不空闲、跨了不能合并的 pageblock。停下来之后 `set_buddy_order` 把最终阶写进 `page->private`。

挂回哪一头由 `to_tail` 决定（`:1014-1019`）：`FPI_TO_TAIL` 强制尾部；shuffle 打开且这个阶参与 shuffle 时随机；否则 `buddy_merge_likely` 看再升一阶的机会大不大，机会大就放尾部，让接下来的释放更容易撞上它。

合并后如果 order 达到 `page_reporting_order`，`page_reporting_notify_free`（`:1025`）通知空闲页上报。这是 [12](./12-balloon-fpr-and-overcommit.md) 里 FPR 的入口。`FPI_SKIP_REPORT_NOTIFY` 时跳过。

直接 compact 可以在合并环里把刚空出来的页截走：`compaction_capture`（`:963`）。截走的页不再进空闲链，`account_freepages` 把刚才加上的计数减回去。`struct capture_control` 的成员在 [07](./03-07-high-order-and-compaction.md)。

---

## PCP 批量归还

单页释放若 order 允许进 PCP，先挂到 `per_cpu_pages.lists`，`count++`。超过 `high`，或者高阶释放把 `PCPF_FREE_HIGH_BATCH` 用完，调用 `free_pcppages_bulk`（`:1471`）。

```text
free_pcppages_bulk(zone, count, pcp, pindex)   # :1471
  count = min(pcp->count, count)
  拿 zone->lock
  按 pindex 轮转各条 PCP 链
    摘页，count 减去页数
    每页 __free_one_page                    # 在这把锁里合并
```

注释（`:1478-1480`）要求 count 不能比 `pcp->count` 大，否则下面的循环会在空链表上转死。先倒调用者指定的那条链（`pindex - 1` 起），再轮转，避免某一类迁移页永远留在 PCP 里。

`decay_pcp_high`（`:2576`）在分配不忙时把 `high` 往 `high_min` 收，避免每个 CPU 都囤着一批页、buddy 看起来很碎。`drain_zone_pages`（`:2615`）是主动排空一条 PCP。

`free_unref_folios`（声明 `page_alloc.h:237`）是 folio 批量释放进 PCP/buddy 的入口。LRU 批量加入时滤掉的死 folio 走这里（`mm/folio.c:205`），和单页 `free_pages` 是同一类归还，只是一批做完。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `free_pages` | `mm/page_alloc.c:5589` | 按虚拟地址释放。地址 0 直接返回 |
| `__free_pages` | `:5565` | 按 `struct page *` 释放 |
| `___free_pages` | `:5520` | 引用到 0 才进 frozen 释放；非复合尾页在这里补放 |
| `free_pages_nolock` | `:5575` | `FPI_NOLOCK`，允许 raw 锁 / IRQ / NMI |
| `free_pages_prepare` | `:1461` | 进空闲链之前的坏页检查 |
| `__free_one_page` | `:940` | 持 `zone->lock` 合并伙伴并挂回 `free_area` |
| `find_buddy_page_pfn` | `mm/page_alloc.h:196` | 找伙伴并校验 |
| `__find_buddy_pfn` | `page_alloc.h:176` | `pfn ^ (1 << order)` |
| `page_is_buddy` | `page_alloc.h:138` | 阶、zone、PageBuddy 三关 |
| `free_pcppages_bulk` | `page_alloc.c:1471` | PCP 超额后批量还回 buddy |
| `__free_frozen_pages` | `page_alloc.c:2949` | 引用已到 0 之后进 PCP 或 buddy |
| `free_unref_folios` | `page_alloc.c:3010`（声明 `page_alloc.h:237`） | 一批未引用 folio 归还 |
| `page_reporting_notify_free` | 调用点 `page_alloc.c:1025` | 合并结果够大时通知 FPR |

---

[上一篇 PCP](./03-04-pcp-and-rmqueue.md) · [本目录](./03-00-page-alloc.md) · [下一篇 慢路径](./03-06-slowpath.md)
