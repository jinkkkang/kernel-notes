# 12. balloon、free page reporting 与超卖

**结论**：超卖是宿主把同一堆物理页答应给多个 guest。guest 里交回页的机制不是一条。virtio-balloon 的 inflate 把页从 buddy 拿出来，经 inflate virtqueue 把 PFN 告诉宿主；deflate 再还回 buddy。free page reporting（`VIRTIO_BALLOON_F_REPORTING`）不把页从 guest 的空闲链表里拿走，只是短暂隔离、报告、放回，并打上 `PageReported`。free page hinting（`VIRTIO_BALLOON_F_FREE_PAGE_HINT`）是另一条 virtqueue：按宿主给的 cmd id，用 `alloc_pages` 把 `MAX_ORDER` 的空闲块拿出来送给宿主，之后再 `free_pages` 还回去。三条路不要写成同一个回调。

宿主自己的 `vm.overcommit_memory` 是另一层，记账在 [06](./06-mm-and-vma.md)，这里不重复那三档策略。

> 源码树：Linux **v6.6.160**。
> FPR：`mm/page_reporting.c`，挂钩在 `__free_one_page`。驱动：`drivers/virtio/virtio_balloon.c`。热插拔：`mm/memory_hotplug.c`，virtio-mem 在 `drivers/virtio/virtio_mem.c`。
> 本环境没有 virtio-balloon 设备，inflate / deflate / KSM 扫描都没有跑，操作步骤标 **RECIPE**。
> 导航：[上一节 KVM](./11-kvm-memory.md) · [目录](./README.md) · [下一节 观测](./13-observability.md)

**在 6.12 上采样**（观测笔记里已有的 `/proc/vmstat` 数字；本文结构体按 v6.6.160 核对，这组计数没有重采）：`balloon_inflate` 4718592、`balloon_deflate` 2097152、`balloon_migrate` 45672。单位是 Linux 页。按 4KiB 页换算，净 inflate (4718592−2097152)×4KiB = 10GiB，这是历史累计。vmstat 只累加。当前大小看设备 config 里的 `actual`（驱动写回的 `num_pages`）。`cow_ksm` 0、`ksm_swpin_copy` 0，同样是这组 6.12 采样。

---

## 12.1 三条通道，加上 virtio-mem

| 机制 | guest 内核做什么 | 页还在 guest buddy 里吗 | 和宿主回收的关系 |
|---|---|---|---|
| inflate | `balloon_page_alloc`（一次一页）→ `balloon_page_enqueue` → inflate virtqueue 送出 PFN | 不在。页在 `vb_dev_info.pages` 上 | 内核把 PFN 交给宿主后就结束。qemu 怎么 `madvise` 不在这棵树里 |
| deflate | `balloon_page_dequeue` → deflate virtqueue → `put_page` | 回到 buddy | 宿主必须先把页准备好。`F_MUST_TELL_HOST` 时顺序是先告诉宿主再放页（`leak_balloon` 注释 `:293`） |
| FPR，`VIRTIO_BALLOON_F_REPORTING` | 不分配。从空闲链表隔离，`report` 之后放回，并 `__SetPageReported` | 报告期间短暂离开，之后仍是空闲页 | guest 的 `MemFree` 不因报告成功下降 |
| free page hint，`VIRTIO_BALLOON_F_FREE_PAGE_HINT` | `alloc_pages(MAX_ORDER)`，页地址放进 `free_page_vq`，链在 `free_page_list` | 不在。等 cmd id `DONE` 或 shrinker 再 `free_pages` | 和 FPR 不是同一个 vq，也不是 `page_reporting_*` |
| virtio-mem | `add_memory_driver_managed` / 热插拔 offline | 拔掉的块不是「空闲页」，是地址范围没了 | 改变 guest 的 `MemTotal` |

`struct virtio_balloon` 里这些队列是分开的成员，见 12.2。

和 `vm.overcommit_memory`：那是宿主（或 guest 自己）在 `mmap` 时记的虚拟承诺，三档策略在 [06](./06-mm-and-vma.md)。balloon 动的是已经分配的页和可选的 `managed_pages`，不走 `__vm_enough_memory`。宿主回收（kswapd / 直接回收）扫的是 qemu 进程的页表；guest 里的 inflate 是 guest 内核主动把页从自己的 buddy 拿走。两边可以同时发生，但不是同一个函数。

vmstat 名字在 `mm/vmstat.c:1374`，要 `CONFIG_MEMORY_BALLOON`。`balloon_inflate` 在 `balloon_page_enqueue_one` 里每入队一个 `struct page` 加 1（`mm/balloon_compaction.c:26`）。`balloon_deflate` 在出队时加（`:98`）。`balloon_migrate` 要再开 `CONFIG_BALLOON_COMPACTION`。

没有 `F_DEFLATE_ON_OOM` 时，inflate 对每一页 `adjust_managed_page_count(page, -1)`（`virtio_balloon.c:241`），deflate 加回去（`:263`）。guest 的 `MemTotal` 会跟着掉。有这个特性时不改 managed count，OOM 时靠 notifier 把页放回来。

---

## 12.2 inflate / deflate

通用气球页在 `include/linux/balloon_compaction.h`，实现在 `mm/balloon_compaction.c`。`page->private` 在开了 compaction 时指向 `balloon_dev_info`（`balloon_page_insert`，头文件 `:96`）。`page->mapping` 指向 `balloon_mops`，页同时是 `PageOffline` 和 `PageMovable`。

头文件 `:14` 起的规则：

1. 改 `page->mapping` 要先锁页，再拿 `b_dev_info->pages_lock`。
2. 隔离或 dequeue 必须在 `pages_lock` 下把页从设备链表摘掉。

`balloon_page_enqueue`（`balloon_compaction.c:141`）拿 `pages_lock`，锁页，插入链表，计 `BALLOON_INFLATE`。`balloon_page_dequeue`（`:172`）反向操作。链表暂时是空的（页正在被 compaction 隔离）时，dequeue 会返回 NULL，即使账上还有页。

### `struct balloon_dev_info`（`balloon_compaction.h:54`）

```c
struct balloon_dev_info {
	unsigned long isolated_pages;		/* 正在被迁移隔离的页数 */
	spinlock_t pages_lock;			/* 保护 pages */
	struct list_head pages;			/* 已经交给宿主的页 */
	/*
	 * compaction 要搬家时的回调。virtio 在 CONFIG_BALLOON_COMPACTION 下
	 * 设成 virtballoon_migratepage（virtio_balloon.c:751）。
	 */
	int (*migratepage)(struct balloon_dev_info *, struct page *newpage,
			   struct page *page, enum migrate_mode mode);
};
```

`balloon_mapping_gfp_mask`：开了 compaction 是 `GFP_HIGHUSER_MOVABLE`，否则 `GFP_HIGHUSER`（`:109`）。`balloon_page_alloc` 在这个掩码上再或 `__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN`，然后 `alloc_page`，也就是 order 0（`balloon_compaction.c:119`）。

### `struct virtio_balloon`（`drivers/virtio/virtio_balloon.c:56`）

驱动按 4K 气球页记账。`VIRTIO_BALLOON_PAGES_PER_PAGE = PAGE_SIZE >> 12`（`:28`）。一次告诉宿主的 PFN 数组长度 `VIRTIO_BALLOON_ARRAY_PFNS_MAX` 是 256（`:29`）。

```c
struct virtio_balloon {
	/* --- 设备和四条常驻队列。reporting_vq 在结构后部 --- */
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *free_page_vq;

	struct workqueue_struct *balloon_wq;	/* hint 用，WQ_FREEZABLE | WQ_CPU_INTENSIVE */
	struct work_struct report_free_page_work; /* hint，不是 FPR */

	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	spinlock_t stop_update_lock;
	bool stop_update;			/* 正在拆除，不再排队 */
	unsigned long config_read_bitmap;	/* 哪几个 config 字段要读 */

	/* --- free page hint 拿出来的页，还没还给 mm --- */
	struct list_head free_page_list;
	spinlock_t free_page_list_lock;
	unsigned long num_free_page_blocks;	/* 上面链表里的块数 */
	u32 cmd_id_received_cache;		/* 宿主发来的 cmd id 缓存 */
	__virtio32 cmd_id_active;		/* 这一轮正在用的 id */
	__virtio32 cmd_id_stop;			/* 结束标记的缓冲区 */

	wait_queue_head_t acked;		/* 等宿主读完 virtqueue */

	unsigned int num_pages;			/* 已经告诉宿主「不用」的气球页数 */
	struct balloon_dev_info vb_dev_info;	/* 这些页的链表 */
	struct mutex balloon_lock;

	unsigned int num_pfns;
	__virtio32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];

	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

	struct shrinker shrinker;		/* 仅 F_FREE_PAGE_HINT。把 hint 页还给 mm */
	struct notifier_block oom_nb;		/* 仅 F_DEFLATE_ON_OOM */

	struct virtqueue *reporting_vq;		/* FPR */
	struct page_reporting_dev_info pr_dev_info;
};
```

特性位（`include/uapi/linux/virtio_balloon.h:34`）：

| 宏 | 值 | 作用 |
|---|---|---|
| `VIRTIO_BALLOON_F_MUST_TELL_HOST` | 0 | 放页之前必须先告诉宿主 |
| `VIRTIO_BALLOON_F_STATS_VQ` | 1 | 统计队列 |
| `VIRTIO_BALLOON_F_DEFLATE_ON_OOM` | 2 | OOM 时放气。有它就不改 `managed_pages` |
| `VIRTIO_BALLOON_F_FREE_PAGE_HINT` | 3 | hint virtqueue |
| `VIRTIO_BALLOON_F_PAGE_POISON` | 4 | guest 在用 page poison |
| `VIRTIO_BALLOON_F_REPORTING` | 5 | FPR virtqueue |

`VIRTIO_BALLOON_CMD_ID_STOP` 是 0，`VIRTIO_BALLOON_CMD_ID_DONE` 是 1（`:44`）。

stats 的下标（`:64`）到 `VIRTIO_BALLOON_S_NR`（10）：swap-in、swap-out、major-fault、minor-fault、free、total、available、caches、hugetlb 分配、hugetlb 失败。`update_balloon_stats`（`virtio_balloon.c:315`）按这个顺序填。hugetlb 两项要 `CONFIG_HUGETLB_PAGE`，前四项要 `CONFIG_VM_EVENT_COUNTERS`。stats 队列是宿主先要、guest 再填（`stats_request`，`:364`），是提示，不是账本。

### inflate / deflate 路径

```text
virtballoon_changed                         # :441，config 变化
  queue_work(update_balloon_size_work)
  若有 F_FREE_PAGE_HINT：再排队 hint 工作

update_balloon_size_func                    # :473
  diff = towards_target                     # :390，读 config 的 num_pages，减去 vb->num_pages
  diff > 0：fill_balloon                    # :205
    一轮最多 256 个气球 PFN
    balloon_page_alloc → balloon_page_enqueue
    没有 F_DEFLATE_ON_OOM：adjust_managed_page_count(page, -1)
    tell_host(inflate_vq)                   # :146，等 acked
  diff < 0：leak_balloon                    # :269
    balloon_page_dequeue
    先 tell_host(deflate_vq)，再 release_pages_balloon
    release 里 put_page；没有 F_DEFLATE_ON_OOM 则 managed +1
  update_balloon_size                       # :455，把 num_pages 写回 config 的 actual
  diff 还没消完：把自己再排队               # :491
```

OOM：`VIRTIO_BALLOON_F_DEFLATE_ON_OOM` 时 probe 注册 notifier，优先级 `VIRTIO_BALLOON_OOM_NOTIFY_PRIORITY` 80（`:32`）。`virtio_balloon_oom_notify`（`:837`）调用 `leak_balloon(vb, VIRTIO_BALLOON_OOM_NR_PAGES)`，256 是气球页（4K 单位），再除以 `VIRTIO_BALLOON_PAGES_PER_PAGE` 记进 `*freed`。这是 guest 自救。宿主不一定立刻有页可还。

迁移：`virtballoon_migratepage`（`:751`，要 `CONFIG_BALLOON_COMPACTION`）先把新页 inflate 告诉宿主，再把旧页 deflate 告诉宿主。`balloon_lock` 拿不到就返回忙，避免和 `fill_balloon` 死锁（注释 `:758`）。

驱动名是 `KBUILD_MODNAME`，也就是 `virtio_balloon`（`:1120`）。这个文件没有注册 misc 设备。`virtballoon_validate`（`:1086`）：guest 既没有 `init_on_free` 也没有 page poisoning 时，清掉 `F_PAGE_POISON`；否则设备没提供 `F_PAGE_POISON` 就清掉 `F_REPORTING`，避免报告过程改掉 guest 以为有毒的空闲页。同时清掉 indirect descriptor 和 `VIRTIO_F_ACCESS_PLATFORM`（`:1103`）。

---

## 12.3 free page reporting

文档在 `Documentation/mm/free_page_reporting.rst`：设备注册之后，空闲页凑够阶，框架把它们放进 scatterlist 交给 `report`。报告期间分配器碰不到这些页；`report` 返回后页回到原来的 free area。同时只能有一个设备。

### `struct page_reporting_dev_info`（`include/linux/page_reporting.h:11`）

```c
struct page_reporting_dev_info {
	/* 处理 sg 里的页。virtio 的实现是 virtballoon_free_page_report */
	int (*report)(struct page_reporting_dev_info *prdev,
		      struct scatterlist *sg, unsigned int nents);
	struct delayed_work work;		/* 后台处理 */
	atomic_t state;				/* IDLE / REQUESTED / ACTIVE */
	unsigned int order;			/* 设备建议的最小阶。0 表示没指定 */
};
```

没有「一批多少页」的成员。一批的上限是宏 `PAGE_REPORTING_CAPACITY`，32（`:9`），必须是 2 的幂。

`page_reporting_order`（`mm/page_reporting.c:15`）是 `unsigned int`，初值 `-1`（无符号里是全 1）。模块参数同名，`0644`（`:36`）。get 用 `param_get_int`，所以没人设置时 sysfs 上看到 `-1`。set 的范围是 0 到 `MAX_ORDER`（`:23`）。`MAX_ORDER` 默认 10，除非 `CONFIG_ARCH_FORCE_MAX_ORDER`（`include/linux/mmzone.h:29`）。

`page_reporting_register`（`:354`）拿 `page_reporting_mutex`。已经有设备就 `-EBUSY`。`page_reporting_order` 仍是初值时：设备的 `order` 在 1 和 `MAX_ORDER` 之间就用它，否则用 `pageblock_order`（`:374`）。然后 `INIT_DELAYED_WORK(..., page_reporting_process)`，打开静态键 `page_reporting_enabled`。

状态（`:53`）：`PAGE_REPORTING_IDLE = 0`，`REQUESTED`，`ACTIVE`。

### virtio 怎么注册

`virtballoon_probe`（`:865`）在有 `F_REPORTING` 时（`:959`）：

- reporting virtqueue 的 vring 小于 `PAGE_REPORTING_CAPACITY` 就 `-ENOSPC`。
- `CONFIG_ARM64 && CONFIG_ARM64_64K_PAGES` 时把 `pr_dev_info.order` 设成 5（`:981`）。注释：64K 页上默认 `pageblock_order` 是 512MB，空闲块凑不出来；order 5 是 2MB，也避免宿主 4K 页时把 THP 拆碎。x86 4K 页不进这个 `#if`。
- `pr_dev_info.report = virtballoon_free_page_report`（`:958`），然后 `page_reporting_register`。

`virtballoon_free_page_report`（`:162`）把 scatterlist 加进 `reporting_vq`（`virtqueue_add_inbuf`），kick，等 `balloon_ack`。

如果用户已经把 `page_reporting_order` 设成不是 `-1`，注册时不再看设备的 `order`。ARM64 那个 5 只在参数还是初值时生效。

### 和 buddy 的挂钩

```text
__free_one_page                         # page_alloc.c:792
  合并 buddy
  没有 FPI_SKIP_REPORT_NOTIFY 才通知    # :869。FPI_SKIP_REPORT_NOTIFY 是 BIT(0)，:74
    page_reporting_notify_free(order)   # mm/page_reporting.h:33
      没编 CONFIG_PAGE_REPORTING：空函数
      静态键 page_reporting_enabled 没开：返回
      order < page_reporting_order：返回
      __page_reporting_notify()         # page_reporting.c:88
        __page_reporting_request        # :61
          已经是 REQUESTED 或不是 IDLE：合并掉
          否则排队 delayed_work，延迟 PAGE_REPORTING_DELAY（2*HZ，:50）

page_reporting_process                  # :308
  state = ACTIVE
  分配 PAGE_REPORTING_CAPACITY 长的 scatterlist
  每个 zone：page_reporting_process_zone # :261
    水位：low_wmark + (32 << page_reporting_order)。不够就跳过这个 zone
    从 page_reporting_order 到 NR_PAGE_ORDERS-1，每种 migratetype
      跳过 isolate
      page_reporting_cycle              # :147
        拿 zone->lock
        跳过已经 PageReported 的页
        允许调用 report 的次数是 DIV_ROUND_UP(nr_free, 32*16)
        次数用完就把 state 设回 REQUESTED
        __isolate_free_page             # 短暂离开 buddy
        攒满 32 个：放锁，prdev->report()，再拿锁
        page_reporting_drain            # :106
          __putback_isolated_page
          报告成功且页仍是同阶 buddy：__SetPageReported
  一轮结束 state 仍是 REQUESTED：再延迟 2 秒
```

`PageReported` 是页标志，`PG_reported = PG_uptodate`（`include/linux/page-flags.h:178`），注释写明只对 buddy 页有意义。`page->flags` 是 `unsigned long`（`mm_types.h:75`）。测试和设置走非原子版本，因为调用点持有 `zone->lock`（`page-flags.h:619`）。页被从空闲链表摘走时，`del_page_from_free_list`（`page_alloc.c:723`）若 `page_reported` 就 `__ClearPageReported`。所以分配再释放之后可以再报。

`page_reporting_order` 太高：空闲块凑不出这个阶，FPR 什么都不做。太低：order 0 的释放会把工作队列打满。默认跟 pageblock 走。

---

## 12.4 free page hinting

和 FPR 并列。特性位是 `VIRTIO_BALLOON_F_FREE_PAGE_HINT`，队列是 `free_page_vq`，工作项是 `report_free_page_work`。不走 `page_reporting_register`。

```text
report_free_page_func                     # virtio_balloon.c:715
  cmd id == VIRTIO_BALLOON_CMD_ID_DONE (1)
    return_free_pages_to_mm(ULONG_MAX)    # :408，全部 free_pages 回 buddy
  cmd id 不是 STOP(0)，也不是正在用的 active
    virtio_balloon_report_free_page       # :695
      send_cmd_id_start                   # 把收到的 id 用 outbuf 送给宿主
      send_free_pages                     # :666
        get_free_page_and_send            # :619
          alloc_pages(GFP_NORETRY|NOWARN|NOMEMALLOC, MAX_ORDER)
          失败：-EINTR，表示能拿的空闲块拿完了
          页地址作为 inbuf 放进 free_page_vq
          链到 free_page_list，num_free_page_blocks++
      send_cmd_id_stop
```

`VIRTIO_BALLOON_HINT_BLOCK_ORDER` 就是 `MAX_ORDER`（`:37`）。一块的字节数是 `1 << (MAX_ORDER + PAGE_SHIFT)`。vring 至少要有 2 个槽，一个留给 cmd id（probe，`:903`）。

这些页离开 buddy，直到：

- 宿主把 cmd id 设成 `DONE`，`return_free_pages_to_mm` 调用 `free_pages`；或
- shrinker `virtio_balloon_shrinker_scan`（`:819`）按扫描数把块还回去。注册名是 `"virtio-balloon"`（`:862`）。

hint 是一轮一轮的：宿主换 cmd id，guest 停掉旧的一轮（`send_free_pages` 里比较 active 和刚收到的 id，`:676`）。FPR 是持续的，挂在 buddy 合并上。两个特性位可以同时存在。看 feature，不要看「有没有气球」。

---

## 12.5 KSM

KSM（`mm/ksm.c`，`CONFIG_KSM`）把内容相同的匿名页合成一页，写时再拆开（COW）。`ksm_init`（`:3592`）拉起线程，名字 `"ksmd"`。

`ksm_scan_thread`（`:2611`）把自己标成 freezable，nice 5。循环里拿 `ksm_thread_mutex`，先 `wait_while_offlining`，再看 `ksmd_should_run`（`:2606`）：`ksm_run & KSM_RUN_MERGE` 且 `ksm_mm_head.slot.mm_node` 非空。该跑就 `ksm_do_scan(ksm_thread_pages_to_scan)`。`pages_to_scan` 默认 100（`:273`），`sleep_millisecs` 默认 20（`:276`）。跑完若仍该跑，就在 `ksm_iter_wait` 上睡这么多毫秒；不该跑就睡在 `ksm_thread_wait` 上。

```text
ksm_do_scan(scan_npages)                  # ksm.c:2590
  while 还没扫够，且当前线程没在 freezing：
    cond_resched
    rmap_item = scan_get_next_rmap_item(&page)   # :2396
    没有了：return
      整张 mm 链表走完时，scan_get_next_rmap_item 末尾把 ksm_scan.seqnr++（:2582）
    cmp_and_merge_page(page, rmap_item)   # :2114
    put_page(page)
    ksm_pages_scanned++
```

`cmp_and_merge_page` 的注释（`:2106`）：先在稳定树里找相同页；找不到就和上次的校验和比，一样才进不稳定树，或和不稳定树里的另一页合成后一起进稳定树。

进程用 `madvise(MADV_MERGEABLE)`。`ksm_madvise`（`:2778`）先 `vma_ksm_compatible`（`:533`）。`VM_SHARED`、`VM_MAYSHARE`、`VM_PFNMAP`、`VM_IO`、`VM_DONTEXPAND`、`VM_HUGETLB`、`VM_MIXEDMAP` 以及 DAX 直接忽略。所以 hugetlb 不参与 KSM。第一次进入会 `__ksm_enter`（`:2818`），把 mm 插进 ksmd 扫描的链表。

`ksm_run`（`:296`）：`KSM_RUN_STOP` 0，`KSM_RUN_MERGE` 1，`KSM_RUN_UNMERGE` 2，`KSM_RUN_OFFLINE` 4。写 sysfs `run` 时大于 `KSM_RUN_UNMERGE` 会失败（`:3298`），所以 4 是热插拔内部用的，不是给用户写的。没编 `CONFIG_SYSFS` 时 `ksm_init` 直接把 `ksm_run` 设成 `KSM_RUN_MERGE`（`:3621`）。

和超卖：宿主开 KSM，扫的是标了 `VM_MERGEABLE` 的匿名页。qemu 的客户机内存如果是这类匿名页，两个 guest 里相同的零页或代码页可以合成一页。写的时候 COW 拆开。`get_mergeable_page`（`:586`）要求 `PageAnon`，`vma_ksm_compatible` 拒绝 `VM_HUGETLB`。hugetlb 后端不走这条扫描。

`mm_struct` 在 `CONFIG_KSM` 下有三个计数（`include/linux/mm_types.h:957`）：`ksm_merging_pages`（正在合并的页，不含零页）、`ksm_rmap_items`（查过的页，含没合并上的）、`atomic_long_t ksm_zero_pages`（和内核零页合并的空页）。细节见 [06](./06-mm-and-vma.md)。

### `struct ksm_stable_node`（`mm/ksm.c:157`）

稳定树的节点。共享页数超过 `max_page_sharing` 时，节点变成链，下面挂 dup。`STABLE_NODE_CHAIN` 是 `-1024`（`:178`），写在 `rmap_hlist_len` 里表示「这是链头」。

```c
struct ksm_stable_node {
	union {
		struct rb_node node;		/* 在稳定树里 */
		struct {			/* 正在等迁移落位 */
			struct list_head *head;
			struct {
				struct hlist_node hlist_dup;
				struct list_head list;
			};
		};
	};
	struct hlist_head hlist;		/* 用这个 KSM 页的 rmap_item */
	union {
		unsigned long kpfn;		/* 这一页的 PFN */
		unsigned long chain_prune_time; /* 上次做完整 GC 的时间 */
	};
	int rmap_hlist_len;			/* hlist 长度，或 STABLE_NODE_CHAIN */
#ifdef CONFIG_NUMA
	int nid;				/* 挂在哪个 node 的稳定树上 */
#endif
};
```

### `struct ksm_rmap_item`（`:197`）

```c
struct ksm_rmap_item {
	struct ksm_rmap_item *rmap_list;	/* mm_slot 里的单向链表 */
	union {
		struct anon_vma *anon_vma;	/* 在稳定树里时 */
#ifdef CONFIG_NUMA
		int nid;			/* 在不稳定树里时 */
#endif
	};
	struct mm_struct *mm;
	unsigned long address;			/* 虚拟地址。低位是标志 */
	unsigned int oldchecksum;		/* 不稳定时上一轮的校验和 */
	union {
		struct rb_node node;		/* 不稳定树的节点 */
		struct {			/* 挂在稳定树下 */
			struct ksm_stable_node *head;
			struct hlist_node hlist;
		};
	};
};
```

`address` 的低位（`:216`）：`SEQNR_MASK` 0x0ff 是不稳定树的序号，`UNSTABLE_FLAG` 0x100，`STABLE_FLAG` 0x200。

### sysfs

`CONFIG_SYSFS` 时 `ksm_init` 对 `mm_kobj` 调用 `sysfs_create_group`，组名 `"ksm"`（`:3586`）。路径是 `/sys/kernel/mm/ksm/`。6.6 实际创建的属性（`ksm_attrs`，`:3563`）：

| 文件 | 读写 | 说明 |
|---|---|---|
| `sleep_millisecs` | 读写 | 两轮之间睡多久。默认 20 |
| `pages_to_scan` | 读写 | 一轮扫多少页。默认 100 |
| `run` | 读写 | 0 停，1 合并，2 拆开全部 |
| `pages_scanned` | 只读 | `ksm_pages_scanned` |
| `pages_shared` | 只读 | 稳定树节点数 |
| `pages_sharing` | 只读 | 额外共享的页槽 |
| `pages_unshared` | 只读 | 不稳定树节点数 |
| `pages_volatile` | 只读 | `rmap_items - pages_shared - pages_sharing - pages_unshared`，小于 0 印 0（`:3485`） |
| `ksm_zero_pages` | 只读 | 和零页合并的页 |
| `full_scans` | 只读 | `ksm_scan.seqnr` |
| `merge_across_nodes` | 读写 | 仅 `CONFIG_NUMA` |
| `max_page_sharing` | 读写 | 一个稳定节点最多多少页槽 |
| `stable_node_chains` | 只读 | 链的个数 |
| `stable_node_dups` | 只读 | 链上的 dup |
| `stable_node_chains_prune_millisecs` | 读写 | 默认 2000 |
| `use_zero_pages` | 读写 | 初值 false（`:3600`） |
| `general_profit` | 只读 | `(pages_sharing + ksm_zero_pages) * PAGE_SIZE - rmap_items * sizeof(ksm_rmap_item)`（`:3509`） |

---

## 12.6 宿主页大小和 guest 页大小

| 层 | 6.6 里实际发生的事 |
|---|---|
| guest 大页 | 只影响 guest 自己的页表。EPT 的级别看宿主映射，见 [11.6](./11-kvm-memory.md) 的 `host_pfn_mapping_level` |
| 宿主 THP / hugetlb | qemu 的 HVA 若是 2M/1G，`kvm_mmu_hugepage_adjust` 才可能把 `req_level` 抬上去。脏页日志开着则直接保持 4K |
| inflate | `alloc_page`，order 0。buddy 可能把空闲的高阶块拆开以满足它。这不是把已映射的 THP 拆开 |
| FPR order 5 | 只在 ARM64 64K 页的那个 `#if` 里。注释写明是为了少拆宿主 THP |

宿主把 guest 内存放在 hugetlb 上时，inflate 交回去的是 guest 的一页 PFN。KVM 和 virtio 驱动都不在 inflate 路径里拆宿主大页。宿主用户态拿这些 PFN 做什么，不在这棵树里，标 RECIPE。

---

## 12.7 virtio-mem

`struct virtio_mem`（`drivers/virtio/virtio_mem.c:100`）是热插拔设备，不是气球。插上的内存走 `virtio_mem_add_memory`（`:629`）→ `add_memory_driver_managed`（`mm/memory_hotplug.c:1561`），资源名必须是 `"System RAM (...)"` 这种格式。驱动用的字符串是 `"System RAM (virtio_mem)"`（`:639`）。

```c
struct virtio_mem {
	/* --- 设备和请求 --- */
	struct virtio_device *vdev;
	bool unplug_all_required;		/* 启动时可能得先把内存全部拔掉 */
	struct work_struct wq;
	atomic_t wq_active;
	atomic_t config_changed;
	struct virtqueue *vq;			/* guest → 宿主 */
	wait_queue_head_t host_resp;
	struct virtio_mem_req req;
	struct virtio_mem_resp resp;

	/* --- 大小和地址 --- */
	uint64_t plugged_size;			/* 当前已经插上 */
	uint64_t requested_size;		/* 宿主要求的大小 */
	uint64_t device_block_size;		/* 和设备通信的块 */
	int nid;
	uint64_t addr;				/* 区域物理起始 */
	uint64_t region_size;			/* 最大区域 */
	struct resource *parent_resource;
	const char *resource_name;		/* add_memory_driver_managed 用 */
	/*
	 * memory group id。virtio_mem_add_memory 把它当作
	 * add_memory_driver_managed 的第一个参数，并带上 MHP_NID_IS_MGID。
	 * add_memory_resource（memory_hotplug.c:1412）先按 id 找 group，
	 * 再把真正的 NUMA nid 换成 group->nid。
	 */
	int mgid;

	/*
	 * 默认阈值 1GiB（:150）。加上「至少两个 offline 块」里更大的那个，
	 * 避免插得太快把系统插到 OOM。
	 */
	atomic64_t offline_size;
	uint64_t offline_threshold;

	bool in_sbm;				/* 真：sub-block。假：big-block */

	union {
		struct {			/* sub-block 模式 */
			unsigned long first_mb_id;
			unsigned long last_usable_mb_id;
			unsigned long next_mb_id;
			uint64_t sb_size;
			uint32_t sbs_per_mb;
			bool have_unplugged_mb;
			unsigned long mb_count[VIRTIO_MEM_SBM_MB_COUNT];
			uint8_t *mb_states;	/* 每块一字节。vmalloc */
			unsigned long *sb_states; /* 每个 subblock 一位。1 表示已插 */
		} sbm;
		struct {			/* big-block 模式 */
			unsigned long first_bb_id;
			unsigned long last_usable_bb_id;
			unsigned long next_bb_id;
			unsigned long bb_count[VIRTIO_MEM_BBM_BB_COUNT];
			uint8_t *bb_states;
			uint64_t bb_size;
		} bbm;
	};

	struct mutex hotplug_mutex;		/* 保护上面的状态数组。注释 :222 */
	bool hotplug_active;
	bool broken;				/* 出错后停止处理请求 */
	bool in_kdump;				/* probe 时的 is_kdump_kernel() */
	spinlock_t removal_lock;
	bool removing;

	struct hrtimer retry_timer;
	unsigned int retry_timer_ms;
	/* 最小 50000 ms，最大 300000 ms。宏在 :249 */

	struct notifier_block memory_notifier;	/* online / offline */

#ifdef CONFIG_PROC_VMCORE
	struct vmcore_cb vmcore_cb;
	uint64_t last_block_addr;
	bool last_block_plugged;
#endif
	struct list_head next;			/* 所有 virtio-mem 设备的链表 */
};
```

`add_memory_resource`（`memory_hotplug.c:1392`）建好 memory block 之后，若 `mhp_default_online_type != MMOP_OFFLINE`，就 `walk_memory_blocks` → `online_memory_block`（`:1313`）→ `device_online`。online 的函数在 12.9。virtio-mem 改变的是 guest 的物理内存范围。balloon 不改 memmap 的范围，只在没有 `F_DEFLATE_ON_OOM` 时改 `managed_pages`。FPR 两者都不改。

---

## 12.8 操作步骤（RECIPE）

下面假设宿主能跑 qemu，guest 里有 virtio-balloon。本环境没有这套设备，**不要在这台机器上执行**。内核树里也没有 qemu 监视器。

```text
# guest 里看驱动是否绑上。驱动名是 virtio_balloon，挂在 virtio 总线。
ls /sys/bus/virtio/drivers/virtio_balloon/

# 宿主 qemu 监视器不在内核里。目标大小写在 virtio config 的 num_pages，
# guest 用 towards_target 读它。具体监视器命令以该 qemu 的文档为准。

# guest 里看结果：
grep -E 'MemTotal|MemFree|MemAvailable' /proc/meminfo
grep balloon /proc/vmstat

# FPR：要 CONFIG_PAGE_REPORTING，设备要有 F_REPORTING。
# validate 可能因为没有 PAGE_POISON 把 REPORTING 清掉，见 12.2。
cat /sys/module/page_reporting/parameters/page_reporting_order
# 改阶是模块参数，范围 0..MAX_ORDER。会改变哪些空闲块被报告。

# KSM，宿主上，扫标了 MADV_MERGEABLE 的匿名页：
# echo 1 > /sys/kernel/mm/ksm/run
# cat /sys/kernel/mm/ksm/pages_sharing

# 热插拔块的状态。读出来是 online / offline / going-offline。
# 写入才用 online、online_kernel、online_movable、offline。
grep . /sys/devices/system/memory/memory*/state
```

`MemTotal` 变了，是 virtio-mem，或 balloon 在没有 `F_DEFLATE_ON_OOM` 时改了 managed count。`MemTotal` 没变、guest 的 `MemFree` 也没降，但宿主把页拿去用了，更像 FPR。hint 在 cmd id 还没 `DONE` 时，guest 的空闲页是真的被 `alloc_pages` 拿走的。

---

## 12.9 内存热插拔：online / offline 与 `ZONE_MOVABLE`

**结论**：用户看到的单位是 memory block。`online_pages` 把 PFN 段挂进某个 zone；`offline_pages` 先隔离，再把还能动的页迁走，迁不走就失败。virtio-mem 经 `add_memory_driver_managed` 走到这里。气球不走。

`struct memory_block` 在 `include/linux/memory.h:78`。

```c
struct memory_block {
	unsigned long start_section_nr;
	enum memory_block_state state;		/* dev->lock 串行 */
	enum mmop online_type;			/* 传给 online 的类型 */
	int nid;
	/*
	 * 这块里所有 System RAM 的 PFN 若属于同一个 zone，就是这个 zone。
	 * 跨 zone（含跨 node）则是 NULL。
	 */
	struct zone *zone;
	struct device dev;
	struct vmem_altmap *altmap;		/* memmap_on_memory 用 */
	struct memory_group *group;
	struct list_head group_next;
#if defined(CONFIG_MEMORY_FAILURE) && defined(CONFIG_MEMORY_HOTPLUG)
	atomic_long_t nr_hwpoison;
#endif
};
```

`enum mmop`（`include/linux/memory_hotplug.h:68`）：

| 值 | 数值 | 含义 |
|---|---|---|
| `MMOP_OFFLINE` | 0 | 下线 |
| `MMOP_ONLINE` | 1 | 上线。zone 看 `default_zone_for_pfn` |
| `MMOP_ONLINE_KERNEL` | 2 | 上到 kernel zone |
| `MMOP_ONLINE_MOVABLE` | 3 | 上到 `ZONE_MOVABLE` |

`memory_block_online`（`drivers/base/memory.c:186`）先 `zone_for_pfn_range`，再 `mem_hotplug_begin`（写锁 `mem_hotplug_lock`），然后 `online_pages`。有 `altmap` 时，开头那一段 vmemmap 页先 `mhp_init_memmap_on_memory`，不进 buddy。

选 zone 的函数是 `zone_for_pfn_range`（`mm/memory_hotplug.c:1047`）：

| 条件 | 落到哪 |
|---|---|
| `MMOP_ONLINE_KERNEL` | `default_kernel_zone_for_pfn` |
| `MMOP_ONLINE_MOVABLE` | 该 node 的 `ZONE_MOVABLE`（`:1055`） |
| `online_policy == ONLINE_POLICY_AUTO_MOVABLE` | `auto_movable_zone_for_pfn` |
| 其余 | `default_zone_for_pfn`（`:1023`） |

`default_zone_for_pfn`：区间只和一个 zone 相交就继承它。都不相交，或两个重叠，则只有 `movable_node_enabled` 才用 `ZONE_MOVABLE`，否则用 kernel zone（`:1039`）。

`online_policy` 模块参数默认 `ONLINE_POLICY_CONTIG_ZONES`（`:164`），字符串是 `"contig-zones"`。`"auto-movable"` 时，配置允许才 online 到 `ZONE_MOVABLE`（注释 `:161`）。比例 `auto_movable_ratio` 默认 301（`:181`）。`CONFIG_NUMA` 下还有 `auto_movable_numa_aware`，默认 true。

`movable_node` 是 early_param（`:1814`，`cmdline_parse_movable_node`），把 `movable_node_enabled` 设成 true。默认 online 类型是 `mhp_default_online_type`：没编 `CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE` 时是 `MMOP_OFFLINE`，编了是 `MMOP_ONLINE`（`:222`）。启动参数 `memhp_default_state=` 用 `mhp_online_type_from_str` 改它（`:228`）。sysfs `auto_online_blocks` 打印的就是这张字符串表（`drivers/base/memory.c:490`）。

`state` 文件读和写不是同一组字符串。`state_show`（`:141`）只印 `online`、`offline`、`going-offline`。写入走 `online_type_to_str`（`:31`）：`offline`、`online`、`online_kernel`、`online_movable`。写 `online_movable` 成功之后，读出来仍是 `online`。

```text
online_pages(pfn, nr_pages, zone, group)   # memory_hotplug.c:1137
  调用者必须已经拿着 mem_hotplug_lock 写锁
  要求 pageblock 对齐，且结束于 section 边界，否则 -EINVAL
  注释 :1149：memmap_on_memory 例外，它先留一段 pageblock 对齐的 vmemmap
  move_pfn_range_to_zone(..., MIGRATE_ISOLATE)   # :1159。先隔离，不是 MOVABLE
  memory_notify(MEM_GOING_ONLINE)
  失败：MEM_CANCEL_ONLINE，remove_pfn_range_from_zone
  zone 还没 populated：setup_zone_pageset，记下来稍后 build_all_zonelists
  online_pages_range                      # :644。按 MAX_ORDER 对齐的块调用
    online_page_callback，默认 generic_online_page（:631）→ __free_pages_core
  adjust_present_page_count
  node_states_set_node                    # :695
    该 node 还没有 N_MEMORY：置上
    zone 不超过 ZONE_NORMAL 且还没有 N_NORMAL_MEMORY：再置上
  undo_isolate_page_range(..., MIGRATE_MOVABLE)  # :1196，这时才允许按可移动分配
  shuffle_zone，init_per_zone_wmark_min
  memory_notify(MEM_ONLINE)

offline_pages(start_pfn, nr_pages, zone, group)  # :1876，同样要写锁
  同样的对齐检查
  walk_system_ram_range 对不上 nr_pages：有洞，-EINVAL
  起止页不在同一个 zone：-EINVAL
  zone_pcp_disable + lru_cache_disable
  start_isolate_page_range(..., MIGRATE_MOVABLE,
      MEMORY_OFFLINE | REPORT_FAILURE,
      GFP_USER | __GFP_MOVABLE | __GFP_RETRY_MAYFAIL)   # :1934
  memory_notify(MEM_GOING_OFFLINE)
  内层循环：
    scan_movable_pages                    # :1657
      PageLRU 或 __PageMovable：返回 0，*movable_pfn 指向它
      PageOffline 且 page_count 非 0：-EBUSY（:1680）
      PageHuge 且 HPageMigratable：返回 0
      扫完没有可迁页：-ENOENT
    返回 0：do_migrate_range              # :1704，void
      migrate_pages(..., MIGRATE_SYNC, MR_MEMORY_HOTPLUG)  # :1794
      先从 nmask 清掉本 node；没有别的在线 node 才迁回本 node（:1791）
      失败只 putback。调用点 :1976 不看返回值
      :1972 的 TODO：致命失败本应退出，当前没有
    返回值既不是 0 也不是 -ENOENT：unmovable page，offline 失败
  -ENOENT 之后：dissolve_free_huge_pages（:1990），再 test_pages_isolated
    隔离检查失败就再来一轮
  __offline_isolated_pages                # :2001，从 buddy 摘下空闲页
  adjust_managed_page_count(..., -nr_pages)   # :2017
  adjust_present_page_count(..., -nr_pages)   # :2018
  zone 不再 populated：zone_pcp_reset，build_all_zonelists（:2023）
  memory_notify(MEM_OFFLINE)
  remove_pfn_range_from_zone
```

`ZONE_MOVABLE` 上的页必须能迁。`scan_movable_pages` 把「`PageOffline` 且引用还在」的页当成 `-EBUSY`。这种页没有走 `__PageMovable` 那一支（那一支在前面已经 `goto found`）。要能拔的内存，online 时就得进 `ZONE_MOVABLE`，并且上面不要放不可移动的分配。

`add_memory`（`:1528`）和 `add_memory_driver_managed`（`:1561`）都进 `add_memory_resource`（`:1392`）。资源登记、建 memory block 之后，默认类型不是 `MMOP_OFFLINE` 才自动 online（`:1498`）。

---

## 12.10 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `balloon_page_alloc` | `mm/balloon_compaction.c:119` | `alloc_page`，order 0 |
| `balloon_page_enqueue` | `:141` | 入设备链表，`BALLOON_INFLATE` |
| `balloon_page_dequeue` | `:172` | 出链表，可能因隔离返回 NULL |
| `fill_balloon` | `drivers/virtio/virtio_balloon.c:205` | 分配、入队、inflate vq |
| `leak_balloon` | `:269` | 出队、deflate vq、`put_page` |
| `update_balloon_size_func` | `:473` | 读目标，inflate 或 deflate，写回 `actual` |
| `towards_target` | `:390` | config `num_pages` 减 `vb->num_pages` |
| `virtio_balloon_oom_notify` | `:837` | 最多放 256 个气球页 |
| `virtballoon_migratepage` | `:751` | compaction：新页 inflate，旧页 deflate |
| `virtballoon_free_page_report` | `:162` | FPR 的 `report`。走 `reporting_vq` |
| `page_reporting_register` | `mm/page_reporting.c:354` | 注册唯一的 FPR 设备 |
| `__page_reporting_notify` | `:88` | 合并通知，调度 delayed work |
| `page_reporting_process` | `:308` | 扫 zone，隔离，报告，放回 |
| `page_reporting_cycle` | `:147` | 一个 free list 上跳过已报告页 |
| `page_reporting_notify_free` | `mm/page_reporting.h:33` | `__free_one_page` 里的筛子 |
| `report_free_page_func` | `virtio_balloon.c:715` | hint 的工作函数 |
| `virtio_balloon_report_free_page` | `:695` | 按 cmd id 走 `free_page_vq` |
| `get_free_page_and_send` | `:619` | `alloc_pages(MAX_ORDER)` 再送出 |
| `return_free_pages_to_mm` | `:408` | hint 页 `free_pages` 回 buddy |
| `ksm_scan_thread` | `mm/ksm.c:2611` | ksmd |
| `ksmd_should_run` | `:2606` | `KSM_RUN_MERGE` 且 mm 链表非空 |
| `ksm_do_scan` | `:2590` | 一轮扫描 |
| `scan_get_next_rmap_item` | `:2396` | 下一个候选页 |
| `cmp_and_merge_page` | `:2114` | 稳定树 / 不稳定树 |
| `ksm_madvise` | `:2778` | `MADV_MERGEABLE` |
| `vma_ksm_compatible` | `:533` | 拒绝 hugetlb、共享、IO |
| `get_mergeable_page` | `:586` | `follow_page` 之后要求 `PageAnon` |
| `virtio_mem_add_memory` | `drivers/virtio/virtio_mem.c:629` | `add_memory_driver_managed` |
| `add_memory_driver_managed` | `mm/memory_hotplug.c:1561` | 驱动管理的 System RAM |
| `add_memory_resource` | `:1392` | 建 memory block，按默认类型 online |
| `online_pages` | `:1137` | PFN 进 zone 的 buddy |
| `offline_pages` | `:1876` | 隔离、迁移、摘下 |
| `zone_for_pfn_range` | `:1047` | 按 online 类型选 zone |
| `memory_block_online` | `drivers/base/memory.c:186` | sysfs online → `online_pages` |
| `do_migrate_range` | `mm/memory_hotplug.c:1704` | offline 时迁走可移动页 |
| `scan_movable_pages` | `:1657` | 找下一段可迁页，或报告不可迁 |

---

[上一节 KVM](./11-kvm-memory.md) · [目录](./README.md) · [下一节 观测](./13-observability.md)
