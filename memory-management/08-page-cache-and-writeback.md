# 8. 页缓存与回写（内存侧）

**结论**：一个文件在内存里有哪些页，由 `address_space->i_pages` 这棵 xarray 回答，里面挂的是 folio。读路径按文件内的页偏移把 folio 找出来，没有就分配并插进去，再让文件系统把磁盘内容读上来。写路径把 folio 标脏。脏页在回写完成前还占着内存，回收经常要等这次写，或者自己调 `->writepage`。

> 源码：Linux v6.6.160。结构在 `include/linux/fs.h`、`include/linux/mm_types.h`、`include/linux/writeback.h`、`include/linux/backing-dev-defs.h`。查找和缺页在 `mm/filemap.c`。脏页限速在 `mm/page-writeback.c`。回写线程在 `fs/fs-writeback.c`。
> 文件系统怎么实现 `a_ops->read_folio` / `writepages`，本章不展开。
> 导航：[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 THP](./09-thp-and-hugetlb.md)

---

## 8.1 `struct address_space`（`include/linux/fs.h:472`）

每个 inode 有一个，指针是 `inode->i_mapping`。swap cache 另有一组 `address_space`：`init_swap_address_space()`（`mm/swap_state.c:673`）只初始化 `i_pages` 和 `a_ops`，`host` 留空，并调用 `mapping_set_no_writeback_tags()`（`include/linux/pagemap.h:269`）。本章不讲 swap 项怎么编码。

源码声明顺序如下。末尾 `aligned(sizeof(long))` 让这个结构的地址按 `long` 对齐，这样 folio 里 `mapping` 指针的最低位可以单独拿来做标记（见下面的 `PAGE_MAPPING_ANON`）。`__randomize_layout` 是给 randomize_layout 插件的标记：插件打开时，编译进内核的成员物理顺序可以和下面的声明不一样。读代码按声明顺序。

```c
struct address_space {
	struct inode		*host;		/* 属主 inode。方法里用它拿文件大小、超级块 */
	struct xarray		i_pages;	/* 页缓存：index → folio。锁是 xarray 自己的 */
	struct rw_semaphore	invalidate_lock; /* 截断、打洞、失效页缓存。和 i_pages 的锁不是同一把 */
	gfp_t			gfp_mask;	/* 给这个文件分配 folio 用的 GFP */
	atomic_t		i_mmap_writable; /* 有多少个 VM_SHARED 且 VM_MAYWRITE 的映射 */
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	/*
	 * 页缓存里 PMD 大页的个数。只给非 shmem 文件用。
	 */
	atomic_t		nr_thps;
#endif
	struct rb_root_cached	i_mmap;		/* 映射了这个文件的 VMA 区间树 */
	unsigned long		nrpages;	/* 页缓存里的页数（按基页计）。由 i_pages 的锁保护 */
	pgoff_t			writeback_index; /* 循环回写的起点，避免每次都从文件头写 */
	const struct address_space_operations *a_ops; /* 文件系统方法 */
	unsigned long		flags;		/* AS_* 位，见下面的 enum */
	struct rw_semaphore	i_mmap_rwsem;	/* 保护 i_mmap 和 i_mmap_writable */
	errseq_t		wb_err;		/* 最近一次回写错误。fsync 用它向用户报告 */
	spinlock_t		private_lock;	/* 属主私用。页缓存通用代码不解释它 */
	struct list_head	private_list;	/* 属主私用 */
	void			*private_data;	/* 属主私用 */
} __attribute__((aligned(sizeof(long)))) __randomize_layout;
```

对齐注释在 `include/linux/fs.h:493`。`PAGE_MAPPING_ANON` 定义在 `include/linux/page-flags.h:656`，值是 `0x1`。`folio_test_anon()`（同文件 `:677`）看的就是 `folio->mapping` 的这一位。文件页的 `mapping` 是真实的 `address_space` 指针，最低位是 0；匿名页把 `anon_vma` 指针和这一位或在一起。地址不对齐，匿名标记会和真实指针撞车。

新建 inode 时 `inode_init_always()`（`fs/inode.c:156`）把 `a_ops` 设成 `empty_aops`，`host` 指回这个 inode，再用 `mapping_set_gfp_mask()`（`include/linux/pagemap.h:325`）把 `gfp_mask` 设成 `GFP_HIGHUSER_MOVABLE`（调用点 `fs/inode.c:211`）。文件系统随后可以改这套 GFP。

`i_mmap` 上挂的是映射了这个文件的 VMA。VMA 侧的挂钩是 `vma->shared`，见 [06](./06-mm-and-vma.md)。`mapping_writably_mapped()`（`include/linux/fs.h:569`）在 `i_mmap_writable` 大于 0 时返回真，表示有人还能把文件映射写脏。

### xarray 标记

```c
/* include/linux/fs.h:500。XA_MARK_0 的值是 0（include/linux/xarray.h:251） */
#define PAGECACHE_TAG_DIRTY	XA_MARK_0
#define PAGECACHE_TAG_WRITEBACK	XA_MARK_1
#define PAGECACHE_TAG_TOWRITE	XA_MARK_2
```

| 标记 | 值 | 含义 |
|---|---|---|
| `PAGECACHE_TAG_DIRTY` | `XA_MARK_0` | 脏 |
| `PAGECACHE_TAG_WRITEBACK` | `XA_MARK_1` | 正在回写 |
| `PAGECACHE_TAG_TOWRITE` | `XA_MARK_2` | 这一轮回写要写的 |

`mapping_tagged()`（`include/linux/fs.h:507`）问这棵树上有没有某个标记。回写和回收用它，不必自己扫全部 folio。`AS_NO_WRITEBACK_TAGS` 置位时，`mapping_use_writeback_tags()`（`include/linux/pagemap.h:274`）返回 0，这条标记路径停用。

### `mapping->flags`（`include/linux/pagemap.h:197`）

```c
enum mapping_flags {
	AS_EIO		= 0,	/* 异步写 IO 错误 */
	AS_ENOSPC	= 1,	/* 异步写遇到 ENOSPC */
	AS_MM_ALL_LOCKS	= 2,	/* 正处在 mm_take_all_locks() */
	AS_UNEVICTABLE	= 3,	/* 例如 ramdisk、SHM_LOCK。回收跳过 */
	AS_EXITING	= 4,	/* 最后一次截断进行中 */
	AS_NO_WRITEBACK_TAGS = 5, /* 不用上面的脏/回写标记 */
	AS_LARGE_FOLIO_SUPPORT = 6, /* 允许大于一页的 folio。还要 CONFIG_TRANSPARENT_HUGEPAGE */
	AS_RELEASE_ALWAYS,	/* 即使没有 private 数据也调用 ->release_folio() */
	AS_STABLE_WRITES,	/* 改 folio 内容之前必须等回写结束 */
};
```

`mapping_set_error()`（`include/linux/pagemap.h:225`）在回写失败时做三件事：写入 `wb_err`，若有 `host` 再写入超级块的 `s_wb_err`，然后把 `AS_ENOSPC` 或 `AS_EIO` 打进 `flags`。

### `address_space_operations`（`include/linux/fs.h:404`）

这是文件系统填的方法表。内存路径会调用下面这些，顺序和源码一致：

```c
struct address_space_operations {
	int (*writepage)(struct page *page, struct writeback_control *wbc); /* 写出一页。回收的 pageout 走这里 */
	int (*read_folio)(struct file *, struct folio *); /* 把一个不在内存里的 folio 读上来 */

	/* 把这个 mapping 上的一批脏页写回去 */
	int (*writepages)(struct address_space *, struct writeback_control *);

	/* 标脏。返回 true 表示这次是从干净变成脏 */
	bool (*dirty_folio)(struct address_space *, struct folio *);

	void (*readahead)(struct readahead_control *); /* 批量预读 */

	int (*write_begin)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len,
				struct page **pagep, void **fsdata); /* 锁定或分配即将写入的页 */
	int (*write_end)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata); /* 和 write_begin 成对 */

	/* FIBMAP 的旧接口。源码注释写了别用 */
	sector_t (*bmap)(struct address_space *, sector_t);
	void (*invalidate_folio) (struct folio *, size_t offset, size_t len); /* 截断时作废 folio 的一段 */
	bool (*release_folio)(struct folio *, gfp_t); /* 丢掉页缓存前，问文件系统私有数据能不能放 */
	void (*free_folio)(struct folio *folio); /* folio 离开页缓存时的通知 */
	ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *iter); /* 绕过页缓存 */
	/*
	 * 把内容迁到另一个 folio。migrate_mode 是 MIGRATE_ASYNC 时不许睡觉。
	 */
	int (*migrate_folio)(struct address_space *, struct folio *dst,
			struct folio *src, enum migrate_mode);
	int (*launder_folio)(struct folio *); /* 回收前把脏页洗干净 */
	bool (*is_partially_uptodate) (struct folio *, size_t from,
			size_t count); /* 这一段是不是已经在 folio 里 */
	void (*is_dirty_writeback) (struct folio *, bool *dirty, bool *wb); /* 文件系统自己的脏/回写状态 */
	int (*error_remove_page)(struct address_space *, struct page *); /* 硬件中毒时把页从页缓存摘掉 */

	/* swap 文件，不是普通页缓存 */
	int (*swap_activate)(struct swap_info_struct *sis, struct file *file,
				sector_t *span);
	void (*swap_deactivate)(struct file *file);
	int (*swap_rw)(struct kiocb *iocb, struct iov_iter *iter);
};
```

`empty_aops`（`include/linux/fs.h:448`）是一张空表，inode 刚造出来时先挂它。

---

## 8.2 folio 怎么进页缓存

页缓存里的每一项是 `struct folio`（`include/linux/mm_types.h:293`）。它和 `struct page` 叠在同一块内存上。页缓存用到的是第一段：`flags`、`mapping`、`index`。第二段、第三段是大页映射计数、hugetlb 和延迟拆分，布局和 [02](./02-physical-layout.md) 是同一份，这里按源码把整份列出来，读页缓存时看第一段即可。

```c
struct folio {
	/* private: 匿名 union，不当 API */
	union {
		struct {
	/* public: */
			unsigned long flags;		/* 同 page->flags */
			union {
				struct list_head lru;	/* LRU 链 */
	/* private: */
				struct {
					void *__filler;
	/* public: */
					unsigned int mlock_count; /* mlock 次数 */
	/* private: */
				};
	/* public: */
			};
			struct address_space *mapping;	/* 文件页：address_space。匿名页：带 ANON 位的指针 */
			pgoff_t index;			/* 文件内页偏移，单位是基页 */
			union {
				void *private;		/* 文件系统挂在 folio 上的私有数据 */
				swp_entry_t swap;	/* 和 private 同一块内存。在 swap cache 里时用 */
			};
			atomic_t _mapcount;
			atomic_t _refcount;
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
#endif
	/* private: */
		};
		struct page page;
	};
	/* --- 第二页槽，mm_types.h:325。order 0 的 folio 不会用到 --- */
	union {
		struct {
			unsigned long _flags_1;
			unsigned long _head_1;
			unsigned long _folio_avail;
	/* public: */
			atomic_t _entire_mapcount;
			atomic_t _nr_pages_mapped;
			atomic_t _pincount;
#ifdef CONFIG_64BIT
			unsigned int _folio_nr_pages;
#endif
	/* private: */
		};
		struct page __page_1;
	};
	/* --- 第三页槽，mm_types.h:341。hugetlb 指针和 _deferred_list 叠在这里 --- */
	union {
		struct {
			unsigned long _flags_2;
			unsigned long _head_2;
	/* public: */
			void *_hugetlb_subpool;
			void *_hugetlb_cgroup;
			void *_hugetlb_cgroup_rsvd;
			void *_hugetlb_hwpoison;
	/* private: */
		};
		struct {
			unsigned long _flags_2a;
			unsigned long _head_2a;
	/* public: */
			struct list_head _deferred_list;
	/* private: */
		};
		struct page __page_2;
	};
};
```

插进页缓存时，`__filemap_add_folio()`（`mm/filemap.c:844`）把 `folio->mapping` 设成这个 `address_space`，`folio->index` 设成文件内页偏移，再 `xas_store()` 放进 `i_pages`。同一 index 上已经有 folio 就返回 `-EEXIST`。函数开头有 `VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio)`：带 `swapbacked` 的 folio（shmem 就是这种）不走这里，走 §8.4 的 `shmem_add_to_page_cache()`。

```text
filemap_add_folio                         # filemap.c:960
  __folio_set_locked
  __filemap_add_folio                     # :844，写入 i_pages，nrpages 加上页数
       失败：清锁，返回错误。调用者丢掉自己的 folio
  成功且没有 __GFP_WRITE、又有 shadow：
       workingset_refault                 # mm/workingset.c:537
       shadow 是刚被逐出的页留下的空位。再次读到它，按“刚用过”激活
  folio_add_lru                            # mm/swap.c:493，挂上 LRU

add_to_page_cache_lru                     # mm/folio-compat.c:86
  参数是 struct page *。page_folio()（include/linux/page-flags.h:268）
  找出它所在的 folio，再调用 filemap_add_folio
```

查找：

```text
filemap_get_folio                         # include/linux/pagemap.h:648
  __filemap_get_folio(mapping, index, 0, 0)   # mm/filemap.c:1891
       filemap_get_entry                  # filemap.c:1844
            xarray 里是值（shadow 或 shmem 的 swap 项）：不当成 folio
       没有 FGP_CREAT：找不到就返回错误指针

带 FGP_CREAT 时（filemap.c:1933 起）：
  分配 folio → filemap_add_folio
  -EEXIST：别人刚插入同一个 index，回到开头再查
```

`FGP_*` 在 `include/linux/pagemap.h:599`。缺页用的是 `FGP_CREAT|FGP_FOR_MMAP`。`FGP_FOR_MMAP`（值 `0x40`）表示插入后把锁放开，因为 `filemap_fault` 自己再锁。`AS_LARGE_FOLIO_SUPPORT` 且编了 `CONFIG_TRANSPARENT_HUGEPAGE` 时，`mapping_large_folio_support()`（`include/linux/pagemap.h:370`）为真，分配的 order 可以大于 0，上限是 `MAX_PAGECACHE_ORDER`（`include/linux/pagemap.h:348`）。没开这个位，order 被压成 0，一次一页。分配函数是 `filemap_alloc_folio()`：`CONFIG_NUMA` 时在 `mm/filemap.c:989`，否则是 `include/linux/pagemap.h:545` 的内联，直接 `folio_alloc`。

读：

```text
generic_file_read_iter                    # filemap.c:2827
  IOCB_DIRECT：a_ops->direct_IO。短读且不是 DAX，剩下的落到下面
  filemap_read                              # :2655
       页缓存里有：拷给用户
       没有：filemap_create_folio           # :2516
            filemap_alloc_folio
            filemap_add_folio
            filemap_read_folio              # :2403，调用 a_ops->read_folio
```

缺页：

```text
filemap_fault                             # filemap.c:3293
  index 已经越过 i_size：VM_FAULT_SIGBUS
  filemap_get_folio 命中：
       还没试过：do_async_mmap_readahead    # :3246
       命中且页是新的：这次 fault 不记 PGMAJFAULT
  未命中：
       count_vm_event(PGMAJFAULT)，ret |= VM_FAULT_MAJOR
       do_sync_mmap_readahead              # :3177
       __filemap_get_folio(FGP_CREAT|FGP_FOR_MMAP)
  页不是 uptodate：
       filemap_read_folio → a_ops->read_folio   # :3414
  成功：vmf->page = folio_file_page(...)，返回时带 VM_FAULT_LOCKED
  中途放下 mmap 锁：VM_FAULT_RETRY，调用者重新找 VMA
```

写：

```text
generic_file_write_iter                   # filemap.c:4139
  inode_lock
  __generic_file_write_iter               # :4092
       普通缓冲写：generic_perform_write   # :3990
            a_ops->write_begin            # 拿到 page，没有就分配并插入
            从用户拷贝
            a_ops->write_end              # 文件系统在这里把页标脏
            balance_dirty_pages_ratelimited  # :4061
  inode_unlock
  写成功再 generic_write_sync             # include/linux/fs.h:2675。O_SYNC 一类在这里等回写
```

标脏的通用入口是 `folio_mark_dirty()`（`mm/page-writeback.c:2785`）。它清掉 `PG_reclaim`，再调用 `a_ops->dirty_folio`。没有 `mapping` 时走 `noop_dirty_folio()`（`:2613`）。真正记账在 `folio_account_dirtied()`（`:2628`）：`NR_FILE_DIRTY`、`NR_DIRTIED` 加上页数，回写域的 `WB_RECLAIMABLE` / `WB_DIRTIED` 也加上，`current->nr_dirtied` 累加。xarray 上的 `PAGECACHE_TAG_DIRTY` 由 `dirty_folio` 的实现去打。

一个文件页可以被多个进程的 PTE 同时映射，`i_mmap` 上有多个 VMA。`folio_mapcount()`（`include/linux/mm.h:1231`）大于 1 就是这种情况。回收要先拆掉这些 PTE，再丢页缓存。

`__filemap_add_folio` 若发现 xarray 里留着 shadow，会交给 `workingset_refault()`。那是“这页刚被逐出又被读回来”的记账。`/proc/vmstat` 里对应的 node 计数是 `workingset_refault_file`（`mm/vmstat.c:1208`）。本章不再展开 `mm/workingset.c` 里的年龄算法。

---

## 8.3 脏页、回写、回收

**结论**：限速发生在写的时候，不发生在回收里。`balance_dirty_pages()` 看全局脏页和这个回写域的份额。超过后台阈值就叫醒 flusher；超过空转上限，写进程自己睡觉。回写线程再按 `writeback_control` 调用文件系统的 `writepages`。

### 阈值从哪来

`/proc/sys/vm/` 下这几项由 `vm_page_writeback_sysctls[]`（`mm/page-writeback.c:2248`）注册，`page_writeback_init()` 在 `CONFIG_SYSCTL` 下执行 `register_sysctl_init("vm", ...)`（`:2345`）。`dirty_ratio` 和 `dirty_bytes` 写一个就把另一个清零，后台那一对也一样（`dirty_ratio_handler` `:536`，`dirty_bytes_handler` `:550`）。

| proc 名 | 变量 | 初值 | 含义 |
|---|---|---|---|
| `dirty_background_ratio` | `dirty_background_ratio`（`:74`） | 10 | 可弄脏内存的百分之多少时，开始后台回写 |
| `dirty_background_bytes` | `dirty_background_bytes`（`:80`） | 0 | 非 0 时改用字节数，ratio 被清零 |
| `dirty_ratio` | `vm_dirty_ratio`（`:91`） | 20 | 写进程开始被限速的比例 |
| `dirty_bytes` | `vm_dirty_bytes`（`:97`） | 0 | 非 0 时改用字节数。最小 `2 * PAGE_SIZE`（`:2246`） |
| `dirty_writeback_centisecs` | `dirty_writeback_interval`（`:102`） | `5 * 100` | 周期回写间隔，单位是百分之一秒。0 表示关掉周期回写 |
| `dirty_expire_centisecs` | `dirty_expire_interval`（`:109`） | `30 * 100` | 脏数据最多放多久，kupdate 用来算 `dirtied_before` |
| `laptop_mode` | `laptop_mode`（`:115`） | 0 | 非 0 时，后台回写推迟到碰到更高的阈值 |
| `highmem_is_dirtyable` | `vm_highmem_is_dirtyable` | 0 | 仅 `CONFIG_HIGHMEM`。为真时 highmem 也算进可弄脏内存 |

`domain_dirty_limits()`（`:375`）把比例或字节数换成页数，写进 `dirty_throttle_control` 的 `thresh` 和 `bg_thresh`。实时任务（`rt_task`）再把这两个值抬高四分之一。`bg_thresh` 若不低于 `thresh`，被收成 `thresh / 2`（`:430`）。`global_dirty_limits()`（`:448`）对全局域做同一次计算。`/proc/vmstat` 里的 `nr_dirty_threshold` 和 `nr_dirty_background_threshold` 就是这次算出来的两个数（`include/linux/vmstat.h:37`，填数在 `mm/vmstat.c:1821`）。

每个回写域还有自己的份额 `wb_thresh`。`__wb_calc_thresh()`（`mm/page-writeback.c:883`）按这个域最近写完的页占全局的比例，从 `thresh` 里切一块，再用 bdi 的 min/max ratio 夹住。对外的包装是 `wb_calc_thresh()`（`:910`）。

```c
/* mm/page-writeback.c:124。balance_dirty_pages 和它的子程序共用这一包参数 */
struct dirty_throttle_control {
#ifdef CONFIG_CGROUP_WRITEBACK
	struct wb_domain	*dom;	/* 这个域。全局是 global_wb_domain（:121） */
	struct dirty_throttle_control *gdtc; /* 只有 memcg 的 dtc 指向全局那份 */
#endif
	struct bdi_writeback	*wb;	/* 正在限速的回写域 */
	struct fprop_local_percpu *wb_completions; /* 用来算这个域写完了多少 */

	unsigned long		avail;		/* 可弄脏的页数 */
	unsigned long		dirty;		/* 脏页 + 正在回写的页 */
	unsigned long		thresh;		/* dirty_ratio / dirty_bytes 算出的上限 */
	unsigned long		bg_thresh;	/* 后台回写阈值 */

	unsigned long		wb_dirty;	/* 这个 wb 自己的脏 + 回写 */
	unsigned long		wb_thresh;	/* 这个 wb 分到的上限 */
	unsigned long		wb_bg_thresh;

	unsigned long		pos_ratio;	/* 把任务的脏页速率往目标上拉的系数 */
};
```

### 写进程怎么被限速

```text
balance_dirty_pages_ratelimited          # page-writeback.c:2084
  balance_dirty_pages_ratelimited_flags(mapping, 0)   # :2014
       bdi 没有 BDI_CAP_WRITEBACK：直接返回
       开了 cgroup writeback：wb_get_create_current，否则用 bdi->wb
       current->nr_dirtied 还没到 nr_dirtied_pause：先返回，少做几次检查
       到了：balance_dirty_pages(wb, nr_dirtied, flags)  # :1691

balance_dirty_pages
  gdtc->dirty = NR_FILE_DIRTY + NR_WRITEBACK
  domain_dirty_limits(gdtc)
  非 laptop_mode，且可回收脏页 > bg_thresh，且这个 wb 还没在回写：
       wb_start_background_writeback     # fs/fs-writeback.c:1320，只是叫醒线程
  dirty <= dirty_freerun_ceiling(thresh, bg_thresh)：
       空转。记下 nr_dirtied_pause，本次不睡觉
  否则写进程在这个循环里睡觉，等脏页降下来
```

`dirty_freerun_ceiling()`（`mm/page-writeback.c:835`）就是 `(thresh + bg_thresh) / 2`。注释写明：写进程是在越过这个中点之后才被强制等待（`mm/page-writeback.c:1687`）。`thresh` 是硬上限那一侧，不是“一超过 dirty_ratio 立刻 sleep”的那一条线。bdi 带 `BDI_CAP_STRICTLIMIT` 时，比较用的是 `wb_dirty` / `wb_thresh`，不是全局数。编了 `CONFIG_CGROUP_WRITEBACK` 时，memcg 域再算一遍，两边都要在空转上限之下才放行。

### 回写域和回写线程

`struct bdi_writeback`（`include/linux/backing-dev-defs.h:105`）是一个设备（或一个 memcg 和设备的组合）上的回写上下文。`backing_dev_info` 里嵌着根 wb（`include/linux/backing-dev-defs.h:190` 的 `wb`）。本章只讲内存侧：脏页记账、谁该被叫醒、一次回写要写多少页。inode 怎么进这些链表是 VFS 的事，这里把链表当成队列。

```c
struct bdi_writeback {
	/* --- 身份和状态 --- */
	struct backing_dev_info *bdi;	/* 父设备 */
	unsigned long state;		/* WB_registered 等，用原子位操作 */
	unsigned long last_old_flush;	/* 上次把旧数据刷出去的时间 */

	/* --- 回写队列。链上是 inode，本章不展开 inode --- */
	struct list_head b_dirty;	/* 脏 inode */
	struct list_head b_io;		/* 这一轮正在写 */
	struct list_head b_more_io;	/* 这一轮没写完，下一轮再来 */
	struct list_head b_dirty_time;	/* 只有时间戳脏了 */
	spinlock_t list_lock;		/* 保护上面四条链 */

	atomic_t writeback_inodes;	/* 正处在回写中的 inode 数 */
	struct percpu_counter stat[NR_WB_STAT_ITEMS]; /* WB_RECLAIMABLE、WB_WRITEBACK、WB_DIRTIED、WB_WRITTEN */

	/* --- 带宽，用来算这个域该占多少脏页额度 --- */
	unsigned long bw_time_stamp;
	unsigned long dirtied_stamp;
	unsigned long written_stamp;
	unsigned long write_bandwidth;
	unsigned long avg_write_bandwidth; /* 平滑后的写带宽，保证大于 0 */

	unsigned long dirty_ratelimit;		/* 任务脏页速率的基线 */
	unsigned long balanced_dirty_ratelimit;	/* 估计的平衡速率 */

	struct fprop_local_percpu completions; /* 这个 wb 写完的页，用来切 wb_thresh */
	int dirty_exceeded;		/* 已经越过本域上限。限速会把检查间隔缩短 */
	enum wb_reason start_all_reason;

	spinlock_t work_lock;		/* 保护 work_list 和 dwork 的调度 */
	struct list_head work_list;
	struct delayed_work dwork;	/* 回写工作项 */
	struct delayed_work bw_dwork;	/* 估带宽 */

	struct list_head bdi_node;	/* 挂在 bdi->wb_list 上 */

#ifdef CONFIG_CGROUP_WRITEBACK
	/* --- 非根 wb：一个 memcg 和这个 bdi 的组合。根 wb 不用 refcnt --- */
	struct percpu_ref refcnt;
	struct fprop_local_percpu memcg_completions;
	struct cgroup_subsys_state *memcg_css;
	struct cgroup_subsys_state *blkcg_css;
	struct list_head memcg_node;
	struct list_head blkcg_node;
	struct list_head b_attached;	/* 挂在这个 wb 上的 inode，list_lock 保护 */
	struct list_head offline_node;
	struct work_struct switch_work;
	struct llist_head switch_wbs_ctxs;
	union {
		struct work_struct release_work;
		struct rcu_head rcu;
	};
#endif
};
```

`enum wb_reason`（`include/linux/backing-dev-defs.h:44`）说明这次回写是谁叫醒的：

| 枚举 | 谁 |
|---|---|
| `WB_REASON_BACKGROUND` | 越过后台阈值 |
| `WB_REASON_VMSCAN` | 回收看见一串还没进回写队列的脏页 |
| `WB_REASON_SYNC` | sync |
| `WB_REASON_PERIODIC` | `dirty_writeback_centisecs` 到点 |
| `WB_REASON_LAPTOP_TIMER` | laptop mode 定时器 |
| `WB_REASON_FS_FREE_SPACE` | 文件系统要腾空间 |
| `WB_REASON_FORKER_THREAD` | 名字留下给用户态 tracepoint，注释写明已经没有 bdi forker 线程 |
| `WB_REASON_FOREIGN_FLUSH` | cgroup 回写里的外来 inode |

一次回写要写多少、等不等，由栈上的 `struct writeback_control` 描述（`include/linux/writeback.h:42`）。没填的字段保持 0。

```c
struct writeback_control {
	long nr_to_write;		/* 还要写多少页。每写一页减一 */
	long pages_skipped;		/* 没写成的页 */

	/*
	 * a_ops->writepages 用。start 或 end 非 0 时，提示只写这个字节范围。
	 * end 那个字节包括在内。
	 */
	loff_t range_start;
	loff_t range_end;

	enum writeback_sync_modes sync_mode; /* WB_SYNC_NONE 不等；WB_SYNC_ALL 每个 mapping 都等 */

	unsigned for_kupdate:1;		/* 周期回写，按 dirty_expire 挑旧页 */
	unsigned for_background:1;	/* 后台回写，降到 bg_thresh 以下就停 */
	unsigned tagged_writepages:1;	/* 先打 TOWRITE 标记再写，避免活锁 */
	unsigned for_reclaim:1;		/* 从回收来的。shmem 只在这时才换出 */
	unsigned range_cyclic:1;	/* 从 writeback_index 起循环 */
	unsigned for_sync:1;		/* sync(2) 的 WB_SYNC_ALL */
	unsigned unpinned_fscache_wb:1;

	unsigned no_cgroup_owner:1;	/* 异步层后面的阶段不再做 cgroup 归属 */

	struct swap_iocb **swap_plug;	/* 非块设备的 swap 写，用来攒一批再提交 */

#ifdef CONFIG_CGROUP_WRITEBACK
	struct bdi_writeback *wb;	/* 这次回写挂在哪个 wb 上 */
	struct inode *inode;		/* 正在写的 inode。本章不展开 VFS */
	int wb_id;
	int wb_lcand_id;
	int wb_tcand_id;
	size_t wb_bytes;
	size_t wb_lcand_bytes;
	size_t wb_tcand_bytes;
#endif
};
```

```text
wakeup_flusher_threads(reason)            # fs/fs-writeback.c:2405
  先把当前任务 plug 住的 IO 交出去
  每个有脏 IO 的 bdi：
       wb_start_writeback(wb, reason)     # :1289
            置 WB_start_all，再 wb_wakeup（:136）。同一时刻只留一个“全部写完”的工作

后台阈值那条是另一条叫醒：
  wb_start_background_writeback           # :1320
       只 wb_wakeup，不置 WB_start_all

flusher 真正干活：
  wb_writeback                            # :2115
       for_background：wb_over_bg_thresh 为假就停   # page-writeback.c:2099
       for_kupdate：dirtied_before = jiffies - dirty_expire_interval 换成的 jiffies
       b_io 空了就把到期的 inode 搬进来
       再对每个 mapping 调用 a_ops->writepages，带上 writeback_control
```

`wb_writeback` 是 `static`。内存侧要记住的是：后台回写写到低于 `bg_thresh` 为止；kupdate 只写“放得够久”的脏页；`nr_pages` 用完了也停。

### 回收看见脏页

`shrink_folio_list()`（`mm/vmscan.c:1712`）处理一批从 LRU 上摘下来的 folio。

```text
folio_check_dirty_writeback               # vmscan.c:1589
  文件系统若实现了 a_ops->is_dirty_writeback，用它，避免只看 xarray 标记看漏
folio 正在 writeback（PG_writeback）：
  直接回收、kswapd 都可能在这里选择激活或等待，不能立刻当空闲页
  注释分三种情况，见 :1784
folio 仍是脏的（:1981）：
  文件页：普通直接回收不自己写，只打 reclaim 标记然后放回
  kswapd，且这页已经带 reclaim，且节点标了 PGDAT_DIRTY，并且 GFP 允许进文件系统：
       pageout                            # :1312
            a_ops->writepage == NULL：PAGE_ACTIVATE，激活，不写
            构造 writeback_control：WB_SYNC_NONE，for_reclaim = 1
            mapping->a_ops->writepage(&folio->page, &wbc)
            成功：NR_VMSCAN_WRITE（:1375），/proc/vmstat 里叫 nr_vmscan_write
整批扫完若全是还没进回写队列的脏页：
  wakeup_flusher_threads(WB_REASON_VMSCAN)  # vmscan.c:2633
```

`pageout` 先问 `is_page_cache_freeable()`（`mm/vmscan.c:1136`）。引用还多（别人正 pin 着）就返回 `PAGE_KEEP`，不写。写不走的页留在 LRU 上等 flusher。

memcg 有自己的回写域，字段在上面 `bdi_writeback` 的 `CONFIG_CGROUP_WRITEBACK` 段。cgroup 怎么选 wb，见 [cgroup/06-memory.md](../cgroup/06-memory.md)。

### 在 6.12 上采样

下面这组数是作者在 6.12.94+ 主机上读 `/proc/meminfo` 和 `/proc/vmstat` 得到的，原样留下。v6.6.160 里这些名字都在：`Dirty` 由 `NR_FILE_DIRTY` 打出来（`fs/proc/meminfo.c:98`），`Writeback` 由 `NR_WRITEBACK` 打出来（`:100`）；`nr_dirty`、`nr_writeback`、`nr_dirtied`、`nr_written`、`nr_vmscan_write` 在 `mm/vmstat.c:1218` 起；`pgsteal_file` 在 `:1288`，属于 `CONFIG_VM_EVENT_COUNTERS` 的事件计数。

| 项 | 在 6.12 上采样 | 读法 |
|---|---|---|
| `Dirty` | 764 kB | 采样当时文件脏页很少 |
| `Writeback` | 0 | 没有正在写回的页 |
| `nr_dirty` | 191 页 | 和 Dirty 同一量级。按采样机 4KB 基页，191×4KB 约 764KB |
| `nr_dirty_threshold` | 166635 页 | `global_dirty_limits` 的 thresh |
| `nr_dirty_background_threshold` | 83216 页 | 同一次计算的 bg_thresh |
| `nr_dirtied` / `nr_written` | 2389870 / 2112397 | 启动以来的累计，不是当前值 |
| `nr_vmscan_write` | 0 | 回收路径自己写出的页 |
| `pgsteal_file` / `pgsteal_anon` | 1629411 / 0 | 回收丢掉的文件页 / 匿名页 |

阈值按可弄脏内存的比例算。`MemAvailable` 下降时阈值跟着降。采样机上脏页只有几百 KB，离 `nr_dirty_threshold` 很远。没有 swap 时匿名页换不走，见 [10-07-swap-zswap.md](./10-07-swap-zswap.md)。

---

## 8.4 shmem：匿名，又能 swap 的页缓存

**结论**：tmpfs、SysV shm、`memfd_create`（没带 `MFD_HUGETLB`）、以及 `MAP_SHARED` 且没有文件的匿名映射，在内核里都是 shmem。用户看它像没有磁盘文件的共享内存；内核看它是页缓存，folio 挂在 `address_space` 上，内存紧了可以由回收换成 swap。换出之后 xarray 里留下 swap 项，不是空洞。挂载选项见 `Documentation/mm/shmfs.rst`。

`get_unmapped_area()`（`mm/mmap.c:1820`）在 `MAP_SHARED` 且没有 `struct file` 时，把选地址函数换成 `shmem_get_unmapped_area`（赋值在 `:1845`）。上面的注释（`:1840`）写明后面会建文件。`mmap_region()`（`:2925`）调用 `__mmap_region()`（`:2687`，调用点 `:2949`）。`__mmap_region` 看见 `VM_SHARED` 又没有文件，调用 `shmem_zero_setup()`（`:2842`，实现在 `mm/shmem.c:4874`）。后者用 `shmem_kernel_file_setup("dev/zero", ...)`（`:4839`）造一个内部 shmem 文件，挂到 `vma->vm_file`，`vm_ops` 换成 `shmem_anon_vm_ops`（定义在 `mm/shmem.c:4581`）。从这一刻起缺页走 `shmem_fault`，不再走 `do_anonymous_page()`（`mm/memory.c:4104`）。

`!CONFIG_SHMEM` 时，`shmem_get_unmapped_area` 只是转回 `current->mm->get_unmapped_area`（`mm/shmem.c:4761`）。下面的 inode 布局和缺页路径都在 `CONFIG_SHMEM` 打开的实现里，`shmem_get_unmapped_area` 的完整版从 `mm/shmem.c:2257` 开始。

### `struct shmem_inode_info`（`include/linux/shmem_fs.h:20`）

它嵌在 inode 里。`SHMEM_I(inode)`（`:74`）用 `container_of` 从 `vfs_inode` 反查。

```c
struct shmem_inode_info {
	spinlock_t		lock;		/* 保护 alloced、swapped 等 */
	unsigned int		seals;		/* F_SEAL_*。memfd 用 */
	unsigned long		flags;		/* 只保存 VM_NORESERVE；shmem_lock 再或上 VM_LOCKED */
	unsigned long		alloced;	/* 记在这个文件上的数据页 */
	unsigned long		swapped;	/* 其中正在 swap 上的页 */
	pgoff_t			fallocend;	/* fallocate 到过的最大下标 */
	struct list_head	shrinklist;	/* 可收缩的大页 inode，挂到超级块的 shrinklist */
	struct list_head	swaplist;	/* 可能在 swap 上的 inode 链 */
	struct shared_policy	policy;		/* NUMA 策略。CONFIG_NUMA 时是树，否则是空结构 */
	struct simple_xattrs	xattrs;		/* xattr 的红黑树，不是一条 list_head */
	atomic_t		stop_eviction;	/* 非 0 时先别把这个 inode 换出去 */
	struct timespec64	i_crtime;	/* 创建时间 */
	unsigned int		fsflags;	/* FS_IOC_GETFLAGS / SETFLAGS */
#ifdef CONFIG_TMPFS_QUOTA
	struct dquot __rcu	*i_dquot[MAXQUOTAS]; /* 配额。MAXQUOTAS 在 include/uapi/linux/quota.h:40 */
#endif
	struct offset_ctx	dir_offsets;	/* 目录项的稳定偏移。和 shrinklist 同时存在，不是同一块 union */
	struct inode		vfs_inode;	/* 嵌进去的 VFS inode */
};
```

`__shmem_get_inode()`（`mm/shmem.c:2451`）把 `seals` 初始化成 `F_SEAL_SEAL`，`flags` 只留下 `VM_NORESERVE`（`:2482`）。`shmem_get_inode()` 是它的包装（开了 `CONFIG_TMPFS_QUOTA` 时在 `:2532`，否则在 `:2560`）。`shmem_lock()`（`:2364`）加锁时执行 `info->flags |= VM_LOCKED`（`:2378`）。`SHMEM_I(inode)->flags & VM_LOCKED` 为真时，`shmem_writepage()` 拒绝换出（`:1463`）。

`struct shared_policy` 在 `CONFIG_NUMA` 下是 `rb_root` 加一把 `rwlock`（`include/linux/mempolicy.h:122`），否则是空结构（`:208`）。策略怎么用见 [03-08](./03-08-numa-policy.md)。

超级块侧是 `struct shmem_sb_info`（`include/linux/shmem_fs.h:53`）：

```c
struct shmem_sb_info {
	unsigned long max_blocks;	/* 允许的块数 */
	struct percpu_counter used_blocks; /* 已经分配的块 */
	unsigned long max_inodes;	/* 允许的 inode 数 */
	unsigned long free_ispace;	/* 还剩多少 inode 空间 */
	raw_spinlock_t stat_lock;	/* 串行化本结构的修改 */
	umode_t mode;			/* 根目录的挂载 mode */
	unsigned char huge;		/* 要不要试大页。取值 SHMEM_HUGE_* */
	kuid_t uid;
	kgid_t gid;
	bool full_inums;		/* i_ino 用满宽还是 ino_t */
	bool noswap;			/* 为真：忽略回收 / swap 请求 */
	ino_t next_ino;
	ino_t __percpu *ino_batch;
	struct mempolicy *mpol;		/* 这个挂载的默认 NUMA 策略 */
	spinlock_t shrinklist_lock;	/* 保护下面的 shrinklist */
	struct list_head shrinklist;	/* 可收缩的大页 inode */
	unsigned long shrinklist_len;
	struct shmem_quota_limits qlimits; /* 默认配额上限 */
};
```

挂载参数 `noswap` 把 `sbinfo->noswap` 设为真（`mm/shmem.c:4211`）。建 inode 时若已经是 `noswap`，会 `mapping_set_unevictable()`（`:2492`），回收不再挑它。`shmem_writepage()` 见到 `sbinfo->noswap` 同样直接 redirty（`:1463`）。

`shmem_aops`（`mm/shmem.c:4475`）的 `writepage` 是 `shmem_writepage`，`dirty_folio` 是 `noop_dirty_folio`。编了 `CONFIG_TMPFS` 才有 `write_begin` / `write_end`。

### 缺页

`shmem_fault()`（`mm/shmem.c:2180`）处理正在打洞的窗口之后，调用 `shmem_get_folio_gfp()`（`:2248`，函数在 `:1948`）。`shmem_get_folio()`（`:2161`）是不带 vma 的包装，GFP 用 `mapping_gfp_mask`。

`enum sgp_type`（`include/linux/shmem_fs.h:136`）告诉这次查找能不能分配、能不能越过 `i_size`：

| 值 | 行为 |
|---|---|
| `SGP_READ` | 不超过 `i_size`。洞上不分配，返回空 folio |
| `SGP_NOALLOC` | 洞上失败 |
| `SGP_CACHE` | 不超过 `i_size`，可以分配 |
| `SGP_WRITE` | 可以越过 `i_size`，新页不必先标成 uptodate |
| `SGP_FALLOC` | 和 `SGP_WRITE` 一样分配，但会把已有页清成 uptodate |

缺页传入的是 `SGP_CACHE`。`sgp <= SGP_CACHE` 且下标已经超出 `i_size` 时，`shmem_get_folio_gfp` 直接 `-EINVAL`（`:1967`）。

```text
shmem_get_folio_gfp                         # mm/shmem.c:1948
  filemap_get_entry(mapping, index)         # 页缓存里找
  userfaultfd minor 且找到的不是 swap 值：
       handle_userfault(VM_UFFD_MINOR)，不在这里装 PTE
  xarray 里是值（swap 项）：
       shmem_swapin_folio                   # :1834
            换入后 folio_mark_dirty
            注释（:1941）：一页不能同时活在 swap 和页缓存里，所以换入就标脏并释放 swap
  已经是 folio：锁上，核对 mapping 没被截断
  没有，且 SGP_READ：返回空 folio
  没有，且 userfaultfd missing：把缺页交给用户态
  否则 shmem_alloc_and_acct_folio           # :1685
       大页失败再按 order 0 分
       shmem_add_to_page_cache              # :765
       folio_add_lru
       shmem_recalc_inode(inode, 页数, 0)   # :417，alloced 增加
  新分配的页不在这里标脏。写路径自己标
```

换出是 `shmem_writepage()`（`:1443`）。它要求 `wbc->for_reclaim`，普通 flusher 和 sync 不应走到这里。通过检查之后 `folio_alloc_swap()`（`mm/swap_slots.c:302`）拿一个 swap 项，`shmem_recalc_inode(inode, 0, 1)` 让 `swapped` 加一（`:1536`），再 `shmem_delete_from_page_cache()`（`:834`）把 xarray 里的 folio 换成 swap 项，最后 `swap_writepage()`（`mm/page_io.c:179`）。`VM_LOCKED` 或 `noswap` 则 `folio_mark_dirty` 之后返回 `AOP_WRITEPAGE_ACTIVATE`。

`/proc/meminfo` 的 `Shmem` 来自共享内存页的统计。换出的页记在 swap 上，不再算在这份还在内存里的 shmem 里。`/proc/vmstat` 的 `nr_shmem` 对应 `NR_SHMEM`（`mm/vmstat.c:1221`）。

### memfd

`memfd_create`（`mm/memfd.c:294`，`SYSCALL_DEFINE2`）造一个没有目录项的文件。没带 `MFD_HUGETLB` 时调用 `shmem_file_setup(name, 0, VM_NORESERVE)`（`:363`，实现 `mm/shmem.c:4850`），之后的缺页和换出就是上面的 shmem。带 `MFD_HUGETLB` 时走 `hugetlb_file_setup()`（`fs/hugetlbfs/inode.c:1632`），那是 hugetlb 池，见 [09](./09-thp-and-hugetlb.md)。

标志在 `include/uapi/linux/memfd.h`：`MFD_CLOEXEC`、`MFD_ALLOW_SEALING`、`MFD_HUGETLB`、`MFD_NOEXEC_SEAL`、`MFD_EXEC`。inode 刚造出来时 `seals` 是 `F_SEAL_SEAL`（`include/uapi/linux/fcntl.h:41`），表示不许再加别的封印。`MFD_ALLOW_SEALING` 或 `MFD_NOEXEC_SEAL` 会清掉这一位（`mm/memfd.c:371`），之后才能加 `F_SEAL_WRITE`、`F_SEAL_SHRINK`、`F_SEAL_GROW` 等。封印存在 `shmem_inode_info.seals` 里，不改变 `shmem_fault` 的查找顺序。

---

## 8.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `inode_init_always` | `fs/inode.c:148` | 新建 inode 时初始化 `address_space` |
| `mapping_set_gfp_mask` | `include/linux/pagemap.h:325` | 设置 `gfp_mask`。调用点 `fs/inode.c:211` |
| `mapping_writably_mapped` | `include/linux/fs.h:569` | `i_mmap_writable > 0` 时还有可写共享映射 |
| `mapping_tagged` | `include/linux/fs.h:507` | xarray 上有没有脏/回写/待写标记 |
| `mapping_set_error` | `include/linux/pagemap.h:225` | 回写错误写入 `wb_err` 和 `AS_EIO` / `AS_ENOSPC` |
| `init_swap_address_space` | `mm/swap_state.c:673` | 给 swap cache 建 `address_space`，`host` 为空 |
| `mapping_set_no_writeback_tags` | `include/linux/pagemap.h:269` | 置 `AS_NO_WRITEBACK_TAGS`。swap cache 会调用它 |
| `mapping_use_writeback_tags` | `include/linux/pagemap.h:274` | `AS_NO_WRITEBACK_TAGS` 是否关掉标记 |
| `mapping_large_folio_support` | `include/linux/pagemap.h:370` | 这个 mapping 能不能放大于一页的 folio |
| `folio_test_anon` | `include/linux/page-flags.h:677` | `mapping` 最低位是不是 `PAGE_MAPPING_ANON` |
| `folio_mapcount` | `include/linux/mm.h:1231` | 这个 folio 被映射了多少次 |
| `__filemap_add_folio` | `mm/filemap.c:844` | 写入 `i_pages`，拒绝 swapbacked folio |
| `filemap_add_folio` | `mm/filemap.c:960` | 加锁、插入、必要时 `workingset_refault`、挂 LRU |
| `add_to_page_cache_lru` | `mm/folio-compat.c:86` | `struct page *` 包装，转去 `filemap_add_folio` |
| `page_folio` | `include/linux/page-flags.h:268` | 从 `struct page *` 找到它所在的 folio |
| `workingset_refault` | `mm/workingset.c:537` | shadow 再次命中时的工作集记账 |
| `folio_add_lru` | `mm/swap.c:493` | 把 folio 放上 LRU |
| `filemap_get_entry` | `mm/filemap.c:1844` | 无锁读 xarray。值项原样返回 |
| `__filemap_get_folio` | `mm/filemap.c:1891` | 按 `FGP_*` 查找或创建 |
| `filemap_get_folio` | `include/linux/pagemap.h:648` | 只查找，不创建 |
| `filemap_alloc_folio` | `mm/filemap.c:989`（`CONFIG_NUMA`）；否则 `include/linux/pagemap.h:545` | 按 GFP 和 order 分配 folio |
| `folio_alloc` | `mm/mempolicy.c:2287`（`CONFIG_NUMA`）；否则 `include/linux/gfp.h:273` | 非 NUMA 的 `filemap_alloc_folio` 直接调用它 |
| `do_sync_mmap_readahead` | `mm/filemap.c:3177` | 缺页未命中时的同步预读 |
| `do_async_mmap_readahead` | `mm/filemap.c:3246` | 缺页命中后、还没拿锁时的异步预读 |
| `filemap_fault` | `mm/filemap.c:3293` | 文件缺页。未命中记 major，必要时 `read_folio` |
| `folio_file_page` | `include/linux/pagemap.h:822` | 按文件内下标取出 folio 里的那一页 |
| `filemap_create_folio` | `mm/filemap.c:2516` | 读路径上分配、插入、读盘 |
| `filemap_read_folio` | `mm/filemap.c:2403` | 调用 `a_ops->read_folio` |
| `filemap_read` | `mm/filemap.c:2655` | 缓冲读的页缓存循环 |
| `generic_file_read_iter` | `mm/filemap.c:2827` | 读迭代器。直接 IO 短读会落到 `filemap_read` |
| `generic_perform_write` | `mm/filemap.c:3990` | `write_begin` / 拷贝 / `write_end` / 限速 |
| `__generic_file_write_iter` | `mm/filemap.c:4092` | 直接 IO 或缓冲写的分发 |
| `generic_file_write_iter` | `mm/filemap.c:4139` | 拿 `i_rwsem` 后调用上面的写 |
| `generic_write_sync` | `include/linux/fs.h:2675` | 写成功后，O_SYNC 一类在这里等回写 |
| `folio_mark_dirty` | `mm/page-writeback.c:2785` | 调用 `a_ops->dirty_folio` |
| `noop_dirty_folio` | `mm/page-writeback.c:2613` | 没有 mapping 时的标脏。shmem 的 `dirty_folio` 也用它 |
| `folio_account_dirtied` | `mm/page-writeback.c:2628` | `NR_FILE_DIRTY`、`NR_DIRTIED` 和 wb 计数 |
| `rt_task` | `include/linux/sched/rt.h:16` | 实时任务。脏页阈值再抬高四分之一 |
| `domain_dirty_limits` | `mm/page-writeback.c:375` | 由 ratio/bytes 算出 `thresh` 和 `bg_thresh` |
| `global_dirty_limits` | `mm/page-writeback.c:448` | 全局域的同一对阈值。`/proc/vmstat` 用它 |
| `dirty_freerun_ceiling` | `mm/page-writeback.c:835` | `(thresh + bg_thresh) / 2`，低于它不睡觉 |
| `__wb_calc_thresh` | `mm/page-writeback.c:883` | 按写完成比例切出 `wb_thresh` |
| `wb_calc_thresh` | `mm/page-writeback.c:910` | 上面函数的对外包装 |
| `balance_dirty_pages` | `mm/page-writeback.c:1691` | 越过空转上限时让写进程睡觉，并叫醒后台回写 |
| `balance_dirty_pages_ratelimited_flags` | `mm/page-writeback.c:2014` | 按 `nr_dirtied_pause` 节流后调用上面的函数 |
| `balance_dirty_pages_ratelimited` | `mm/page-writeback.c:2084` | flags 传 0 的包装 |
| `wb_over_bg_thresh` | `mm/page-writeback.c:2099` | 这个 wb 是否还高于后台阈值 |
| `wb_wakeup` | `fs/fs-writeback.c:136` | 叫醒一个 wb 的 flusher |
| `wb_start_background_writeback` | `fs/fs-writeback.c:1320` | 叫醒 flusher 做后台回写 |
| `wb_start_writeback` | `fs/fs-writeback.c:1289` | 排队一次“把脏页写出去” |
| `wb_writeback` | `fs/fs-writeback.c:2115` | 后台写到低于 `bg_thresh`，kupdate 只写旧页 |
| `wakeup_flusher_threads` | `fs/fs-writeback.c:2405` | 按 `wb_reason` 叫醒所有有脏 IO 的 flusher |
| `folio_check_dirty_writeback` | `mm/vmscan.c:1589` | 回收前问这页脏不脏、是不是正在回写 |
| `is_page_cache_freeable` | `mm/vmscan.c:1136` | 引用是否已经少到可以写出并回收 |
| `pageout` | `mm/vmscan.c:1312` | 回收路径调用 `a_ops->writepage` |
| `shrink_folio_list` | `mm/vmscan.c:1712` | 回收一批 folio。脏文件页多数留给 flusher |
| `SHMEM_I` | `include/linux/shmem_fs.h:74` | 从 VFS inode 反查 `shmem_inode_info` |
| `__shmem_get_inode` | `mm/shmem.c:2451` | 初始化 `seals`、`flags`，`noswap` 时标成不可回收 |
| `shmem_get_inode` | `mm/shmem.c:2532` 或 `:2560` | `__shmem_get_inode` 的包装。开了配额时包装里会先做配额初始化 |
| `shmem_recalc_inode` | `mm/shmem.c:417` | 更新 `alloced` 和 `swapped` |
| `shmem_add_to_page_cache` | `mm/shmem.c:765` | shmem 自己的插入。可以替换 swap 项 |
| `shmem_delete_from_page_cache` | `mm/shmem.c:834` | 换出时把 folio 换成 swap 项 |
| `folio_alloc_swap` | `mm/swap_slots.c:302` | 给 folio 分配一个 swap 项 |
| `swap_writepage` | `mm/page_io.c:179` | 把换出的页写到 swap |
| `shmem_writepage` | `mm/shmem.c:1443` | 只在 `for_reclaim` 时把页换成 swap 项 |
| `shmem_alloc_and_acct_folio` | `mm/shmem.c:1685` | 分配并记账。大页失败由调用者改小 |
| `shmem_swapin_folio` | `mm/shmem.c:1834` | 把 xarray 里的 swap 项换回 folio，并标脏 |
| `shmem_get_folio_gfp` | `mm/shmem.c:1948` | 页缓存 / swap 换入 / 新分配 |
| `shmem_get_folio` | `mm/shmem.c:2161` | 不带 vma 的包装 |
| `shmem_fault` | `mm/shmem.c:2180` | shmem 缺页 |
| `handle_userfault` | `fs/userfaultfd.c:429` | shmem 缺页把 minor / missing 交给用户态 |
| `get_unmapped_area` | `mm/mmap.c:1820` | 选一段用户地址。共享匿名映射改走 shmem |
| `__mmap_region` | `mm/mmap.c:2687` | 真正建 VMA。`VM_SHARED` 且没有文件时调用 `shmem_zero_setup` |
| `mmap_region` | `mm/mmap.c:2925` | 检查标志后调用 `__mmap_region`（`:2949`） |
| `do_anonymous_page` | `mm/memory.c:4104` | 私有匿名缺页。shmem 映射不走这里 |
| `shmem_anon_vm_ops` | `mm/shmem.c:4581` | 共享匿名 VMA 的 `vm_ops`，`fault` 是 `shmem_fault` |
| `shmem_get_unmapped_area` | `mm/shmem.c:2257` | 共享匿名映射选地址。`!CONFIG_SHMEM` 的桩在 `:4761` |
| `shmem_lock` | `mm/shmem.c:2364` | `VM_LOCKED` 打进 `info->flags`，换出被拒绝 |
| `mapping_set_unevictable` | `include/linux/pagemap.h:244` | `noswap` 的 inode 在创建时打上 `AS_UNEVICTABLE` |
| `shmem_kernel_file_setup` | `mm/shmem.c:4839` | 内核内部 shmem 文件。`shmem_zero_setup` 用它 |
| `shmem_file_setup` | `mm/shmem.c:4850` | 无目录项的 shmem 文件。`memfd_create` 用它 |
| `shmem_zero_setup` | `mm/shmem.c:4874` | `MAP_SHARED` 匿名映射接上 shmem 文件 |
| `memfd_create` | `mm/memfd.c:294` | 系统调用。普通 memfd 是 shmem，`MFD_HUGETLB` 是 hugetlb |
| `hugetlb_file_setup` | `fs/hugetlbfs/inode.c:1632` | `MFD_HUGETLB` 时建 hugetlb 文件 |

---

[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 THP](./09-thp-and-hugetlb.md)
