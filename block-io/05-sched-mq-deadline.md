# 5. mq-deadline

**结论**：`mq-deadline` 把请求按扇区放进红黑树，同时按到期时间放进 FIFO；派发时尽量接着上一个扇区往下读，到期的请求或被饿太久的写会打断这串顺序。

> 实现全在 `block/mq-deadline.c`。注册名 `"mq-deadline"`（`:1281`），别名 `"deadline"`（`:1282`），`MODULE_ALIAS("mq-deadline-iosched")`（`:1286`）。配置项 `CONFIG_MQ_IOSCHED_DEADLINE`（`block/Kconfig.iosched:3`）。

C 新手先记两份名单。`sort_list` 回答「下一个扇区上是谁」，适合磁盘少寻道。`fifo_list` 回答「谁等得最久」，避免一个顺序流把别人饿死。同一个 request 同时出现在两份名单里。`struct request` 的 `rb_node` 挂树，`queuelist` 挂 FIFO，`fifo_time` 存到期的 jiffies（`include/linux/blk-mq.h:165`、`:184`）。

它是整块盘一份状态，不是每条硬件队列一份。`dd_init_sched` 置 `QUEUE_FLAG_SQ_SCHED`（`:742`），注释写「从 request queue 派发，而不是从 hw queue」。`dd_dispatch_request` 的注释（`:588`）说明：虽然 blk-mq 按某条硬件队列来问，返回的 request 可能属于另一条硬件队列。

---

## 5.1 对象

优先级三档，和 ioprio 类的对应在 `ioprio_class_to_prio`（`:109`）：

| `enum dd_prio`（`:48`） | 来自 |
|---|---|
| `DD_RT_PRIO = 0` | `IOPRIO_CLASS_RT` |
| `DD_BE_PRIO = 1` | `IOPRIO_CLASS_NONE` 和 `IOPRIO_CLASS_BE` |
| `DD_IDLE_PRIO = 2` | `IOPRIO_CLASS_IDLE` |

读和写各算一个方向：`DD_READ = READ`，`DD_WRITE = WRITE`（`:41`）。

```c
/* block/mq-deadline.c:62。计数溢出没关系，注释说宽度至少要盖住在途请求数的 log2 */
struct io_stats_per_prio {
	uint32_t inserted;
	uint32_t merged;
	uint32_t dispatched;
	atomic_t completed;
};

/* :73。每个优先级一份。请求同时在 sort_list 和 fifo_list 上 */
struct dd_per_prio {
	struct list_head dispatch;              /* 插到队头的请求，优先于排序 */
	struct rb_root sort_list[DD_DIR_COUNT]; /* [0] 读 [1] 写，按扇区 */
	struct list_head fifo_list[DD_DIR_COUNT];
	sector_t latest_pos[DD_DIR_COUNT];      /* 这个优先级最近派发出去的扇区 */
	struct io_stats_per_prio stats;
};

/* :82 */
struct deadline_data {
	struct dd_per_prio per_prio[DD_PRIO_COUNT];

	enum dd_data_dir last_dir;   /* 最近一次派发是读还是写 */
	unsigned int batching;       /* 当前这串顺序请求已经派了几个 */
	unsigned int starved;        /* 读连续抢在写前面的次数 */

	int fifo_expire[DD_DIR_COUNT]; /* 读、写各自的到期长度，jiffies */
	int fifo_batch;                /* 一串顺序最多几个 */
	int writes_starved;            /* 读可以抢写这么多次，然后必须写 */
	int front_merges;              /* 非 0 才尝试前向合并 */
	u32 async_depth;               /* 写和异步请求的浅 tag 深度 */
	int prio_aging_expire;         /* 低优先级被高优先级挡住多久后允许插队 */

	spinlock_t lock;
	spinlock_t zone_lock;
};
```

`dd_init_sched`（`:705`）把初始值抄自文件顶部的常量（`:30` 起）：

| 字段 | 初始值 | 行 |
|---|---|---|
| `fifo_expire[DD_READ]` | `HZ / 2` | `:30`，赋在 `:731` |
| `fifo_expire[DD_WRITE]` | `5 * HZ` | `:31`，`:732` |
| `writes_starved` | 2 | `:37`，`:733` |
| `front_merges` | 1 | `:734` |
| `fifo_batch` | 16 | `:38`，`:736` |
| `prio_aging_expire` | `10 * HZ` | `:36`，`:737` |
| `last_dir` | `DD_WRITE` | `:735` |

读的到期更短：注释说这些限制是软的（`:31`）。到期不是「到点就失败」，而是「到点就不再让顺序流插队，先把这个 FIFO 头送走」。

sysfs 在调度器目录下，属性表 `deadline_attrs`（`:1036`）：`read_expire`、`write_expire`、`writes_starved`、`front_merges`、`async_depth`、`fifo_batch`、`prio_aging_expire`。时间以毫秒进出，内部是 jiffies（`SHOW_JIFFIES` / `STORE_JIFFIES`，`:991`、`:1021`）。

---

## 5.2 插入

`dd_insert_requests`（`:877`）拿 `dd->lock`，对链表里每个 request 调 `dd_insert_request`（`:808`）。

```text
按 ioprio 类选 per_prio                         :827
第一次插入：stats.inserted++，elv.priv[0] 设成非空  :829
blk_mq_sched_try_insert_merge：能并进已有 request 就返回
trace_block_rq_insert
BLK_MQ_INSERT_AT_HEAD：
  放进 per_prio->dispatch，fifo_time = jiffies     :839
否则：
  deadline_add_rq_rb          按扇区进 sort_list
  可合并：elv_rqhash_add，可能更新 q->last_merge
  fifo_time = jiffies + fifo_expire[读或写]        :856
  尾插进 fifo_list[方向]
  zoned 写：若下一个请求在同一 zone，插到它前面    :858 起
```

zone 写锁如果是重排队带进来的，插入前先 `blk_req_zone_write_unlock`（`:825`）。

`dd_limit_depth`（`:642`）在分配 tag 时调用。同步读不限。写和异步请求把 `shallow_depth` 设成 `async_depth` 换算后的浅深度，避免它们把同步读的 tag 占光。`dd_depth_updated`（`:658`）在队列的 `nr_requests` 变化时把 `async_depth` 设成 `q->nr_requests`。

---

## 5.3 派发

入口是 `dd_dispatch_request`（`:596`），挂在 `elevator_mq_ops.dispatch_request` 上（`:1262`）。blk-mq 在 `blk_mq_do_dispatch_sched` 里一次次问它要一个 request。

```text
spin_lock(&dd->lock)
先 dd_dispatch_prio_aged_requests          :564
  至少两个优先级里有请求时
  从 BE 到 IDLE 试 __dd_dispatch_request
  latest_start = now - prio_aging_expire
  也就是：低优先级里「很早以前就插入」的可以越过还没到期的高优先级
然后按 prio 从 0 到 DD_PRIO_MAX：
  rq = __dd_dispatch_request(per_prio, now)
  若拿到 rq，或这一档 dd_queued 仍非 0：停止
  高优先级还有人在排队时，不去看更低的一档     :612 的条件和 :609 的注释
spin_unlock
```

`__dd_dispatch_request`（`:443`）在一个优先级内部决定读还是写、顺序还是 FIFO：

```text
per_prio->dispatch 非空：
  取第一个。若它的开始时间晚于 latest_start：返回 NULL
  否则摘下来，算完成

还在一批顺序里（batching < fifo_batch）且 sort_list 里有「下一块」：
  继续同一方向                              :467

否则重新选方向：
  有读：
    若写的 FIFO 也有，且 starved 已经达到 writes_starved：改去写
    否则选读，starved++
  没有读、有写：选写，starved = 0
  都没有：返回 NULL

选中方向之后：
  若该方向 FIFO 头已到期，或排序树里没有「下一块」：
    取 FIFO 里最早到期的那个                 deadline_check_fifo :303
  否则：
    取排序树里紧挨 latest_pos 的下一个
  zoned 写可能暂时都不能派发，这时 rq 为 NULL

batching++，从树和 FIFO 里摘掉（deadline_move_request）
记下 latest_pos，stats.dispatched++
需要的话 blk_req_zone_write_lock
rq->rq_flags |= RQF_STARTED
```

`started_after`（`:429`）用 `fifo_time - fifo_expire[方向]` 当作插入时刻，和 `latest_start` 比。老化路径传入的 `latest_start` 是过去的一个点，所以只有足够老的请求才出得去；普通路径传入 `now`，相当于不因「太新」而拒绝。

读连续赢过写的次数到了 `writes_starved`（默认 2）就必须派一次写。一串顺序默认最多 `fifo_batch`（16）个，然后重新看 FIFO 有没有到期。

`dd_has_work`（`:969`）看三个优先级的 dispatch、fifo 是否非空。blk-mq 用它决定还要不要再问 `dispatch_request`。

---

## 5.4 完成与合并

`dd_prepare_request`（`:899`）把 `elv.priv[0]` 清掉，让重排队时 `inserted` 计数还能再加。`dd_finish_request`（`:932`）增加 `stats.completed`。

合并：

| 回调 | 行 | 作用 |
|---|---|---|
| `dd_bio_merge` | 788 | bio 到来时尝试并进已有 request |
| `dd_request_merge` | 756 | 在排序树里找能接上的 request |
| `dd_request_merged` | 227 | 合并后在树上重新定位 |
| `dd_merged_requests` | 247 | 两个 request 合成一个，被吃掉的那个离开 FIFO |

`front_merges` 为 0 时不做前向合并。后向合并（bio 接在 request 尾部，扇区变大）仍然按电梯的通用规则走。

`elevator_features` 含 `ELEVATOR_F_ZBD_SEQ_WRITE`（`:1283`）。需要顺序写 zone 的队列会优先挑带这个特性的调度器（`elevator_get_by_features`，`elevator.c:586`）。

---

## 5.5 回调表

```c
/* block/mq-deadline.c:1257 */
static struct elevator_type mq_deadline = {
	.ops = {
		.depth_updated    = dd_depth_updated,     /* :658 */
		.limit_depth      = dd_limit_depth,       /* :642 */
		.insert_requests  = dd_insert_requests,   /* :877 */
		.dispatch_request = dd_dispatch_request,  /* :596 */
		.prepare_request  = dd_prepare_request,   /* :899 */
		.finish_request   = dd_finish_request,    /* :932 */
		.next_request     = elv_rb_latter_request,
		.former_request   = elv_rb_former_request,
		.bio_merge        = dd_bio_merge,         /* :788 */
		.request_merge    = dd_request_merge,     /* :756 */
		.requests_merged  = dd_merged_requests,   /* :247 */
		.request_merged   = dd_request_merged,    /* :227 */
		.has_work         = dd_has_work,          /* :969 */
		.init_sched       = dd_init_sched,        /* :705 */
		.exit_sched       = dd_exit_sched,        /* :676 */
		.init_hctx        = dd_init_hctx,         /* :670 */
	},
	.elevator_attrs = deadline_attrs,                 /* :1036 */
	.elevator_name = "mq-deadline",
	.elevator_alias = "deadline",
	.elevator_features = ELEVATOR_F_ZBD_SEQ_WRITE,
};
```

`init_hctx` 只调 `dd_depth_updated`，不在每条硬件队列上再放一份 `deadline_data`。状态在 `eq->elevator_data`，全队列共享，所以多队列盘上它仍像单队列调度器那样全局排序。这和 kyber「每条 hctx 一份」不同，见 [06](./06-sched-kyber.md)。

---

## 5.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `block/mq-deadline.c` | `struct deadline_data` | 82 | 全队列的到期与批量状态 |
| `block/mq-deadline.c` | `struct dd_per_prio` | 73 | 一个优先级的树、FIFO、dispatch |
| `block/mq-deadline.c` | `dd_init_sched` | 705 | 分配状态，置 `QUEUE_FLAG_SQ_SCHED` |
| `block/mq-deadline.c` | `dd_insert_request` | 808 | 进红黑树和 FIFO，写下 `fifo_time` |
| `block/mq-deadline.c` | `dd_insert_requests` | 877 | 调度器插入回调 |
| `block/mq-deadline.c` | `__dd_dispatch_request` | 443 | 一批顺序、到期、读写轮换 |
| `block/mq-deadline.c` | `dd_dispatch_prio_aged_requests` | 564 | 低优先级老化后允许先走 |
| `block/mq-deadline.c` | `dd_dispatch_request` | 596 | 先老化，再按 RT、BE、IDLE |
| `block/mq-deadline.c` | `dd_limit_depth` | 642 | 写和异步走浅 tag |
| `block/mq-deadline.c` | `deadline_attrs` | 1036 | sysfs：`read_expire` 等 |
| `block/mq-deadline.c` | `read_expire` 等常量 | 30 | 默认 `HZ/2`、`5*HZ`、batch 16 |
| `block/elevator.h` | `struct elevator_mq_ops` | 26 | 上面这些函数填进去的表 |

---

**导航**：[← none](./04-sched-none.md) · [目录](./README.md) · [kyber →](./06-sched-kyber.md)
