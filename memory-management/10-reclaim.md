# 10. 回收、swap、OOM

**结论**：v7.3 同时有传统 LRU 和 MGLRU（`CONFIG_LRU_GEN`）。`shrink_node` 在全局回收且 MGLRU 打开时走 `lru_gen_shrink_node`，否则走按 memcg 的 `shrink_node_memcgs`。kswapd 在后台把水位拉回 HIGH；直接回收在分配者上下文里做同一件事，延迟算在那个任务上。没有 swap 时匿名页收不走。

> 主文件：`mm/vmscan.c`。OOM：`mm/oom_kill.c`。swap 压缩缓存：`mm/zswap.c`。
> memcg 的 high/max/charge 不在这里重复，见 [cgroup/06-memory.md](../cgroup/06-memory.md)。
> 导航：[上一节 THP](./09-thp-and-hugetlb.md) · [目录](./README.md) · [下一节 KVM](./11-kvm-memory.md)

---

## 10.1 谁在 LRU 上

可回收的 folio 在 `struct lruvec`（`mmzone.h:766`）上。没开 memcg 时，node 的 `__lruvec` 就是唯一的一份。开了 memcg，每个 memcg×node 有自己的 lruvec，`mem_cgroup_lruvec()` 查找。`pgdat->__lruvec` 那份不用（注释 `:1587`）。

`struct lruvec` 成员：

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct list_head lists[NR_LRU_LISTS]` | 始终 | 传统 LRU：inactive/active × anon/file，加上 unevictable |
| `spinlock_t lru_lock` | 始终 | 这把 lruvec 的锁。memcg 每组一把，避免全局一把锁 |
| `struct lru_cost cost[ANON_AND_FILE]` | 始终 | 回收匿名和回收文件的代价。代价高的那边少扫。`prepare_scan_control()` 时更新 |
| `spinlock_t cost_lock` | 始终 | 保护 `cost[]` |
| `atomic_long_t nonresident_age` | 始终 | 不在内存中的“年龄”，由 LRU 移动推动。workingset 用 |
| `unsigned long refaults[ANON_AND_FILE]` | 始终 | 上一轮回收时的 refault 计数 |
| `unsigned long flags` | 始终 | `enum lruvec_flags` |
| `struct lru_gen_folio lrugen` | `CONFIG_LRU_GEN` | 分代链表。成员见下 |
| `struct lru_gen_mm_state mm_state` | `CONFIG_LRU_GEN_WALKS_MMU` | 并发走 mm 链表的 Bloom filter 状态 |
| `struct pglist_data *pgdat` | `CONFIG_MEMCG` | 回指 node |
| `struct zswap_lruvec_state zswap_lruvec_state` | 始终 | 这个 lruvec 的 zswap 状态 |

`struct lru_cost`（`:760`）：`count`、`last_rotated`、`last_io`。

### MGLRU：`struct lru_gen_folio`（`mmzone.h:563`）

v7.3 **有**这套结构，包在 `CONFIG_LRU_GEN` 里。没编进去时，`mm_inline.h:315` 的 `lru_gen_enabled()` 恒返回 false，`lru_gen_age_node`（`vmscan.c:5952`）和 `lru_gen_shrink_node`（`:5962`）的函数体是 `BUILD_BUG()`。调用点包在 `lru_gen_enabled() || lru_gen_switching()` 里，常量条件把调用裁掉，编译器不生成对这两个桩的调用。运行中的内核是否启用，看 `/sys/kernel/mm/lru_gen/enabled` 或 `lruvec` 里的 `enabled` 位。本机 6.12 快照里有 `workingset_*`，不能据此证明 6.12 或 v7.3 的 MGLRU 开关，那个计数两套 LRU 都有。

| 成员 | 中文 |
|---|---|
| `unsigned long max_seq` | 最年轻一代的编号。老化时增加。匿名和文件共用 |
| `unsigned long min_seq[ANON_AND_FILE]` | 各自最老一代。回收时增加。允许和另一类差若干代，用来适应极端 swappiness |
| `unsigned long timestamps[MAX_NR_GENS]` | 每一代的出生时间（jiffies） |
| `struct list_head folios[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES]` | 分代链表。驱逐时才懒排序 |
| `long nr_pages[...]` | 每一格的页数。注释写明最终一致，`reset_batch_size()` 还没做完时可以暂时是负数 |
| `unsigned long avg_refaulted[ANON_AND_FILE][MAX_NR_TIERS]` | refault 的指数滑动平均 |
| `unsigned long avg_total[...]` | 被驱逐加被保护的滑动平均 |
| `unsigned long protected[NR_HIST_GENS][...]` | 受保护页。只能在 LRU 锁下改 |
| `atomic_long_t evicted[...]` | 被驱逐的。可以不拿 LRU 锁 |
| `atomic_long_t refaulted[...]` | 被重新缺页进来的 |
| `bool enabled` | 这个 lruvec 是否启用 MGLRU |
| `u8 gen` | 所属 memcg 代 |
| `u8 seg` | 在 memcg 链表的哪一段（head/tail/default） |
| `struct hlist_nulls_node list` | 挂到节点的全局回收 memcg 链表 |

`lru_gen_mm_state`（`:604`）成员：`seq`、`head`、`tail`、`filters[NR_BLOOM_FILTERS]`、`stats[NR_HIST_GENS][NR_MM_STATS]`。Bloom filter 用来跳过页表扫描时已经看过的 mm。

工作方式一句话：访问把 folio 提升到更年轻的代（缺页里 `lru_gen_enter_fault`，以及 `lru_gen_look_around` 顺手看相邻 PTE，声明 `mmzone.h:692`，定义 `vmscan.c:4256`）。回收从最老的代开始扔。比 active/inactive 两段更能分辨「刚刚碰过」和「很久没碰」。

### 代际老化：`lru_gen_age_node`

老化只给 kswapd 做。`balance_pgdat`（`:7157`）在 `:7265` 调 `kswapd_age_node`（`:6906`）。MGLRU 打开时它转入 `lru_gen_age_node`（`:4209`），函数开头 `VM_WARN_ON_ONCE(!current_is_kswapd())`。

```text
kswapd_age_node                         # vmscan.c:6906
  lru_gen_enabled 或正在切换：
    lru_gen_age_node                    # :4209
      set_initial_priority
      mem_cgroup_iter 扫这个 node 上的每个 memcg
        lruvec_is_reclaimable           # :4185
          低于 min：false
          不够大：false
          最老一代的出生时间 + min_ttl 已经过了：true
      一个都不可回收，并且 trylock(oom_lock) 成功：
        out_of_memory                   # 保护最近 min_ttl 这段工作集，宁可 OOM
  没在切换：直接 return，不走下面的传统 inactive 老化
```

`min_ttl` 来自 `lru_gen_min_ttl`（`:4207`，jiffies）。注释（`:4229-4232`）写了两种「全都不可回收」：每一代都比 `min_ttl` 年轻，或者所有 memcg 太小 / 低于 min。两条都进同一个 `out_of_memory`。`trylock` 失败就跳过，不在 kswapd 里排队等 OOM 锁。

`lru_gen_look_around`（`:4256`）是回收走 rmap 时的反馈。它要求已经拿着 PTL。清掉当前 PTE 的 young 之后，若锁没人抢、VMA 不是 `VM_SPECIAL`，就在同一个 PMD 覆盖的相邻 PTE 里找热页，把它们提升到更年轻的代。扫得够整齐就把这个 PMD 记进 Bloom filter，下一轮页表扫描可以跳过。声明在 `mmzone.h:692`，没编 MGLRU 时 `mm_inline.h` 里的内联版本直接返回 false。

---

## 10.2 kswapd 与直接回收

```text
水位低于 LOW（分配快路径或唤醒）
  wakeup_kswapd                         # vmscan.c:7573
    → kswapd 线程                       # :7492，每 node 一个，pgdat->kswapd
         balance_pgdat                  # :7157
           kswapd_shrink_node           # :7084
             shrink_node                # :6239

分配慢路径
  __alloc_pages_direct_reclaim          # page_alloc.c:4464
    → try_to_free_pages                 # vmscan.c:6769
         do_try_to_free_pages           # :6547
           shrink_node
```

`kswapd` 把自己标成 `PF_MEMALLOC | PF_KSWAPD`（`:7511`），这样它为了回收而做的小分配不会再递归进回收。它睡在 `pgdat->kswapd_wait` 上，目标是把 zone 拉回 HIGH。

`shrink_node`（`:6239`）开头：

```c
if ((lru_gen_enabled() || lru_gen_switching()) && root_reclaim(sc)) {
    lru_gen_shrink_node(pgdat, sc);    /* :5229；!CONFIG_LRU_GEN 时 :5962 是 BUILD_BUG()，调用被裁掉 */
    if (!lru_gen_switching())
        return;                        /* 纯 MGLRU 全局回收，不走下面的 memcg 循环 */
}
shrink_node_memcgs(pgdat, sc);         /* :6160 */
```

`root_reclaim` 是全局回收（没有指定目标 memcg）。memcg 自己超了 high/max 触发的回收不是 root，走 `shrink_node_memcgs`，里面仍可能用 MGLRU 的 per-memcg lruvec。cgroup 章讲的是「谁被 charge、限额是多少」；这里讲的是「限额到了以后，页从哪条链表上摘」。

`shrink_node_memcgs` 对每个 memcg 的 lruvec：按 swappiness 和 `lru_cost` 分匿名/文件的扫描量，`isolate_lru_folios`（`:1679`）摘下来，`shrink_folio_list`（`:1058`）决定释放、写出还是放回。

本机 6.12 快照：`pgscan_kswapd` 2082453，`pgsteal_kswapd` 1611280；`pgscan_direct` 19314，`pgsteal_direct` 18131。后台回收是大头。`pgsteal_anon` 0、`pgsteal_file` 1629411：偷走的全是文件页。`pswpin`/`pswpout` 为 0，且 `SwapTotal` 为 0，匿名页没有地方去。`kswapd_low_wmark_hit_quickly` 399：kswapd 刚睡下又被 LOW 水位叫醒，说明水位在 LOW 附近晃。

直接回收的次数看 `allocstall_*`（本机 normal 11、movable 373），不是看 `pgscan_direct` 的页数。页数是那些次数里扫了多少。

---

## 10.3 swap 与 zswap

没有 swap 设备时，匿名页的回收只能：

- 丢掉 `MADV_FREE` / `LazyFree` 的页（内容允许丢失）
- 或者什么都不做

本机 `SwapTotal` 0，`Zswap` 0，`Zswapped` 0。匿名活跃集约 1.8GB（`AnonPages`）是钉在内存里的。

有 swap 时，`shrink_folio_list` 把匿名 folio 写成 swap 项，PTE 变成 swap PTE，缺页走 `do_swap_page`（`memory.c:4881`）。

zswap（`mm/zswap.c`）在写 swap 设备之前把页压进 zsmalloc 池。换出入口在 `mm/page_io.c:238`：`zswap_store` 返回真就不再写盘。换入在 `page_io.c:480`：`zswap_load` 不是 `-ENOENT` 就不再读盘。

`zswap_store`（`zswap.c:1488`）要求 folio 已锁且在 swap cache。`zswap_enabled` 为假就跳到 `check_old`，把这个偏移上可能残留的旧项擦掉，避免回写覆盖 swap 文件里的新数据。打开时：

```text
zswap_store(folio)                        # zswap.c:1488
  obj_cgroup_may_zswap 失败：先 shrink_memcg，再失败则放弃
  zswap_check_limits 到顶：放弃
  zswap_pool_current_get
  for 每一页：zswap_store_page            # 压缩，zs_malloc 进池
  成功：计 ZSWPOUT
  失败：xa_erase 掉这个 folio 每个偏移上的旧 zswap_entry
```

`zswap_load`（`:1594`）同样要求 locked + swapcache。返回值写在函数注释里：

| 返回 | folio 状态 | 含义 |
|---|---|---|
| `0` | 解锁，标 uptodate | 解压成功。标脏，`xa_erase` 删掉压缩副本，swap cache 成为唯一内存副本 |
| `-ENOENT` | 保持锁定 | 这个槽不在 zswap，调用者继续读盘。`zswap_never_enabled()` 也走这里 |
| `-EINVAL` | 解锁，不标 uptodate | 大 folio。注释写明 zswap 不完整装载大 folio，调用者（`do_swap_page`）会 SIGBUS |
| `-EIO` | 解锁，不标 uptodate | 槽在 zswap 里但解压失败，同样 SIGBUS |

池满了就落到真正的 swap 设备。`/proc/meminfo` 的 `Zswap` 是压缩后占用，`Zswapped` 是原页大小。zswap 仍然算内存，不是磁盘。它和 memcg 的交换限额（`memory.swap.*`，见 cgroup 章）是两层账。

压缩对象从 zsmalloc 来。`zs_malloc`（`mm/zsmalloc.c:1397`）按字节大小要一块，返回 handle，失败返回 `ERR_PTR`。

```text
zs_malloc(pool, size, gfp, nid)           # zsmalloc.c:1397
  size == 0：-EINVAL
  size > ZS_MAX_ALLOC_SIZE：-ENOSPC       # 太大，不进 zsmalloc
  cache_alloc_handle
  lookup_size_class                       # 按 size 找 size_class
  find_get_zspage                         # 这个 class 里还有空位的 zspage
    有：obj_malloc，fix_fullness_group
    无：alloc_zspage，再 obj_malloc，SetZsPageMovable
```

`struct zs_pool` / `struct size_class` 的成员表这里不列。定义在 `mm/zsmalloc.c`，字段随 fullness group 和 stat 变，本节只钉分配路径。handle 不是内核虚拟地址，映射时再 `kmap`。

`swappiness` 是匿名和文件的扫描比例，不是「百分之多少内存拿去 swap」。MGLRU 下 `min_seq[anon]` 和 `min_seq[file]` 可以差几代，注释说就是为了极端 swappiness（`mmzone.h:557`）。

---

## 10.4 OOM

慢路径里回收和压缩都没能拿出页，`__alloc_pages_may_oom`（`page_alloc.c:4044`）调用 `out_of_memory`（`oom_kill.c:1103`）。

```text
out_of_memory
  oom_badness                              # oom_kill.c:199，给每个候选打分
  oom_kill_process                         # :1008
    __oom_kill_process                     # :912
      发 SIGKILL，标 TIF_MEMDIE / 相关标志，让它的退出路径能分配
```

`oom_badness` 返回 `LONG_MIN` 的进程不参与选择：`oom_unkillable_task`、`find_lock_task_mm` 失败、`oom_score_adj == OOM_SCORE_ADJ_MIN`、mm 已标 `MMF_OOM_SKIP`、或正在 `in_vfork`。剩下的分数是三笔账加一次平移：

```c
points = get_mm_rss_sum(p->mm)
       + get_mm_counter_sum(p->mm, MM_SWAPENTS)
       + mm_pgtables_bytes(p->mm) / PAGE_SIZE;   /* rss + swap 项 + 页表页 */
adj = (long)p->signal->oom_score_adj;
adj *= totalpages / 1000;                        /* 把 -1000..1000 摊到总页数上 */
points += adj;
```

`oom_score_adj` 为 0 时，分数就是这个 mm 占的页。调到 `OOM_SCORE_ADJ_MIN` 直接排除，调高则在同样 rss 下更先被选中。`/proc/<pid>/oom_score_adj` 写的就是这个字段。`totalpages` 由 `constrained_alloc`（`:248`）按约束算：memcg OOM 用 `mem_cgroup_get_max`，cpuset / mempolicy 只用允许的节点。

memcg 的 OOM 是另一条：charge 撞上 `memory.max` 且回收失败。事件在 `memory.events` 的 `oom` / `oom_kill`，`memory.oom.group` 决定杀一个还是一组。实现仍在 `mm/memcontrol.c` 和 `oom_kill.c` 的 memcg 辅助函数里。不要把全局 `oom_kill` 计数当成某个容器的 OOM。本机 `oom_kill` 是 0。

OOM 之后不一定立刻有空闲页。被杀进程要跑退出、拆 VMA、还 folio。这段时间别的 `__GFP_NOFAIL` 分配会在 slowpath 里转。`dmesg` 里的 `Out of memory: Killed process` 是这个路径打的。

tracepoint（`include/trace/events/oom.h`）：`mark_victim`（`:76`）、`wake_reaper`（`:118`）、`reclaim_retry_zone`（`:34`）、`compact_retry`（`:183`）。

---

## 10.5 和 cgroup 章的衔接

| 现象 | 去哪看 |
|---|---|
| `memory.current` 怎么涨的 | charge：`try_charge_memcg`，cgroup 章第 6 节 |
| 超过 high 但还没 OOM | memcg 回收 + high 节流，仍是 charge 路径发起，页的摘除在本章 `shrink_node` |
| `memory.stat` 的 anon/file/slab | 统计面。读法在 [13](./13-observability.md)，字段定义以 `Documentation/admin-guide/cgroup-v2.rst` 为准 |
| 全局 kswapd 很忙但某个容器 current 不高 | 全局回收。`pgscan_kswapd` 涨、该 memcg 的 `memory.events` 不动 |

本章不复制 `struct mem_cgroup` 成员表。cgroup 笔记里那张表是节选；要全成员直接看 `include/linux/memcontrol.h` 的定义。

---

## 10.6 unevictable LRU 与 mlock

**结论**：mlock 住的 folio 不进 inactive/active 的扫描。`folio_set_unevictable` 之后，`folio_lru_list`（`mm_inline.h:82`）返回 `LRU_UNEVICTABLE`（`mmzone.h:394`），`lruvec_add_folio` 把它放进 `lruvec->lists[LRU_UNEVICTABLE]`。回收看这个标志，不从这条链上偷页。

`mlock.c:55-58` 的注释写「unevictable 链表其实不存在」。v7.3 的 `folio_lru_list` 和 `enum lru_list` 里这条链是有的。以这两个定义为准，那句注释和当前链表对不上。

系统调用：

```text
mlock(start, len)                         # SYSCALL mm/mlock.c:667
  do_mlock                                # :619
    can_do_mlock 否则 -EPERM
    长度按页对齐
    RLIMIT_MEMLOCK（页数）。超了且没有 CAP_IPC_LOCK：
      扣掉区间里已经 mlock 的页（count_mm_mlocked_page_nr，:578）再比一次
    apply_vma_lock_flags                  # :519，mmap 写锁下改 VMA
      mlock_fixup                         # :467
        mlock_vma_pages_range             # :423，走页表，已在的页进 mlock batch
    mmap_write_unlock
    __mm_populate                         # :661，把还没缺页的地址补上
      populate_vma_page_range             # mm/gup.c:1813

mlock2(start, len, MLOCK_ONFAULT)         # :674
  多设 VMA_LOCKONFAULT_BIT（:682）
  同样进 do_mlock
  populate_vma_page_range 看到 VM_LOCKONFAULT（mm.h:425，判断在 gup.c:1832）
    直接返回页数，不在这里 fault-in。页留到以后真正缺页时再锁

munlock                                   # :687，flags 清空 VMA_LOCKED_MASK
```

已经映射的页进 `mlock_folio_batch`（`:186`）。folio 指针的低两位是标志，不是地址：`LRU_FOLIO`（`0x1`）走 `__mlock_folio`（`:61`），`NEW_FOLIO`（`0x2`）走 `__mlock_new_folio`（`:103`），都没有就 `__munlock_folio`（`:122`）。

`__mlock_folio` 拿 lruvec 锁。folio 不可驱逐时：从当前链摘下，`folio_set_unevictable`，`mlock_count` 记成「是否已经 PG_mlocked」，再 `lruvec_add_folio` 放回 unevictable 链，计 `UNEVICTABLE_PGCULLED`。已经在这条链上且仍 mlocked，只把 `mlock_count++`。`__munlock_folio` 把 `mlock_count` 减到 0 且 `folio_evictable` 为真时，清掉 unevictable，放回普通 LRU。

`mlock_count` 少算会把页留在 unevictable 上。`mlock_fixup` 的注释（`:432-436`）写了并发迁移或回收可能重复加计数。统计看 vmstat 的 `unevictable_pgs_culled` / `unevictable_pgs_rescued` / `unevictable_pgs_mlocked`。

secretmem 和 `vma_supports_mlock` 为假的 VMA 在 `mlock_fixup`（`:476`）里直接跳过，不设 `VMA_LOCKED_BIT`。

---

## 10.7 `migrate_pages`：压缩、NUMA、热插拔共用

**结论**：迁移是「隔离出一批 folio，分配目标 folio，把内容搬过去，失败的放回原链」。`migrate_pages`（`mm/migrate.c:2100`）是这条公共路径。返回没迁走的 folio 数（普通页、大页、hugetlb 都算），或者负数错误。调用者在返回非 0 时自己 `putback_movable_pages`。

模式在 `include/linux/migrate_mode.h:11`：

| 模式 | 注释里的含义 |
|---|---|
| `MIGRATE_ASYNC` | 不睡眠 |
| `MIGRATE_SYNC_LIGHT` | 大部分操作可以等，但不走 `->writepage` |
| `MIGRATE_SYNC` | 迁移过程中允许睡眠 |

```text
migrate_pages(from, get_new_folio, ...)   # migrate.c:2100
  migrate_hugetlbs                        # 大页先单独迁，失败的留在 ret_folios
  again：
    按页数切一批，上限 NR_MAX_BATCHED_MIGRATION
      CONFIG_TRANSPARENT_HUGEPAGE：HPAGE_PMD_NR（:1611）
      否则 512（:1613）
    MIGRATE_ASYNC → migrate_pages_batch，重试 NR_MAX_MIGRATE_PAGES_RETRY（10，:1615）
    其它模式 → migrate_pages_sync
    被拆开的大页再以 MIGRATE_ASYNC 试一次
    from 还有剩余：回到 again
  永久失败的 folio 接回 from
  计 PGMIGRATE_SUCCESS / PGMIGRATE_FAIL / THP_MIGRATION_*
```

谁在调（理由在 `enum migrate_reason`，`migrate_mode.h:17`）：

| 调用点 | 模式 | reason |
|---|---|---|
| `compact_zone` `mm/compaction.c:2698` | `cc->mode` | `MR_COMPACTION` |
| `__alloc_contig_migrate_range` `mm/page_alloc.c:7174` | `cc->mode` | `MR_CONTIG_RANGE` |
| `do_mbind` `mm/mempolicy.c:1617` | `MIGRATE_SYNC` | `MR_MEMPOLICY_MBIND` |
| `migrate_to_node` `mempolicy.c:1322` | `MIGRATE_SYNC` | `MR_SYSCALL` |
| `migrate_misplaced_folio` `mm/migrate.c:2765` | `MIGRATE_ASYNC` | `MR_NUMA_MISPLACED` |
| `demote_folio_list` `mm/vmscan.c:1033` | `MIGRATE_ASYNC` | `MR_DEMOTION` |
| `do_migrate_range` `mm/memory_hotplug.c:1920` | `MIGRATE_SYNC` | `MR_MEMORY_HOTPLUG` |

热插拔那条在 [12.9](./12-balloon-fpr-and-overcommit.md)。NUMA 策略本身在 [03](./03-buddy.md) 的 mempolicy 一节，这里只标它最后也进 `migrate_pages`。

---

## 10.8 swap table：一个 cluster 一张表

**结论**：v7.3 的 swap 槽状态在 `struct swap_table` 里，每个 cluster 一张，和槽一一对应。定义在 `mm/swap_table.h:10`。`Documentation/mm/swap-table.rst` 讲的就是这张表。槽状态以头文件 `:26-33` 的布局为准。

`SWAPFILE_CLUSTER`（`mm/swap.h:32`）：`CONFIG_THP_SWAP` 时等于 `HPAGE_PMD_NR`，否则 256（`:35`）。

### `struct swap_table`（`swap_table.h:10`）

| 成员 | 中文 |
|---|---|
| `atomic_long_t entries[SWAPFILE_CLUSTER]` | 这个 cluster 里每个槽一项。一项是一个 `atomic_long_t` |

### `struct swap_memcg_table`（`:15`，仅 `CONFIG_MEMCG` 的读写路径会用）

| 成员 | 中文 |
|---|---|
| `unsigned short id[SWAPFILE_CLUSTER]` | 每个槽的 memcg 私有 id。挂在 `swap_cluster_info->memcg_table` 上，和 entry 里的类型分开存 |

头文件 `:26-33` 写的布局（低位是类型标记）：

| 类型 | 低位标记 | 含义 |
|---|---|---|
| NULL | 全 0（`SWP_TB_NULL`） | 空槽，可以分配 |
| Shadow | 最低位 `0b1`（`SWP_TB_SHADOW_MARK`） | 已换出、不在 swap cache。复用 xarray value 的格式，好和 workingset shadow 兼容。`SHADOW_VAL` 可以是 0，仍保持 shadow 形态占位。memcg id 嵌在 `SHADOW_VAL` 里 |
| PFN | `0b10`（`SWP_TB_PFN_MARK`） | 槽在用，而且有 cache 页。memcg 记在 page 上，不在这项里 |
| Pointer | 低三位 `0b100` | 头文件写 Unused。留给 8 字节对齐指针，低三位才能当标记 |
| Bad | 低三位 `0b1000` 且其余位全 1（`SWP_TB_BAD`） | 坏槽。保护 swap 头，或设备上的洞 |

高位是标志。`SWP_TB_FLAGS_BITS`（`:72`）是 `min(5, BITS_PER_LONG - SWP_TB_PFN_BITS)`。其中计数占 `SWP_TB_COUNT_BITS`，剩下的一位在 `SWAP_TABLE_HAS_ZEROFLAG` 为真时是 zero flag（`SWP_TB_ZERO_FLAG`，`:79`）。位数不够就把 zero 记到 `swap_cluster_info->zero_bitmap`（`__swap_table_set_zero`，`:267`）。

读写必须在 cluster 锁下，除了 `swap_table_get`（`:251`）走 RCU 读 `ci->table`。表指针空就当成 NULL 项。`__swap_table_set` / `__swap_table_xchg` / `__swap_table_get` 都 `VM_WARN` 偏移越过 `SWAPFILE_CLUSTER`。

`SWP_TABLE_USE_PAGE`（`:19`）在 `sizeof(struct swap_table) == PAGE_SIZE` 时为真，这时一张表正好占一页。

---

## 10.9 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `wakeup_kswapd` | `mm/vmscan.c:7573` | 叫醒某 zone 的 kswapd |
| `kswapd` | `:7492` | 每 node 的线程 |
| `balance_pgdat` | `:7157` | 把水位做到 HIGH |
| `kswapd_shrink_node` | `:7084` | kswapd 侧的 shrink |
| `try_to_free_pages` | `:6769` | 直接回收入口 |
| `do_try_to_free_pages` | `:6547` | 扫 zonelist 上的 node |
| `shrink_node` | `:6239` | 一个 node |
| `lru_gen_shrink_node` | `:5229`（`!CONFIG_LRU_GEN` 时 `:5962` 为 `BUILD_BUG()`） | MGLRU 全局回收 |
| `lru_gen_age_node` | `:4209`（`!CONFIG_LRU_GEN` 时 `:5952` 为 `BUILD_BUG()`） | kswapd 代际老化，必要时 OOM |
| `kswapd_age_node` | `:6906` | `balance_pgdat` 里调用老化 |
| `lru_gen_look_around` | `:4256`（声明 `mmzone.h:692`） | rmap 扫描时提升相邻热页 |
| `oom_badness` | `mm/oom_kill.c:199` | rss + swap + 页表，再加 `oom_score_adj` |
| `constrained_alloc` | `:248` | OOM 约束与 `totalpages` |
| `zs_malloc` | `mm/zsmalloc.c:1397` | 从 zsmalloc 池要一块 |
| `zswap_store_page` | `mm/zswap.c:1408` | 压缩一页并挂进 zswap 树 |
| `do_mlock` / `mlock` | `mm/mlock.c:619` / `:667` | mlock 系统调用 |
| `mlock2` / `munlock` | `:674` / `:687` | onfault 锁 / 解锁 |
| `mlock_folio_batch` | `:186` | 批量把 folio 挪到 unevictable |
| `migrate_pages` | `mm/migrate.c:2100` | 迁移一批 folio |
| `migrate_misplaced_folio` | `:2755` | NUMA balancing 迁一页 |
| `demote_folio_list` | `mm/vmscan.c:997` | 回收降级到更慢的节点 |
| `shrink_node_memcgs` | `:6160` | 按 memcg |
| `isolate_lru_folios` | `:1679` | 从 LRU 摘 folio |
| `shrink_folio_list` | `:1058` | 释放 / 写出 / 放回 |
| `kswapd_test_hopeless` | `mmzone.h:1649` 声明，实现 `vmscan.c:7639` | 连续回收失败 |
| `out_of_memory` | `mm/oom_kill.c:1103` | 全局 OOM |
| `oom_kill_process` | `:1008` | 选完之后杀 |
| `__oom_kill_process` | `:912` | SIGKILL |
| `zswap_store` / `zswap_load` | `mm/zswap.c:1488` / `:1594` | 压缩换出 / 换入 |
| `do_swap_page` | `mm/memory.c:4881` | 换入缺页 |

---

[上一节 THP](./09-thp-and-hugetlb.md) · [目录](./README.md) · [下一节 KVM](./11-kvm-memory.md)
