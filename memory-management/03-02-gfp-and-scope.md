# 3.2 GFP 标志与 scope API

**结论**：gfp 是调用者的愿望，scope 是这段上下文的硬约束。文件系统或块层拿着自己的锁时，分配不能再递归进文件系统或提交 IO。新代码用 `memalloc_nofs_save` / `memalloc_noio_save` 把约束记在 `current->flags` 上，让这段范围内的每一次分配自动降级。调用点继续写 `GFP_KERNEL`。在每个 `kmalloc` 上手工改成 `GFP_NOFS` 会漏，也会在锁已经放下之后仍然不敢进文件系统。

> 位掩码 `___GFP_*` 和组合宏 `GFP_KERNEL` 等都在 `include/linux/gfp_types.h`。zone 查表在 `include/linux/gfp.h`。scope 在 `include/linux/sched/mm.h`，说明在 `Documentation/core-api/gfp_mask-from-fs-io.rst`。
> 内核版本：**v6.6.160**。
> 导航：[上一篇 总入口](./03-01-overview.md) · [本目录](./03-00-page-alloc.md) · [下一篇 快路径](./03-03-fastpath-freelist.md)

---

## 位是怎么编码的

`gfp_t` 是一个按位使用的整数。`gfp_types.h:27` 起用 `#define ___GFP_*` 写出每个位的掩码，再用 `__GFP_*` 转成 `gfp_t`。位号就是掩码里那一个 1 的位置。`0x200`（位 9）空着，源码写了 `/* 0x200u unused */`（`:36`）。

低 4 位同时是 zone 选择。`gfp_zone`（`gfp.h:129`）拿 `flags & GFP_ZONEMASK` 当 `GFP_ZONE_TABLE` 的下标。

| 宏 | 行 | 掩码 | 位 | 含义 |
|---|---|---|---|---|
| `___GFP_DMA` | 27 | `0x01` | 0 | 只要 ZONE_DMA。没编 DMA 时查表落到 NORMAL |
| `___GFP_HIGHMEM` | 28 | `0x02` | 1 | 允许高端内存。x86-64 通常没有 HIGHMEM，查表落到 NORMAL |
| `___GFP_DMA32` | 29 | `0x04` | 2 | DMA32 |
| `___GFP_MOVABLE` | 30 | `0x08` | 3 | 允许 ZONE_MOVABLE，migratetype 走 MOVABLE |
| `___GFP_RECLAIMABLE` | 31 | `0x10` | 4 | 可回收，migratetype 走 RECLAIMABLE |
| `___GFP_HIGH` | 32 | `0x20` | 5 | 高优先级，可以动 min 保留的一半 |
| `___GFP_IO` | 33 | `0x40` | 6 | 回收时允许发起 IO。没有它就不能 compact |
| `___GFP_FS` | 34 | `0x80` | 7 | 回收时允许进文件系统 |
| `___GFP_ZERO` | 35 | `0x100` | 8 | 返回前把页清零 |
| （空） | 36 | `0x200` | 9 | 源码标明不用 |
| `___GFP_DIRECT_RECLAIM` | 37 | `0x400` | 10 | 调用者可以自己回收，也就可以睡眠 |
| `___GFP_KSWAPD_RECLAIM` | 38 | `0x800` | 11 | 可以唤醒 kswapd。数值等于 `ALLOC_KSWAPD` |
| `___GFP_WRITE` | 39 | `0x1000` | 12 | 这笔分配会计入写脏页，快路径看 `node_dirty_ok` |
| `___GFP_NOWARN` | 40 | `0x2000` | 13 | 失败不警告 |
| `___GFP_RETRY_MAYFAIL` | 41 | `0x4000` | 14 | 多试，但仍允许失败 |
| `___GFP_NOFAIL` | 42 | `0x8000` | 15 | 不许失败。必须同时能直接回收，否则 slowpath `:4210` 有 `WARN` |
| `___GFP_NORETRY` | 43 | `0x10000` | 16 | 少试，尽快失败 |
| `___GFP_MEMALLOC` | 44 | `0x20000` | 17 | 这次分配可以不看水位 |
| `___GFP_COMP` | 45 | `0x40000` | 18 | 组成复合页 |
| `___GFP_NOMEMALLOC` | 46 | `0x80000` | 19 | 明确不碰保留。比 `__GFP_MEMALLOC` 和 `PF_MEMALLOC` 都优先 |
| `___GFP_HARDWALL` | 47 | `0x100000` | 20 | cpuset 硬墙 |
| `___GFP_THISNODE` | 48 | `0x200000` | 21 | 不跨 node。zonelist 用 `ZONELIST_NOFALLBACK` |
| `___GFP_ACCOUNT` | 49 | `0x400000` | 22 | 记到 memcg 的内核内存 |
| `___GFP_ZEROTAGS` | 50 | `0x800000` | 23 | 清零内存的同时清架构 tag |
| `___GFP_SKIP_ZERO` | 52 | `0x1000000` | 24 | 只在 `CONFIG_KASAN_HW_TAGS` 下占这位。调用者保证自己会清零 |
| `___GFP_SKIP_KASAN` | 53 | `0x2000000` | 25 | 同上。跳过 KASAN unpoison |
| `___GFP_NOLOCKDEP` | 59 | `0x4000000` | 26 | 只在 `CONFIG_LOCKDEP` 下占这位。不进 lockdep |

没编对应选项时，`SKIP_ZERO`、`SKIP_KASAN`、`NOLOCKDEP` 的宏是 0，不占位（`:54-61`）。`__GFP_BITS_SHIFT` 是 `26 + IS_ENABLED(CONFIG_LOCKDEP)`（`:254`）。

`GFP_ZONEMASK`（`:76`）是低 4 位：`__GFP_DMA | __GFP_HIGHMEM | __GFP_DMA32 | __GFP_MOVABLE`。

`__GFP_RECLAIM`（`:214`）是 `DIRECT_RECLAIM | KSWAPD_RECLAIM`。

## zone 查表

`gfp.h:57-86` 的注释把低 4 位的 16 种组合列完了。回退顺序是 MOVABLE → HIGHMEM → NORMAL → DMA32 → DMA。DMA、DMA32、HIGHMEM 这三个里最多只能有一个为 1，否则是 `GFP_ZONE_BAD`（`:118`），`gfp_zone` 里 `VM_BUG_ON`。

| 低 4 位 | 结果 |
|---|---|
| `0x0` | NORMAL |
| `0x1` | DMA，没编 DMA 则 NORMAL |
| `0x2` | HIGHMEM，没编则 NORMAL |
| `0x3` | 非法（DMA+HIGHMEM） |
| `0x4` | DMA32，没编则 NORMAL |
| `0x5`–`0x7` | 非法 |
| `0x8` | NORMAL（只设了 MOVABLE） |
| `0x9` | DMA 或 NORMAL（MOVABLE+DMA） |
| `0xa` | MOVABLE（注释：Movable 只在同时有 HIGHMEM 时才成立；没编 HIGHMEM 时这格查的是 `ZONE_MOVABLE`，见 `GFP_ZONE_TABLE` `:108`） |
| `0xc` | DMA32 或 NORMAL（MOVABLE+DMA32） |
| 其余带 MOVABLE 的组合 | 非法 |

`GFP_ZONE_TABLE`（`:101`）是一张塞进一个整数里的表。`gfp_zone` 用 `bit * GFP_ZONES_SHIFT` 去右移。有 `ZONE_DEVICE` 且 zone 数不超过 4 时，`GFP_ZONES_SHIFT` 固定为 2（`:90-95`），因为 `ZONE_DEVICE` 不能用 gfp 选。

`highest_zoneidx` 就是这张表查出来的 `enum zone_type`。比它低的 zone 仍可能被用到，但要扣 `lowmem_reserve`，见 [03](./03-03-fastpath-freelist.md)。

## 常用组合

| 宏 | 行 | 展开 |
|---|---|---|
| `GFP_ATOMIC` | 327 | `__GFP_HIGH \| __GFP_KSWAPD_RECLAIM`。不直接回收，可以叫醒 kswapd，并能动 min 保留 |
| `GFP_KERNEL` | 328 | `RECLAIM \| IO \| FS`。进程上下文的默认 |
| `GFP_KERNEL_ACCOUNT` | 329 | 再加 `ACCOUNT` |
| `GFP_NOWAIT` | 330 | 只有 `__GFP_KSWAPD_RECLAIM`。不直接回收，也不带 `NOWARN` |
| `GFP_NOIO` | 331 | 只有 `RECLAIM`。块层自己的分配，不能再进 IO，也不能 compact |
| `GFP_NOFS` | 332 | `RECLAIM \| IO`。文件系统里不能再进 FS，但可以 compact |
| `GFP_USER` | 333 | `RECLAIM \| IO \| FS \| HARDWALL` |
| `GFP_DMA` | 334 | 只要 `__GFP_DMA` |
| `GFP_DMA32` | 335 | 只要 `__GFP_DMA32` |
| `GFP_HIGHUSER` | 336 | `GFP_USER \| __GFP_HIGHMEM` |
| `GFP_HIGHUSER_MOVABLE` | 337 | HIGHUSER 再加 `MOVABLE` 和 `SKIP_KASAN` |
| `GFP_TRANSHUGE_LIGHT` | 338 | 可移动复合大页，把 `RECLAIM` 清掉，再加 `NOMEMALLOC` 和 `NOWARN` |
| `GFP_TRANSHUGE` | 340 | LIGHT 再加 `DIRECT_RECLAIM` |

`GFP_ATOMIC` 的注释（`:267-270`）写明：当前实现不支持 NMI，也不支持 raw 自旋锁一类不能睡眠的上下文。`GFP_NOWAIT` 同样。

`GFP_NOFS` / `GFP_NOIO` 这两个组合宏还在，留给改不到调用栈上游的老路径。能包住临界区时用下面的 scope。源码注释（`:283-292`）直接建议用 `memalloc_noio_save` / `memalloc_nofs_save`，不要在每个调用点手写这两个宏。

---

## scope API 怎么改写 gfp

`Documentation/core-api/gfp_mask-from-fs-io.rst` 讲的就是这件事。实现在 `include/linux/sched/mm.h`。

`memalloc_nofs_save`（`:351`）做两件事：记住 `current->flags` 里原来有没有 `PF_MEMALLOC_NOFS`，再把这一位置上。`memalloc_noio_save`（`:320`）打的是 `PF_MEMALLOC_NOIO`。返回值交给配对的 `memalloc_nofs_restore` / `memalloc_noio_restore`（`:366`、`:335`）。restore 只清掉自己这一位，再把当初记下的那一位或回去，所以嵌套是安全的。save/restore 必须成对，不要把返回值丢掉。

`__alloc_pages` 在用 gfp 之前调用 `current_gfp_context`（`page_alloc.c:4481-4488`）：

```c
gfp &= gfp_allowed_mask;
/*
 * Apply scoped allocation constraints. This is mainly about GFP_NOFS
 * resp. GFP_NOIO which has to be inherited for all allocation requests
 * from a particular context which has been marked by
 * memalloc_no{fs,io}_{save,restore}. And PF_MEMALLOC_PIN which ensures
 * movable zones are not used during allocation.
 */
gfp = current_gfp_context(gfp);
```

`current_gfp_context`（`sched/mm.h:235`）读 `current->flags`，按位改写传入的 gfp，不改任务标志：

```c
if (unlikely(pflags & (PF_MEMALLOC_NOIO | PF_MEMALLOC_NOFS | PF_MEMALLOC_PIN))) {
	if (pflags & PF_MEMALLOC_NOIO)
		flags &= ~(__GFP_IO | __GFP_FS);
	else if (pflags & PF_MEMALLOC_NOFS)
		flags &= ~__GFP_FS;

	if (pflags & PF_MEMALLOC_PIN)
		flags &= ~__GFP_MOVABLE;
}
```

| 任务标志 | 对 gfp 做的事 |
|---|---|
| `PF_MEMALLOC_NOIO` | 清掉 `__GFP_IO` 和 `__GFP_FS`。注释写明 NOIO 更严，两者同时在时先走 NOIO |
| `PF_MEMALLOC_NOFS` | 只清 `__GFP_FS`。上面那个 `else if` 保证 NOIO 已经处理过就不再看 NOFS |
| `PF_MEMALLOC_PIN` | 清 `__GFP_MOVABLE`。长期 pin、RDMA 一类不要把页放进可移动区。`memalloc_pin_save` 在 `:383` |

所以「我在调用点写了 `GFP_KERNEL`」只说明调用者的愿望。真正生效的是 scope 叠完之后的那份。

`memalloc_noreclaim_save`（`:371`）打的是 `PF_MEMALLOC`。`current_gfp_context` 不看这一位，gfp 掩码保持原样。slowpath 里 `__gfp_pfmemalloc_flags`（`page_alloc.c:3809`）看到 `PF_MEMALLOC` 就返回 `ALLOC_NO_WATERMARKS`：

- gfp 带 `__GFP_NOMEMALLOC`：直接返回 0，这次分配退出保留。
- gfp 带 `__GFP_MEMALLOC`：`ALLOC_NO_WATERMARKS`。
- 进程上下文里 `current->flags` 有 `PF_MEMALLOC`：同样不看水位。
- 正在服务的 softirq 里也认 `PF_MEMALLOC`。其它中断上下文不认，OOM 受害者的 `ALLOC_OOM` 也不在中断里给。

这份特权是给「这次分配是为了马上放出更多内存」的路径。slowpath 自己也会在直接回收和直接 compact 期间套上 `memalloc_noreclaim_save`（`:3674`、`:3430`），避免回收再递归进回收。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `gfp_migratetype` | `include/linux/gfp.h:16` | 从 MOVABLE / RECLAIMABLE 推出迁移类型 |
| `gfp_zone` | `gfp.h:129` | 低 4 位查 `GFP_ZONE_TABLE` |
| `gfp_zonelist` | `gfp.h:147` | `__GFP_THISNODE` 选 `ZONELIST_NOFALLBACK` |
| `gfpflags_allow_blocking` | `gfp.h:34` | 有 `__GFP_DIRECT_RECLAIM` 就可以睡眠 |
| `gfp_compaction_allowed` | `gfp.h:351` | 编了 `CONFIG_COMPACTION` 且 gfp 有 `__GFP_IO` |
| `current_gfp_context` | `include/linux/sched/mm.h:235` | 按 PF_MEMALLOC_NOIO / NOFS / PIN 改写 gfp |
| `memalloc_noio_save` / `memalloc_noio_restore` | `sched/mm.h:320` / `:335` | 打开 / 结束 NOIO scope |
| `memalloc_nofs_save` / `memalloc_nofs_restore` | `:351` / `:366` | 打开 / 结束 NOFS scope |
| `memalloc_noreclaim_save` / `memalloc_noreclaim_restore` | `:371` / `:378` | 打开 / 结束 `PF_MEMALLOC` scope |
| `memalloc_pin_save` / `memalloc_pin_restore` | `:383` / `:392` | 打开 / 结束 PIN scope |
| `__gfp_pfmemalloc_flags` | `mm/page_alloc.c:3809` | 把 `__GFP_MEMALLOC` 和 `PF_MEMALLOC` 变成 `ALLOC_NO_WATERMARKS` |

---

[上一篇 总入口](./03-01-overview.md) · [本目录](./03-00-page-alloc.md) · [下一篇 快路径水位](./03-03-fastpath-freelist.md)
