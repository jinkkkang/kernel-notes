# 3.3 快路径：`get_page_from_freelist`

**结论**：快路径沿 zonelist 走。每个 zone 先过 cpuset、脏页配额和水位，过了才 `rmqueue`。水位用 LOW：空闲页掉到 LOW 以下就该把 kswapd 拉起来，而不是立刻在分配上下文里回收。kswapd 把水位拉回 HIGH 再睡。MIN 是给 PF_MEMALLOC、原子分配的底。

> `get_page_from_freelist` 在 `mm/page_alloc.c:3800`。水位判断在同文件 `:3683` 和 `:3598`。
> 导航：[上一篇 GFP](./03-02-gfp-and-scope.md) · [本目录](./03-00-page-alloc.md) · [下一篇 PCP](./03-04-pcp-and-rmqueue.md)

---

## 每个 zone 上做什么

`get_page_from_freelist`（`:3800`）从 `ac->preferred_zoneref` 起扫，上限是 `ac->highest_zoneidx`，节点掩码是 `ac->nodemask`。

1. cpuset 开着且带 `ALLOC_CPUSET` 时，`__cpuset_zone_allowed` 不通过就跳过。这是和 cgroup cpuset 的交界，策略细节不在本章。
2. 脏页分配（`ac->spread_dirty_pages`）会看 `node_dirty_ok`，避免一个 node 吃掉全局脏页配额。
3. `ALLOC_NOFRAGMENT` 且将要落到远程节点时，清掉这个标志再重试：本地性优先于防碎片（`:3856`）。
4. 多节点时，kswapd 已经在跑的 node 先跳过，避免误打成 node reclaim（`:3872` 一带）。`nr_online_nodes > 1` 才启用这步（`:3808`）。
5. 水位不过就下一个 zone。order 0 先走 `zone_watermark_fast`（`:3683`），它内部失败再落到 `__zone_watermark_ok`（`:3598`）。
6. 过了就 `rmqueue()`。PCP、拆 buddy、claim、steal 在 [04](./03-04-pcp-and-rmqueue.md)。

水位四档（`include/linux/mmzone.h:808` 的 `enum zone_watermarks`）：`WMARK_MIN`、`WMARK_LOW`、`WMARK_HIGH`，再加 `WMARK_PROMO`。存在 `struct zone._watermark[]`。

`/proc/zoneinfo` 里 DMA 的 `min/low/high/promo` 就是这四档，单位是页。本机 DMA 很小（managed 3840 页），数字也小，见 [13](./13-observability.md)。

---

## `zone_watermark_fast` 与 `__zone_watermark_ok`

order 0 有一条便宜检查（`:3695-3706`）：

```c
usable_free = NR_FREE_PAGES - min(NR_FREE_PAGES, __zone_watermark_unusable_free(...));
if (usable_free > mark + z->lowmem_reserve[highest_zoneidx])
    return true;   /* 够用，不必再算高阶空闲分布 */
```

注释写明 reserved 可能高估 HIGHATOMIC 保留。这条快检查失败，或者 order 不是 0，才进 `__zone_watermark_ok`：按 order 看空闲页够不够，并且扣掉不可用的保留。

`lowmem_reserve[]` 会从高 zone 的分配里扣掉，防止 DMA32 被 NORMAL 的分配榨干。`alloc_context.highest_zoneidx` 的注释（`page_alloc.h:82-90`）说的就是这件事：比最高可用 zone 更低的 zone，用 `lowmem_reserve[highest_zoneidx]` 保护。

`watermark_boost` 是 zone 上临时抬高的水位（物理布局章的 zone 成员）。碎片或分配压力会 boost，促使 kswapd 多回收一点。快路径对它的特例在 `zone_watermark_fast`（`:3712-3722`）：`__GFP_HIGH` 的 order-0 分配在查 MIN 水位时，忽略 boost。注释写明 MIN 是「boost 被忽略」的那一点，这样水位低于 LOW 时仍能把 kswapd 叫醒，而原子分配不会被临时抬高的水位挡在外面。

水位不够时，`rmqueue` 的末尾在 zone 被 boost 过的情况下还会 `wakeup_kswapd`（见 [04](./03-04-pcp-and-rmqueue.md)）。叫醒之后分配者自己继续找下一个 zone，不在这里睡觉。睡觉是 slowpath 的事。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `get_page_from_freelist` | `mm/page_alloc.c:3800` | 扫 zonelist，水位过了才 `rmqueue` |
| `zone_watermark_fast` | `page_alloc.c:3683` | order 0 的便宜水位检查，失败再进完整判断 |
| `__zone_watermark_ok` | `page_alloc.c:3598` | 按 order、水位、保留算够不够 |
| `zone_watermark_ok` | `include/linux/mmzone.h:1633` | 包装 |
| `wakeup_kswapd` | `mm/vmscan.c:7573` | 水位不够时叫醒后台回收。回收本体在 [10-01-overview.md](./10-01-overview.md) |

---

[上一篇 GFP](./03-02-gfp-and-scope.md) · [本目录](./03-00-page-alloc.md) · [下一篇 PCP 与 rmqueue](./03-04-pcp-and-rmqueue.md)
