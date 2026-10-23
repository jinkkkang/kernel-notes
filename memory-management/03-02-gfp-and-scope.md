# 3.2 GFP 标志与 scope API

**结论**：gfp 是调用者的愿望，scope 是这段上下文的硬约束。文件系统或块层拿着自己的锁时，分配不能再递归进文件系统或提交 IO。新代码用 `memalloc_nofs_save` / `memalloc_noio_save` 把约束记在 `current->flags` 上，让这段范围内的每一次分配自动降级。调用点继续写 `GFP_KERNEL`。在每个 `kmalloc` 上手工改成 `GFP_NOFS` 会漏，也会在锁已经放下之后仍然不敢进文件系统。

> 位定义在 `include/linux/gfp_types.h`。组合宏在同文件后半。scope 在 `include/linux/sched/mm.h`，说明在 `Documentation/core-api/gfp_mask-from-fs-io.rst`。
> 导航：[上一篇 总入口](./03-01-overview.md) · [本目录](./03-00-page-alloc.md) · [下一篇 快路径](./03-03-fastpath-freelist.md)

---

## 区与迁移

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

## 回收与失败策略

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

## 其它

| 标志 | 行 | 含义 |
|---|---|---|
| `__GFP_NOWARN` | 282 | 失败不警告 |
| `__GFP_COMP` | 283 | 组成复合页 / folio |
| `__GFP_ZERO` | 284 | 清零 |
| `__GFP_ZEROTAGS` | 285 | 架构 tag 清零 |
| `__GFP_SKIP_ZERO` | 286 | 调用者保证会自己清零，分配器可以跳过 |
| `__GFP_SKIP_KASAN` | 287 | 跳过 KASAN |
| `__GFP_NOLOCKDEP` | 290 | 不进 lockdep |

## 常用组合

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
| `GFP_TRANSHUGE_LIGHT` | 381 | 可移动复合大页，不直接回收 |
| `GFP_TRANSHUGE` | 383 | LIGHT 再加直接回收 |

`GFP_NOFS` / `GFP_NOIO` 这两个组合宏还在，留给改不到调用栈上游的老路径。能包住临界区时用下面的 scope。

---

## scope API 怎么改写 gfp

`Documentation/core-api/gfp_mask-from-fs-io.rst` 讲的就是这件事。实现在 `include/linux/sched/mm.h`。

`memalloc_nofs_save`（`:386`）只做一件事：`memalloc_flags_save(PF_MEMALLOC_NOFS)`。`memalloc_noio_save`（`:356`）打的是 `PF_MEMALLOC_NOIO`。返回值是「这次新加上的那些位」，交给配对的 `memalloc_nofs_restore` / `memalloc_noio_restore` 清掉。save/restore 必须成对，嵌套按位恢复，不要把返回值丢掉。

分配入口在用 gfp 之前调用 `current_gfp_context`（`sched/mm.h:248`）。`__alloc_frozen_pages_noprof` 里就是这样（`page_alloc.c:5412-5419`）：

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

`current_gfp_context` 读 `current->flags`，按位改写传入的 gfp，不改任务标志：

| 任务标志 | 对 gfp 做的事 |
|---|---|
| `PF_MEMALLOC_NOIO` | 清掉 `__GFP_IO` 和 `__GFP_FS`。NOIO 比 NOFS 更严，两者同时在时 NOIO 优先 |
| `PF_MEMALLOC_NOFS` | 只清 `__GFP_FS` |
| `PF_MEMALLOC_PIN` | 清 `__GFP_MOVABLE`，避免把页放进可移动区（长期 pin、RDMA 一类） |

所以「我在调用点写了 `GFP_KERNEL`」只说明调用者的愿望。真正生效的是 scope 叠完之后的那份。

同文件还有 `memalloc_noreclaim_save`（`:427`），打 `PF_MEMALLOC`，分配隐式加上 `__GFP_MEMALLOC`，能进保留内存、不再进入回收。注释写明只给「分配是为了马上放出更多内存」的路径，并且中断上下文没有这份特权。单次分配可以用 `__GFP_NOMEMALLOC` 退出这个 scope。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `current_gfp_context` | `include/linux/sched/mm.h:248` | 按 PF_MEMALLOC_NOIO/NOFS/PIN 改写 gfp |
| `memalloc_nofs_save` | `sched/mm.h:386` | 打开 NOFS scope |
| `memalloc_noio_save` | `sched/mm.h:356` | 打开 NOIO scope |
| `memalloc_noreclaim_save` | `sched/mm.h:427` | 打开 PF_MEMALLOC scope |

---

[上一篇 总入口](./03-01-overview.md) · [本目录](./03-00-page-alloc.md) · [下一篇 快路径水位](./03-03-fastpath-freelist.md)
