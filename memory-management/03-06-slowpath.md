# 3.6 慢路径，以及和回收的交接

**结论**：快路径拿不到页就进 `__alloc_pages_slowpath`（`mm/page_alloc.c:3948`）。它先把水位从 LOW 放宽到 MIN，叫醒 kswapd，再试一次 freelist。贵的分配、或者不可移动的高阶，接着先 compact。然后才在**分配者自己的上下文**里直接回收。直接回收的延迟算在这个任务头上。kswapd 是后台的。页是怎么从 LRU 上摘下来的，本篇只交到入口，本体在 [10-00-reclaim.md](./10-00-reclaim.md)。

> 内核版本：**v6.6.160**。
> 导航：[上一篇 释放](./03-05-buddy-free.md) · [本目录](./03-00-page-alloc.md) · [下一篇 高阶](./03-07-high-order-and-compaction.md)

---

## 先放宽水位，再决定压缩还是回收

`costly_order` 的定义是 `order > PAGE_ALLOC_COSTLY_ORDER`（`:3953`）。`PAGE_ALLOC_COSTLY_ORDER` 是 3，所以 order ≥ 4（4K 页上是 64KB）算贵。

`can_direct_reclaim` 是 gfp 里有没有 `__GFP_DIRECT_RECLAIM`。`can_compact` 是 `gfp_compaction_allowed`（`include/linux/gfp.h:351`）：编了 `CONFIG_COMPACTION`，并且 gfp 还有 `__GFP_IO`。`GFP_NOIO` 只有 `RECLAIM`，可以自己回收，但没有 `__GFP_IO`，`can_compact` 为假。

slowpath 开头用 `gfp_to_alloc_flags`（`page_alloc.c:3742`）把水位改成 `ALLOC_WMARK_MIN`，并按 gfp 补上 `ALLOC_MIN_RESERVE`、`ALLOC_NON_BLOCK`、`ALLOC_HIGHATOMIC`、`ALLOC_KSWAPD`、`ALLOC_CMA`。然后：

```text
__alloc_pages_slowpath                         # page_alloc.c:3948
  alloc_flags = gfp_to_alloc_flags             # 水位改成 MIN
  重算 preferred_zoneref
  ALLOC_KSWAPD：wake_all_kswapds               # :3722 → wakeup_kswapd
  get_page_from_freelist                       # 用放宽后的标志再试一次

  能直接回收、能 compact、
  而且（贵，或者 order>0 且 migratetype 不是 MOVABLE）、
  并且这次不能无视水位：
    __alloc_pages_direct_compact               # :3417，优先级 INIT_COMPACT_PRIORITY
    贵且 __GFP_NORETRY：
      COMPACT_SKIPPED 或 COMPACT_DEFERRED → 失败返回
      带 __GFP_THISNODE（THP 先只试本节点）→ 失败返回

retry:
  cpuset 或 zonelist 变了 → restart
  再 wake_all_kswapds
  __gfp_pfmemalloc_flags 非 0：
    alloc_flags 改成不看水位（或 OOM 保留），nodemask 清掉
  get_page_from_freelist
  不能直接回收 → nopage
  当前已经是 PF_MEMALLOC → nopage             # 避免回收递归
  __alloc_pages_direct_reclaim                # :3689
  __alloc_pages_direct_compact
  __GFP_NORETRY → nopage
  贵，且（不能 compact 或没有 __GFP_RETRY_MAYFAIL）→ nopage
  should_reclaim_retry                        # :3843，最多 MAX_RECLAIM_RETRIES（16）
  回收有进展且能 compact：should_compact_retry # :3476，最多 MAX_COMPACT_RETRIES（16）
  __alloc_pages_may_oom                       # :3322
  OOM 受害者且（ALLOC_OOM 或 __GFP_NOMEMALLOC）→ nopage
  OOM 有进展 → retry

nopage:
  cpuset / zonelist 变了 → restart
  __GFP_NOFAIL：
    不能直接回收：WARN，然后失败
    用 ALLOC_MIN_RESERVE 再试 cpuset fallback
    cond_resched()，goto retry
  其它：warn_alloc，返回 NULL
```

贵的、或者 order>0 且不是 MOVABLE 的分配，在第一次放宽水位仍然失败之后先 compact（`:4015-4031`）。注释说页往往够，只是不连续；不可移动的高阶还希望从同样 migratetype 的 pageblock 里迁走，免得永久碎片。可移动的小阶（order 1 到 3）不走这个「先压缩」，它们落到后面的直接回收。

`__GFP_NOFAIL` 在 `nopage` 里一直转（`:4205-4240`）。必须同时能直接回收，否则 `:4210` 有 `WARN_ON_ONCE_GFP`。贵的 NOFAIL、以及已经在 `PF_MEMALLOC` 里的 NOFAIL，也会 `WARN`，但仍然继续用 `ALLOC_MIN_RESERVE` 重试。注释写明这里不用 `ALLOC_NO_WATERMARKS`，否则会把保留吃光。

---

## 四条交接

| 交接 | 分配侧入口 | 接到哪 |
|---|---|---|
| 叫醒后台回收 | `wake_all_kswapds`（`page_alloc.c:3722`）对 zonelist 上每个 node 的第一个有管理页的 zone 调 `wakeup_kswapd`（`vmscan.c:7817`） | [10-01-overview.md](./10-01-overview.md) 的 kswapd。目标是把水位拉回 HIGH。`wakeup_kswapd` 自己还会在只缺连续页时去叫 kcompactd |
| 直接回收 | `__alloc_pages_direct_reclaim`（`page_alloc.c:3689`）→ `__perform_reclaim`（`:3663`）→ `try_to_free_pages`（`vmscan.c:7057`）→ `do_try_to_free_pages`（`:6839`）→ `shrink_node`（`:6532`） | 同一篇。`shrink_node` 在那里分叉到经典 LRU 或 MGLRU |
| 直接压缩 | `__alloc_pages_direct_compact`（`page_alloc.c:3417`）→ `try_to_compact_pages`（`compaction.c:2683`） | [07](./03-07-high-order-and-compaction.md) 的 `compact_zone` |
| OOM | `__alloc_pages_may_oom`（`:3322`）→ `out_of_memory`（`oom_kill.c:1113`，调用点 `page_alloc.c:3391`） | [10-08-oom.md](./10-08-oom.md) |

直接回收包在 `psi_memstall_enter` 里，所以这段等待会出现在 PSI memory 上。进入 `try_to_free_pages` 之前会 `memalloc_noreclaim_save`（`:3674`），避免回收路径里的分配再进回收。回收之后如果还是没有页，先 `unreserve_highatomic_pageblock(ac, false)`，再 `drain_all_pages(NULL)`，把 PCP 里的页倒回 buddy，然后重试一次 freelist（`:3710-3714`）。

`__alloc_pages_may_oom` 在真正调用 `out_of_memory` 之前还有几道门（`:3360-3378`）：正在 core dump、order 大于 costly、带了 `__GFP_RETRY_MAYFAIL` 或 `__GFP_THISNODE`、`highest_zoneidx` 低于 `ZONE_NORMAL`、存储被挂起，都直接放弃，不去杀进程。OOM 锁拿不到就睡 1 秒返回，让别人先杀。

直接回收和 kswapd 扫的是 LRU 上的 folio，不是 buddy 空闲页。空闲页已经不在 LRU 上。slowpath 只决定「现在要不要在这个任务里扫」，扫的规则在回收章。

---

## 在 6.12 上采样的慢路径有多稀

`vmstat` 里 `allocstall_*` 是进了 slowpath 的次数。名字来自 `mm/vmstat.c:1261` 的 `TEXTS_FOR_ZONES("allocstall")`，在 6.6.160 的这棵树上就是这一行。`pgscan_direct` / `pgsteal_direct` 是直接回收扫的和偷到的（`vmstat.c:1282`、`:1276`）。

在 6.12 上采样：`allocstall_normal` 11、`allocstall_movable` 373，相对 `pgalloc_dma32` / `pgalloc_normal` 的一千多万和两千多万次，慢路径很少。`pgscan_direct` / `pgsteal_direct` 是 19314 / 18131。见 [13](./13-observability.md)。

直接回收的次数看 `allocstall_*`，不是看 `pgscan_direct` 的页数。页数是那些次数里扫了多少。`allocstall_movable` 高而 `pgalloc_movable` 为 0 时，只说明请求的首选 zone 是 MOVABLE，页可能从别的 zone 给出，或者失败了。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `__alloc_pages_slowpath` | `mm/page_alloc.c:3948` | 放宽水位、压缩、直接回收、OOM 的循环 |
| `gfp_to_alloc_flags` | `:3742` | 水位改成 MIN，并翻译 HIGH / NON_BLOCK / KSWAPD / CMA |
| `wake_all_kswapds` | `:3722` | 沿 zonelist 给每个 node 叫一次 kswapd |
| `__alloc_pages_direct_compact` | `:3417`；没编 `CONFIG_COMPACTION` 时 `:3542` 返回 NULL，结果记 `COMPACT_SKIPPED` | 直接压缩 |
| `try_to_compact_pages` | `mm/compaction.c:2683` | 沿 zonelist 对每个 zone 做 `compact_zone_order` |
| `should_compact_retry` | `page_alloc.c:3476`；没编压缩时 `:3551` | 要不要再压。上限 `MAX_COMPACT_RETRIES`（16，`:3412`） |
| `__alloc_pages_direct_reclaim` | `:3689` | 直接回收，然后排空 PCP 再试一次 |
| `__perform_reclaim` | `:3663` | 套上 `PF_MEMALLOC` 再调 `try_to_free_pages` |
| `try_to_free_pages` | `mm/vmscan.c:7057` | 直接回收入口 |
| `do_try_to_free_pages` | `vmscan.c:6839` | 扫 zonelist 上的 node |
| `shrink_node` | `vmscan.c:6532` | 一个 node 上的回收。分叉在回收章 |
| `should_reclaim_retry` | `page_alloc.c:3843` | 再回收有没有希望。上限 `MAX_RECLAIM_RETRIES`（16，`mm/internal.h:242`） |
| `__alloc_pages_may_oom` | `page_alloc.c:3322` | 进入 OOM 之前的门槛 |
| `out_of_memory` | `mm/oom_kill.c:1113` | 选进程杀掉。调用点 `page_alloc.c:3391` |
| `wakeup_kswapd` | `mm/vmscan.c:7817` | 叫醒后台回收 |
| `__alloc_pages_cpuset_fallback` | `page_alloc.c:3302` | 先尊重 cpuset，失败再忽略。`__GFP_NOFAIL` 用它配 `ALLOC_MIN_RESERVE` |

---

[上一篇 释放](./03-05-buddy-free.md) · [本目录](./03-00-page-alloc.md) · [下一篇 高阶与 compaction](./03-07-high-order-and-compaction.md)
