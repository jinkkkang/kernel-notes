# 2. 物理布局：page / folio / node / zone / sparsemem

**结论**：每一页物理内存有一个 `struct page`；用户和页缓存看到的分配单位是 `folio`（可以是一页或多页）。页按 NUMA node（`pg_data_t`）再按 `zone` 分组。x86-64 默认用 sparsemem vmemmap：`mem_section` 指向 memmap，不是一整张平坦的 `mem_map[]`。

> 定义：`include/linux/mm_types.h`、`include/linux/mmzone.h`。v7.3 `c03c1a14507d`。
> 导航：[上一节 总览](./01-overview.md) · [目录](./README.md) · [下一节 页分配](./03-page-alloc/README.md)

`memdesc_flags_t`（`mm_types.h:39`）只是包了一层 `unsigned long f`。v7.3 的 `page->flags` 不再是裸 `unsigned long`，nid / zone / section 从这层 flags 里拆（`memdesc_nid`、`memdesc_zonenum`、`memdesc_section`，见 `mm.h` / `mmzone.h`）。

---

## 2.1 `struct page`（`mm_types.h:80`）

文件头注释（`:43-73`）仍说：每页一个 `struct page`；`alloc_pages()` 之后 main union 里大约五个 word 可借给分配者用，**第一个 word 的 bit 0 必须保持 0**（`PageTail()` 用它）。当前结构体就是下面这些成员，不另造字段。

对齐：`_struct_page_alignment`。`CONFIG_HAVE_ALIGNED_STRUCT_PAGE` 时按两个 `unsigned long` 对齐，给 SLUB 的 `cmpxchg` 双字用（`:74-78`）。

### 标志与主 union

| 成员 | 条件 | 中文 |
|---|---|---|
| `memdesc_flags_t flags` | 始终 | 原子标志。部分位会被异步改。nid/zone/section 也编码在这里 |
| `union { ... }` | 始终 | 下面几套解释叠在同一块内存上。第一 word 的 bit 0 留给 `PageTail()`，其它用途不能占用这个 bit |

**页缓存 / 匿名页**（union 第一臂，`:90-117`）：

| 成员 | 中文 |
|---|---|
| `struct list_head lru` | 在 LRU 上时的链表节点。锁是对应 `lruvec->lru_lock`。页的主人有时把它当通用链表 |
| `struct list_head buddy_list` | 与 `lru` 同 union。页在 buddy 空闲链表上时用 |
| `struct list_head pcp_list` | 与 `lru` 同 union。页在 per-CPU pageset 链表上时用 |
| `struct llist_node pcp_llist` | 与 `lru` 同 union。无锁 llist 形式的 pcp 链 |
| `struct address_space *mapping` | 文件页：所属 `address_space`。匿名页：带 `FOLIO_MAPPING_ANON` 低位标记，指向 `anon_vma`。空闲或特殊页可能为 NULL。最低位被复用，所以 `address_space` 要求按 `long` 对齐（`fs.h:485` 的注释） |
| `pgoff_t __folio_index` | 在 mapping 里的页偏移 |
| `unsigned long share` | 与 index 同 union。fsdax 的 share 计数 |
| `unsigned long private` | 私有数据。`PagePrivate` 时常放 `buffer_head`；swapcache 时放 `swp_entry_t`；`PageBuddy` 或在 `pcp_llist` 上时表示 order |

**page_pool（网络栈）**（`:118-128`）：

| 成员 | 中文 |
|---|---|
| `unsigned long pp_magic` | 魔数，避免把不是 page_pool 分配的页回收进池 |
| `struct page_pool *pp` | 所属 page_pool |
| `unsigned long _pp_mapping_pad` | 占位，垫出和 `mapping` 对齐的布局 |
| `unsigned long dma_addr` | DMA 地址 |
| `atomic_long_t pp_ref_count` | page_pool 引用 |

**复合页 tail**（`:129-131`）：

| 成员 | 中文 |
|---|---|
| `unsigned long compound_info` | tail page 用。bit 0 置位，这样 `PageTail()` 为真 |

**ZONE_DEVICE**（`:132-149`）：

| 成员 | 中文 |
|---|---|
| `void *_unused_pgmap_compound_info` | 第一 word。与 `compound_info` 或 folio 的 `pgmap` 叠在一起，本臂不单独解释它 |
| `void *zone_device_data` | 设备私有数据 |

内核注释写明：后面三个 word 与第一臂的 `mapping` / index / `private` 重叠。设备私有页在迁移期间用它们保住源匿名页或页缓存页的 mapping、index、private；`MEMORY_DEVICE_FS_DAX` 也用这三项。本臂没有再给这三个 word 起新名字。

**RCU**（`:152`）：

| 成员 | 中文 |
|---|---|
| `struct rcu_head rcu_head` | 用 RCU 延迟释放这一页时的回调头 |

### 映射计数、引用、可选尾字段

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned int page_type` | 与 `_mapcount` 同 union，共 4 字节（`:155`） | typed folio 的头页在这里放类型（列表在 `page-flags.h`）。类型 folio 的 tail 不存类型（`page_type == _mapcount == -1`）。主人可以复用低 16 位，清类型前必须把这 16 位恢复成 -1 |
| `atomic_t _mapcount` | 同上 | 非 typed、走 RMAP 的 folio：这一页被页表直接映射的次数。初值 -1，这样 `atomic_inc_and_test` / `atomic_add_negative(-1)` 能看见进出 0 的边沿。注释要求不要拿它做私有存储 |
| `atomic_t _refcount` | 始终 | 引用计数。不要直接摸，走 `page_ref.h`。`alloc_pages()` 返回时已经是正数 |
| `unsigned long memcg_data` | `CONFIG_MEMCG` | memcg 归属。和 cgroup 章的 charge 对得上 |
| `unsigned long _unused_slab_obj_exts` | 没有 MEMCG、但有 `CONFIG_SLAB_OBJ_EXT` | 占位，让 slab 对象扩展的布局和带 memcg 的构建对齐 |
| `void *virtual` | `WANT_PAGE_VIRTUAL` | 高端内存需要单独记下内核虚拟地址。x86-64 直接映射，通常不编译这个字段。注释说在 x86 上它甚至可以是 16 位，那是历史口吻 |
| `int _last_cpupid` | `LAST_CPUPID_NOT_IN_PAGE_FLAGS` | 最后访问的 CPU/PID，NUMA balancing 用。flags 里放不下才落到这里 |
| `struct page *kmsan_shadow` | `CONFIG_KMSAN` | 影子页：对应 bit 表示原页该 bit 是否未初始化 |
| `struct page *kmsan_origin` | `CONFIG_KMSAN` | 起源页：每 4 字节一个栈轨迹 id，记录未初始化值从哪来 |

`struct page` 到此结束（`:223`）。没有单独的 `index` 字段名在 `struct page` 里；index 叫 `__folio_index`，folio 侧才叫 `index`。

---

## 2.2 `struct folio`（`mm_types.h:404`）

**结论**：folio 是「这一次分配的那一整块页」的句柄。order 0 时它和单页重叠；高阶时头页后面还叠着第二、第三、第四个 `struct page` 的位置，用来放大页映射计数和 hugetlb 私有数据。

内核用 `/* private: */` / `/* public: */` 标文档可见性。下面**全部成员都列出**。标了 private 的是布局占位或过渡字段，不是给你当 API 用的。

### 第一页：与 `struct page` 同布局

外层 `union` 的第一臂（`:407-448`）与一个 `struct page` 对齐；同一 union 的另一臂就是 `struct page page`（`:449`）。

| 成员 | 条件 | 中文 |
|---|---|---|
| `memdesc_flags_t flags` | public | 同 `page->flags` |
| `struct list_head lru` | public | LRU / 空闲链，语义同 page |
| `void *__filler` | private | 躲开 `compound_info` 的 bit 0，给下面的 `mlock_count` 腾位置 |
| `unsigned int mlock_count` | public | Unevictable「LRU」槽里的 mlock 计数 |
| `struct dev_pagemap *pgmap` | public | ZONE_DEVICE 的 pagemap。与 `lru` / mlock 槽同 union |
| `struct address_space *mapping` | public | 文件 `address_space` 或匿名 `anon_vma` |
| `pgoff_t index` | public | 在 mapping 中的偏移。对应 page 的 `__folio_index` |
| `unsigned long share` | 与 index 同 union | fsdax share |
| `void *private` | public | 映射私有数据 |
| `swp_entry_t swap` | 与 private 同 union | swap 项 |
| `unsigned long migrate_info` | 与 private 同 union | 迁移信息 |
| `atomic_t _mapcount` | public | 头页 mapcount，初值语义同 page |
| `atomic_t _refcount` | public | 整 folio 引用 |
| `unsigned long memcg_data` | `CONFIG_MEMCG` | memcg |
| `unsigned long _unused_slab_obj_exts` | 无 MEMCG 且 `CONFIG_SLAB_OBJ_EXT` | 占位 |
| `void *virtual` | `WANT_PAGE_VIRTUAL` | 同 page |
| `int _last_cpupid` | `LAST_CPUPID_NOT_IN_PAGE_FLAGS` | 同 page |
| `struct page page` | 与上面整臂同 union | 把 folio 当 page 用时的视图。注释写明这条 union 是过渡 |

### 第二页槽（`__page_1`，`:451-482`）

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long _flags_2` 的上一页对应物 `_flags_1` | private | 与第二个 `struct page` 的 flags 重叠，占位 |
| `unsigned long _head_1` | private | 与该页第一 word 重叠，占位 |
| `atomic_t _large_mapcount` | public | 大 folio 的 mapcount |
| `atomic_t _nr_pages_mapped` | public | 已被页表映射的子页数量 |
| `atomic_t _entire_mapcount` | `CONFIG_64BIT` | 整个 folio 被一次映射（PMD/PUD）的计数。32 位放到第三页槽 |
| `atomic_t _pincount` | `CONFIG_64BIT` | `pin_user_pages` 一类长期 pin 的计数 |
| `mm_id_mapcount_t _mm_id_mapcount[2]` | public | 按 mm id 记的 mapcount，两个槽 |
| `mm_id_t _mm_id[2]` | public | 对应的两个 mm id |
| `unsigned long _mm_ids` | 与 `_mm_id[2]` 同 union | 把两个 mm id 当成一个字访问 |
| `unsigned long _usable_1[4]` | 与上面这组同 union | 过渡占位，长度 4 |
| `atomic_t _mapcount_1` | private | 与该 `struct page` 的 `_mapcount` 重叠 |
| `atomic_t _refcount_1` | private | 与该 `struct page` 的 `_refcount` 重叠 |
| `unsigned int _nr_pages` | `NR_PAGES_IN_LARGE_FOLIO` | 这个 folio 含多少页。某些配置把页数放进 flags，这个字段就不存在 |
| `struct page __page_1` | 同 union | 第二个 page 的视图 |

### 第三页槽（`__page_2`，`:483-496`）

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long _flags_2` | private | 占位，重叠第三个 page 的 flags |
| `unsigned long _head_2` | private | 占位 |
| `struct list_head _deferred_list` | public | THP 延迟拆分队列。`khugepaged` / shrinker 扫它 |
| `atomic_t _entire_mapcount` | `!CONFIG_64BIT` | 32 位上整个 folio 的 mapcount 放这里 |
| `atomic_t _pincount` | `!CONFIG_64BIT` | 32 位上的 pin 计数 |
| `struct page __page_2` | 同 union | 第三个 page 的视图 |

### 第四页槽（`__page_3`，`:497-509`），hugetlb

| 成员 | 中文 |
|---|---|
| `unsigned long _flags_3` | private 占位 |
| `unsigned long _head_3` | private 占位 |
| `void *_hugetlb_subpool` | hugetlb 子池 |
| `void *_hugetlb_cgroup` | hugetlb cgroup（用量） |
| `void *_hugetlb_cgroup_rsvd` | hugetlb cgroup 预留 |
| `void *_hugetlb_hwpoison` | 这一大页上的 hwpoison 记录 |
| `struct page __page_3` | 第四个 page 的视图 |

小 folio 不会去碰第二页以后的字段。把 order-0 folio 当成多页结构去写 `_deferred_list`，就是踩别的页的 `struct page`。

成员表上面已经列全。`_refcount` 和 `_mapcount` 是两本账，get/put 规则见 [2.9](#29-folio-的-refcount-与-mapcount)。

---

## 2.3 node：`pg_data_t` / `struct pglist_data`（`mmzone.h:1478`）

**结论**：一个 NUMA node 一个 `pg_data_t`。UMA 只有一个（`contig_page_data`，`:1770`）。它拥有本节点的 zone 数组和 zonelist，以及 kswapd/kcompactd。

`NODE_DATA(nid)` 在 `:1771`。

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct zone node_zones[MAX_NR_ZONES]` | 始终 | **只含本节点**的 zone。没填满的槽可以是空的，但数组是完整的。本节点和别的节点的 zonelist 都会引用它们 |
| `struct zonelist node_zonelists[MAX_ZONELISTS]` | 始终 | 分配时用的 zone 回退链。一般前面几项就是本节点的 zone |
| `int nr_zones` | 始终 | 本节点有多少个已填充 zone |
| `struct page *node_mem_map` | `CONFIG_FLATMEM` | 平坦 memmap。SPARSEMEM 不走这里 |
| `struct page_ext *node_page_ext` | FLATMEM 且 `CONFIG_PAGE_EXTENSION` | 页扩展信息 |
| `spinlock_t node_size_lock` | 热插拔或延迟初始化 `struct page` | 保护 `node_start_pfn`、`node_present_pages`、`node_spanned_pages`、`nr_zones`，并同步 `first_deferred_pfn`。嵌套在 `zone->lock` 和 `zone->span_seqlock` 之上 |
| `unsigned long node_start_pfn` | 始终 | 本节点起始 PFN |
| `unsigned long node_present_pages` | 始终 | 物理上存在的页数（不含洞） |
| `unsigned long node_spanned_pages` | 始终 | 从起始到结束的跨度，**含洞** |
| `int node_id` | 始终 | 节点号 |
| `wait_queue_head_t kswapd_wait` | 始终 | 唤醒 kswapd |
| `wait_queue_head_t pfmemalloc_wait` | 始终 | PFMEMALLOC 分配在等水位恢复 |
| `wait_queue_head_t reclaim_wait[NR_VMSCAN_THROTTLE]` | 始终 | 按不同原因节流回收的等待队列 |
| `atomic_t nr_writeback_throttled` | 始终 | 正被回写节流的任务数 |
| `unsigned long nr_reclaim_start` | 始终 | 开始节流时已经写出的页数，用来判断回写有没有进展 |
| `struct mutex kswapd_lock` | `CONFIG_MEMORY_HOTPLUG` | 保护 `kswapd` 任务指针 |
| `struct task_struct *kswapd` | 始终 | 本节点的 kswapd。热插拔配置下由 `kswapd_lock` 保护 |
| `int kswapd_order` | 始终 | kswapd 这次要满足的阶 |
| `enum zone_type kswapd_highest_zoneidx` | 始终 | kswapd 最高扫到哪个 zone |
| `atomic_t kswapd_failures` | 始终 | 「回收量 == 0」的连续次数。过高会把节点标成 hopeless（见 `kswapd_test_hopeless`，`mmzone.h:1649`） |
| `int kcompactd_max_order` | `CONFIG_COMPACTION` | kcompactd 目标阶 |
| `enum zone_type kcompactd_highest_zoneidx` | `CONFIG_COMPACTION` | kcompactd 最高 zone |
| `wait_queue_head_t kcompactd_wait` | `CONFIG_COMPACTION` | 唤醒 kcompactd |
| `struct task_struct *kcompactd` | `CONFIG_COMPACTION` | 本节点压缩线程 |
| `bool proactive_compact_trigger` | `CONFIG_COMPACTION` | 主动压缩被触发 |
| `unsigned long totalreserve_pages` | 始终 | 留给内核、不给用户分配的本节点保留页 |
| `unsigned long min_unmapped_pages` | `CONFIG_NUMA` | node reclaim：未映射页超过这阈值才考虑就地回收 |
| `unsigned long min_slab_pages` | `CONFIG_NUMA` | node reclaim：slab 页阈值 |
| `CACHELINE_PADDING(_pad1_)` | 始终 | 把下面写频繁的字段推到另一条 cache line |
| `unsigned long first_deferred_pfn` | `CONFIG_DEFERRED_STRUCT_PAGE_INIT` | 大机器延迟初始化 `struct page` 时，下一个还没初始化的 PFN |
| `unsigned int nbp_rl_start` | `CONFIG_NUMA_BALANCING` | 当前 promote 限速窗口的起始时间（ms） |
| `unsigned long nbp_rl_nr_cand` | `CONFIG_NUMA_BALANCING` | 该窗口开始时的 promote 候选页数 |
| `unsigned int nbp_threshold` | `CONFIG_NUMA_BALANCING` | promote 阈值（ms） |
| `unsigned int nbp_th_start` | `CONFIG_NUMA_BALANCING` | 阈值调整窗口起始（ms） |
| `unsigned long nbp_th_nr_cand` | `CONFIG_NUMA_BALANCING` | 阈值调整窗口开始时的候选页数 |
| `struct lruvec __lruvec` | 始终 | **开了 MEMCG 就不用这个字段**。注释写明要走 `mem_cgroup_lruvec()`。没开 memcg 时，`lruvec_pgdat()` 用 `container_of` 从这里找回 pgdat（`:1665`） |
| `unsigned long flags` | 始终 | 节点标志 |
| `struct lru_gen_mm_walk mm_walk` | `CONFIG_LRU_GEN` | kswapd 走页表时的 MGLRU 游标。成员见下 |
| `struct lru_gen_memcg memcg_lru` | `CONFIG_LRU_GEN` | 本节点上 memcg 的分代链表 |
| `CACHELINE_PADDING(_pad2_)` | 始终 | 再隔一条 cache line |
| `struct per_cpu_nodestat __percpu *per_cpu_nodestats` | 始终 | 每 CPU 的节点计数差分 |
| `atomic_long_t vm_stat[NR_VM_NODE_STAT_ITEMS]` | 始终 | 节点级 vmstat。`/proc/zoneinfo` 里 “per-node stats” 从这里来 |
| `struct memory_tier __rcu *memtier` | `CONFIG_NUMA` | 这个节点属于哪一档内存（内存分层） |
| `struct memory_failure_stats mf_stats` | `CONFIG_MEMORY_FAILURE` | 硬件中毒页统计。`total/ignored/failed/delayed/recovered`（`:1449`）。注释：各节点 `total * PAGE_SIZE` 对应 `/proc/meminfo` 的 `HardwareCorrupted` |

`lru_gen_mm_walk`（`:617`）成员：`lruvec`、`seq`（可能落后于 `lru_gen_folio.max_seq`）、`next_addr`、`nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES]`、`mm_stats[NR_MM_STATS]`、`batched`、`swappiness`、`force_scan`。

`lru_gen_memcg`（`:679`）成员：`seq`、`nr_memcgs[MEMCG_NR_GENS]`、`fifo[MEMCG_NR_GENS][MEMCG_NR_BINS]`、`lock`。`MEMCG_NR_GENS` 是 3，`MEMCG_NR_BINS` 是 8（`:676-677`）。老代/年轻代分桶，是为了全局回收时公平扫 memcg，不是 memcg 自己的 reclaim 迭代器（那个仍走 `mem_cgroup_iter()`，注释 `:671`）。

---

## 2.4 `struct zone`（`mmzone.h:979`）

**结论**：zone 是分配和水位的单位。x86-64 常见是 DMA、DMA32、NORMAL。`ZONE_MOVABLE` 用来让热拔插和 THP 少碰到不可移动页。本机快照只有 DMA / DMA32 / NORMAL，没有 MOVABLE（见 [13](./13-observability.md)）。

`enum zone_type`（`:884`）：

| 枚举 | 条件 | 中文 |
|---|---|---|
| `ZONE_DMA` | `CONFIG_ZONE_DMA` | DMA 窗口小的设备。x86 上通常 <16MB |
| `ZONE_DMA32` | `CONFIG_ZONE_DMA32` | 32 位 DMA 掩码盖得住的范围 |
| `ZONE_NORMAL` | 始终 | 正常可直接映射、设备若支持全地址 DMA 也能用 |
| `ZONE_HIGHMEM` | `CONFIG_HIGHMEM` | 32 位内核的高端内存。x86-64 没有 |
| `ZONE_MOVABLE` | 始终 | 尽量只放可移动页，方便 offlining，也让不可移动分配别把大页切碎。注释列了 pin、memblock、洞、hwpoison、virtio-mem 的 `PG_offline`、`ZERO_PAGE`、`memmap_on_memory` 这些例外（`:919-966`） |
| `ZONE_DEVICE` | `CONFIG_ZONE_DEVICE` | 设备内存，不进普通 buddy 回收 |
| `__MAX_NR_ZONES` | 始终 | 数组上界 |

### zone 成员（按源码顺序）

读多写少的前半：

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long _watermark[NR_WMARK]` | 始终 | 水位。下标是 `WMARK_MIN/LOW/HIGH/PROMO`（`:809`）。用 `*_wmark_pages(zone)` 读，不要裸读这个数组 |
| `unsigned long watermark_boost` | 始终 | 临时抬高的水位。碎片或分配压力会 boost，促使 kswapd 多回收一点 |
| `unsigned long nr_reserved_highatomic` | 始终 | 为高阶原子分配保留的页数。大约最多占 zone 的 1%（`reserve_highatomic_pageblock`，`page_alloc.c:3447`） |
| `unsigned long nr_free_highatomic` | 始终 | 上述保留里当前仍空闲的页 |
| `long lowmem_reserve[MAX_NR_ZONES]` | 始终 | 低 zone 留给高 zone 分配失败时的保留。`/proc/zoneinfo` 的 `protection:` 就是它。`vm.min_free_kbytes` 和 `lowmem_reserve_ratio` 会重算 |
| `int node` | `CONFIG_NUMA` | 所属节点号 |
| `struct pglist_data *zone_pgdat` | 始终 | 回指 node |
| `struct per_cpu_pages __percpu *per_cpu_pageset` | 始终 | 每 CPU 的热页缓存。order 0 和便宜的高阶分配先走这里 |
| `struct per_cpu_zonestat __percpu *per_cpu_zonestats` | 始终 | 每 CPU zone 计数差分 |
| `int pageset_high_min` | 始终 | 复制到各 pageset 的 high 下限，热路径少去共享 cache line |
| `int pageset_high_max` | 始终 | high 上限 |
| `int pageset_batch` | 始终 | 批量从 buddy 拿/还的大小 |
| `unsigned long *pageblock_flags` | `!CONFIG_SPARSEMEM` | pageblock 的 migratetype 位图。SPARSEMEM 改放在 `mem_section` 里 |
| `unsigned long zone_start_pfn` | 始终 | 起始 PFN。`zone_start_paddr >> PAGE_SHIFT` |
| `atomic_long_t managed_pages` | 始终 | buddy **正在管**的页。水位按它算。`present - reserved` |
| `unsigned long spanned_pages` | 始终 | `zone_end_pfn - zone_start_pfn`，含洞 |
| `unsigned long present_pages` | 始终 | 物理存在的页，不含洞。热插拔写它要 `mem_hotplug_begin/done()` |
| `unsigned long present_early_pages` | `CONFIG_MEMORY_HOTPLUG` | 启动时就在、不含后来热插的 present 页 |
| `unsigned long cma_pages` | `CONFIG_CMA` | 划给 CMA（`MIGRATE_CMA`）的页 |
| `const char *name` | 始终 | `"DMA"` / `"DMA32"` / `"Normal"` / `"Movable"`，`/proc/zoneinfo` 用 |
| `unsigned long nr_isolate_pageblock` | `CONFIG_MEMORY_ISOLATION` | 被隔离的 pageblock 数。解决 migratetype 竞态导致的空闲页计数错误。`zone->lock` 保护 |
| `seqlock_t span_seqlock` | `CONFIG_MEMORY_HOTPLUG` | 保护 `zone_start_pfn` 和 `spanned_pages`。分配快路径用 seqlock 读，因为不能总拿 `zone->lock` |
| `int initialized` | 始终 | zone 是否已初始化 |

写频繁的分配侧（`_pad1_` 之后）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `CACHELINE_PADDING(_pad1_)` | 始终 | 隔离 cache line |
| `struct free_area free_area[NR_PAGE_ORDERS]` | 始终 | buddy 本体。每个 order 一条。`NR_PAGE_ORDERS = MAX_PAGE_ORDER+1`，默认 `MAX_PAGE_ORDER` 是 10（`mmzone.h:31`），也就是 0..10 |
| `struct list_head unaccepted_pages` | `CONFIG_UNACCEPTED_MEMORY` | 还没 accept 的页，全是 `MAX_PAGE_ORDER` |
| `struct work_struct unaccepted_cleanup` | `CONFIG_UNACCEPTED_MEMORY` | 本 zone 最后一页被 accept 后的清理 |
| `unsigned long flags` | 始终 | zone 标志，例如 `ZONE_BOOSTED_WATERMARK` |
| `spinlock_t lock` | 始终 | 主要保护 `free_area` |
| `struct llist_head trylock_free_pages` | 始终 | `zone->lock` 没抢到时先挂这里，下次 trylock 成功再还进 buddy |

压缩与统计（`_pad2_` 之后）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `CACHELINE_PADDING(_pad2_)` | 始终 | 再隔开 |
| `unsigned long percpu_drift_mark` | 始终 | 空闲页低于这点时，读空闲数要避免 per-CPU 计数漂移把水位看错 |
| `unsigned long compact_cached_free_pfn` | COMPACTION 或 CMA | 空闲扫描器下次从哪开始 |
| `unsigned long compact_cached_migrate_pfn[ASYNC_AND_SYNC]` | 同上 | 迁移扫描器，异步/同步各一个。`ASYNC_AND_SYNC` 是 2（`:977`） |
| `unsigned long compact_init_migrate_pfn` | 同上 | 本轮迁移扫描的起点 |
| `unsigned long compact_init_free_pfn` | 同上 | 本轮空闲扫描的起点 |
| `unsigned int compact_considered` | `CONFIG_COMPACTION` | 上次失败后已经尝试过的次数 |
| `unsigned int compact_defer_shift` | `CONFIG_COMPACTION` | 失败后跳过 `1<<shift` 次再试 |
| `int compact_order_failed` | `CONFIG_COMPACTION` | 失败过的最小阶 |
| `bool compact_blockskip_flush` | COMPACTION 或 CMA | 为真时要清 `PG_migrate_skip` |
| `bool contiguous` | 始终 | zone 的 PFN 是否连续（没有洞）。压缩可以走更快的路径 |
| `CACHELINE_PADDING(_pad3_)` | 始终 | 统计再隔开 |
| `atomic_long_t vm_stat[NR_VM_ZONE_STAT_ITEMS]` | 始终 | zone 级计数，`nr_free_pages` 等 |
| `atomic_long_t vm_numa_event[NR_VM_NUMA_EVENT_ITEMS]` | 始终 | NUMA 命中/未命中。非 NUMA 配置这个数组长度是 0 |
| `struct page *vmemmap_tails[NR_VMEMMAP_TAILS]` | `CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP` | HVO 尾部 struct page 的缓存 |

`struct free_area`（`:195`）只有两个成员：`struct list_head free_list[MIGRATE_TYPES]`，以及 `unsigned long nr_free`。

`enum migratetype`（`:118`）：

| 值 | 中文 |
|---|---|
| `MIGRATE_UNMOVABLE` | 不可移动。内核数据结构、页表这一类 |
| `MIGRATE_MOVABLE` | 可移动。用户匿名页、页缓存 |
| `MIGRATE_RECLAIMABLE` | 可回收。inode/dentry slab 一类 |
| `MIGRATE_PCPTYPES` | **不是一种页**。它的值等于 PCP 上有几种类型（上面三种） |
| `MIGRATE_HIGHATOMIC` | 等于 `MIGRATE_PCPTYPES`。高阶原子分配的保留块 |
| `MIGRATE_CMA` | `CONFIG_CMA`。行为上模仿 ZONE_MOVABLE，分配器不会悄悄改这种 pageblock 的类型 |
| `MIGRATE_ISOLATE` | `CONFIG_MEMORY_ISOLATION`。隔离中，不能从这里分 |

能互相合并的只有 `< MIGRATE_PCPTYPES` 的类型（`migratetype_is_mergeable`，`:175`）。HIGHATOMIC、CMA、ISOLATE 不能跟普通块合并，这是外碎片的来源之一。

### `struct per_cpu_pages`（`:844`）

| 成员 | 条件 | 中文 |
|---|---|---|
| `spinlock_t lock` | 始终 | 保护下面的 lists |
| `int count` | 始终 | 列表里的页数 |
| `int high` | 始终 | 高水位，超过就要倒回 buddy |
| `int high_min` / `high_max` | 始终 | high 的允许范围。`decay_pcp_high()`（`page_alloc.c:2576`）会把闲置 CPU 的 high 慢慢降下去 |
| `int batch` | 始终 | 一次向 buddy 批量申请/归还的大小 |
| `u8 flags` | 始终 | `PCPF_PREV_FREE_HIGH_ORDER`、`PCPF_FREE_HIGH_BATCH`（`:841-842`） |
| `u8 alloc_factor` | 始终 | 分配时把 batch 放大的因子 |
| `u8 expire` | `CONFIG_NUMA` | 减到 0 就排空远程节点的 pageset |
| `short free_count` | 始终 | 连续释放次数。分配时右移（`rmqueue_pcplist`，`:3389`） |
| `struct list_head lists[NR_PCP_LISTS]` | 始终 | 按 migratetype × order 分链。`NR_LOWORDER_PCP_LISTS = MIGRATE_PCPTYPES * (PAGE_ALLOC_COSTLY_ORDER+1)`，`PAGE_ALLOC_COSTLY_ORDER` 是 3（`mmzone.h:63`）。开了 THP 再加 `NR_PCP_THP`（2）条 |

---

## 2.5 `struct zonelist` / `struct zoneref`

**结论**：一次分配先选定一条 zonelist，从首选 zone 往回退 zone 扫。`__GFP_THISNODE` 用没有回退的那条。

`enum`（`mmzone.h:1400` 一带）：`ZONELIST_FALLBACK`；NUMA 上还有 `ZONELIST_NOFALLBACK`。`MAX_ZONELISTS` 是上界。

`struct zoneref`（`:1415`）：

| 成员 | 中文 |
|---|---|
| `struct zone *zone` | 这条记录指向的 zone |
| `int zone_idx` | `zone_idx(zone)` 的缓存，避免热路径再去大结构里算 |

`struct zonelist`（`:1434`）只有一个成员：`struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1]`。多出来的一项是结束标记（`zone == NULL`）。`first_zones_zonelist()` 在 `:1874`。

---

## 2.6 sparsemem：`struct mem_section`

**结论**：x86-64 不用一张巨大的 `mem_map[]`。物理地址切成 section，每个 section 一个 `mem_section`，里面藏着该段 `struct page` 数组的地址。PFN → page 先找 section，再加偏移。

`struct mem_section_usage`（`:1997`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct rcu_head rcu` | 始终 | 释放 usage 时的 RCU 头 |
| `subsection_map` 位图 | `CONFIG_SPARSEMEM_VMEMMAP` | 哪些 subsection 有 memmap。subsection 是 2MB（`SUBSECTION_SHIFT 21`，`:1981`） |
| `unsigned long pageblock_flags[0]` | 始终 | 柔性数组。pageblock migratetype 位图。SPARSEMEM 下 zone 不再单独放这份图 |

`struct mem_section`（`:2008`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long section_mem_map` | 始终 | 逻辑上是 `struct page` 数组指针，低位另有用途，早期启动还会编码 node id。用之前要按 `SECTION_MAP_MASK` 清掉低位（`__section_mem_map_addr`，`:2112`） |
| `struct mem_section_usage *usage` | 始终 | 上面的 usage |
| `struct page_ext *page_ext` | `CONFIG_PAGE_EXTENSION` | SPARSEMEM 下 page_ext 挂在 section 上，不挂在 pgdat |
| `unsigned long pad` | `CONFIG_PAGE_EXTENSION` | 把结构补成 2 的幂。注释警告：`SECTION_ROOT_MASK` 依赖这个大小是 2 的幂 |

低位标志（`:2085`）：`SECTION_MARKED_PRESENT`、`SECTION_HAS_MEM_MAP`、`SECTION_IS_ONLINE`、`SECTION_IS_EARLY`，以及可选的 `SECTION_TAINT_ZONE_DEVICE`、`SECTION_IS_VMEMMAP_PREINIT`。nid 存在 `SECTION_MAP_LAST_BIT` 之上（`SECTION_NID_SHIFT`）。

`CONFIG_SPARSEMEM_EXTREME` 时 `mem_section` 是二级指针（`:2048`），根的大小按页算：`SECTIONS_PER_ROOT = PAGE_SIZE / sizeof(struct mem_section)`。否则是二维数组。

FLATMEM 的 `mem_map` 仍声明着（`:1443`），因为有的配置两边都编译到。x86-64 服务器看 sparsemem 就够。三种模型下 PFN 怎么变成 `struct page`，见 [2.8](#28-pfn_to_page三种模型)。

---

## 2.7 x86-64 虚拟布局（点到为止）

权威表在 `Documentation/arch/x86/x86_64/mm.rst`。4 级页表时和本章有关的几行：

| 虚拟范围（文档原文） | 大小 | 是什么 |
|---|---|---|
| `0000000000000000` – `00007fffffffefff` | ~128TB | 用户空间，每个 `mm` 一份 |
| `ffff888000000000` – `ffffc87fffffffff` | 64TB | **直接映射** `page_offset_base`。内核用 `page_address()` / `virt_to_page()` 的那片 |
| `ffffc90000000000` – `ffffe8ffffffffff` | 32TB | **vmalloc/ioremap**（`vmalloc_base`） |
| `ffffea0000000000` – `ffffeaffffffffff` | 1TB | **vmemmap**（`vmemmap_base`）：`struct page` 数组自己住的虚拟地址 |
| `ffffffff80000000` 起 | 512MB | 内核文本，映射到物理 0 |
| `ffffffffa0000000` 起 | 1520MB | 模块 |

5 级页表把用户空间扩到约 64PB，内核映射整体下移。KASLR 会在允许的洞里挪 `page_offset_base` / `vmalloc_base` / `vmemmap_base`，所以上表是文档给出的经典布局，不是某一台机器 `dmesg` 的保证值。

直接映射是物理连续的线性映射。vmalloc 区是虚拟连续、物理页来自 buddy、可以不连续。两者不要混：`virt_addr_valid()` 对 vmalloc 地址为假，`vmalloc_to_page()` 才是那条路（`mm.h:1885`）。

本机 6.12.94+ 的 `DirectMap1G` 约 15GB、`DirectMap2M` 约 3GB、`DirectMap4k` 18MB（[13](./13-observability.md)）。那是直接映射被拆成多大的页，不是 vmalloc。

---

## 2.8 `pfn_to_page`：三种模型

**结论**：`pfn_to_page` 不是一个函数。`include/asm-generic/memory_model.h:74` 把它定义成 `__pfn_to_page`，而 `__pfn_to_page` 按编译期的内存模型三选一。x86-64 服务器默认走 `CONFIG_SPARSEMEM_VMEMMAP`：PFN 就是 vmemmap 数组的下标。

`struct mem_section` 的成员表在 [2.6](#26-sparsememstruct-mem_section)，这里不重复。

三种展开（同一文件，互斥）：

```c
/* FLATMEM，memory_model.h:18。一张 mem_map[]，PFN 减 ARCH_PFN_OFFSET 做下标 */
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))

/* SPARSEMEM_VMEMMAP，:46。vmemmap 在虚拟地址上连续，PFN 直接当下标 */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))

/* 经典 SPARSEMEM（没有 VMEMMAP），:60。先找 section，再加 PFN */
#define __pfn_to_page(pfn)				\
({	unsigned long __pfn = (pfn);			\
	struct mem_section *__sec = __pfn_to_section(__pfn);	\
	__section_mem_map_addr(__sec) + __pfn;		\
})
```

经典 SPARSEMEM 那条能成立，是因为 `section_mem_map` 存的不是数组起点，而是「数组地址减去该 section 的起始 PFN」（文件注释 `:50-52`）。加上 PFN 之后，section 内的偏移自动对齐。低位标志要用 `__section_mem_map_addr`（`mmzone.h:2112`）清掉，[2.6](#26-sparsememstruct-mem_section) 写过。

`page_to_pfn` 是反向的三条（`:19`、`:47`、`:54`）。带 `CONFIG_DEBUG_VIRTUAL` 时 `page_to_phys`（`:77`）会 `WARN_ON_ONCE(!pfn_valid)`。

vmemmap 本身要先被填上页表。`vmemmap_populate_basepages`（`mm/sparse-vmemmap.c:296`）只是转去 `vmemmap_populate_range(..., order=-1)`，按 base page 把 `[start, end)` 这段 `struct page` 虚拟地址铺出来。大页映射走 `vmemmap_populate_hugepages`（同文件 `:404`）。HVO 是铺完之后再把 hugetlb 的尾部 `struct page` 指到同一份，见 [09](./09-thp-and-hugetlb.md)。

x86-64 的 `vmemmap` 符号就是 `vmemmap_base` 上的那片（[2.7](#27-x86-64-虚拟布局点到为止)）。KASLR 会挪基址，公式不变：`page = vmemmap + pfn`。

---

## 2.9 folio 的 refcount 与 mapcount

**结论**：`folio` 是为了让「一次分配的那一整块」有一个句柄。order-0 时它和 `struct page` 重叠；高阶时头页代表整块，tail 不再各自算一笔分配。`_refcount` 回答「谁还抓着这块内存」，`_mapcount` 回答「有多少用户页表项指着它」。两本账不能互相代替。

成员表在 [2.2](#22-struct-folio-mm_typesh404)，这里只讲语义。引入 folio 之前，复合页用 `PageHead`/`PageTail` 和 `compound_head()` 到处找头页，tail 上的计数容易和头页搞混。folio 把分配单位收成一个类型：`folio_nr_pages()` 是这块有几页，引用和映射都记在头上。

`page_ref.h:91-100` 把 refcount 拆成三份，读代码时对得上：

| 成分 | 是什么 |
|---|---|
| expected | 代码路径上说得清的持有者：页表一项、页缓存、LRU、文件系统私有数据、pipe、直接 IO |
| temporary | 某条路径临时 `folio_get`，防止自己用的时候这块被还回分配器 |
| spurious | RCU 扫页表或页缓存拿到的过期指针。发现过期后会 put，但刚分配出来的 folio 可能短暂看到 2 而不是 1 |

规则：

- 已经持有引用，才能 `folio_get`（`mm.h:2119`）。它只做 `folio_ref_inc`。refcount 已经是 0 或接近溢出时 `VM_BUG_ON`。
- 手里没有引用（LRU 隔离、RCU 遍历），用 `folio_try_get`（`page_ref.h:274`）。它是 `folio_ref_add_unless_zero`：refcount 已经是 0（frozen，正在拆分、迁移或释放）就失败，不能把死页救活。
- `folio_put`（`mm.h:2157`）减到 0 就进 `__folio_put`，通常还回分配器。注释写明可以在持有自旋锁时调用。
- `page_ref_count`（`page_ref.h:65`）是 `atomic_read(&page->_refcount)`。`folio_ref_count`（`:105`）读的是头页。`page_count()`（`:110`）先 `page_folio()` 再读 folio 的引用，不要拿 tail 的 `_refcount` 当整块的引用。
- 页表里每条指向这块的用户 PTE/PMD/PUD，必须配恰好一次 folio 引用（`folio_mapcount` 的注释，`mm.h:1931-1933`）。unmap 拆 PTE 时丢掉的是映射，引用要另 put。

mapcount 的读法（`mm.h:1947`）：

```c
static inline int folio_mapcount(const struct folio *folio)
{
	int mapcount;
	if (likely(!folio_test_large(folio))) {
		/* 小 folio：_mapcount 初值 -1，所以 +1 才是真正的映射次数 */
		mapcount = atomic_read(&folio->_mapcount) + 1;
		if (page_mapcount_is_type(mapcount))
			mapcount = 0;   /* typed folio 不走 rmap，映射次数当 0 */
		return mapcount;
	}
	return folio_large_mapcount(folio); /* 大 folio 读 _large_mapcount + 1 */
}
```

普通 folio 上，每条用户页表项算一次。hugetlb 上，一条「抽象的 hugetlb 页表项」算一次，哪怕它在硬件上拆成多条普通 PTE（注释 `:1938-1940`）。slab、页表页这些不能映射进用户空间的，mapcount 报 0。

`_entire_mapcount` 是整块被一条 PMD/PUD 映射的次数，调试和拆大页用，不是 `folio_mapcount()` 的返回值。`mlock_count` 是 unevictable 槽上的锁页计数，和 mapcount 也不是一回事，见 [10-reclaim/09](./10-reclaim/09-mlock-unevictable.md)。

拿到 folio 之后如果要改页表，还要页表锁。引用只保证这块 `struct page` 不会被释放，不保证 PTE 还指着它。

---

## 2.10 memblock：buddy 醒来之前

**结论**：启动早期没有 buddy、没有 slab。可用内存和已保留内存都记在 `struct memblock` 里。内核自己的早期分配走 `memblock_phys_alloc_range` 这一类，从 `memory` 里挖、记进 `reserved`。buddy 接盘之后，这套表可以丢掉（`memblock_discard`），除非 `CONFIG_ARCH_KEEP_MEMBLOCK`。

`include/linux/memblock.h`。

`struct memblock_region`（`:75`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `phys_addr_t base` | 始终 | 区域物理基址 |
| `phys_addr_t size` | 始终 | 长度 |
| `enum memblock_flags flags` | 始终 | 见下 |
| `int nid` | `CONFIG_NUMA` | 这块属于哪个 node |

`memblock_flags`（`:56`）：`MEMBLOCK_NONE`、`HOTPLUG`（可热插拔）、`MIRROR`、`NOMAP`（不要做内核直接映射）、`DRIVER_MANAGED`（驱动报上来的，固件内存图里没有）、`RSRV_NOINIT`（保留区，`struct page` 没完全初始化）、`RSRV_KERN`（内核自己留的；分配 API 都会打上）、`KHO_SCRATCH`（kexec handover 的 scratch，早期只能从这里分）、`RSRV_HUGETLB`。

`struct memblock_type`（`:92`）：

| 成员 | 中文 |
|---|---|
| `unsigned long cnt` | 当前有多少个 region |
| `unsigned long max` | `regions` 数组的容量 |
| `phys_addr_t total_size` | 全部 region 的尺寸和 |
| `struct memblock_region *regions` | region 数组 |
| `char *name` | `"memory"` 或 `"reserved"` 这类名字 |

`struct memblock`（`:107`）四个成员：

| 成员 | 中文 |
|---|---|
| `bool bottom_up` | 自低地址往高地址分 |
| `phys_addr_t current_limit` | 当前允许分配到的物理地址上限 |
| `struct memblock_type memory` | 可用内存 |
| `struct memblock_type reserved` | 已经划走的 |

全局对象 `memblock`（`mm/memblock.c:129`，`__initdata_memblock`）。

```text
memblock_phys_alloc_range(size, align, start, end)   # memblock.c:1624
  → memblock_alloc_range_nid(..., NUMA_NO_NODE, false)
       在 [start, end) 里找一块对齐的空闲物理内存
       成功返回物理地址，失败返回 0
```

它标了 `__init`。buddy 起来之后再调，就是用一张已经废弃或只读的表。运行时要连续物理页，走 CMA / `alloc_contig_pages`，见 [03-page-alloc/09](./03-page-alloc/09-cma.md)。

### KHO scratch

Kexec HandOver（`Documentation/admin-guide/mm/kho.rst`）要跨 kexec 保住一块内存。早期还不知道全部保留区，上一个内核留出 scratch，本内核早期只许从这块分。对应标志是 `MEMBLOCK_KHO_SCRATCH`，`memblock_mark_kho_scratch` / `memblock_clear_kho_scratch`（`memblock.h:161-162`）。

运行时把 folio 交给下一个内核的入口在 `kernel/liveupdate/kexec_handover.c`：`kho_preserve_folio`（`:1147`）、`kho_preserve_pages`（`:1222`）、`kho_preserve_vmalloc`（`:1358`）。要编进 `CONFIG_KEXEC_HANDOVER`，启动参数 `kho=on`。`kho_scratch=` 规定低端、全局、每 node 三块 scratch 的大小。没打开这个配置时，这些符号不在镜像里。

---

## 2.11 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `page_zonenum` / `folio_zonenum` | `mmzone.h:1294` / `:1299` | 从 flags 取 zone |
| `memdesc_zonenum` | `mmzone.h:1286` | 同上，吃 `memdesc_flags_t *` |
| `memdesc_nid` | `mm.h:2384` 一带 | 从 flags 取 nid |
| `NODE_DATA` | `mmzone.h:1771` | nid → `pg_data_t` |
| `lruvec_pgdat` | `mmzone.h:1665` | lruvec → node |
| `first_zones_zonelist` | `mmzone.h:1874` | zonelist 上第一个合格 zone |
| `__nr_to_section` | `mmzone.h:2059` | section 号 → `mem_section` |
| `__section_mem_map_addr` | `mmzone.h:2112` | 清掉 `section_mem_map` 低位，取出 page 数组 |
| `__pfn_to_page` / `pfn_to_page` | `memory_model.h:18` / `:46` / `:60`，别名 `:74` | 三种模型下 PFN → page |
| `__page_to_pfn` / `page_to_pfn` | 同文件 `:19` / `:47` / `:54`，别名 `:73` | 反向 |
| `vmemmap_populate_basepages` | `mm/sparse-vmemmap.c:296` | 按 base page 铺 vmemmap |
| `page_ref_count` | `page_ref.h:65` | 读 `_refcount` |
| `folio_ref_count` | `page_ref.h:105` | 头页引用 |
| `folio_get` / `folio_put` | `mm.h:2119` / `:2157` | 已持有时加减引用 |
| `folio_try_get` | `page_ref.h:274` | 引用为 0 则失败 |
| `folio_mapcount` | `mm.h:1947` | 用户页表映射次数 |
| `memblock_phys_alloc_range` | `mm/memblock.c:1624` | 早期物理分配 |
| `memblock_discard` | `memblock.h:119` 声明；`CONFIG_ARCH_KEEP_MEMBLOCK` 时 `:123` 是空的 inline | buddy 接盘后丢掉 memblock（未 KEEP 时） |
| `kho_preserve_folio` | `kernel/liveupdate/kexec_handover.c:1147` | KHO 保住一个 folio |

---

[上一节 总览](./01-overview.md) · [目录](./README.md) · [下一节 页分配](./03-page-alloc/README.md)
