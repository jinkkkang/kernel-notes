# 3.3 快路径：`get_page_from_freelist`

**结论**：快路径沿 zonelist 走。每个 zone 先过 cpuset、脏页配额和水位，过了才 `rmqueue`。水位用 LOW。空闲页掉到 LOW 以下就叫醒 kswapd，分配者自己先换下一个 zone。kswapd 把水位拉回 HIGH 再睡。MIN 是给 `PF_MEMALLOC`、`GFP_ATOMIC` 这一类的底，slowpath 才把水位从 LOW 改成 MIN。

> `get_page_from_freelist` 在 `mm/page_alloc.c:3099`。水位判断在同文件 `:2971` 和 `:2886`。
> 内核版本：**v6.6.160**。
> 导航：[上一篇 GFP](./03-02-gfp-and-scope.md) · [本目录](./03-00-page-alloc.md) · [下一篇 PCP](./03-04-pcp-and-rmqueue.md)

---

## 每个 zone 上做什么

`get_page_from_freelist`（`:3099`）从 `ac->preferred_zoneref` 起扫。宏 `for_next_zone_zonelist_nodemask` 的上限是 `ac->highest_zoneidx`，节点掩码是 `ac->nodemask`。

1. cpuset 开着且 `alloc_flags` 带 `ALLOC_CPUSET` 时，`__cpuset_zone_allowed` 不通过就跳过这个 zone。这是和 cgroup cpuset 的交界，策略细节不在本章。
2. `ac->spread_dirty_pages` 为真（gfp 有 `__GFP_WRITE`，而且这是快路径）时看 `node_dirty_ok`（`mm/page-writeback.c:495`）。一个 node 的脏页已经超过它那一份配额，就换下一个 zone。同一 node 上的后续 zone 复用刚才的结果。
3. `ALLOC_NOFRAGMENT` 还在，而且机器上不止一个 node，一旦扫到的 zone 不在首选 zone 的那个 node 上，就清掉这个标志，从 zonelist 头重试（`:3153-3166`）。注释写明：本地性比防碎片重要。UMA 上如果所有 zone 都因为防碎片没分到，函数末尾也会清掉标志再试一次（`:3249-3251`）。
4. `cond_accept_memory`：有尚未 accept 的内存时，先接受一块再继续。水位失败或 `rmqueue` 失败后还会再试一次。
5. 水位不过就看能不能在这个 zone 上做 node reclaim。`vm.zone_reclaim_mode`（`kernel/sysctl.c:2176`，变量 `node_reclaim_mode`）没开，或者 `zone_allows_reclaim` 不让，就换下一个 zone。开了就调用 `node_reclaim`（`mm/vmscan.c:8075`）。扫了但收不回、或者收回来水位仍不够，也换下一个。
6. 水位过了，或者上面的补救成功，就 `rmqueue()`。PCP、拆 buddy、fallback 在 [04](./03-04-pcp-and-rmqueue.md)。`rmqueue` 返回 NULL 时，还会再试 accept 和延迟初始化的页（`CONFIG_DEFERRED_STRUCT_PAGE_INIT` 下的 `_deferred_grow_zone`）。

`alloc_flags` 带 `ALLOC_NO_WATERMARKS` 时，水位失败直接去 `rmqueue`，不再 reclaim（`:3192-3193`）。

水位四档（`include/linux/mmzone.h:658` 的 `enum zone_watermarks`）：

| 枚举 | 值 | 谁在用 |
|---|---|---|
| `WMARK_MIN` | 0 | 原子分配、slowpath。再往下就是保留 |
| `WMARK_LOW` | 1 | 快路径。掉到这以下该叫醒 kswapd |
| `WMARK_HIGH` | 2 | kswapd 把空闲页拉回到这里再睡 |
| `WMARK_PROMO` | 3 | 比 HIGH 再高一档，给晋升一类的判断 |
| `NR_WMARK` | 4 | `_watermark[]` 的长度 |

它们存在 `struct zone._watermark[]`（`mmzone.h:819`）。运行时不要直接拿数组元素去比。`min_wmark_pages` / `low_wmark_pages` / `high_wmark_pages` / `wmark_pages`（`:679-682`）都会再加上 `zone->watermark_boost`。

`/proc/zoneinfo` 里 DMA 的 `min/low/high/promo` 就是这四档，单位是页。在 6.12 上采样的这台机器 DMA 很小（managed 3840 页，min/low/high/promo 是 15/18/21/24），见 [13](./13-observability.md)。

---

## 水位数字从哪来

`__setup_per_zone_wmarks`（`page_alloc.c:5718`）在 `min_free_kbytes` 变化或内存热插拔时重算。

- `pages_min` 来自 `min_free_kbytes`。非 HIGHMEM、非 MOVABLE 的 zone 按自己的 managed 页数分摊这份 MIN。
- HIGHMEM 和 MOVABLE 的 MIN 另有上限：managed/1024，再夹在 `SWAP_CLUSTER_MAX` 和 128 页之间。注释说 `__GFP_HIGH` 和 `PF_MEMALLOC` 通常用不到这些 zone。
- LOW、HIGH、PROMO 之间的间距 `tmp`，取「刚才那个比例值右移 2 位」和 `managed * watermark_scale_factor / 10000` 里较大的一个。然后 `LOW = MIN + tmp`，`HIGH = LOW + tmp`，`PROMO = HIGH + tmp`。算的时候先把 `watermark_boost` 清零，所以写进 `_watermark[]` 的是不带 boost 的底数。

相关 sysctl 在 `page_alloc.c:6027` 的 `page_alloc_sysctl_table`：

| proc 名 | 变量 | 默认 | 作用 |
|---|---|---|---|
| `min_free_kbytes` | `min_free_kbytes` | 启动时算 | MIN 的总量 |
| `watermark_scale_factor` | `watermark_scale_factor`（`page_alloc.c:317`） | 10 | LOW/HIGH/PROMO 间距。范围 1 到 3000 |
| `watermark_boost_factor` | `watermark_boost_factor`（`page_alloc.c:316`） | 15000 | 碎片时临时把水位抬高的上限系数 |
| `lowmem_reserve_ratio` | `sysctl_lowmem_reserve_ratio`（`page_alloc.c:270`） | DMA/DMA32 为 256，NORMAL 为 32 | 算 `lowmem_reserve[]` |
| `percpu_pagelist_high_fraction` | `percpu_pagelist_high_fraction` | 0 | PCP 的 high 占 zone 的多少。0 表示用内置算法 |

`vm.zone_reclaim_mode` 不在这张表里，它在 `kernel/sysctl.c:2176`。

---

## `zone_watermark_fast` 与 `__zone_watermark_ok`

order 0 有一条便宜检查（`:2983-2993`）：

```c
usable_free = free_pages;
reserved = __zone_watermark_unusable_free(z, 0, alloc_flags);

/* reserved may over estimate high-atomic reserves. */
usable_free -= min(usable_free, reserved);
if (usable_free > mark + z->lowmem_reserve[highest_zoneidx])
	return true;
```

`__zone_watermark_unusable_free`（`:2858`）先扣掉 `(1 << order) - 1`，因为高阶分配至少要这么多页才凑得齐一块。没有 `ALLOC_RESERVES` 时再加上 `nr_reserved_highatomic`。不能用 CMA 时再加上 `NR_FREE_CMA_PAGES`。注释写明 highatomic 这项可能高估。

这条快检查失败，或者 order 不是 0，才进 `__zone_watermark_ok`（`:2886`）：

- 空闲页扣掉不可用保留之后，必须大于 `mark + lowmem_reserve[highest_zoneidx]`。
- order 0 到这里就可以返回真。
- 高阶还要在 `free_area[order]` 往上找到至少一块合适的空闲页。先看 `MIGRATE_PCPTYPES` 里的类型。带 `ALLOC_CMA` 再看 CMA。带 `ALLOC_HIGHATOMIC` 或 `ALLOC_OOM` 再看 HIGHATOMIC。

带 `ALLOC_RESERVES` 时，min 会先被削掉一块（`:2896-2923`）：

| 标志 | 对 min 做什么 |
|---|---|
| `ALLOC_MIN_RESERVE` | `min -= min / 2`。这是 `__GFP_HIGH` |
| 同时还是 `ALLOC_NON_BLOCK` | 再 `min -= min / 4`。`GFP_ATOMIC` 两条都有 |
| `ALLOC_OOM` | 再 `min -= min / 2`。OOM 受害者 |

`GFP_NOWAIT` 和「`GFP_KERNEL` 去掉 `DIRECT_RECLAIM`」拿不到 `ALLOC_MIN_RESERVE`，也就不能挖这截保留。`gfp_to_alloc_flags` 的注释（`:3754-3773`）写的就是这个区别。

`lowmem_reserve[]` 在 `struct zone` 里（`mmzone.h:833`）。高 zone 的分配来借低 zone 时，要把这份保留扣掉，避免 NORMAL 把 DMA/DMA32 榨干。`alloc_context.highest_zoneidx` 的注释（`mm/internal.h:294-303`）说的就是这件事。

`watermark_boost` 是 zone 上临时抬高的水位。fallback 偷页时 `boost_watermark`（`page_alloc.c:1768`）按 `watermark_boost_factor / 10000` 抬高，一次加一个 pageblock，上限至少也是一个 pageblock。小 zone（4 个 pageblock 都放不下）不抬，避免启动早期直接 OOM。

快路径对 boost 的特例在 `zone_watermark_fast`（`:3000-3011`）：order 0、带 `ALLOC_MIN_RESERVE`、正在查的又是 MIN 时，忽略 boost，改用 `_watermark[WMARK_MIN]` 再算一遍。注释写明 MIN 是「boost 被忽略」的那一点，这样水位低于 LOW 时仍能把 kswapd 叫醒，而带 `__GFP_HIGH` 的原子分配不会被临时抬高的水位挡在外面。

水位不够时，`rmqueue` 的末尾如果 zone 被打过 `ZONE_BOOSTED_WATERMARK`，还会 `wakeup_kswapd`（见 [04](./03-04-pcp-and-rmqueue.md)）。叫醒之后分配者自己继续找下一个 zone，不在这里睡觉。睡觉是 slowpath 的事。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `get_page_from_freelist` | `mm/page_alloc.c:3099` | 扫 zonelist，水位过了才 `rmqueue` |
| `zone_watermark_fast` | `page_alloc.c:2971` | order 0 的便宜水位检查，失败再进完整判断 |
| `__zone_watermark_ok` | 声明 `include/linux/mmzone.h:1424`，定义 `page_alloc.c:2886` | 按 order、水位、保留算够不够 |
| `zone_watermark_ok` | 声明 `mmzone.h:1427`，定义 `page_alloc.c:2964` | 用 `NR_FREE_PAGES` 包一层 |
| `__zone_watermark_unusable_free` | `page_alloc.c:2858` | highatomic 保留、CMA 空闲、高阶对齐浪费 |
| `__setup_per_zone_wmarks` | `page_alloc.c:5718` | 由 `min_free_kbytes` 和 scale factor 填四档水位 |
| `node_dirty_ok` | 声明 `include/linux/writeback.h:335`，定义 `mm/page-writeback.c:495` | 这个 node 还能不能再脏 |
| `node_reclaim` | `mm/vmscan.c:8075` | `vm.zone_reclaim_mode` 打开时，在快路径里对本 node 做一次回收 |
| `wakeup_kswapd` | 声明 `mmzone.h:1422`，定义 `mm/vmscan.c:7817` | 水位不够或 boost 时叫醒后台回收。回收本体在 [10-01-overview.md](./10-01-overview.md) |

---

[上一篇 GFP](./03-02-gfp-and-scope.md) · [本目录](./03-00-page-alloc.md) · [下一篇 PCP 与 rmqueue](./03-04-pcp-and-rmqueue.md)
