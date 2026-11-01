# 7. BFQ

**结论**：BFQ 为每个同步进程（或一组协作进程）维持一条 `bfq_queue`，用 B-WF2Q+ 的虚拟时间按权重分配服务；当前队列有预算、想得太快时会把设备空转一小段，等它的下一个请求，而不是立刻把带宽让给别人。

> 调度器在 `block/bfq-iosched.c`，对象在 `block/bfq-iosched.h`，虚拟时间在 `block/bfq-wf2q.c`，cgroup 在 `block/bfq-cgroup.c`。注册名 `"bfq"`（`bfq-iosched.c:7668`），`MODULE_ALIAS("bfq-iosched")`（`:7671`）。配置项 `CONFIG_IOSCHED_BFQ`（`block/Kconfig.iosched:19`），层次调度还要 `CONFIG_BFQ_GROUP_IOSCHED`（`:30`）。

C 新手不要把 `bfq_queue` 和 `request_queue` 当成同一个东西。`request_queue` 是整块盘。`bfq_queue`（代码里常叫 bfqq）是「这一路 IO 的小队列」，里面是这个进程还没做完的 request。权重高的 bfqq 虚拟时间走得慢，更容易被选中。空转（idling）是故意不派下一个队列，等当前队列马上要来的下一个请求，这样顺序读不会被随机 IO 切碎。

和 deadline 一样，它置 `QUEUE_FLAG_SQ_SCHED`（`:7402`），注释同样是从整条 request queue 派发，不是每条硬件队列各派各的。另外它置 `ELEVATOR_FLAG_DISABLE_WBT` 并 `wbt_disable_default`（`:7404`），避免写回限流和 BFQ 的预算各管一遍。

---

## 7.1 `bfq_entity`：谁在树上被比较

`block/bfq-iosched.h:151`。叶子是 `bfq_queue`，内部节点在开了组调度时是 `bfq_group`。两者都嵌一个 entity，调度器只比较 entity。

```c
struct bfq_entity {
	struct rb_node rb_node;          /* 在 service_tree 里的位置 */
	bool on_st_or_in_serv;           /* 在树上，或正在被服务 */
	u64 start, finish;               /* B-WF2Q+ 的虚拟开始、结束，单位是扇区/权重 */
	struct rb_root *tree;            /* 当前挂在哪棵树，没挂则为 NULL */
	u64 min_start;                   /* 以本节点为根的子树里最小的 start */
	int service;                     /* 这一档已经得到的服务量 */
	int budget;                      /* 这一档的预算。finish = start + budget/weight */
	int allocated;                   /* 子树里已分配的 request 数 */
	int dev_weight;                  /* 非 0 时盖过组的默认权重 */
	int weight;
	int new_weight;                  /* 改权重进行中的下一值 */
	int orig_weight;                 /* 加权提升之前的权重 */
	struct bfq_entity *parent;       /* 层次调度的父节点 */
	struct bfq_sched_data *my_sched_data; /* 非叶子：自己的调度队列。叶子是 NULL */
	struct bfq_sched_data *sched_data;    /* 自己属于哪棵调度树 */
	int prio_changed;                /* 有权重或 ioprio 改动还没生效 */
	/* CONFIG_BFQ_GROUP_IOSCHED：bool in_groups_with_pending_reqs */
	struct bfq_queue *last_bfqq_created;
};
```

`bfq_service_tree`（`bfq-iosched.h:53`）是一棵按虚拟时间组织的树。`bfq_get_next_queue`（声明在 `:1151`，定义在 `bfq-wf2q.c`）从这些树里挑下一个。三个 ioprio 类（RT、BE、IDLE）各有各的树，类之间是严格优先：有 RT 就不会去服务 BE。类内部才按 weight 分。

---

## 7.2 `bfq_queue`：一路 IO

`bfq-iosched.h:246`。注释写明：它是叶子；可以绑一个 `io_context`，异步队列或协作进程可以共享；只含一个 actuator 的请求。字段很多，按源码顺序分成几段。保护锁是所属 `bfq_data` 的队列锁。

```c
struct bfq_queue {
	/* --- 身份与优先级 --- */
	int ref;
	int stable_ref;                  /* 延迟的稳定合并，别的队列还指着它 */
	struct bfq_data *bfqd;
	unsigned short ioprio, ioprio_class;
	unsigned short new_ioprio, new_ioprio_class;

	/* --- 注入：在服务空洞里塞别人的请求，个数有上限 --- */
	u64 last_serv_time_ns;
	unsigned int inject_limit;
	unsigned long decrease_time_jif;

	/* --- 协作合并 --- */
	struct bfq_queue *new_bfqq;      /* 和别的队列合作时指向共享队列 */
	struct rb_node pos_node;
	struct rb_root *pos_root;        /* 按请求位置找邻近队列 */

	/* --- 本队列上的 request --- */
	struct rb_root sort_list;        /* 按扇区 */
	struct request *next_rq;         /* FIFO 还没到期时下一个要服务的 */
	int queued[2];                   /* 同步、异步各有多少 */
	int meta_pending;
	struct list_head fifo;

	struct bfq_entity entity;        /* 嵌在这里，不是指针 */
	struct bfq_weight_counter *weight_counter;

	int max_budget;
	unsigned long budget_timeout;    /* jiffies，防止寻道队列占住设备 */
	int dispatched;                  /* 已派发出去、驱动里还没完成的个数 */
	unsigned long flags;             /* BFQQF_* */

	struct list_head bfqq_list;      /* 挂在 bfqd 的 active 或 idle 链表 */
	struct bfq_ttime ttime;          /* 想一想的时间，用来决定要不要空转 */

	/* --- 这一观察窗口里忙了多久、空了多久 --- */
	u64 io_start_time;
	u64 tot_idle_time;
	u32 seek_history;                /* 每一位记录一次是否寻道 */
	struct hlist_node burst_list_node;
	sector_t last_request_pos;
	unsigned int requests_within_timer;

	pid_t pid;                       /* 打日志用 */
	struct bfq_io_cq *bic;           /* 拥有它的 io_cq。共享队列这里是 NULL */

	/* --- 权重提升（weight raising）：交互和软实时 --- */
	unsigned long wr_cur_max_time;
	unsigned long soft_rt_next_start;
	unsigned long last_wr_start_finish;
	unsigned int wr_coeff;           /* 权重乘上这个系数 */
	unsigned long last_idle_bklogged;
	unsigned long service_from_backlogged;
	unsigned long service_from_wr;
	unsigned long wr_start_at_switch_to_srt;
	unsigned long split_time;
	unsigned long first_IO_time;
	unsigned long creation_time;

	/* --- waker：本队列经常在另一条队列完成后立刻来请求 --- */
	struct bfq_queue *waker_bfqq;
	struct bfq_queue *tentative_waker_bfqq;
	unsigned int num_waker_detections;
	u64 waker_detection_started;
	struct hlist_node woken_list_node;
	struct hlist_head woken_list;

	unsigned int actuator_idx;       /* 多磁头盘上属于哪个 actuator */
};
```

`enum bfqq_state_flags`（`:856`）里和路径有关的几位：`BFQQF_busy`（有请求或正在服务）、`BFQQF_wait_request`（空转等下一个请求）、`BFQQF_has_short_ttime`（想得短，值得空转）、`BFQQF_sync`、`BFQQF_IO_bound`（预算没用完就超时，说明它吃不满）、`BFQQF_in_large_burst`（和一大批队列一起醒来，不当成交互）。

`struct bfq_ttime`（`:221`）：`last_end_request`、`ttime_total`、`ttime_samples`、`ttime_mean`。平均想得比空转片还短，BFQ 才愿意为它停一下设备。

---

## 7.3 `bfq_data`：整盘的 BFQ 状态

`bfq-iosched.h:512`。一份挂在 `elevator_queue->elevator_data`。按顺序分成运行时、速率估计、可调参数、突发与权重提升。

```c
struct bfq_data {
	/* --- 队列与在途 --- */
	struct request_queue *queue;
	struct list_head dispatch;       /* 已经决定派发、还没被驱动取走 */
	struct bfq_group *root_group;
	struct rb_root_cached queue_weights_tree; /* 活跃队列的不同权重各一个计数 */
	/* CONFIG_BFQ_GROUP_IOSCHED：unsigned int num_groups_with_pending_reqs */
	unsigned int busy_queues[3];     /* RT、BE、IDLE 各有多少忙队列 */
	int wr_busy_queues;
	int queued;
	int tot_rq_in_driver;
	int rq_in_driver[BFQ_MAX_ACTUATORS];
	bool nonrot_with_queueing;
	int max_rq_in_driver;
	int hw_tag_samples;
	int hw_tag;                      /* 设备是否表现出内部队列 */
	int budgets_assigned;

	struct hrtimer idle_slice_timer; /* 空转到期 */
	struct bfq_queue *in_service_queue;
	sector_t last_position;
	sector_t in_serv_last_pos;
	u64 last_completion;
	struct bfq_queue *last_completed_rq_bfqq;
	struct bfq_queue *last_bfqq_created;
	u64 last_empty_occupied_ns;

	/* --- 注入用的一次「等首个请求」采样 --- */
	bool wait_dispatch;
	struct request *waited_rq;
	bool rqs_injected;

	/* --- 峰值速率，用来自动算预算 --- */
	u64 first_dispatch;
	u64 last_dispatch;
	ktime_t last_budget_start;
	ktime_t last_idling_start;
	unsigned long last_idling_start_jiffies;
	int peak_rate_samples;
	u32 sequential_samples;
	u64 tot_sectors_dispatched;
	u32 last_rq_max_size;
	u64 delta_from_first;
	u32 peak_rate;                   /* (扇区/微秒) << BFQ_RATE_SHIFT */
	int bfq_max_budget;

	struct list_head active_list[BFQ_MAX_ACTUATORS];
	struct list_head idle_list;

	/* --- sysfs 可调。时间多是纳秒或 jiffies，以属性的 show 函数为准 --- */
	u64 bfq_fifo_expire[2];          /* 异步、同步 FIFO 到期 */
	unsigned int bfq_back_penalty;
	unsigned int bfq_back_max;
	u32 bfq_slice_idle;              /* 最长空转 */
	int bfq_user_max_budget;         /* 0 表示自动 */
	unsigned int bfq_timeout;
	bool strict_guarantees;          /* 为了顺序保证，设备里同时只放一个请求 */

	/* --- 突发：短时间醒来的一大批队列不当成交互 --- */
	unsigned long last_ins_in_burst;
	unsigned long bfq_burst_interval;
	int burst_size;
	struct bfq_entity *burst_parent_entity;
	unsigned long bfq_large_burst_thresh;
	bool large_burst;
	struct hlist_head burst_list;

	bool low_latency;
	unsigned int bfq_wr_coeff;
	unsigned int bfq_wr_rt_max_time;
	unsigned int bfq_wr_min_idle_time;
	unsigned long bfq_wr_min_inter_arr_async;
	unsigned int bfq_wr_max_softrt_rate;
	u64 rate_dur_prod;

	struct bfq_queue oom_bfqq;       /* 极端 OOM 时的替身队列 */
	spinlock_t lock;

	struct bfq_io_cq *bio_bic;       /* 合并查找用，避免拿着调度器锁再找 bic */
	struct bfq_queue *bio_bfqq;

	unsigned int word_depths[2][2];  /* bfq_limit_depth 用的浅 tag */
	unsigned int full_depth_shift;

	unsigned int num_actuators;
	sector_t sector[BFQ_MAX_ACTUATORS];
	sector_t nr_sectors[BFQ_MAX_ACTUATORS];
	struct blk_independent_access_range ia_ranges[BFQ_MAX_ACTUATORS];
	unsigned int actuator_load_threshold;
};
```

`actuator_load_threshold` 的注释（`:832`）说明多磁头盘：某个磁头在途请求太少，而正在服务的队列并不使用它时，可以从别的队列「注入」一个针对那个磁头的请求。单磁头盘 `num_actuators` 是 1。

sysfs 属性 `bfq_attrs`（`bfq-iosched.c:7628`）：`fifo_expire_sync`、`fifo_expire_async`、`back_seek_max`、`back_seek_penalty`、`slice_idle`、`slice_idle_us`、`max_budget`、`timeout_sync`、`strict_guarantees`、`low_latency`。

cgroup 文件不是 `io.weight`。`io.weight` 属于 iocost（[cgroup/07](../cgroup/07-io-pids-misc.md)）。BFQ 自己的权重在 `block/bfq-cgroup.c`，挂上之后名字是 `io.bfq.*`。`bfq_init`（`:7673`）在 `CONFIG_BFQ_GROUP_IOSCHED` 下 `blkcg_policy_register(&blkcg_policy_bfq)`。

---

## 7.4 插入

`bfq_insert_requests`（`:6307`）对每个 request 调 `bfq_insert_request`（`:6251`）。

```text
bfq_init_rq                         :6890
  找到或分配这个进程的 bfq_queue（bfq_get_queue，:5843）
  看 ioprio 有没有变（bfq_check_ioprio_change，:5574）
  可能和位置相近的队列做稳定合并
__bfq_insert_request                :6162
  bfq_add_request                   :2215
    按扇区进 bfqq->sort_list，同时进 fifo
    更新 next_rq
    队列从空变成有请求：bfq_bfqq_handle_idle_busy_switch（:1829）
      可能开始权重提升，或认定这是一次突发
插到队头的请求进 bfqd->dispatch，不进某个 bfqq 的树
```

`bfq_limit_depth`（`:700`）对异步请求使用更浅的 tag，避免异步占满后同步队列分配不到 request。

---

## 7.5 派发

`bfq_dispatch_request`（`:5313`）拿 `bfqd->lock`，调用 `__bfq_dispatch_request`（`:5172`）。

```text
bfqd->dispatch 非空：
  摘下第一个。若它属于某个 bfqq，dispatched++，计入 rq_in_driver
  置 RQF_STARTED，返回

busy 队列数是 0：返回 NULL

strict_guarantees 且 tot_rq_in_driver > 0：
  返回 NULL
  注释 :5230：排队设备可以打乱完成顺序，
  要保证服务顺序就只能设备里同时留一个请求

bfqq = bfq_select_queue(bfqd)       :4814
  看 in_service_queue 的预算、FIFO 到期、空转定时器
  需要换人时 bfq_get_next_queue 从虚拟时间树取下一个
  可能在服务空洞里注入别的队列的一个请求

rq = bfq_dispatch_rq_from_bfqq      :5116
  取 bfqq->next_rq
  bfq_bfqq_served：按扇区把 service 加进 entity，虚拟时间往前走
  bfq_dispatch_remove               :3666    离开 sort_list 和 fifo
  IDLE 类且还有别的忙队列：立刻按预算耗尽过期
  rq_in_driver[actuator]++，tot_rq_in_driver++
  RQF_STARTED
```

`bfq_select_queue` 是厚的一部分：预算用完、超时、或 `bfq_bfqq_must_idle`（`:4653`）为真时不会立刻换队列。`bfq_better_to_idle`（`:4602`）综合「想得短」「不对称权重」「设备没有内部队列」来决定空转值不值得。空转用 `bfqd->idle_slice_timer`（hrtimer）。定时器响了还没新请求，才让出设备。

`bfq_has_work`（`:5160`）不加锁看 `bfqd->dispatch` 或 `bfqd->queued`。注释说这里竞态最多导致空跑一次派发。

完成走 `bfq_finish_request`（`:6715`）→ `bfq_finish_requeue_request`（`:6660`）→ `bfq_completed_request`（`:6361`）：减少 `dispatched` 和 `tot_rq_in_driver`，更新峰值速率，预算耗尽则 `bfq_bfqq_expire`（`:4350`）。过期原因在 `enum bfqq_expiration`（`bfq-iosched.h:904`）：预算耗尽、预算超时、想得太久、队列空了等。

---

## 7.6 回调表

```c
/* block/bfq-iosched.c:7642 */
static struct elevator_type iosched_bfq_mq = {
	.ops = {
		.limit_depth       = bfq_limit_depth,          /* :700 */
		.prepare_request   = bfq_prepare_request,      /* :6822 */
		.requeue_request   = bfq_finish_requeue_request,
		.finish_request    = bfq_finish_request,       /* :6715 */
		.exit_icq          = bfq_exit_icq,             /* :5479 */
		.insert_requests   = bfq_insert_requests,      /* :6307 */
		.dispatch_request  = bfq_dispatch_request,     /* :5313 */
		.next_request      = elv_rb_latter_request,
		.former_request    = elv_rb_former_request,
		.allow_merge       = bfq_allow_bio_merge,      /* :3246 */
		.bio_merge         = bfq_bio_merge,            /* :2456 */
		.request_merge     = bfq_request_merge,        /* :2496 */
		.requests_merged   = bfq_requests_merged,      /* :2571 */
		.request_merged    = bfq_request_merged,       /* :2514 */
		.has_work          = bfq_has_work,             /* :5160 */
		.depth_updated     = bfq_depth_updated,        /* :7169 */
		.init_hctx         = bfq_init_hctx,            /* :7178 */
		.init_sched        = bfq_init_queue,           /* :7238 */
		.exit_sched        = bfq_exit_queue,           /* :7184 */
	},
	.icq_size  = sizeof(struct bfq_io_cq),
	.icq_align = __alignof__(struct bfq_io_cq),
	.elevator_attrs = bfq_attrs,
	.elevator_name = "bfq",
};
```

`icq` 是 per-process 的 `io_cq`，BFQ 在里面记住这个进程的同步 bfqq。`bfq_io_cq` 定义在 `bfq-iosched.h:477`。进程退出时 `bfq_exit_icq` 放下这些引用。

和前两个调度器并排看：deadline 用到期时间保护延迟，不认进程。kyber 用令牌保护读延迟，也不认进程。BFQ 认进程和权重，用预算和空转换顺序带宽，代价是状态和锁都更重，并且默认关掉 wbt。多队列 SSD 上它仍把派发收成全队列一份（`QUEUE_FLAG_SQ_SCHED`），不是每条硬件队列各跑一棵树。

---

## 7.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `block/bfq-iosched.h` | `struct bfq_entity` | 151 | 虚拟时间与权重 |
| `block/bfq-iosched.h` | `struct bfq_queue` | 246 | 一路 IO 的叶子队列 |
| `block/bfq-iosched.h` | `struct bfq_data` | 512 | 整盘状态、预算、空转、突发 |
| `block/bfq-iosched.h` | `enum bfqq_state_flags` | 856 | busy、空转、同步、IO bound |
| `block/bfq-wf2q.c` | `bfq_get_next_queue` | 1454 | 从虚拟时间树取下一条 `bfq_queue`。声明在 `bfq-iosched.h:1151` |
| `block/bfq-iosched.c` | `bfq_limit_depth` | 700 | 异步请求浅 tag |
| `block/bfq-iosched.c` | `bfq_add_request` | 2215 | 进 sort_list 和 fifo |
| `block/bfq-iosched.c` | `bfq_select_queue` | 4814 | 预算、空转、注入之后决定服务谁 |
| `block/bfq-iosched.c` | `bfq_dispatch_rq_from_bfqq` | 5116 | 取出 `next_rq` 并记账 |
| `block/bfq-iosched.c` | `__bfq_dispatch_request` | 5172 | dispatch 链表优先；`strict_guarantees` 时设备里只留一个 |
| `block/bfq-iosched.c` | `bfq_dispatch_request` | 5313 | 持 `bfqd->lock` 的派发入口 |
| `block/bfq-iosched.c` | `bfq_insert_request` | 6251 | 插入入口 |
| `block/bfq-iosched.c` | `bfq_completed_request` | 6361 | 完成：速率、预算、过期 |
| `block/bfq-iosched.c` | `bfq_init_queue` | 7238 | `init_sched`，建根组，关 wbt |
| `block/bfq-iosched.c` | `bfq_attrs` | 7628 | `slice_idle`、`low_latency` 等 |
| `block/bfq-cgroup.c` | `blkcg_policy_bfq` | 由 `bfq_init` `:7673` 注册 | `io.bfq.*`，不是 `io.weight` |

---

**导航**：[← kyber](./06-sched-kyber.md) · [目录](./README.md) · [多队列 →](./08-multiqueue-affinity.md)
