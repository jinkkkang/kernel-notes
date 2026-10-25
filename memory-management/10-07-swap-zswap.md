# 10.7 swap 与 zswap

**结论**：没有 swap 设备时，匿名页的回收只能丢掉 `MADV_FREE` 的页，或者什么都不做。有 swap 时，`shrink_folio_list` 经 `add_to_swap` 拿到一个槽，PTE 变成 swap PTE，缺页走 `do_swap_page`。槽的使用计数在 `swap_info_struct->swap_map[]` 的一个字节里。SSD 上这些字节按 `swap_cluster_info` 分簇加锁，一簇 `SWAPFILE_CLUSTER` 页。zswap 在写盘之前把页压进 zpool，默认压缩算法是 lzo，默认分配器是 zbud。

> 槽的结构在 `include/linux/swap.h`。分配和释放在 `mm/swapfile.c`。换出入口在 `mm/page_io.c`。压缩在 `mm/zswap.c`。
> 导航：[上一篇 对照](./10-06-lru-vs-mglru.md) · [本目录](./10-00-reclaim.md) · [下一篇 OOM](./10-08-oom.md)

---

## 没有 swap 时

`can_reclaim_anon_pages`（`mm/vmscan.c:605`）在 swap 页大于 0，或者能降级到更慢的节点时返回真。`get_swappiness`（`:3290`）在 `may_swap` 为假，或者 swap 页少于 `MIN_LRU_BATCH` 且不能降级时返回 0。两边都不行，经典路径 `get_scan_count` 走 `SCAN_FILE`，MGLRU 的 swappiness 变成 0，只扫文件。

`MADV_FREE` 的匿名页可以丢掉内容。`shrink_folio_list` 里 `folio_test_anon && !folio_test_swapbacked` 时直接计 `PGLAZYFREED` 并放掉（`vmscan.c:2096-2109`）。

在 6.12 上采样：`SwapTotal` 为 0，`pswpin` / `pswpout` 为 0，`Zswap` 0，`Zswapped` 0。`AnonPages` 大约 1.8GB，这些匿名页当时没有 swap 可去。

---

## 有 swap 时的路径

```text
shrink_folio_list                         # vmscan.c:1712
  add_to_swap                             # swap_state.c:176
    folio_alloc_swap                      # swap_slots.c:302，里面要槽
    add_to_swap_cache
    folio_mark_dirty                      # MADV_FREE 的脏位可能和 SwapBacked 不一致
  try_to_unmap                            # PTE 变成 swap PTE
  pageout                                 # vmscan.c:1312
    mapping->a_ops->writepage
      swap_writepage                      # page_io.c:179
        zswap_store 成功：不再写盘       # :198
        否则 __swap_writepage

换入
  do_swap_page                            # memory.c:3739
    swap_readpage                         # page_io.c:493
      zswap_load 成功：标 uptodate，解锁 # :516
      否则按设备读盘
```

`swappiness` 是匿名和文件的扫描比例，不是「百分之多少内存拿去 swap」。经典路径怎么用它，见 [02](./10-02-lru-classic.md) 的 `get_scan_count`。MGLRU 下 `min_seq[anon]` 和 `min_seq[file]` 可以错开，见 [04](./10-04-mglru-design.md)。

---

## 一个槽是 `swap_map` 里的一个字节

`include/linux/swap.h:228-238`：

| 名字 | 值 | 含义 |
|---|---|---|
| `SWAP_HAS_CACHE` | `0x40` | 这一槽在 swap cache 里 |
| `COUNT_CONTINUED` | `0x80` | 引用计数溢出，后面还有续页 |
| `SWAP_MAP_MAX` | `0x3e` | 第一字节能记下的最大引用 |
| `SWAP_MAP_BAD` | `0x3f` | 坏槽 |
| `SWAP_MAP_SHMEM` | `0xbf` | shmem / tmpfs 占用 |
| `SWAP_CONT_MAX` | `0x7f` | 续页里的最大计数 |

普通引用是低 6 位的计数。计数到 `SWAP_MAP_MAX` 还要再加，就走 continuation page（`swap_count_continued`，`mm/swapfile.c:3545`）。

设备级标志在同一个头文件 `:207-222`，和槽字节不是一回事：

| 标志 | 值 | 含义 |
|---|---|---|
| `SWP_USED` | `1 << 0` | `swap_info[]` 这一项在用 |
| `SWP_WRITEOK` | `1 << 1` | 可以写 |
| `SWP_DISCARDABLE` | `1 << 2` | 块设备支持 discard |
| `SWP_DISCARDING` | `1 << 3` | 正在 discard 一个空簇 |
| `SWP_SOLIDSTATE` | `1 << 4` | 寻道便宜，走簇锁 |
| `SWP_CONTINUED` | `1 << 5` | `swap_map` 有续页 |
| `SWP_BLKDEV` | `1 << 6` | 是块设备 |
| `SWP_ACTIVATED` | `1 << 7` | `swap_activate` 成功 |
| `SWP_FS_OPS` | `1 << 8` | 读写走文件系统 |
| `SWP_AREA_DISCARD` | `1 << 9` | 整块 swap 区只 discard 一次 |
| `SWP_PAGE_DISCARD` | `1 << 10` | 释放时按簇 discard |
| `SWP_STABLE_WRITES` | `1 << 11` | 不覆盖还在 `PG_writeback` 的页 |
| `SWP_SYNCHRONOUS_IO` | `1 << 12` | 同步 IO 便宜 |
| `SWP_SCANNING` | `1 << 14` | `scan_swap_map` 期间的引用 |

---

## `struct swap_cluster_info`（`include/linux/swap.h:250`）

一簇是磁盘上连续的 `SWAPFILE_CLUSTER` 页，自然对齐。空簇串成链表，分配时从链表上取一簇（`:240-244`）。

```c
struct swap_cluster_info {
	spinlock_t lock;	/*
				 * 保护本结构的字段，以及
				 * swap_map 里属于这一簇的那些字节。
				 */
	unsigned int data:24;	/* 空簇：下一簇下标。在用：这一簇里已分配的页数 */
	unsigned int flags:8;	/* 见下面的 CLUSTER_FLAG_* */
};
```

头上的注释（`:246-248`）还写：`data` / `flags` 的「空闲链表」用法由 `swap_info_struct.lock` 保护。簇自己的 `lock` 则保护簇字段和对应的 `swap_map` 字节。两把锁各管一段，见下面「谁拿哪把锁」。

```c
#define CLUSTER_FLAG_FREE 1		/* 这一簇是空的 */
#define CLUSTER_FLAG_NEXT_NULL 2	/* 没有下一簇 */
#define CLUSTER_FLAG_HUGE 4		/* 这一簇整个给一个透明大页 */
```

`SWAPFILE_CLUSTER` 在 `mm/swapfile.c:272-277`：编了 `CONFIG_THP_SWAP` 时等于 `HPAGE_PMD_NR`，否则是 256。

旁边两个小结构：

```c
struct percpu_cluster {
	struct swap_cluster_info index;	/* 这个 CPU 当前用的簇 */
	unsigned int next;		/* 下一次分配大概从簇内哪里开始 */
};

struct swap_cluster_list {
	struct swap_cluster_info head;	/* 空簇链表头。data 存第一簇下标 */
	struct swap_cluster_info tail;
};
```

`cluster_info` 只在非旋转设备上分配。`swapon` 看到 `bdev_nonrot` 就置 `SWP_SOLIDSTATE`，按 `DIV_ROUND_UP(maxpages, SWAPFILE_CLUSTER)` 个簇 `kvcalloc`，并为每个簇 `spin_lock_init`（`swapfile.c:3095-3123`）。旋转设备不分配 `cluster_info`，`nr_rotate_swap` 加一（`:3136`）。

---

## `struct swap_info_struct`（`include/linux/swap.h:282`）

一块 swap 设备或一个 swap 文件在内存里的记账。

```c
struct swap_info_struct {
	/* --- 身份 --- */
	struct percpu_ref users;		/* 设备还在，并挡住 swapoff */
	unsigned long	flags;			/* SWP_USED 等，见上表 */
	signed short	prio;			/* 优先级 */
	struct plist_node list;			/* 挂在 swap_active_head */
	signed char	type;			/* swap 类型下标 */
	unsigned int	max;			/* swap_map 的长度 */

	/* --- 槽 --- */
	unsigned char *swap_map;		/* vmalloc 出来的每槽一字节 */
	struct swap_cluster_info *cluster_info; /* 只有 SSD 才有 */
	struct swap_cluster_list free_clusters;	/* 空簇链表 */
	unsigned int lowest_bit;		/* swap_map 里第一块空闲 */
	unsigned int highest_bit;		/* 最后一块空闲 */
	unsigned int pages;			/* 可用页总数 */
	unsigned int inuse_pages;		/* 其中正在用的 */

	/* --- 顺序分配的游标 --- */
	unsigned int cluster_next;		/* 下一次大概从哪一页开始 */
	unsigned int cluster_nr;		/* 还要在当前簇里找多少页 */
	unsigned int __percpu *cluster_next_cpu; /* SSD：每个 CPU 自己的起点 */
	struct percpu_cluster __percpu *percpu_cluster;

	/* --- 很少在热路径上碰的 --- */
	struct rb_root swap_extent_root;	/* swap 文件的区段 */
	struct block_device *bdev;		/* 设备，或 swap 文件所在的块设备 */
	struct file *swap_file;
	unsigned int old_block_size;
	struct completion comp;

	/*
	 * 保护扫描相关的字段：swap_map、lowest_bit、highest_bit、
	 * inuse_pages、cluster_next、cluster_nr、free/discard 簇链表。
	 * 其它字段只在 swapon/swapoff 改，由 swap_lock 保护。
	 * 改 flags 要同时拿这把锁和 swap_lock；两把都要时先拿 swap_lock。
	 */
	spinlock_t lock;

	spinlock_t cont_lock;			/* 保护引用计数的续页链表 */
	struct work_struct discard_work;	/* discard 工作队列 */
	struct swap_cluster_list discard_clusters;

	/*
	 * 挂在每个 node 的 swap_avail_heads 上。
	 * 必须是最后一个成员：数组长度是 nr_node_ids，要动态分配。
	 */
	struct plist_node avail_lists[];
};
```

---

## 谁拿哪把锁

`lock_cluster`（`swapfile.c:357`）：有 `cluster_info` 时，按 `offset / SWAPFILE_CLUSTER` 找到那一簇，拿 `ci->lock`。没有 `cluster_info` 就返回 NULL，不拿锁。

`lock_cluster_or_swap_info`（`:380`）：先试簇锁；没有簇就退回 `si->lock`。注释写这是 SSD 的细粒度锁，否则用传统的粗锁。

| 路径 | 锁 |
|---|---|
| `get_swap_pages`（`:1047`） | 先拿 `swap_avail_lock`，再拿 `si->lock`，然后 `scan_swap_map_slots`（`:799`）。注释（`:1108`）写扫描过程中可能暂时放下 `si->lock` |
| `scan_swap_map_slots` | SSD 用 per-CPU 的扫描起点，减少簇锁和 swap cache 的竞争；HDD 更在意顺序（`:823-826`）。注释（`:812-819`）写先按簇顺序分配，凑满 `SWAPFILE_CLUSTER` 页再找新的空簇，避免槽散落在整块盘上 |
| `swap_free`（`:1333`） | `__swap_entry_free`（`:1296`）用 `lock_cluster_or_swap_info`，引用降到 0 再 `free_swap_slot` |
| `swap_entry_free`（`:1312`） | `swapcache_free_entries`（`:1416`）已经经 `swap_info_get_cont` 拿着 `si->lock`。函数自己再 `lock_cluster`，要求这一字节正好是 `SWAP_HAS_CACHE`，然后清成 0 |
| `put_swap_folio`（`:1345`） | `lock_cluster_or_swap_info`。整簇（`size == SWAPFILE_CLUSTER`）且每槽都只剩 cache 位时，放下簇锁，改拿 `si->lock`，`swap_free_cluster` |

`get_swap_pages` 一次要多页时，`entry_size == SWAPFILE_CLUSTER` 走 `swap_alloc_cluster`（`:1093-1095`），把整簇标成 `CLUSTER_FLAG_HUGE`，`swap_map` 的这一段写成 `SWAP_HAS_CACHE`。

---

## zswap

zswap 在写 swap 设备之前把页压进内存里的池。Kconfig（`mm/Kconfig`）：

| 选项 | 默认 |
|---|---|
| 压缩算法 `ZSWAP_COMPRESSOR_DEFAULT` | lzo（`ZSWAP_COMPRESSOR_DEFAULT_LZO`，`:67`） |
| 分配器 `ZSWAP_ZPOOL_DEFAULT` | zbud（`ZSWAP_ZPOOL_DEFAULT_ZBUD`，`:133`）。帮助文字写默认 zbud 是为了兼容 |
| 启动时是否打开 | `ZSWAP_DEFAULT_ON` 单独一项，命令行 `zswap.enabled=` 可以覆盖 |

可选分配器还有 z3fold 和 zsmalloc。z3fold 在 `mm/z3fold.c`，zpool 类型字符串是 `"z3fold"`（`:1410`），Kconfig 选项是 `Z3FOLD_DEPRECATED`（`mm/Kconfig:184`），choice 里的提示文字是 `z3foldi (DEPRECATED)`（`:151`）。zsmalloc 见下一节。

`zswap_store`（`mm/zswap.c:1197`）要求 folio 已锁且在 swap cache。返回值是 bool：

```text
zswap_store(folio)                        # zswap.c:1197
  大 folio：返回 false                    # 注释：Large folios aren't supported
  同一偏移上已有旧项：先从红黑树摘掉，避免写失败时旧数据盖住新数据
  zswap_enabled 为假：返回 false
  objcg 不允许 zswap：拒绝
  池满：排队 shrink_work，拒绝
  整页同一个字节：可以不压缩，length = 0，记下那个字节
  否则 zswap_pool_current_get，压缩，zpool_malloc
  成功：插进这台 swap 设备的红黑树，计 ZSWPOUT，返回 true
```

`swap_writepage`（`page_io.c:198`）看到 true 就结束写回，不再 `__swap_writepage`。

`zswap_load`（`zswap.c:1400`）同样要求 folio 已锁。树里没有这项就返回 false，`swap_readpage`（`page_io.c:516`）继续读盘。找到了就解压（或填回 same-filled 的那个字节），`swap_readpage` 标 uptodate 并解锁。

池满了就落到真正的 swap 设备。`/proc/meminfo` 的 `Zswap` 是压缩后占用，`Zswapped` 是原页大小。zswap 仍然算内存，不是磁盘。它和 memcg 的交换限额（`memory.swap.*`，见 cgroup 章）是两层账。`zswap_store` 自己的注释（`:1244-1246`）写：zswap 回收不按 cgroup 做。没有 cgroup 感知的 entry LRU 时，会按本地限额把项在全局推出去。

---

## zsmalloc

`Documentation/mm/zsmalloc.rst`：这个分配器是给 zram 设计的，内存紧时也不做高阶分配。它把若干 0 阶页串成一个 zspage，对象可以跨页。对象最大 `PAGE_SIZE`（`ZS_MAX_ALLOC_SIZE`，`mm/zsmalloc.c:128`），再大就失败。`zs_malloc` 返回的是不透明 handle，不是能直接解引用的指针；用之前要 `zs_map_object`，用完 `zs_unmap_object`。

```text
zs_malloc(pool, size, gfp)                # zsmalloc.c:1361
  size == 0 或 size > ZS_MAX_ALLOC_SIZE：ERR_PTR(-EINVAL)
  cache_alloc_handle                      # 失败：ERR_PTR(-ENOMEM)
  size += ZS_HANDLE_SIZE                  # 块里要留 handle 自己的位置
  find_get_zspage                         # 这个 size class 里还有空位的 zspage
    有：obj_malloc，fix_fullness_group
    无：alloc_zspage，再 obj_malloc，SetZsPageMovable
  返回 handle
```

zswap 默认不用 zsmalloc，要用的话在 Kconfig 里选 `ZSWAP_ZPOOL_DEFAULT_ZSMALLOC`，或用启动参数 `zswap.zpool=`。`struct zs_pool` / `struct size_class` 的成员这里不展开。handle 映射时再 `kmap` 一类的操作，文档写的是 `zs_map_object`。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `add_to_swap` | `mm/swap_state.c:176` | 分配槽、挂进 swap cache、标脏 |
| `folio_alloc_swap` | `mm/swap_slots.c:302` | 向槽缓存要一项 |
| `get_swap_pages` | `mm/swapfile.c:1047` | 从某台设备的 `swap_map` 里拿出槽 |
| `scan_swap_map_slots` | `swapfile.c:799` | 按簇或按位图找空槽 |
| `swap_alloc_cluster` | `swapfile.c:1002`（`get_swap_pages` 在 `:1094` 调用） | 整簇分配给透明大页 |
| `lock_cluster` | `:357` | 拿这一簇的锁 |
| `lock_cluster_or_swap_info` | `:380` | 有簇锁用簇锁，否则用 `si->lock` |
| `__swap_entry_free` | `:1296` | 引用减一，到 0 就释放槽 |
| `swap_free` | `:1333` | 换入或取消映射时放引用 |
| `swap_entry_free` | `:1312` | cache 位清掉，槽回到空闲 |
| `put_swap_folio` | `:1345` | folio 离开 swap cache 时放掉它的槽 |
| `swapcache_free_entries` | `:1416` | 一批槽，按设备排序后逐个 `swap_entry_free` |
| `pageout` | `mm/vmscan.c:1312` | 回收上下文写出脏页 |
| `swap_writepage` | `mm/page_io.c:179` | 换出。先试 zswap |
| `swap_readpage` | `page_io.c:493` | 换入。先试 zswap |
| `zswap_store` | `mm/zswap.c:1197` | 压缩进 zpool。成功返回 true |
| `zswap_load` | `zswap.c:1400` | 从 zpool 解压。没有这项返回 false |
| `zs_malloc` | `mm/zsmalloc.c:1361` | 从 zsmalloc 池要一块，返回 handle |
| `do_swap_page` | `mm/memory.c:3739` | 换入缺页 |

---

[上一篇 对照](./10-06-lru-vs-mglru.md) · [本目录](./10-00-reclaim.md) · [下一篇 OOM](./10-08-oom.md)
