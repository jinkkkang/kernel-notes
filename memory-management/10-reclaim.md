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

v7.3 **有**这套结构，包在 `CONFIG_LRU_GEN` 里。没编进去时，`lru_gen_shrink_node` 是空函数（`vmscan.c:5962` 的 `#else` 分支）。运行中的内核是否启用，看 `/sys/kernel/mm/lru_gen/enabled` 或 `lruvec` 里的 `enabled` 位。本机 6.12 快照里有 `workingset_*`，不能据此证明 6.12 或 v7.3 的 MGLRU 开关，那个计数两套 LRU 都有。

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

工作方式一句话：访问把 folio 提升到更年轻的代（缺页里 `lru_gen_enter_fault`，以及 `lru_gen_look_around` 顺手看相邻 PTE，`mmzone.h:692`）。回收从最老的代开始扔。比 active/inactive 两段更能分辨「刚刚碰过」和「很久没碰」。

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
    lru_gen_shrink_node(pgdat, sc);    /* :5229，未配置时是 :5962 的空函数 */
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

zswap（`mm/zswap.c`）在写 swap 设备之前把页压进内存池：

| 符号 | 位置 | 作用 |
|---|---|---|
| `zswap_store` | `zswap.c:1488` | 换出时尝试压缩进 zpool |
| `zswap_load` | `zswap.c:1594` | 换入时解压 |

池满了就落到真正的 swap 设备。`/proc/meminfo` 的 `Zswap` 是压缩后占用，`Zswapped` 是原页大小。zswap 仍然算内存，不是磁盘。它和 memcg 的交换限额（`memory.swap.*`，见 cgroup 章）是两层账。

`swappiness` 是匿名和文件的扫描比例，不是「百分之多少内存拿去 swap」。MGLRU 下 `min_seq[anon]` 和 `min_seq[file]` 可以差几代，注释说就是为了极端 swappiness（`mmzone.h:557`）。

---

## 10.4 OOM

慢路径里回收和压缩都没能拿出页，`__alloc_pages_may_oom`（`page_alloc.c:4044`）调用 `out_of_memory`（`oom_kill.c:1103`）。

```text
out_of_memory
  选 victim（oom_score，oom_score_adj）
  oom_kill_process                         # :1008
    __oom_kill_process                     # :912
      发 SIGKILL，标 TIF_MEMDIE / 相关标志，让它的退出路径能分配
```

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

## 10.6 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `wakeup_kswapd` | `mm/vmscan.c:7573` | 叫醒某 zone 的 kswapd |
| `kswapd` | `:7492` | 每 node 的线程 |
| `balance_pgdat` | `:7157` | 把水位做到 HIGH |
| `kswapd_shrink_node` | `:7084` | kswapd 侧的 shrink |
| `try_to_free_pages` | `:6769` | 直接回收入口 |
| `do_try_to_free_pages` | `:6547` | 扫 zonelist 上的 node |
| `shrink_node` | `:6239` | 一个 node |
| `lru_gen_shrink_node` | `:5229`（无配置时 `:5962`） | MGLRU 全局回收 |
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
