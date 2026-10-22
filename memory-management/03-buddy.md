# 3. buddy 与页分配

**结论**：v7.3 没有名叫 `__alloc_pages` 的 C 函数。宏 `__alloc_pages()`（`mm/page_alloc.h:259`）进 `__alloc_pages_noprof`。快路径只扫 zonelist 上水位够的 zone，从 per-CPU 列表或 buddy 拿页；不够才进 slowpath：唤醒 kswapd、直接回收、直接 compact，最后才可能 OOM。

> 主文件：`mm/page_alloc.c`。水位在 `struct zone._watermark[]`。
> 导航：[上一节 物理布局](./02-physical-layout.md) · [目录](./README.md) · [下一节 SLUB](./04-slab.md)

---

## 3.1 调用链

```text
alloc_pages(gfp, order)                         # 宏 gfp.h:302
  → alloc_pages_noprof                          # mempolicy.c:2575
       → alloc_frozen_pages_noprof              # 带 mempolicy；无策略时 page_alloc.h:242
            → __alloc_frozen_pages_noprof       # page_alloc.c:5383

__alloc_pages(gfp, order, nid, nodemask, flags) # 宏 page_alloc.h:259
  → __alloc_pages_noprof                        # page_alloc.c:5466
       → __alloc_frozen_pages_noprof
       → set_page_refcounted                    # 返回给调用者之前把引用设上

__alloc_frozen_pages_noprof                     # :5383
  → current_gfp_context                         # 继承 NOFS/NOIO、PF_MEMALLOC_PIN
  → prepare_alloc_pages                         # 填 alloc_context：zonelist、migratetype、最高 zone
  → get_page_from_freelist                      # 快路径，水位用 WMARK_LOW（nolock 用 WMARK_MIN）
  → 失败且不是 ALLOC_NOLOCK：
       __alloc_pages_slowpath                   # :4784
  → __GFP_ACCOUNT 时 __memcg_kmem_charge_page   # 内核页 charge，失败则把页还回去
  → trace_mm_page_alloc
```

`__alloc_pages_noprof`（`:5466`）自己几乎不分配：它调用 frozen 版本，成功再 `set_page_refcounted`。frozen 的意思是返回时引用计数还没交给调用者，slowpath 和 charge 失败都可以直接还回 buddy。

```c
/* page_alloc.c:5435 附近，注释按源码意图写 */
page = get_page_from_freelist(alloc_gfp, order, fastpath_alloc_flags, &ac);
if (likely(page) || (alloc_flags & ALLOC_NOLOCK))
    goto out;                          /* 拿到了，或者明确不许睡觉 */
page = __alloc_pages_slowpath(alloc_gfp, order, &ac);
```

`ALLOC_NOLOCK` 是不能睡眠、不能拿 zone 锁的分配（`alloc_pages_nolock` 一类）。它只有一次尝试。

---

## 3.2 `get_page_from_freelist`（`:3800`）

沿 zonelist 走。每个 zone：

1. cpuset 开着且带 `ALLOC_CPUSET` 时，`__cpuset_zone_allowed` 不通过就跳过。这是和 cgroup cpuset 的交界，策略细节不在本章。
2. 脏页分配（`ac->spread_dirty_pages`）会看 `node_dirty_ok`，避免一个 node 吃掉全局脏页配额。
3. `ALLOC_NOFRAGMENT` 且将要落到远程节点时，清掉这个标志再重试：本地性优先于防碎片（`:3856`）。
4. 多节点时，kswapd 已经在跑的 node 先跳过，避免误打成 node reclaim（`:3872` 一带）。
5. `__zone_watermark_ok`（`:3598`）不过就下一个 zone。
6. 过了就 `rmqueue()`。

水位四档（`mmzone.h:809`）：`WMARK_MIN`、`WMARK_LOW`、`WMARK_HIGH`，再加 `WMARK_PROMO`。快路径用 LOW：空闲页掉到 LOW 以下就该把 kswapd 拉起来，而不是立刻在分配上下文里回收。kswapd 把水位拉回 HIGH 再睡。MIN 是给 PF_MEMALLOC、原子分配的底。`lowmem_reserve[]` 会从高 zone 的分配里扣掉，防止 DMA32 被 NORMAL 的分配榨干。

`/proc/zoneinfo` 里 DMA 的 `min/low/high/promo` 就是这四档，单位是页。本机 DMA 很小（managed 3840 页），数字也小，见 [13](./13-observability.md)。

---

## 3.3 `rmqueue` → PCP → `__rmqueue`

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

`pcp_allowed_order`：order 0，以及「便宜」的高阶（含 THP 用的 PCP 链）。热路径不碰 `zone->lock`。

`__rmqueue` 在 **已经拿着 `zone->lock`** 时做真正的摘链。模式（`*mode` 会被记住，因为 `rmqueue_bulk` 在同一把锁里循环）：

| 模式 | 做什么 |
|---|---|
| `RMQUEUE_NORMAL` | `__rmqueue_smallest`（`:1901`）：从目标 order 往高阶找，拆 buddy |
| `RMQUEUE_CMA` | 调用者允许 `ALLOC_CMA` 时，`__rmqueue_cma_fallback` |
| `RMQUEUE_CLAIM` | `__rmqueue_claim`（`:2373`）：按 fallback 把别的 migratetype 的空 pageblock 整块认领过来 |
| `RMQUEUE_STEAL` | 没有 `ALLOC_NOFRAGMENT` 时 `__rmqueue_steal`（`:2428`）：从别的类型偷页，这会制造外碎片 |

另外，CMA 页超过该 zone 空闲页的一半时，可移动分配会先从 CMA 拿（`:2477`），免得 CMA 区囤着不用、普通区先碎掉。

拆页在 `__rmqueue_smallest` 里完成：高阶块一切二，另一半挂回 `free_area[order]`。归还时 `__free_one_page`（`:940`）按 buddy 地址合并，能合就升阶，直到合并不了。合并后如果 order 达到 `page_reporting_order`，调用 `page_reporting_notify_free`（`:1025`，见 [12](./12-balloon-fpr-and-overcommit.md)）。

`set_buddy_order` 把 order 写进 `page->private`。这就是 [02](./02-physical-layout.md) 里 `private` 在 `PageBuddy` 时的含义。

---

## 3.4 GFP 标志

位定义在 `include/linux/gfp_types.h`。组合宏在同文件后半。

### 区与迁移

| 标志 | 行 | 含义 |
|---|---|---|
| `__GFP_DMA` | 106 | 只要 ZONE_DMA |
| `__GFP_DMA32` | 108 | DMA32 |
| `__GFP_HIGHMEM` | 107 | 允许高端内存。x86-64 基本无感 |
| `__GFP_MOVABLE` | 109 | 允许 ZONE_MOVABLE，migratetype 走 MOVABLE |
| `GFP_ZONEMASK` | 110 | 上面四个的掩码 |
| `__GFP_RECLAIMABLE` | 140 | 可回收，migratetype 走 RECLAIMABLE |
| `__GFP_WRITE` | 141 | 这笔分配会计入写脏页 |
| `__GFP_HARDWALL` | 142 | cpuset 硬墙 |
| `__GFP_THISNODE` | 143 | 不跨 node。zonelist 用 `ZONELIST_NOFALLBACK` |
| `__GFP_ACCOUNT` | 144 | 记到 memcg（内核内存） |

### 回收与失败策略

| 标志 | 行 | 含义 |
|---|---|---|
| `__GFP_HIGH` | 170 | 允许动到更低水位，原子分配用 |
| `__GFP_MEMALLOC` | 171 | 允许用保留内存（PF_MEMALLOC 一类） |
| `__GFP_NOMEMALLOC` | 172 | 明确不碰保留 |
| `__GFP_IO` | 249 | 回收时允许发起 IO |
| `__GFP_FS` | 250 | 回收时允许进文件系统 |
| `__GFP_DIRECT_RECLAIM` | 251 | 调用者可以自己回收 |
| `__GFP_KSWAPD_RECLAIM` | 252 | 可以唤醒 kswapd |
| `__GFP_RECLAIM` | 253 | 上面两者 |
| `__GFP_RETRY_MAYFAIL` | 254 | 多试，但仍允许失败 |
| `__GFP_NOFAIL` | 255 | 不许失败。必须同时能直接回收，否则 `WARN`（slowpath `:4810`） |
| `__GFP_NORETRY` | 256 | 少试，尽快失败 |

### 其它

| 标志 | 行 | 含义 |
|---|---|---|
| `__GFP_NOWARN` | 282 | 失败不警告 |
| `__GFP_COMP` | 283 | 组成复合页 / folio |
| `__GFP_ZERO` | 284 | 清零 |
| `__GFP_ZEROTAGS` | 285 | 架构 tag 清零 |
| `__GFP_SKIP_ZERO` | 286 | 调用者保证会自己清零，分配器可以跳过 |
| `__GFP_SKIP_KASAN` | 287 | 跳过 KASAN |
| `__GFP_NOLOCKDEP` | 290 | 不进 lockdep |

### 常用组合

| 宏 | 行 | 展开 |
|---|---|---|
| `GFP_ATOMIC` | 370 | `HIGH \| KSWAPD_RECLAIM`。不直接回收，可以叫醒 kswapd |
| `GFP_KERNEL` | 371 | `RECLAIM \| IO \| FS`。进程上下文的默认 |
| `GFP_KERNEL_ACCOUNT` | 372 | 再加 `ACCOUNT` |
| `GFP_NOWAIT` | 373 | `KSWAPD_RECLAIM \| NOWARN`。不睡眠 |
| `GFP_NOIO` | 374 | 只 `RECLAIM`。块层自己的分配，不能再进 IO |
| `GFP_NOFS` | 375 | `RECLAIM \| IO`。文件系统里不能再进 FS |
| `GFP_USER` | 376 | `RECLAIM \| IO \| FS \| HARDWALL` |
| `GFP_HIGHUSER_MOVABLE` | 380 | 用户页：HIGHMEM + MOVABLE + SKIP_KASAN |
| `GFP_TRANSHUGE_LIGHT` | 381 | 可移动复合大页，**不**直接回收 |
| `GFP_TRANSHUGE` | 383 | LIGHT 再加直接回收 |

`GFP_NOFS` / `GFP_NOIO` 也会通过 `memalloc_nofs_save()` / `memalloc_noio_save()` 变成上下文范围，`current_gfp_context()` 在 frozen 分配入口把它叠上去（`:5412`）。在一个持有 FS 锁的路径里随手 `GFP_KERNEL`，会被这条机制降级；不要靠「我记得这里写了 GFP_KERNEL」判断它能不能进文件系统。

---

## 3.5 slowpath（`:4784`）

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

`__GFP_NOFAIL` 会一直转。其它标志按 `NORETRY` / `RETRY_MAYFAIL` 决定何时放弃。

直接回收发生在**分配者自己的上下文**里，所以延迟算在这个任务头上。kswapd 是后台的。`vmstat` 里 `allocstall_*` 是进了 slowpath 的次数（`vmstat.c:1338` 的 `TEXTS_FOR_ZONES(ALLOCSTALL)`），`pgscan_direct` / `pgsteal_direct` 是直接回收扫的和偷到的。本机 `allocstall_normal` 11、`allocstall_movable` 373，相对 `pgalloc_*` 的几千万次，慢路径很少，见 [13](./13-observability.md)。

---

## 3.6 高阶分配为什么失败

页的总量够，不代表有连续的 2^order 页。常见原因：

1. **外碎片**。不可移动页（UNMOVABLE）钉在 MOVABLE pageblock 里，buddy 合并不上去。`/proc/buddyinfo` 高阶列是 0 就是这个样子。本机 Normal zone 的 order 10 是 0，order 9 还有 11 块（[13](./13-observability.md)）。
2. **要的 migratetype 没有整块，steal 被禁**。`ALLOC_NOFRAGMENT` 时 `__rmqueue` 不做 STEAL（`:2519`）。
3. **order 超过 `MAX_PAGE_ORDER`（默认 10，4MB）**。`alloc_order_allowed` 直接拒绝。再大用 vmalloc 或 CMA / `alloc_contig_pages`。
4. **水位 + lowmem_reserve**。DMA32 看起来有空闲，但 reserve 留给更低 zone 的分配，高 zone 的请求进不去。
5. **cpuset / `__GFP_THISNODE`**。别的节点有连续页，这条 zonelist 不让用。
6. **HIGHATOMIC 保留占着 pageblock**。原子高阶分配会把大约 1% 的 zone 划成不可随便合并的保留（`:3447`）。压力大时 `unreserve_highatomic_pageblock`（`:3496`）会吐出来。
7. **CMA / ISOLATE pageblock** 不能和普通类型合并。
8. **直接 compact 没做成**。`compact_stall` / `compact_fail` / `compact_success`（`vmstat.c:1389` 一带）。本机直接 compact 的 stall/fail/success 都是 0，但 `compact_daemon_wake` 72：是 kcompactd 在后台扫，不是分配路径上的同步压缩。

`/proc/pagetypeinfo` 能看见每种 migratetype 的 buddy 分布。本机快照读它是 `Permission denied`（通常要 root 或放宽 `kptr_restrict` 一类限制，具体看发行版）。没有这份数就不要猜哪种类型碎了。

---

## 3.7 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `alloc_pages` | 宏 `gfp.h:302` | 最常见入口 |
| `alloc_pages_noprof` | `mempolicy.c:2575` | 加 mempolicy 后的入口 |
| `__alloc_pages` | 宏 `page_alloc.h:259` | 指定 nid / nodemask |
| `__alloc_pages_noprof` | `page_alloc.c:5466` | 上面这个宏的实现 |
| `__alloc_frozen_pages_noprof` | `page_alloc.c:5383` | 快路径 + slowpath |
| `get_page_from_freelist` | `page_alloc.c:3800` | 扫 zonelist |
| `__zone_watermark_ok` | `page_alloc.c:3598` | 水位判断 |
| `zone_watermark_ok` | `mmzone.h:1633` | 包装 |
| `rmqueue` | `page_alloc.c:3413` | PCP 或 buddy |
| `rmqueue_pcplist` | `:3371` | 每 CPU 列表 |
| `__rmqueue_pcplist` | `:3326` | 空了就批量补 |
| `rmqueue_bulk` | `:2535` | 持锁批量 `__rmqueue` |
| `rmqueue_buddy` | `:3221` | 不走 PCP |
| `__rmqueue` | `:2466` | smallest / cma / claim / steal |
| `__rmqueue_smallest` | `:1901` | 拆高阶 |
| `__rmqueue_claim` | `:2373` | 认领 pageblock |
| `__rmqueue_steal` | `:2428` | 偷页 |
| `__free_one_page` | `:940` | 合并 buddy |
| `__free_pages` | `:5565` | 释放 |
| `decay_pcp_high` | `:2576` | 闲时降低 PCP high |
| `reserve_highatomic_pageblock` | `:3447` | 高阶原子保留 |
| `unreserve_highatomic_pageblock` | `:3496` | 压力下归还保留 |
| `__alloc_pages_slowpath` | `:4784` | 回收 / 压缩 / OOM |
| `__alloc_pages_direct_compact` | `:4139` | 直接压缩 |
| `should_compact_retry` | `:4248` | 要不要再压 |
| `__alloc_pages_direct_reclaim` | `:4464` | 直接回收 |
| `__alloc_pages_may_oom` | `:4044` | 进入 OOM |
| `wakeup_kswapd` | `vmscan.c:7573` | 叫醒后台回收 |
| `alloc_pages_exact_noprof` | `page_alloc.c:5633` | 非 2 的幂，内部分高阶再砍 |
| `get_free_pages_noprof` | `:5503` | 返回虚拟地址。不能带 HIGHMEM |

---

[上一节 物理布局](./02-physical-layout.md) · [目录](./README.md) · [下一节 SLUB](./04-slab.md)
