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

`GFP_NOFS` / `GFP_NOIO` 也会通过 scope API 变成当前任务上的标志，分配入口再改写 gfp。位怎么清、为什么新代码该用 scope 而不是在调用点写死 `GFP_NOFS`，见 [3.7](#37-scope-api-怎么改写-gfp)。

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

## 3.7 scope API 怎么改写 gfp

**结论**：文件系统或块层拿着自己的锁时，分配不能再递归进文件系统或提交 IO。新代码用 `memalloc_nofs_save` / `memalloc_noio_save` 把约束记在 `current->flags` 上，让这段范围内的每一次分配自动降级。调用点继续写 `GFP_KERNEL`。在每个 `kmalloc` 上手工改成 `GFP_NOFS` 会漏，也会在锁已经放下之后仍然不敢进文件系统。

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

所以「我在调用点写了 `GFP_KERNEL`」只说明调用者的愿望。真正生效的是 scope 叠完之后的那份。`GFP_NOFS` / `GFP_NOIO` 这两个组合宏还在（[3.4](#34-gfp-标志)），留给改不到调用栈上游的老路径。能包住临界区时用 scope。

同文件还有 `memalloc_noreclaim_save`（`:427`），打 `PF_MEMALLOC`，分配隐式加上 `__GFP_MEMALLOC`，能进保留内存、不再进入回收。注释写明只给「分配是为了马上放出更多内存」的路径，并且中断上下文没有这份特权。单次分配可以用 `__GFP_NOMEMALLOC` 退出这个 scope。

---

## 3.8 NUMA 策略与 numactl

**结论**：策略回答「这次页从哪个 node 来」。进程有一份，VMA 可以另有一份。VMA 上的优先。中断不看当前进程的策略（`mempolicy.h:30`）。用户态入口是 `set_mempolicy`、`mbind`、`migrate_pages`，numactl 是它们的命令行壳。

`struct mempolicy`（`include/linux/mempolicy.h:47`，`CONFIG_NUMA`）：

| 成员 | 中文 |
|---|---|
| `atomic_t refcnt` | 引用。`mpol_put` 减到 0 释放。`mpol_dup` 出来的新对象引用从 1 起 |
| `unsigned short mode` | `MPOL_*`，见下 |
| `unsigned short flags` | `MPOL_F_STATIC_NODES` / `RELATIVE_NODES` / `NUMA_BALANCING`，以及内部的 `MPOL_F_SHARED`、`MPOL_F_MOF`、`MPOL_F_MORON` |
| `nodemask_t nodes` | bind / interleave / preferred 用的节点集合 |
| `int home_node` | `MPOL_BIND` 和 `MPOL_PREFERRED_MANY` 的 home node |
| `nodemask_t cpuset_mems_allowed` | 与下一字段同 union。相对模式时，相对这组 cpuset 允许的节点 |
| `nodemask_t user_nodemask` | 同 union。用户传入的 nodemask |
| `struct rcu_head rcu` | 释放走 RCU |

没编 `CONFIG_NUMA` 时，`struct mempolicy` 是空结构（`:188`），`get_task_policy` 返回 NULL。

模式（`include/uapi/linux/mempolicy.h:19`）：

| 值 | 行为 |
|---|---|
| `MPOL_DEFAULT` | 删掉这段上的策略，退回进程默认。进程默认再空，就是本地 node 优先、允许回退 |
| `MPOL_PREFERRED` | 优先一个 node，没有就回退 |
| `MPOL_BIND` | 只从 `nodes` 里拿 |
| `MPOL_INTERLEAVE` | 按页在 `nodes` 里轮转。进程自己的 interleave 游标不加锁（注释 `:34`） |
| `MPOL_LOCAL` | 分配发生在哪个 CPU 的 node，就从哪拿 |
| `MPOL_PREFERRED_MANY` | 一组优先 node，都没有再回退 |
| `MPOL_WEIGHTED_INTERLEAVE` | 按节点带宽加权轮转 |

`MPOL_F_STATIC_NODES`：用户给的节点号不随 cpuset 重新解释。`MPOL_F_RELATIVE_NODES`：当成相对 cpuset 允许集合的偏移。两个不要同时设。`MPOL_F_NUMA_BALANCING`：允许 NUMA balancing 在这个策略上做 migrate-on-fault。

共享映射（shmem、某些文件）的策略不挂在单个 VMA 上，挂在区间树里。`struct shared_policy`（`:113`）两个成员：`struct rb_root root`、`rwlock_t lock`。树上的 `struct sp_node`（`:117`）：`struct rb_node nd`、`pgoff_t start`、`pgoff_t end`、`struct mempolicy *policy`。

```text
set_mempolicy(mode, nmask, maxnode)          # SYSCALL_DEFINE3 mempolicy.c:1857
  → kernel_set_mempolicy                      # :1838
       sanitize_mpol_flags
       装到 current 的进程策略

mbind(start, len, mode, nmask, maxnode, flags)  # SYSCALL_DEFINE6 :1830
  → kernel_mbind                              # :1742
       → do_mbind                             # :1489
            拆/改 VMA 上的 vm_policy
            MPOL_MF_MOVE / MOVE_ALL 时把已有页迁走
            MPOL_MF_STRICT 且迁不动：返回 -EIO

分配
  alloc_pages_noprof                          # :2575，函数表在 3.10
    → alloc_pages_mpol                        # :2441
         policy_nodemask() 算出 nid 和 nodemask
         MPOL_PREFERRED_MANY → alloc_pages_preferred_many
         PMD 阶且策略允许当前 node：先 __GFP_THISNODE|__GFP_NORETRY
         否则 __alloc_frozen_pages_noprof
```

`flags` 里 `MPOL_MF_STRICT`（`:51`）要求现有页也符合策略，`MPOL_MF_MOVE` 迁本进程的页，`MPOL_MF_MOVE_ALL` 连别人的共享页一起迁。`MPOL_MF_LAZY` 注释标明是不支持的标志。

numactl 常见对应：`numactl --membind` → `MPOL_BIND`，`--interleave` → `MPOL_INTERLEAVE`，`--preferred` → `MPOL_PREFERRED`，`--localalloc` → `MPOL_LOCAL`。`--physcpubind` 是 CPU 亲和，不进 `struct mempolicy`。

VMA 上的指针是 `vm_policy`（[06](./06-mm-and-vma.md)）。缺页分配匿名页时走这份策略。迁移本身见 [10](./10-reclaim.md) 的 `migrate_pages`。

### 节点性能坐标（numaperf）

`Documentation/admin-guide/mm/numaperf.rst` 讲的是：一个 node 上的内存带宽和延迟可以不一样（本地 DRAM、慢一档的内存）。内核用 `struct access_coordinate`（`include/linux/node.h:29`）装四个数：

| 成员 | 中文 |
|---|---|
| `unsigned int read_bandwidth` | 读带宽，MB/s |
| `unsigned int write_bandwidth` | 写带宽，MB/s |
| `unsigned int read_latency` | 读延迟，纳秒 |
| `unsigned int write_latency` | 写延迟，纳秒 |

`ACCESS_COORDINATE_LOCAL` 是到最近 initiator 的坐标，`ACCESS_COORDINATE_CPU` 是到最近 CPU node 的坐标（`:42`）。用户态从 `/sys/devices/system/node/nodeN/access0` / `access1` 读，具体文件名以该文档和当前内核的 sysfs 为准。

`mempolicy_set_node_perf`（`mm/mempolicy.c:221`）把坐标收成带宽：`min(read_bandwidth, write_bandwidth)`，写进 `node_bw_table`。`MPOL_WEIGHTED_INTERLEAVE` 的权重在 `struct weighted_interleave_state`（`mempolicy.c:158`）：`bool mode_auto`、柔性数组 `u8 iw_table[]`。auto 模式下按带宽重算权重；手工模式只更新带宽表，不改当前权重（函数注释 `:242-245`）。

---

## 3.9 CMA

**结论**：CMA 在启动时划出物理连续的页，平时借给可移动分配用，设备要大块连续内存时再迁走借用者、把这块还回来。它解决的是「平时不能闲置一大块，驱动偶尔却要连续物理页」。

区描述在 `mm/cma.h`。一块区可以有多段物理范围（中间有洞），最多 `CMA_MAX_RANGES`（8，`:38`）。

`struct cma_memrange`（`:27`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long base_pfn` | 始终 | 这段的起始 PFN |
| `unsigned long count` | 始终 | 这段有多少页 |
| `unsigned long early_pfn` | 与 `bitmap` 同 union | 早期 `cma_reserve_early` 还没划走的第一个 PFN |
| `unsigned long *bitmap` | 同 union | 运行时的分配位图。1 bit 代表 `1 << order_per_bit` 页 |
| `struct debugfs_u32_array dfs_bitmap` | `CONFIG_CMA_DEBUGFS` | 把位图露到 debugfs |

`struct cma_kobject`（`:9`）：`struct kobject kobj`、`struct cma *cma`。

`struct cma`（`:40`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long count` | 始终 | 各 range 页数之和 |
| `unsigned long available_count` | 始终 | 还没被 CMA 分配走的页 |
| `unsigned int order_per_bit` | 始终 | 位图里 1 bit 对应的页阶 |
| `spinlock_t lock` | 始终 | 位图和计数 |
| `struct mutex alloc_mutex` | 始终 | 分配路径，避免并发在同一块里找空洞 |
| `struct hlist_head mem_head` | `CONFIG_CMA_DEBUGFS` | debugfs 跟踪的已分配块 |
| `spinlock_t mem_head_lock` | 同上 | 保护 `mem_head` |
| `char name[CMA_MAX_NAME]` | 始终 | 区名，tracepoint 和失败日志用 |
| `int nranges` | 始终 | `ranges` 里有效的段数 |
| `struct cma_memrange ranges[CMA_MAX_RANGES]` | 始终 | 物理段 |
| `atomic64_t nr_pages_succeeded` | `CONFIG_CMA_SYSFS` | 成功页数 |
| `atomic64_t nr_pages_failed` | 同上 | 失败页数 |
| `atomic64_t nr_pages_released` | 同上 | 放回页数 |
| `struct cma_kobject *cma_kobj` | 同上 | sysfs 对象 |
| `unsigned long flags` | 始终 | `CMA_RESERVE_PAGES_ON_ERROR`、`CMA_ZONES_VALID`、`CMA_ZONES_INVALID`、`CMA_ACTIVATED`（`:68`） |
| `int nid` | 始终 | 所属 node。没指定时是 `NUMA_NO_NODE` |

全局数组 `cma_areas[MAX_CMA_AREAS]`（`mm/cma.c:38`）。

```text
cma_alloc(cma, count, align, no_warn)     # cma.c:949
  → cma_alloc_frozen                        # :924，gfp = GFP_KERNEL，no_warn 则加 __GFP_NOWARN
       → __cma_alloc_frozen                 # :866
            逐个 range 调 cma_range_alloc   # 位图里找连续的 count 页
            成功：清 KASAN tag，CMA_ALLOC_SUCCESS
            失败：CMA_ALLOC_FAIL，按 no_warn 决定要不要 pr_err
  → set_pages_refcounted                    # 交给调用者之前把引用立上
```

`cma_alloc` 返回的页带正常引用。`cma_alloc_frozen` 留下的是 frozen（refcount 0），调用者自己负责立引用或走 frozen 释放。还回用 `cma_release`（`:1019`），内部 `free_contig_frozen_range` 再清位图。

buddy 侧：可移动分配在 CMA 空闲超过 zone 一半时会先从 CMA 拿（[3.3](#33-rmqueue--pcp--__rmqueue)）。CMA pageblock 的 migratetype 不能和普通块合并，这是高阶分配失败的原因之一（[3.6](#36-高阶分配为什么失败)）。debugfs 在 `CONFIG_CMA_DEBUGFS` 下看位图；`vmstat` 里有 `cma_alloc_success` / `cma_alloc_fail`（`mm/vmstat.c:1402`）。

---

## 3.10 关键函数表

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
| `current_gfp_context` | `sched/mm.h:248` | 按 PF_MEMALLOC_NOIO/NOFS/PIN 改写 gfp |
| `memalloc_nofs_save` / `memalloc_noio_save` | `sched/mm.h:386` / `:356` | 打开 NOFS / NOIO scope |
| `memalloc_noreclaim_save` | `sched/mm.h:427` | 打开 PF_MEMALLOC scope |
| `SYSCALL_DEFINE3(set_mempolicy)` | `mempolicy.c:1857` | 设进程策略 |
| `SYSCALL_DEFINE6(mbind)` | `mempolicy.c:1830` | 设 VMA 策略，可顺带迁移 |
| `kernel_mbind` / `do_mbind` | `:1742` / `:1489` | mbind 的两层实现 |
| `alloc_pages_mpol` | `:2441` | 按 mempolicy 分配 |
| `mempolicy_set_node_perf` | `:221` | 用带宽坐标更新 weighted interleave |
| `cma_alloc` | `mm/cma.c:949` | 从 CMA 区拿连续页 |
| `__cma_alloc_frozen` | `mm/cma.c:866` | 位图查找，返回 frozen 页 |
| `cma_release` | `mm/cma.c:1019` | 还回 CMA |

---

[上一节 物理布局](./02-physical-layout.md) · [目录](./README.md) · [下一节 SLUB](./04-slab.md)
