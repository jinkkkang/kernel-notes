# 6. kyber

**结论**：kyber 不按扇区排序。它把请求分成读、写、丢弃、其它四个域，每个域用一把设备范围的令牌限制在途数量；定时器看延迟直方图，p99 好就收紧令牌，p99 差或检测到拥塞就放宽。

> 实现在 `block/kyber-iosched.c`。`elevator_name` 是 `"kyber"`（`:1036`）。配置项 `CONFIG_MQ_IOSCHED_KYBER`（`block/Kconfig.iosched:10`），Kconfig 帮助文字写它适合多队列和快设备。文件头注释（`:3`）说它用可扩展的办法限制队列深度来控制延迟。

C 新手把令牌想成「这个域现在还允许几个请求待在设备里」。没有令牌就先留在每 CPU 的 `kyber_ctx_queue` 里，还能继续和后来的 bio 合并。拿到令牌才从这条链表搬到硬件队列的 `rqs[]`，再交给 `queue_rq`。

它清掉 `QUEUE_FLAG_SQ_SCHED`（`kyber_init_sched` `:422`）。派发状态在每条硬件队列的 `hctx->sched_data`（`struct kyber_hctx_data`）里，和 deadline「全队列一把锁」不同。令牌是全设备一份，放在 `kyber_queue_data`。

---

## 6.1 四个域和默认深度

```c
/* block/kyber-iosched.c:29 */
enum {
	KYBER_READ,
	KYBER_WRITE,
	KYBER_DISCARD,
	KYBER_OTHER,
	KYBER_NUM_DOMAINS,
};
```

`kyber_sched_domain`（`:196`）只看操作码：读、写、丢弃各自一档，其余（flush、zone 管理等）进 `KYBER_OTHER`。

设备范围的深度上限（`:60`）和默认延迟目标（`:70`，纳秒）：

| 域 | `kyber_depth` | `kyber_latency_targets` | `kyber_batch_size`（`:80`） |
|---|---|---|---|
| READ | 256 | `2 * NSEC_PER_MSEC`（2 ms） | 16 |
| WRITE | 128 | `10 * NSEC_PER_MSEC`（10 ms） | 8 |
| DISCARD | 64 | `5 * NSEC_PER_SEC`（5 s） | 1 |
| OTHER | 16 | 没有延迟目标 | 1 |

注释（`:53`）说明即使 NVMe 有很多 tag，也不该把深度开到设备最大值，所以这里再盖一层更小的帽。`KYBER_ASYNC_PERCENT` 是 75（`:50`）：异步请求最多占 75% 的浅深度，给同步留 25%。`kyber_limit_depth`（`:554`）在分配 tag 时对异步请求套这个浅深度。

延迟直方图的桶（`:100`）：

```text
KYBER_LATENCY_SHIFT = 2
桶宽 = target / 4
前 4 个桶（KYBER_GOOD_BUCKETS）是「不超过目标」
后 4 个桶是「超过目标」
一共 KYBER_LATENCY_BUCKETS = 8
```

两种时间（`:119`）：`KYBER_TOTAL_LATENCY` 从请求分配算起，`KYBER_IO_LATENCY` 从交给设备算起。`KYBER_OTHER` 不进直方图。

sysfs 只有两个可写目标：`read_lat_nsec`、`write_lat_nsec`（`kyber_sched_attrs`，`:892`）。单位是纳秒，直接写进 `latency_targets[]`（`:883`）。丢弃的 5 秒目标没有对应属性。

---

## 6.2 三层结构

```c
/* :133。每 CPU 一份直方图。域 × {total, io} × 8 桶 */
struct kyber_cpu_latency {
	atomic_t buckets[KYBER_OTHER][2][KYBER_LATENCY_BUCKETS];
};

/* :141。一个软件队列在某条 hctx 上的四条链表。和 ctx 的对应用 index_hw */
struct kyber_ctx_queue {
	spinlock_t lock;                              /* 保护 rq_list 和 kcq_map 的配套更新 */
	struct list_head rq_list[KYBER_NUM_DOMAINS];
} ____cacheline_aligned_in_smp;

/* :150。全队列。令牌在这里 */
struct kyber_queue_data {
	struct request_queue *q;
	dev_t dev;
	struct sbitmap_queue domain_tokens[KYBER_NUM_DOMAINS];
	unsigned int async_depth;
	struct kyber_cpu_latency __percpu *cpu_latency;
	struct timer_list timer;                     /* 聚合并改深度 */
	unsigned int latency_buckets[KYBER_OTHER][2][KYBER_LATENCY_BUCKETS];
	unsigned long latency_timeout[KYBER_OTHER];
	int domain_p99[KYBER_OTHER];                 /* 上次看到的 p99，没有样本时记着 */
	u64 latency_targets[KYBER_OTHER];
};

/* :181。每条硬件队列 */
struct kyber_hctx_data {
	spinlock_t lock;
	struct list_head rqs[KYBER_NUM_DOMAINS];     /* 已经拿到令牌、等待 queue_rq */
	unsigned int cur_domain;                     /* 正在批量派发的域 */
	unsigned int batching;                       /* 这个域已经连续派了几个 */
	struct kyber_ctx_queue *kcqs;
	struct sbitmap kcq_map[KYBER_NUM_DOMAINS];   /* 哪个 kcq 的该域非空 */
	struct sbq_wait domain_wait[KYBER_NUM_DOMAINS];
	struct sbq_wait_state *domain_ws[KYBER_NUM_DOMAINS];
	atomic_t wait_index[KYBER_NUM_DOMAINS];
};
```

令牌存在 `sbitmap_queue` 里。`kyber_get_domain_token`（`:706`）拿不到就把自己挂到这把位图的等待队列，有人归还时再唤醒硬件队列。请求上的令牌编号放在调度器私有区：`rq_set_domain_token`（`:535`），初始值 -1（`kyber_prepare_request`，`:585`）。完成或重排队时 `kyber_finish_request`（`:614`）调用 `rq_clear_domain_token`（`:540`）还回去。`requeue_request` 也指向 `kyber_finish_request`（回调表 `:1025`），重排队不能占着令牌。

---

## 6.3 插入

`kyber_insert_requests`（`:590`）：

```text
对每个 request：
  sched_domain = kyber_sched_domain(rq->cmd_flags)
  kcq = khd->kcqs[rq->mq_ctx->index_hw[hctx->type]]
  拿 kcq->lock
  trace_block_rq_insert
  BLK_MQ_INSERT_AT_HEAD：list_move 到 rq_list[domain] 头
  否则 list_move_tail
  sbitmap_set_bit(khd->kcq_map[domain], index_hw)
```

请求留在 kcq 里时还没占域令牌。`kyber_bio_merge`（`:567`）就在这些链表上尝试合并。注释在 `kyber_dispatch_cur_domain`（`:766`）：拿不到令牌时不要把请求从 kcq 搬出来，否则就失去合并的机会。

---

## 6.4 派发

`kyber_dispatch_request`（`:803`）拿 `khd->lock`：

```text
若 batching < kyber_batch_size[cur_domain]：
  继续 kyber_dispatch_cur_domain
  拿到就返回，批量还没用完

否则 batching = 0，从下一个域开始转一圈（KYBER_NUM_DOMAINS 次）：
  cur_domain 加一，到 OTHER 之后回到 READ
  谁先能派出一个，就停在那个域
一个都没有：返回 NULL
```

`kyber_dispatch_cur_domain`（`:753`）：

```text
khd->rqs[cur_domain] 上已经有请求：
  再拿一个令牌
  拿到：batching++，记下令牌，从链表摘下，返回
  拿不到：trace_kyber_throttled
否则 kcq_map 显示这个域还有请求：
  先拿令牌，拿到才 kyber_flush_busy_kcqs     :681
  把各软件队列上该域的请求倒进 rqs[]
  摘下第一个返回
没有请求或没有令牌：返回 NULL
```

`kyber_has_work`（`:849`）看四个域的 `rqs[]` 或 `kcq_map` 是否非空。

批量的意思是：读可以连续派 16 个再让写，写连续 8 个，丢弃和其它一次一个。这不是扇区顺序，只是减少域之间的切换。

---

## 6.5 定时器怎么改深度

`kyber_timer_fn`（`:277`）由 `kqd->timer` 触发。

```text
把每个 CPU 的直方图原子计数加进 kqd->latency_buckets，并清掉 per-cpu 桶
  flush_latency_buckets             :210

对 READ/WRITE/DISCARD 算 IO 延迟的 p90（calculate_percentile，:226，百分位 90）：
  p90 >= KYBER_GOOD_BUCKETS（4，即超过目标）：认为设备拥塞，bad = true
  注释 :299：用 p90 是为了不被个别离群点带跑

再对每个域算 TOTAL 延迟的 p99：
  这次没有样本（p99 < 0）且 bad：沿用上次记下的 domain_p99
  bad 时把 domain_p99 重置成 -1，避免同一窗口反复收紧
  不 bad 且算出了 p99：记下来，本轮先不动深度

  若 bad，或这个域自己的 p99 已经落到「坏桶」：
    depth = orig_depth * (p99 + 1) >> KYBER_LATENCY_SHIFT
    kyber_resize_domain             :266
      夹在 1 和 kyber_depth[域] 之间
      sbitmap_queue_resize
      trace_kyber_adjust
```

缩放是线性的，注释在 `:340`：p99 是目标的 3/4 时，深度变成当前的 3/4；p99 是目标的 2 倍时，深度翻倍。`KYBER_LATENCY_SHIFT` 是 2，右移 2 就是除以 4，和「桶号相对目标」的刻度一致。

完成时 `kyber_completed_request`（`:639`）把样本 `add_latency_sample`（`:621`）进当前 CPU 的直方图。总延迟用 `rq->start_time_ns`，IO 延迟用 `rq->io_start_time_ns`（`blk_mq_start_request` 里写的）。队列要开统计，`kyber_init_sched` 调了 `blk_stat_enable_accounting`（`:420`）。

---

## 6.6 回调表

```c
/* :1014 */
static struct elevator_type kyber_sched = {
	.ops = {
		.init_sched       = kyber_init_sched,        /* :405 */
		.exit_sched       = kyber_exit_sched,        /* :430 */
		.init_hctx        = kyber_init_hctx,         /* :464 */
		.exit_hctx        = kyber_exit_hctx,         /* :519 */
		.limit_depth      = kyber_limit_depth,       /* :554 */
		.bio_merge        = kyber_bio_merge,         /* :567 */
		.prepare_request  = kyber_prepare_request,   /* :585 */
		.insert_requests  = kyber_insert_requests,   /* :590 */
		.finish_request   = kyber_finish_request,    /* :614 */
		.requeue_request  = kyber_finish_request,
		.completed_request = kyber_completed_request,/* :639 */
		.dispatch_request = kyber_dispatch_request,  /* :803 */
		.has_work         = kyber_has_work,          /* :849 */
		.depth_updated    = kyber_depth_updated,     /* :453 */
	},
	.elevator_attrs = kyber_sched_attrs,                  /* read_lat_nsec, write_lat_nsec */
	.elevator_name = "kyber",
};
```

`kyber_init`（`:1040`）只做 `elv_register(&kyber_sched)`。源码里没有 `MODULE_ALIAS("kyber-iosched")`。`elevator_change` 在找不到已注册名字时会 `request_module("%s-iosched", name)`（`elevator.c:738`）。模块是否因此自动加载，取决于构建系统有没有另外写别名；内核树里能核对的只有 `elv_register` 这一处。

和 deadline 的差别可以收成三句。kyber 不看扇区是否连续。它的公平是「四个域各有令牌和批量」，不是「读饿了写就让写」。深度会随 p99 自己变，deadline 的 `fifo_expire` 要人去改 sysfs。

debugfs（`CONFIG_BLK_DEBUG_FS`）在硬件队列目录下有 `read_rqs`、`write_rqs`、`discard_rqs`、`other_rqs` 以及 `cur_domain`、`batching`（`kyber_hctx_debugfs_attrs`，`:1002`）。队列目录下有各域的 tokens（`:989` 一带）。

---

## 6.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `block/kyber-iosched.c` | `kyber_depth` | 60 | 四域的深度帽：256/128/64/16 |
| `block/kyber-iosched.c` | `kyber_latency_targets` | 70 | 读 2 ms、写 10 ms、丢弃 5 s |
| `block/kyber-iosched.c` | `kyber_batch_size` | 80 | 连续派发个数 |
| `block/kyber-iosched.c` | `struct kyber_queue_data` | 150 | 令牌、直方图、定时器 |
| `block/kyber-iosched.c` | `struct kyber_hctx_data` | 181 | 每硬件队列的批量与 kcq |
| `block/kyber-iosched.c` | `kyber_sched_domain` | 196 | 操作码 → 域 |
| `block/kyber-iosched.c` | `kyber_init_sched` | 405 | 分配状态，清 `QUEUE_FLAG_SQ_SCHED` |
| `block/kyber-iosched.c` | `kyber_insert_requests` | 590 | 按域挂到 kcq，不占令牌 |
| `block/kyber-iosched.c` | `kyber_get_domain_token` | 706 | 拿域令牌，没有就等待 |
| `block/kyber-iosched.c` | `kyber_dispatch_cur_domain` | 753 | 有令牌才从 kcq 倒出 |
| `block/kyber-iosched.c` | `kyber_dispatch_request` | 803 | 按批量在四个域之间转 |
| `block/kyber-iosched.c` | `kyber_timer_fn` | 277 | p90 判拥塞，p99 改深度 |
| `block/kyber-iosched.c` | `kyber_resize_domain` | 266 | 把令牌位图缩到新深度 |
| `block/kyber-iosched.c` | `kyber_completed_request` | 639 | 采样进直方图 |
| `block/kyber-iosched.c` | `kyber_sched_attrs` | 892 | `read_lat_nsec`、`write_lat_nsec` |
| `include/trace/events/kyber.h` | `trace_kyber_throttled` / `trace_kyber_adjust` | 由本文件 `CREATE_TRACE_POINTS` 引用 | 限流与调深度 |

---

**导航**：[← mq-deadline](./05-sched-mq-deadline.md) · [目录](./README.md) · [bfq →](./07-sched-bfq.md)
