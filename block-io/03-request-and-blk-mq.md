# 3. request 与 blk-mq

**结论**：`blk_mq_submit_bio` 为 bio 分配一个 `struct request` 并占一个 tag；请求挂在提交 CPU 的软件队列上，再由硬件队列调用 `blk_mq_ops.queue_rq` 交给驱动。

> `struct request` 在 `include/linux/blk-mq.h:80`。软件队列 `struct blk_mq_ctx` 在 `block/blk-mq.h:18`，不在公开头文件里。队列在 `include/linux/blkdev.h:391`。

C 新手先分清两个 tag。`rq->tag` 是驱动 tag，请求真正派给硬件时才占用，对应 `hctx->tags`。`rq->internal_tag` 在挂了调度器时用来占调度器自己的 tag 池 `hctx->sched_tags`。没有调度器时，两套往往是同一池。tag 不是扇区号，只是「这个硬件队列上第几张工单」。

---

## 3.1 `struct request`

头文件注释（`:74`）说常一起访问的字段放在同一条缓存行，改字段要同步改 `blk_rq_init` 和 `blk_mq_rq_ctx_init`。下面按 `:80` 的顺序。条件编译的成员只在对应 `CONFIG_*` 打开时存在。

```c
struct request {
	struct request_queue *q;          /* 所属队列 */
	struct blk_mq_ctx *mq_ctx;        /* 提交时的软件队列 */
	struct blk_mq_hw_ctx *mq_hctx;    /* 映射到的硬件队列 */

	blk_opf_t cmd_flags;              /* 从 bio->bi_opf 抄来的操作码和 REQ_* */
	req_flags_t rq_flags;             /* RQF_*，块层内部，见下 */

	int tag;                          /* 驱动 tag，未分配时为 BLK_MQ_NO_TAG */
	int internal_tag;                 /* 调度器 tag */

	unsigned int timeout;             /* 超时，jiffies。来自 tag_set->timeout */

	/* 下面两个不要直接读，用 blk_rq_pos / blk_rq_bytes */
	unsigned int __data_len;          /* 总字节 */
	sector_t __sector;                /* 当前扇区 */

	struct bio *bio;                  /* 第一个 bio */
	struct bio *biotail;             /* 最后一个 bio，合并时往这里接 */

	union {
		struct list_head queuelist; /* 在软件队列或派发链表上 */
		struct request *rq_next;    /* 另一种单链表，plug 和批量提交用 */
	};

	struct block_device *part;        /* 统计记在哪个 block_device 上 */
	/* CONFIG_BLK_RQ_ALLOC_TIME：u64 alloc_time_ns，第一个 bio 开始分配 request 的时刻 */
	u64 start_time_ns;                /* 这个 request 分配完成的时刻 */
	u64 io_start_time_ns;             /* blk_mq_start_request 里记下，交给设备的时刻 */

	/* CONFIG_BLK_WBT：unsigned short wbt_flags */

	unsigned short stats_sectors;     /* 统计用的扇区数，完成时不会被清成 0 */
	unsigned short nr_phys_segments;  /* DMA 映射合并后的 sg 段数 */
	/* CONFIG_BLK_DEV_INTEGRITY：unsigned short nr_integrity_segments */
	/* CONFIG_BLK_INLINE_ENCRYPTION：crypt_ctx、crypt_keyslot */

	unsigned short ioprio;            /* 从 bio->bi_ioprio 来 */

	enum mq_rq_state state;           /* IDLE / IN_FLIGHT / COMPLETE */
	atomic_t ref;                     /* 引用，减到 0 才真正释放 */

	unsigned long deadline;           /* 超时到期的 jiffies */

	union {
		struct hlist_node hash;   /* 调度器合并哈希，进派发链表前拆掉 */
		struct llist_node ipi_list; /* 软中断完成时用 */
	};

	union {
		struct rb_node rb_node;   /* 调度器按扇区排序 */
		struct bio_vec special_vec; /* RQF_SPECIAL_PAYLOAD 时的单独载荷 */
	};

	/* 调度器私有。三个指针不够就自己再分配 */
	struct {
		struct io_cq *icq;
		void *priv[2];
	} elv;

	struct {
		unsigned int seq;
		rq_end_io_fn *saved_end_io;
	} flush;

	u64 fifo_time;                    /* mq-deadline 用来放到期时间 */

	rq_end_io_fn *end_io;             /* request 级完成，不是 bio->bi_end_io */
	void *end_io_data;
};
```

`mq_rq_state`（`blk-mq.h:68`）：`MQ_RQ_IDLE = 0`，`MQ_RQ_IN_FLIGHT = 1`，`MQ_RQ_COMPLETE = 2`。读状态用 `blk_mq_rq_state`（`:786`），它是 `READ_ONCE`。

和合并、统计有关的 `RQF_*`（`:30` 起，位号不连续）：

| 宏 | 位 | 含义 |
|---|---|---|
| `RQF_STARTED` | 1 | 驱动可能已经开始 |
| `RQF_FLUSH_SEQ` | 4 | flush 状态机生成的 |
| `RQF_MIXED_MERGE` | 5 | 合并了不同 failfast，要分开失败 |
| `RQF_MQ_INFLIGHT` | 6 | 计入在途 |
| `RQF_DONTPREP` | 7 | 不要再 prep |
| `RQF_SCHED_TAGS` | 8 | tag 在 `hctx->sched_tags` |
| `RQF_USE_SCHED` | 9 | 走 IO 调度器 |
| `RQF_FAILED` | 10 | 驱动内部错误，块层忽略 |
| `RQF_QUIET` | 11 | 出错少警告 |
| `RQF_IO_STAT` | 13 | 计入磁盘统计 |
| `RQF_HASHED` | 16 | 在调度器合并哈希里 |
| `RQF_STATS` | 17 | 记录完成时间 |
| `RQF_SPECIAL_PAYLOAD` | 18 | 数据在 `special_vec`，不在 bio 链 |
| `RQF_ZONE_WRITE_LOCKED` | 19 | 持有 zone 写锁 |
| `RQF_TIMED_OUT` | 21 | 超时回调已经跑过 |
| `RQF_RESV` | 23 | 保留 tag |

`RQF_NOMERGE_FLAGS`（`:65`）是 `RQF_STARTED | RQF_FLUSH_SEQ | RQF_SPECIAL_PAYLOAD`。已经开始或正在 flush 的请求不再参与合并。

驱动把私有数据放在 request 后面。`blk_mq_rq_to_pdu`（`blk-mq.h:947`）返回 `rq + 1` 的地址，长度是 `tag_set->cmd_size`。NVMe 的 `nvme_iod`、virtio-blk 的 `virtblk_req` 都从这里拿。

---

## 3.2 软件队列 `blk_mq_ctx`

每个 CPU 一个，挂在 `request_queue->queue_ctx`（per-cpu）。定义在 `block/blk-mq.h:18`。

```c
struct blk_mq_ctx {
	struct {
		spinlock_t		lock;
		struct list_head	rq_lists[HCTX_MAX_TYPES]; /* 按硬件队列类型分链表 */
	} ____cacheline_aligned_in_smp;

	unsigned int		cpu;                 /* 这个 ctx 属于哪颗 CPU */
	unsigned short		index_hw[HCTX_MAX_TYPES]; /* 在 hctx->ctxs[] 里的下标 */
	struct blk_mq_hw_ctx	*hctxs[HCTX_MAX_TYPES];  /* 三种类型各指向哪个 hctx */

	struct request_queue	*queue;
	struct blk_mq_ctxs	*ctxs;
	struct kobject		kobj;
} ____cacheline_aligned_in_smp;
```

`HCTX_MAX_TYPES` 是 3（`enum hctx_type`，`include/linux/blk-mq.h:457`）：

| 值 | 名字 | 什么请求走进它 |
|---|---|---|
| 0 | `HCTX_TYPE_DEFAULT` | 写、丢弃、以及没有单独读队列时的读 |
| 1 | `HCTX_TYPE_READ` | `REQ_OP_READ`，且驱动提供了读映射 |
| 2 | `HCTX_TYPE_POLL` | 带 `REQ_POLLED` |

选择函数是 `blk_mq_get_hctx_type`（`block/blk-mq.h:85`）：先看 `REQ_POLLED`，再看是不是读，否则 `DEFAULT`。`blk_mq_map_queue`（`:105`）直接读 `ctx->hctxs[type]`。取当前 CPU 的 ctx 是 `blk_mq_get_ctx`（`:142`），用 `raw_smp_processor_id()`。注释写明 ctx 是常驻的，不因为抢占就失效，所以 ctx 不一定等于「此刻正在跑的那颗 CPU」——请求可能被迁移之后还留着原来的 `mq_ctx`。

---

## 3.3 硬件队列 `blk_mq_hw_ctx`

`include/linux/blk-mq.h:288`。一条硬件队列对着设备上的一条提交通道（NVMe 的一个 I/O SQ，virtio 的一个 virtqueue）。

```c
struct blk_mq_hw_ctx {
	struct {
		spinlock_t		lock;       /* 保护 dispatch 链表 */
		struct list_head	dispatch;   /* 已经决定下发、但驱动当时没接住的请求 */
		unsigned long		state;      /* BLK_MQ_S_*：STOPPED、TAG_ACTIVE、SCHED_RESTART、INACTIVE */
	} ____cacheline_aligned_in_smp;

	struct delayed_work	run_work;       /* 稍后在别的 CPU 上再跑这条队列 */
	cpumask_var_t		cpumask;        /* 哪些 CPU 映射到这里 */
	int			next_cpu;       /* 轮转运行 run_work 时的下一颗 CPU */
	int			next_cpu_batch; /* 同一颗 CPU 上还剩几次 */

	unsigned long		flags;          /* BLK_MQ_F_*，和 tag_set 的标志同一套位 */
	void			*sched_data;    /* 调度器私有，kyber 放 kyber_hctx_data */
	struct request_queue	*queue;
	struct blk_flush_queue	*fq;          /* 这条队列上的 flush 请求 */

	void			*driver_data;   /* 驱动私有。NVMe 放 struct nvme_queue * */

	struct sbitmap		ctx_map;        /* 哪几个软件队列上有待派发请求 */
	struct blk_mq_ctx	*dispatch_from; /* none 路径轮转的起点 */
	unsigned int		dispatch_busy;  /* 驱动是否忙的滑动估计 */

	unsigned short		type;           /* HCTX_TYPE_* */
	unsigned short		nr_ctx;         /* 映射到本 hctx 的软件队列个数 */
	struct blk_mq_ctx	**ctxs;         /* 那些软件队列 */

	spinlock_t		dispatch_wait_lock;
	wait_queue_entry_t	dispatch_wait;  /* tag 不够时挂在这里等 */
	atomic_t		wait_index;

	struct blk_mq_tags	*tags;          /* 驱动 tag。派发时才分配 */
	struct blk_mq_tags	*sched_tags;    /* 调度器 tag。分配 request 时就占 */

	unsigned long		run;            /* blk_mq_sched_dispatch_requests 进来的次数 */
	unsigned int		numa_node;
	unsigned int		queue_num;      /* 本队列在 tag set 里的下标 */

	atomic_t		nr_active;      /* 共享 tag set 时的在途数 */
	struct hlist_node	cpuhp_online;
	struct hlist_node	cpuhp_dead;
	struct kobject		kobj;
	/* CONFIG_BLK_DEBUG_FS：debugfs_dir、sched_debugfs_dir */
	struct list_head	hctx_list;      /* 不用时挂在 q->unused_hctx_list */
};
```

`BLK_MQ_S_*` 与 `BLK_MQ_F_*` 共用一个 `enum`（`blk-mq.h:650`）。和本章有关的标志：

| 宏 | 值 | 含义 |
|---|---|---|
| `BLK_MQ_F_SHOULD_MERGE` | `1 << 0` | 允许合并 |
| `BLK_MQ_F_TAG_QUEUE_SHARED` | `1 << 1` | 多个 request_queue 共用 tag |
| `BLK_MQ_F_STACKING` | `1 << 2` | 叠在另一个 blk-mq 设备上 |
| `BLK_MQ_F_BLOCKING` | `1 << 5` | `queue_rq` 可能睡眠，派发时用 srcu |
| `BLK_MQ_F_NO_SCHED` | `1 << 6` | 不许配置调度器，sysfs 只显示 `none` |
| `BLK_MQ_F_NO_SCHED_BY_DEFAULT` | `1 << 7` | 单队列也不要默认 `mq-deadline` |
| `BLK_MQ_MAX_DEPTH` | 10240 | tag 深度上限 |
| `BLK_MQ_CPU_WORK_BATCH` | 8 | `next_cpu_batch` 的批量 |

`BLK_MQ_S_STOPPED` 是 0，表示 `state` 的第 0 位，不是和 `BLK_MQ_F_*` 混在同一个数值空间里给驱动填的 flags。驱动停队列用 `blk_mq_stop_hw_queue`（`blk-mq.c:2429`）。

---

## 3.4 tag 与 `blk_mq_tag_set`

一个 `blk_mq_tag_set`（`blk-mq.h:498`）可以被多块盘共用（NVMe 控制器上的多个 namespace）。驱动在 probe 时填好它，再 `blk_mq_alloc_tag_set`（`blk-mq.c:4658`）。

```c
struct blk_mq_tag_set {
	const struct blk_mq_ops	*ops;
	struct blk_mq_queue_map	map[HCTX_MAX_TYPES]; /* CPU → 硬件队列下标 */
	unsigned int		nr_maps;         /* 1 到 HCTX_MAX_TYPES */
	unsigned int		nr_hw_queues;
	unsigned int		queue_depth;     /* 每条硬件队列的 tag 数，含保留 */
	unsigned int		reserved_tags;   /* BLK_MQ_REQ_RESERVED 专用 */
	unsigned int		cmd_size;        /* request 后面留给驱动的字节 */
	int			numa_node;
	unsigned int		timeout;         /* jiffies */
	unsigned int		flags;           /* BLK_MQ_F_* */
	void			*driver_data;
	struct blk_mq_tags	**tags;          /* 每条硬件队列一套 */
	struct blk_mq_tags	*shared_tags;    /* 非空则各队列共用 */
	struct mutex		tag_list_lock;
	struct list_head	tag_list;        /* 使用这个 set 的 request_queue */
	struct srcu_struct	*srcu;           /* BLK_MQ_F_BLOCKING 时的锁 */
};
```

`struct blk_mq_queue_map`（`:444`）：`mq_map` 是长度 `nr_cpu_ids` 的数组，元素落在 `[queue_offset, queue_offset + nr_queues)`。NVMe 用 `queue_offset` 把读队列和默认队列错开，见 [08](./08-multiqueue-affinity.md)。

```c
/* include/linux/blk-mq.h:735 */
struct blk_mq_tags {
	unsigned int nr_tags;
	unsigned int nr_reserved_tags;
	unsigned int active_queues;
	struct sbitmap_queue bitmap_tags;     /* 普通 tag 的位图 */
	struct sbitmap_queue breserved_tags;  /* 保留 tag */
	struct request **rqs;                 /* tag → 正在用的 request */
	struct request **static_rqs;          /* tag → 预分配的 request 对象 */
	struct list_head page_list;           /* 分配 request 时用的页 */
	spinlock_t lock;
};
```

`blk_mq_get_tag`（`block/blk-mq-tag.c:134`）从 `sbitmap_queue` 拿一个空位。`BLK_MQ_NO_TAG` 是 `-1U`（`block/blk-mq.h:34`）。拿不到且带了 `REQ_NOWAIT`，分配失败，bio 以 `BLK_STS_AGAIN` 结束（`bio_wouldblock_error`，调用点 `blk-mq.c:2967`）。

请求对象是预分配的：`static_rqs[tag]` 在 `blk_mq_alloc_rqs`（`blk-mq.c:3445`）里建好。热路径不 `kmalloc` 一个 request，只是从位图里领一个已经存在的对象。`__blk_mq_alloc_requests`（`:439`）领到之后调用 `blk_mq_rq_ctx_init`（`:351`）填 `mq_ctx`、`mq_hctx`、`cmd_flags`、时间戳。

`blk_mq_unique_tag`（声明在 `blk-mq.h:770`）把硬件队列号放在高 16 位、tag 放在低 16 位（`BLK_MQ_UNIQUE_TAG_BITS = 16`，`:766`）。完成路径用它在共享 tag 时找回请求。

---

## 3.5 `struct request_queue` 里和 mq 有关的字段

整份结构在 `include/linux/blkdev.h:391`。下面按字段顺序，只给读这条路径时会碰到的成员；条件编译段用注释标出，顺序不跳。

```c
struct request_queue {
	struct request		*last_merge;     /* 上次成功合并的 request */
	struct elevator_queue	*elevator;       /* NULL 就是 none */

	struct percpu_ref	q_usage_counter; /* 进队列时 get，冻队列时等它归零 */

	struct blk_queue_stats	*stats;
	struct rq_qos		*rq_qos;         /* iolatency / iocost / wbt 挂在这条链上 */
	struct mutex		rq_qos_mutex;

	const struct blk_mq_ops	*mq_ops;         /* 驱动回调。NULL 表示不是 mq 队列 */

	struct blk_mq_ctx __percpu *queue_ctx;   /* 每 CPU 软件队列 */
	unsigned int		queue_depth;

	struct xarray		hctx_table;      /* 硬件队列号 → hctx */
	unsigned int		nr_hw_queues;

	void			*queuedata;      /* 驱动私有。NVMe 放 ctrl，virtio 放 virtio_blk */
	unsigned long		queue_flags;     /* QUEUE_FLAG_* */
	atomic_t		pm_only;         /* 非 0 时只处理 RQF_PM */

	int			id;
	spinlock_t		queue_lock;
	struct gendisk		*disk;
	refcount_t		refs;
	struct kobject		*mq_kobj;

	/* CONFIG_BLK_DEV_INTEGRITY：struct blk_integrity integrity */
	/* CONFIG_PM：dev、rpm_status */
	unsigned long		nr_requests;     /* 调度器可见的最大请求数 */
	unsigned int		dma_pad_mask;
	/* CONFIG_BLK_INLINE_ENCRYPTION：crypto_profile、crypto_kobject */

	unsigned int		rq_timeout;
	struct timer_list	timeout;
	struct work_struct	timeout_work;    /* blk_mq_timeout_work */

	atomic_t		nr_active_requests_shared_tags;
	struct blk_mq_tags	*sched_shared_tags;

	struct list_head	icq_list;        /* 每进程的 io_cq，BFQ 用 */
	/* CONFIG_BLK_CGROUP：blkcg_pols、root_blkg、blkg_list、blkcg_mutex */

	struct queue_limits	limits;          /* max_sectors、逻辑块大小等 */

	unsigned int		required_elevator_features;
	int			node;
	/* CONFIG_BLK_DEV_IO_TRACE：struct blk_trace *blk_trace */

	struct blk_flush_queue	*fq;
	struct list_head	flush_list;
	struct list_head	requeue_list;
	spinlock_t		requeue_lock;
	struct delayed_work	requeue_work;

	struct mutex		sysfs_lock;
	struct mutex		sysfs_dir_lock;
	struct list_head	unused_hctx_list;
	spinlock_t		unused_hctx_lock;
	int			mq_freeze_depth;

	/* CONFIG_BLK_DEV_THROTTLING：struct throtl_data *td */
	struct rcu_head		rcu_head;
	wait_queue_head_t	mq_freeze_wq;
	struct mutex		mq_freeze_lock;
	int			quiesce_depth;

	struct blk_mq_tag_set	*tag_set;
	struct list_head	tag_set_list;

	struct dentry		*debugfs_dir;
	struct dentry		*sched_debugfs_dir;
	struct dentry		*rqos_debugfs_dir;
	struct mutex		debugfs_mutex;
	bool			mq_sysfs_init_done;
};
```

`queue_is_mq`（`blkdev.h:622`）就是 `q->mq_ops` 非空。6.6 的磁盘驱动走 mq。`QUEUE_FLAG_*` 从 `:542` 起，常用的有：`QUEUE_FLAG_NONROT`（6，SSD）、`QUEUE_FLAG_IO_STAT`（7）、`QUEUE_FLAG_POLL`（16）、`QUEUE_FLAG_WC`（17，写回缓存）、`QUEUE_FLAG_FUA`（18）、`QUEUE_FLAG_STATS`（20）、`QUEUE_FLAG_NOWAIT`（29）、`QUEUE_FLAG_SQ_SCHED`（30，单队列风格的派发）。

`struct gendisk`（`blkdev.h:128`）是用户看见的盘：`disk_name`、`queue`、`part0`、`fops`。分区的 `block_device` 通过 `bd_disk` 找到它。bio 的 `bi_bdev` 可以是分区；`blk_partition_remap` 把扇区加上 `bd_start_sect` 再送给整盘队列。

`struct blk_plug`（`:976`）在任务栈上：

```c
struct blk_plug {
	struct request *mq_list;     /* 已经生成、还没下发的 request */
	struct request *cached_rq;   /* 预领的空 request，下一次 bio 可以复用 */
	unsigned short nr_ios;       /* 还打算再领几个 tag */
	unsigned short rq_count;
	bool multiple_queues;
	bool has_elevator;
	struct list_head cb_list;    /* md 的 unplug 回调 */
};
```

---

## 3.6 `blk_mq_ops`：驱动填的表

`include/linux/blk-mq.h:537`。块层只调用非空的函数指针。

```c
struct blk_mq_ops {
	blk_status_t (*queue_rq)(struct blk_mq_hw_ctx *,
				 const struct blk_mq_queue_data *);
	void (*commit_rqs)(struct blk_mq_hw_ctx *);          /* bd->last 没送到时补一脚 */
	void (*queue_rqs)(struct request **rqlist);          /* 可选的批量提交 */
	int (*get_budget)(struct request_queue *);           /* SCSI 用，NVMe 不填 */
	void (*put_budget)(struct request_queue *, int);
	void (*set_rq_budget_token)(struct request *, int);
	int (*get_rq_budget_token)(struct request *);
	enum blk_eh_timer_return (*timeout)(struct request *);
	int (*poll)(struct blk_mq_hw_ctx *, struct io_comp_batch *);
	void (*complete)(struct request *);
	int (*init_hctx)(struct blk_mq_hw_ctx *, void *, unsigned int);
	void (*exit_hctx)(struct blk_mq_hw_ctx *, unsigned int);
	int (*init_request)(struct blk_mq_tag_set *, struct request *, unsigned, unsigned);
	void (*exit_request)(struct blk_mq_tag_set *, struct request *, unsigned);
	void (*cleanup_rq)(struct request *);
	bool (*busy)(struct request_queue *);
	void (*map_queues)(struct blk_mq_tag_set *);         /* 不填则用默认 CPU 映射 */
	/* CONFIG_BLK_DEBUG_FS：void (*show_rq)(struct seq_file *, struct request *) */
};
```

`struct blk_mq_queue_data`（`:526`）只有两个字段：`struct request *rq` 和 `bool last`。`last` 为真表示这一批的最后一个，驱动可以在这里敲一次门铃（NVMe 的 SQ doorbell、virtio 的 kick）。中途失败时块层会再调 `commit_rqs`，免得设备一直等那个没来的 last。

`queue_rq` 的返回值是 `blk_status_t`。`BLK_STS_OK` 表示驱动收下了。`BLK_STS_RESOURCE` 表示系统资源不够，请求回到 `hctx->dispatch` 稍后再试。`BLK_STS_DEV_RESOURCE` 表示设备自己的槽满了，同样会重跑队列，但语义是「在途的 IO 完成就会腾出槽」。区别写在 `blk_types.h:122` 和 `:137` 的注释里。

---

## 3.7 从 bio 到 `queue_rq` 的注释路径

```text
blk_mq_submit_bio                 blk-mq.c:3014
  plug 里有 cached_rq 且队列匹配：优先复用
  可能超过 limits：__bio_split_to_limits
  bio_integrity_prep
  blk_mq_attempt_bio_merge        并进已有 request 就返回
  blk_mq_get_new_requests         :2940
    rq_qos_throttle               iolatency / wbt 可能在这里睡
    __blk_mq_alloc_requests       :439
      blk_mq_get_ctx              当前 CPU 的软件队列
      blk_mq_map_queue            按 opf 选 hctx
      blk_mq_get_tag              blk-mq-tag.c:134
      blk_mq_rq_ctx_init
  trace_block_getrq
  blk_mq_bio_to_request           :2629    把 bio 挂到 rq->bio，拷 ioprio、段数
  带 flush 标志：blk_insert_flush 可能把请求留下，函数返回
  有 plug：blk_add_rq_to_plug     :1291    先不碰硬件
  RQF_USE_SCHED，或 hctx 忙且（单队列或这次不是同步）：
    blk_mq_insert_request         :2567    进 ctx->rq_lists[type]，ctx_map 置位
    blk_mq_run_hw_queue(hctx, true)        异步跑
  否则：
    blk_mq_try_issue_directly     :2705    当前上下文里 queue_rq
```

`blk_mq_try_issue_directly` 失败且返回的不是「资源暂缺」时，会 `blk_mq_end_request` 把错误送回 bio（`:2732`）。资源暂缺则改插入软件队列再跑。

硬件队列被调度起来：

```text
blk_mq_run_hw_queue               :2296
  队列停了或 quiesce：返回
  blk_mq_sched_dispatch_requests  blk-mq-sched.c:319
    先把 hctx->dispatch 上上次没送走的请求送掉
    elevator 非空：blk_mq_do_dispatch_sched
    否则见 04
  blk_mq_dispatch_rq_list         blk-mq.c:2045
    预算（SCSI 的 get_budget）
    __blk_mq_get_driver_tag       :1794     这时才占驱动 tag
    mq_ops->queue_rq(hctx, &bd)
    BLK_STS_OK：留在驱动里
    BLK_STS_RESOURCE / DEV_RESOURCE：放回 hctx->dispatch，稍后再跑
```

驱动在 `queue_rq` 里面、硬件可见之前调用 `blk_mq_start_request`（`:1252`）：`trace_block_rq_issue`，`state = MQ_RQ_IN_FLIGHT`，`blk_add_timer`。

完成不一定在硬中断里把 bio 收尾。`blk_mq_complete_request`（`:1237`）先问 `blk_mq_complete_request_remote`（`:1203`）。需要换 CPU 时把请求挂到那颗 CPU 的 llist，再 `raise_softirq`（`BLOCK_SOFTIRQ`，`:1192`）。同一颗 CPU 上则直接 `mq_ops->complete`。软中断和 IPI 的通用机制在 [interrupts/08](../interrupts/08-softirq-tasklet.md)。

`__blk_mq_end_request`（`:1039`）在 `RQF_STATS` 时记完成时间，然后走 `end_io` 或 `blk_update_request`（`:889`）把错误和字节数推进 bio，最终 `bio_endio`。

---

## 3.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/blk-mq.h` | `struct request` | 80 | 驱动工单 |
| `include/linux/blk-mq.h` | `struct blk_mq_hw_ctx` | 288 | 硬件队列 |
| `include/linux/blk-mq.h` | `struct blk_mq_tag_set` | 498 | 驱动的队列与 tag 配置 |
| `include/linux/blk-mq.h` | `struct blk_mq_ops` | 537 | 驱动回调 |
| `include/linux/blk-mq.h` | `struct blk_mq_tags` | 735 | tag 位图和 request 池 |
| `block/blk-mq.h` | `struct blk_mq_ctx` | 18 | 每 CPU 软件队列 |
| `include/linux/blkdev.h` | `struct request_queue` | 391 | 一块盘的队列 |
| `include/linux/blkdev.h` | `struct blk_plug` | 976 | 任务暂存 |
| `block/blk-mq.c` | `__blk_mq_alloc_requests` | 439 | 拿 tag 并初始化 request |
| `block/blk-mq-tag.c` | `blk_mq_get_tag` | 134 | 从 sbitmap 领 tag |
| `block/blk-mq.c` | `blk_mq_submit_bio` | 3014 | bio → request |
| `block/blk-mq.c` | `blk_mq_insert_request` | 2567 | 插入软件队列 |
| `block/blk-mq.c` | `blk_mq_try_issue_directly` | 2705 | 不经调度器直接 `queue_rq` |
| `block/blk-mq.c` | `blk_mq_dispatch_rq_list` | 2045 | 批量调用 `queue_rq` |
| `block/blk-mq.c` | `blk_mq_start_request` | 1252 | 启动超时，标记 IN_FLIGHT |
| `block/blk-mq.c` | `blk_mq_complete_request` | 1237 | 完成，可能 IPI |
| `block/blk-mq.c` | `blk_mq_end_request` | 1056 | 结束 request 并推进 bio |
| `block/blk-mq.c` | `blk_mq_alloc_tag_set` | 4658 | 驱动 probe 时建 tag set |

---

**导航**：[← bio](./02-bio-and-bvec.md) · [目录](./README.md) · [none →](./04-sched-none.md)
