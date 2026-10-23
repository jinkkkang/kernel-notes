# 3.6 慢路径，以及和回收的交接

**结论**：快路径拿不到页，并且这次分配允许睡觉，就进 `__alloc_pages_slowpath`（`mm/page_alloc.c:4784`）。贵的分配先 compact，可移动的小阶先回收。直接回收发生在**分配者自己的上下文**里，延迟算在这个任务头上。kswapd 是后台的。页是怎么从 LRU 上摘下来的，本篇只交到入口，本体在 [10-reclaim/](../10-reclaim/README.md)。

> 导航：[上一篇 释放](./05-buddy-free.md) · [本目录](./README.md) · [下一篇 高阶](./07-high-order-and-compaction.md)

---

## 先压缩还是先回收

`costly_order` 的定义是 `order > PAGE_ALLOC_COSTLY_ORDER`，而 `PAGE_ALLOC_COSTLY_ORDER` 是 3。也就是 order ≥ 4（64KB，4K 页）算贵。

贵的分配，或者 order>0 且 migratetype 不是 MOVABLE：先 compact（`compact_first`，`:4834`），因为页往往够，只是不连续。可移动的小阶分配则先回收。

slowpath 里会重新算 `alloc_flags`（`alloc_flags_slowpath`），放宽水位、允许 HIGHATOMIC 保留。然后循环大致是：

```text
__alloc_pages_direct_compact     # :4139（无压缩配置时 :4316 是空实现）
get_page_from_freelist           # 再试
__alloc_pages_direct_reclaim     # :4464 → try_to_free_pages
get_page_from_freelist
should_compact_retry             # :4248
__alloc_pages_may_oom            # :4044 → out_of_memory
```

`__GFP_NOFAIL` 会一直转。必须同时能直接回收，否则 slowpath `:4810` 有 `WARN`。其它标志按 `NORETRY` / `RETRY_MAYFAIL` 决定何时放弃。

---

## 三条交接

| 交接 | 分配侧入口 | 接到哪 |
|---|---|---|
| 叫醒后台回收 | `wakeup_kswapd`（`vmscan.c:7573`），快路径水位不够或 zone 被 boost 时就会叫 | [10-reclaim/01](../10-reclaim/01-overview.md) 的 kswapd。目标是把水位拉回 HIGH |
| 直接回收 | `__alloc_pages_direct_reclaim`（`:4464`）→ `try_to_free_pages`（`vmscan.c:6769`）→ `do_try_to_free_pages`（`:6547`）→ `shrink_node`（`:6239`） | 同一篇。`shrink_node` 在那里分叉到经典 LRU 或 MGLRU |
| 直接压缩 | `__alloc_pages_direct_compact`（`:4139`） | [07](./07-high-order-and-compaction.md) 的 `compact_zone` |
| OOM | `__alloc_pages_may_oom`（`:4044`）→ `out_of_memory`（`oom_kill.c:1103`） | [10-reclaim/08](../10-reclaim/08-oom.md) |

直接回收和 kswapd 扫的是 LRU 上的 folio，不是 buddy 空闲页。空闲页已经不在 LRU 上。slowpath 只决定「现在要不要在这个任务里扫」，扫的规则在回收章。

---

## 本机慢路径有多稀

`vmstat` 里 `allocstall_*` 是进了 slowpath 的次数（`mm/vmstat.c:1338` 的 `TEXTS_FOR_ZONES(ALLOCSTALL)`），`pgscan_direct` / `pgsteal_direct` 是直接回收扫的和偷到的。本机 `allocstall_normal` 11、`allocstall_movable` 373，相对 `pgalloc_*` 的几千万次，慢路径很少，见 [13](../13-observability.md)。

直接回收的次数看 `allocstall_*`，不是看 `pgscan_direct` 的页数。页数是那些次数里扫了多少。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `__alloc_pages_slowpath` | `mm/page_alloc.c:4784` | 回收 / 压缩 / OOM 的循环 |
| `__alloc_pages_direct_compact` | `:4139` | 直接压缩。无压缩配置时 `:4316` 是空实现 |
| `should_compact_retry` | `:4248` | 要不要再压 |
| `__alloc_pages_direct_reclaim` | `:4464` | 直接回收 |
| `try_to_free_pages` | `mm/vmscan.c:6769` | 直接回收入口 |
| `do_try_to_free_pages` | `vmscan.c:6547` | 扫 zonelist 上的 node |
| `__alloc_pages_may_oom` | `page_alloc.c:4044` | 进入 OOM |
| `wakeup_kswapd` | `vmscan.c:7573` | 叫醒后台回收 |

---

[上一篇 释放](./05-buddy-free.md) · [本目录](./README.md) · [下一篇 高阶与 compaction](./07-high-order-and-compaction.md)
