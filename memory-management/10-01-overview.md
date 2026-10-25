# 10.1 回收总入口

**结论**：水位低于 LOW，分配路径叫醒 kswapd，kswapd 在后台把 zone 拉回 HIGH。分配慢路径自己进 `try_to_free_pages`，延迟算在那个任务头上。两边都进 `shrink_node`。全局回收且 `lru_gen_enabled()` 为真，走 `lru_gen_shrink_node` 后返回；否则走 `shrink_node_memcgs`，里面按 memcg 扫 LRU，并在每个 memcg 上调 `shrink_slab`。

> 主文件：`mm/vmscan.c`。`struct scan_control` 在 `:74`。
> 导航：[上一篇 回收](./10-00-reclaim.md) · [本目录](./10-00-reclaim.md) · [下一篇 经典 LRU](./10-02-lru-classic.md)

---

## 谁被叫醒，谁自己扫

```text
水位低于 LOW（分配快路径或唤醒）
  wakeup_kswapd                         # vmscan.c:7817
    → kswapd 线程                       # :7731，每 node 一个，参数是 pgdat
         balance_pgdat                  # :7409
           kswapd_age_node              # :7182，调用点 :7518
                                        #   MGLRU 时做代际老化，见 05
           kswapd_shrink_node           # :7327，调用点 :7539
             shrink_node                # :6532

分配慢路径
  __alloc_pages_direct_reclaim          # page_alloc.c:3689
    → __perform_reclaim                 # :3662
         try_to_free_pages              # vmscan.c:7057
           do_try_to_free_pages         # :6839
             shrink_zones               # :6716，里面 :6795 调 shrink_node
```

`kswapd` 把自己标成 `PF_MEMALLOC | PF_KSWAPD`（`:7754`）。注释（`:7742-7752`）写：它偶尔为了换出别的页要做一点小分配，这个标志避免再递归进回收。它睡在 `pgdat->kswapd_wait` 上（`kswapd_try_to_sleep`，`:7643` 起）。`balance_pgdat` 的注释（`:7403-7407`）写：跳过 `free_pages > high_wmark_pages(zone)` 的 zone；一旦某个 zone 的空闲页不高于 HIGH，这一档以及更低的页都可以收，直到至少有一个能用的 zone 达到平衡。

刚睡下又被 LOW 水位叫醒时，`kswapd_try_to_sleep` 计 `KSWAPD_LOW_WMARK_HIT_QUICKLY`（`:7711`）。

水位四档和 `wakeup_kswapd` 的分配侧在 [03-03-fastpath-freelist.md](./03-03-fastpath-freelist.md)。慢路径怎么决定要不要直接回收，在 [03-06-slowpath.md](./03-06-slowpath.md)。

---

## `shrink_node` 在哪分叉（`:6532`）

```c
if (lru_gen_enabled() && root_reclaim(sc)) {
    lru_gen_shrink_node(pgdat, sc);    /* :5570；没编 CONFIG_LRU_GEN 时 :6280 是空函数 */
    return;
}
/* 下面先 prepare_scan_count，再： */
shrink_node_memcgs(pgdat, sc);         /* :6474 */
```

`root_reclaim`（编了 memcg 时 `:442`，没编时 `:497` 恒为真）是全局回收：没有指定目标 memcg，或者目标就是 root。目标 memcg 不是 root 时走 `shrink_node_memcgs`。那条循环里 `shrink_lruvec`（`:6286`）对非 root 且 MGLRU 开着的 lruvec 会进 `lru_gen_shrink_lruvec`（`:5535`）然后返回。cgroup 章讲的是「谁被 charge、限额是多少」；这里讲的是「限额到了以后，页从哪条链上摘」。

`lru_gen_enabled()` 在 `include/linux/mm_inline.h`。编了 `CONFIG_LRU_GEN_ENABLED` 时用 `static_branch_likely`（`:105`），没编这个默认打开、但编了 `CONFIG_LRU_GEN` 时用 `static_branch_unlikely`（`:112`）。完全没编 `CONFIG_LRU_GEN` 时（`:295`）恒返回 false。运行中的开关是 `/sys/kernel/mm/lru_gen/enabled`，见 [06](./10-06-lru-vs-mglru.md)。

经典路径的摘链、访问位、anon/file 比例在 [02](./10-02-lru-classic.md)。MGLRU 的驱逐在 [05](./10-05-mglru-code-path.md)。

---

## `struct scan_control`（`mm/vmscan.c:74`）

一次回收调用的全部参数和计数。kswapd、直接回收、memcg 回收、proactive reclaim 各填各的，进的是同一个 `shrink_node`。

```c
struct scan_control {
	/* shrink_list() 朝这个数做 */
	unsigned long nr_to_reclaim;

	/*
	 * 调用者允许扫的节点。NULL 表示全扫。
	 * 类型是 nodemask_t *，不是 const。
	 */
	nodemask_t	*nodemask;

	/*
	 * 撞上限、因而成为这次回收主要目标的 memcg。
	 * 全局回收是 NULL 或 root。cgroup_reclaim() / root_reclaim() 看的就是它。
	 */
	struct mem_cgroup *target_mem_cgroup;

	/* 匿名侧的回收代价。prepare_scan_count() 从 lruvec->anon_cost 抄过来 */
	unsigned long	anon_cost;
	/* 文件侧的回收代价。同上，来自 lruvec->file_cost */
	unsigned long	file_cost;

	/* 是否允许把 active 降到 inactive */
#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;	/* 强制两侧都允许降级 */
	unsigned int skipped_deactivate:1;	/* 本轮想降级但 may_deactivate 没允许 */

	/* 笔记本模式或 boost 回收会关掉写出；注释：Writepage batching in laptop mode */
	unsigned int may_writepage:1;
	unsigned int may_unmap:1;		/* 能不能 unmap 已经映射的 folio */
	unsigned int may_swap:1;		/* 能不能把匿名页换出去 */

	unsigned int proactive:1;		/* 用户态主动回收（memory.reclaim） */

	/*
	 * 已经因为 memory.low 跳过一轮、什么都没收回来，
	 * 回来再扫受保护的内存，避免直接 OOM。
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;	/* 这一轮有 memcg 因为 memory.low 被跳过或减轻 */

	unsigned int hibernation_mode:1;	/* 休眠路径上的回收 */
	unsigned int compaction_ready:1;	/* 某个 zone 已经可以 compact */

	/* 当前 node 上有容易回收的冷文件页，先剪缓存、先别动匿名 */
	unsigned int cache_trim_mode:1;
	/*
	 * 当前 node 的文件 folio 少到危险。经典路径会转去扫匿名。
	 * 见 10-02 的 cache trap。
	 */
	unsigned int file_is_tiny:1;

	unsigned int no_demotion:1;		/* 只丢弃，不向更慢的内存层迁移 */

	s8 order;				/* 触发这次回收的分配阶 */
	/*
	 * 扫描强度。每次扫 total_size >> priority 页。
	 * DEF_PRIORITY 是 12（mmzone.h:1174）。优先级往下掉，扫的比例翻倍。
	 */
	s8 priority;
	s8 reclaim_idx;				/* 最高隔离到哪一档 zone */

	gfp_t gfp_mask;				/* 这次回收上下文的 GFP */

	unsigned long nr_scanned;		/* 已经扫过的 inactive 页数 */
	unsigned long nr_reclaimed;		/* shrink_zones() 到目前为止放出的页数 */

	struct {
		unsigned int dirty;		/* 遇到的脏页 */
		unsigned int unqueued_dirty;	/* 脏、但还没进写回队列 */
		unsigned int congested;		/* 拥塞 */
		unsigned int writeback;		/* 写回中 */
		unsigned int immediate;		/* 需要立刻处理 */
		unsigned int file_taken;	/* 从文件 LRU 隔离的计数 */
		unsigned int taken;		/* 实际隔离的计数 */
	} nr;

	/* 嵌在 scan_control 里，不是指针 */
	struct reclaim_state reclaim_state;
};
```

`struct reclaim_state`（`include/linux/swap.h:155`）。注释：任务正在回收时，`current->reclaim_state` 指向这样一分。`shrink_node` 用的是内嵌的那份，并通过 `set_task_reclaim_state`（`vmscan.c:508`）挂到 `current` 上，好让 slab 等 LRU 之外的释放能把页数加进来。

```c
struct reclaim_state {
	/* LRU 回收之外放出的页。flush_reclaim_state()（vmscan.c:524）把它加进 nr_reclaimed */
	unsigned long reclaimed;
#ifdef CONFIG_LRU_GEN
	/* 这个线程正在做的页表遍历。成员在 10-04 */
	struct lru_gen_mm_walk *mm_walk;
#endif
};
```

`flush_reclaim_state` 的注释（`:527-531`）写明目前三类：slab 页、高端内存上从裁剪过的 inode 放下的干净文件页、XFS 放出的 buffer 页。这三类不能一律算进某一个 memcg，所以只在全局回收里计入（`:539` 一带）。

全局默认 `vm_swappiness` 是 60（`vmscan.c:189`）。上面的注释写范围是 0 到 200，越大越倾向换出。经典路径在 `get_scan_count` 里读 `mem_cgroup_swappiness`（`:3021`）。MGLRU 用 `get_swappiness`（`:3290`）。

---

## `shrink_node_memcgs` 与 shrinker（`:6474`）

对目标 memcg 子树里的每个 memcg：

```text
mem_cgroup_iter
  mem_cgroup_calculate_protection
  below min：跳过（注释：硬保护；没有可回收内存时留给 OOM）
  below low 且还没到 memcg_low_reclaim：记 memcg_low_skipped，跳过
  shrink_lruvec(lruvec, sc)             # 经典 LRU：按 swappiness 和 cost 分扫描量
  shrink_slab(sc->gfp_mask, nid, memcg, sc->priority)   # 同文件 :1040
  vmpressure(...)                       # 非 proactive 时记这组的回收效率
```

`shrink_lruvec` 里经典路径调用 `get_scan_count`、`shrink_active_list`、`shrink_inactive_list`、`shrink_folio_list`，见 [02](./10-02-lru-classic.md)。

`shrink_slab`（`mm/vmscan.c:1040`）是 slab 回收的入口：按 gfp、node、memcg、priority 去调各个 shrinker 的 `count_objects` / `scan_objects`。dentry、inode、自定义驱动的缓存都挂在这里。对象怎么从 slab 上摘下来是 shrinker 自己的事，本篇停在这个调用点。debugfs 怎么看某个 shrinker，见 [13](./13-observability.md) 的 shrinker debugfs 一节。`flush_reclaim_state` 把 shrinker 放出的页补进 `sc->nr_reclaimed`，否则 `shrink_node` 会以为这一轮白扫。

---

## 和 cgroup 章的衔接

| 现象 | 去哪看 |
|---|---|
| `memory.current` 怎么涨的 | charge：`try_charge_memcg`，cgroup 章第 6 节 |
| 超过 high 但还没 OOM | memcg 回收 + high 节流，仍是 charge 路径发起，页的摘除在本章 `shrink_node` |
| `memory.stat` 的 anon/file/slab | 统计面。读法在 [13](./13-observability.md)，字段定义以 `Documentation/admin-guide/cgroup-v2.rst` 为准 |
| 全局 kswapd 很忙但某个容器 current 不高 | 全局回收。`pgscan_kswapd` 涨、该 memcg 的 `memory.events` 不动 |

本章不复制 `struct mem_cgroup` 成员表。cgroup 笔记里那张表是节选；要全成员直接看 `include/linux/memcontrol.h` 的定义。

---

## 在 6.12 上采样

下面的数字是在 6.12 上采样，不是 v6.6.160 这棵树跑出来的。`pgscan_kswapd` 2082453，`pgsteal_kswapd` 1611280；`pgscan_direct` 19314，`pgsteal_direct` 18131。后台回收是大头。`pgsteal_anon` 0、`pgsteal_file` 1629411：偷走的全是文件页。`pswpin` / `pswpout` 为 0，且 `SwapTotal` 为 0，匿名页没有地方去。`kswapd_low_wmark_hit_quickly` 399：kswapd 刚睡下又被 LOW 水位叫醒，说明水位在 LOW 附近晃。

直接回收的次数看 `allocstall_*`（这次采样 normal 11、movable 373），不是看 `pgscan_direct` 的页数。页数是那些次数里扫了多少。

这次采样里有 `workingset_*`。这个计数两套 LRU 都有，不能据此判断 MGLRU 开关。开关看 `/sys/kernel/mm/lru_gen/enabled`。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `wakeup_kswapd` | `mm/vmscan.c:7817` | 叫醒某 zone 的 kswapd |
| `kswapd` | `:7731` | 每 node 的线程 |
| `balance_pgdat` | `:7409` | 把水位做到 HIGH |
| `kswapd_age_node` | `:7182` | `balance_pgdat` 里先老化。MGLRU 分支见 [05](./10-05-mglru-code-path.md) |
| `kswapd_shrink_node` | `:7327` | kswapd 侧的 shrink，里面调 `shrink_node` |
| `__perform_reclaim` | `mm/page_alloc.c:3662` | 直接回收的同步包装 |
| `__alloc_pages_direct_reclaim` | `mm/page_alloc.c:3689` | 慢路径进入直接回收 |
| `try_to_free_pages` | `mm/vmscan.c:7057` | 直接回收入口 |
| `do_try_to_free_pages` | `:6839` | 扫 zonelist 上的 node |
| `shrink_zones` | `:6716` | 对每个 node 调 `shrink_node` |
| `shrink_node` | `:6532` | 一个 node。分叉点在这里 |
| `shrink_node_memcgs` | `:6474` | 按 memcg 扫，并调 shrinker |
| `shrink_lruvec` | `:6286` | 一个 lruvec。非 root 且 MGLRU 开着时转去分代回收 |
| `root_reclaim` | `:442`（无 memcg 时 `:497`） | 是不是全局回收 |
| `cgroup_reclaim` | `:433`（无 memcg 时 `:492`） | 是不是 cgroup 限额触发的 |
| `shrink_slab` | `:1040` | slab shrinker 入口 |
| `flush_reclaim_state` | `:524` | 把 LRU 之外放出的页加进 `nr_reclaimed` |
| `set_task_reclaim_state` | `:508` | 把 `reclaim_state` 挂到 `current` |
| `lru_gen_enabled` | `include/linux/mm_inline.h:105` / `:112` / `:295` | 运行时主开关。三种编译条件下的三个定义 |

---

[上一篇 回收](./10-00-reclaim.md) · [本目录](./10-00-reclaim.md) · [下一篇 经典双链表 LRU](./10-02-lru-classic.md)
