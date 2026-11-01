# 17. 性能分析配方

**结论**：先分清时间花在排队、调度器、还是设备上，再决定改队列深度、换调度器，或把同步 IO 换成 io_uring。下面的命令都是 **RECIPE**，没有对着 v6.6.160 跑出过输出。

事件名以 [16](./16-observability.md) 的头文件为准。数字列的含义以 `part_stat_show`（`block/genhd.c:960`）为准。

---

## 17.1 先分类

```text
1. /sys/block/<disk>/stat 的 inflight 是否长期贴着设备的队列深度？
2. block_rq_insert 到 block_rq_issue 长，还是 issue 到 complete 长？
3. 当前调度器是 none、mq-deadline、kyber 还是 bfq？
4. 应用是每次 read/write 进一次内核，还是 io_uring / libaio 一批提交？
```

| 现象 | 下一跳 |
|---|---|
| inflight 很低，issue→complete 已经几十到几百微秒，CPU 却花在系统调用上 | 17.5。先看提交方式，不是先换调度器 |
| inflight 顶满，insert→issue 很长，issue→complete 也长 | 设备或队列深度。17.2 |
| insert→issue 很长，issue→complete 很短 | 请求堆在调度器或软件队列。17.3、17.4 |
| 读延迟抖动、写很猛 | kyber 的读域令牌，或 deadline 的 `writes_starved`。17.4 |
| 多进程抢同一块盘、要权重 | bfq。单线程吞吐不是它的目标 |

`none` 在硬件队列空闲时会 `blk_mq_try_issue_directly`（`blk-mq.c:2705`），insert 事件可能很少。这时不要因为看不到 `block_rq_insert` 就认为没有排队：plug 里的请求在 unplug 时才一起下去，排队发生在任务自己的 `blk_plug` 上。

---

## 17.2 队列深度

设备能同时消化的命令数，上限是 `tag_set->queue_depth`（`include/linux/blk-mq.h:503`），还要再被调度器盖一层。

| 调度器 | 深度还被谁限制 |
|---|---|
| `none` | 主要是 tag。没有每域令牌 |
| mq-deadline | 同步读不限。写和异步走 `async_depth` 换算的浅 tag（`dd_limit_depth`，`mq-deadline.c:642`） |
| kyber | 四域令牌，帽是 256/128/64/16（`kyber_depth`，`kyber-iosched.c:60`），定时器按 p99 缩放 |
| bfq | `strict_guarantees` 为真时设备里同时只留一个请求（`bfq-iosched.c:5241`）。默认还会关掉 wbt |

NVMe 的 tag 超时默认 30 秒（`nvme_io_timeout`，`drivers/nvme/host/core.c:50`），这是等完成的上限，不是队列深度。队列深度在驱动 probe 时按控制器的最大队列项数填进 `queue_depth`。

```bash
# RECIPE：mq 队列的 nr_requests 是调度器可见的请求上限
cat /sys/block/<disk>/queue/nr_requests

# RECIPE：每条硬件队列的 cpumask 和是否 busy，在 mq 目录下
# 目录由 blk_mq_sysfs 创建，名字是硬件队列号
ls /sys/block/<disk>/mq/
```

inflight 长期等于深度，同时 `block_rq_complete` 的间隔稳定，说明设备在满负荷工作。再加大 `nr_requests` 只会让更多请求堆在软件队列里，insert→issue 变长，issue→complete 不一定变短。

inflight 很低而延迟高，先看是不是同步提交：每个 IO 都要等上一个完成才发下一个，深度用不满。这是 17.5 的问题。

---

## 17.3 延迟直方图

```bash
# RECIPE：issue 到 complete。字段以 include/trace/events/block.h 为准
perf record -e block:block_rq_issue,block:block_rq_complete -a -- sleep 3
perf script
```

读的时候把同一条请求的 issue 和 complete 配成一对。只看平均值会把「大量很快的读」和「少量很慢的写」抹平。至少按 `rwbs` 里读还是写分开。discard 和 flush 不要和读写画在同一张图里：flush 的 complete 包括把写缓存推下去的时间。

```bash
# RECIPE：把排队也算上。insert 可能不存在于直接下发的同步 IO
perf record -e block:block_rq_insert,block:block_rq_issue,block:block_rq_complete -a -- sleep 3
perf script
```

bio 视角（含拆分和调度）：

```bash
# RECIPE
perf record -e block:block_bio_queue,block:block_bio_complete -a -- sleep 3
perf script
```

`block_bio_queue` 到 `block_getrq` 很长：卡在限流（`blk_throtl_bio`）或 `rq_qos_throttle`。那是 cgroup 的 `io.max` / `io.latency` / iocost，见 [09](./09-ioprio-and-cgroup-io.md)，不是设备变慢。

bpftrace 的直方图同样是 RECIPE。在 issue 记时间戳，在 complete 做差，按读写分开。本环境没有跑过，不写具体脚本。

---

## 17.4 选哪个调度器

默认规则在 `elevator_get_default`（`block/elevator.c:570`）：多硬件队列是 `none`，单队列且没有 `BLK_MQ_F_NO_SCHED_BY_DEFAULT` 时找 `mq-deadline`。这是注册时的选择，不是对工作负载的判断。

| 想要的行为 | 6.6 里对应的名字 | 不要指望它做的事 |
|---|---|---|
| 少一层锁和排序，让多队列设备自己调度 | `none` | 不看 ioprio，不保证读不被写挡住 |
| 旋转盘或想要「顺序优先，但到期必须走」 | `mq-deadline` | 不按进程权重分带宽。多队列上仍是全盘一把锁（`QUEUE_FLAG_SQ_SCHED`） |
| 快设备上把读延迟压在目标附近 | `kyber` | 不按扇区排序。调的是令牌深度，不是到期时间 |
| 多进程按权重分享，交互进程要低延迟 | `bfq` | 单线程顺序吞吐。它会空转等下一个请求，也会关掉 wbt |

```bash
# RECIPE：先读再写。写会冻队列
cat /sys/block/<disk>/queue/scheduler
echo mq-deadline > /sys/block/<disk>/queue/scheduler
echo none > /sys/block/<disk>/queue/scheduler
```

切换的代码路径是 `elv_iosched_store` → `elevator_change`（`elevator.c:748`、`:718`）。失败时队列留在 `none`，内核有 `falling back to "none"` 的警告。

改 deadline 的软到期（单位毫秒，内部是 jiffies）：

```bash
# RECIPE：属性名来自 deadline_attrs（mq-deadline.c:1036）
cat /sys/block/<disk>/queue/iosched/read_expire
cat /sys/block/<disk>/queue/iosched/write_expire
cat /sys/block/<disk>/queue/iosched/writes_starved
cat /sys/block/<disk>/queue/iosched/fifo_batch
```

默认读 `HZ/2`、写 `5*HZ`、`writes_starved` 为 2、`fifo_batch` 为 16（`mq-deadline.c:30`）。读被写饿，是 `writes_starved` 和写的 `fifo_expire` 的事。写被读饿，是读可以连续赢过写的次数到了没有。

kyber 的目标是纳秒：

```bash
# RECIPE：kyber_sched_attrs（kyber-iosched.c:892）
cat /sys/block/<disk>/queue/iosched/read_lat_nsec
cat /sys/block/<disk>/queue/iosched/write_lat_nsec
```

默认读 2 ms、写 10 ms（`kyber_latency_targets`，`:70`）。把目标改得很小会让 `kyber_timer_fn` 更容易把深度往下收（p99 落到坏桶，或 p90 判定拥塞）。深度帽仍然是 `kyber_depth`，不会超过 256/128。

bfq 上先看是不是空转太多：

```bash
# RECIPE
cat /sys/block/<disk>/queue/iosched/slice_idle
cat /sys/block/<disk>/queue/iosched/low_latency
cat /sys/block/<disk>/queue/iosched/strict_guarantees
```

`strict_guarantees` 为真时设备里只留一个请求（`__bfq_dispatch_request`，`:5241`）。延迟顺序更接近调度器的决定，吞吐会掉。SSD 上若空转没有带来顺序收益，`slice_idle` 是要看的旋钮，不是先去加队列深度。

ioprio 只在 deadline 和 bfq 上改变顺序。`none` 和 kyber 不读 `rq->ioprio` 来挑请求。cgroup 的 `io.max` 在调度器之前就生效，换调度器不会绕过它。

---

## 17.5 io_uring、同步 IO、libaio

三条路在块层下面汇合，差别在提交和等待。

| 方式 | 每次 IO 的内核入口 | 等待 |
|---|---|---|
| `read` / `write` | `vfs_read` / `vfs_write`，同步 kiocb，`ki_complete == NULL` | 系统调用里睡到 bio 完成 |
| libaio `io_submit` | `fs/aio.c:2041`，`aio_read` 里 `call_read_iter` | `io_getevents` 另一次系统调用 |
| io_uring | `io_uring_enter` 一次可以提交多个 SQE。`io_read` 仍是 `read_iter` | CQ 在共享内存里，可以不进内核就收割 |

同步 `read` 的深度是 1：下一个 bio 要等这一个的 `ki_complete` 路径把系统调用叫醒。inflight 低、延迟「看起来正常」但 IOPS 上不去，先确认是不是这种用法。

io_uring 要真正并行，需要：

```text
多个 SQE 同时在飞，而不是提交一个等一个 CQE
文件是 O_DIRECT，或块设备 direct。缓冲读仍可能在页缓存里等
若用 IORING_SETUP_IOPOLL：设备要有 HCTX_TYPE_POLL 队列
  NVMe 的 poll 队列数来自驱动参数，virtio 来自 poll_queues 模块参数
固定缓冲（IORING_REGISTER_BUFFERS）省掉每次钉页
SQE 的 IOSQE_ASYNC 会进 io-wq，那是内核线程，不是「更少的系统调用」
```

`IORING_OP_READ` 的 `opdef` 把 `plug` 设成 1（`io_uring/opdef.c:233`）。一批读写会进同一个 `blk_plug`，unplug 时一起下发，相邻的才有机会合并。一个一个 enter 的写法和同步 IO 在合并上差不多。

对照经典 AIO：`io_submit` 每次把 `iocb` 从用户内存拷进来，完成靠 `io_getevents` 再拷出去。io_uring 的环是 mmap 的。两者的 `read_iter` 相同，见 [12](./12-io-uring.md) §12.7。libaio 没有 `IORING_SETUP_IOPOLL` 这条轮询完成的路径。

```bash
# RECIPE：比较同步和 io_uring 时，块层事件是同一套
# 应用侧的差异看 io_uring:io_uring_submit_req 的密度，
# 以及 block:block_rq_issue 是否在时间上叠在一起
perf record -e io_uring:io_uring_submit_req,io_uring:io_uring_complete,block:block_rq_issue,block:block_rq_complete -a -- sleep 3
perf script
```

`io_uring_complete` 早于 `block_rq_complete` 并不奇怪：缓冲读命中页缓存时根本没有 block 事件。只有 direct 或未命中才会出现成对的 block 事件。

fdinfo 里 `SQEs` 很大：内核还没取走提交，SQPOLL 线程可能睡了（看环上的 `IORING_SQ_NEED_WAKEUP`）或 io-wq 满了。`CQEs` 很大：用户没有收割，环会走到 `io_uring_cqe_overflow`（`include/trace/events/io_uring.h:571`）。

```bash
# RECIPE
cat /proc/<pid>/fdinfo/<uring-fd>
```

---

## 17.6 和中断、CPU 绑在一起看

多队列设备上，完成 IRQ 打在远离提交 CPU 的核上时，`blk_mq_complete_request_remote` 还会再发一次 IPI，把软中断送到 `QUEUE_FLAG_SAME_COMP` 要求的 CPU。现象是 `block_rq_complete` 的 CPU 和 `block_rq_issue` 的 CPU 不同，同时 `/proc/interrupts` 里 NVMe 或 virtio 的行集中在少数核。

亲和怎么来的见 [08](./08-multiqueue-affinity.md)。向量和 `effective_affinity` 的读法在 [interrupts/12](../interrupts/12-observability.md)。先读 `mq/*/cpu_list` 和中断的有效亲和，再决定要不要减队列数。减队列是少占 MSI-X 向量，不是把所有 IRQ 手写到同一颗 CPU。

```bash
# RECIPE
grep . /sys/block/<disk>/mq/*/cpu_list
```

轮询完成（IOPOLL）时这条 IRQ 应该几乎不涨。若 IOPOLL 开了而中断照样按 IOPS 涨，请求没有进 `HCTX_TYPE_POLL`：要么队列没建 poll 队列，要么 bio 没有 `REQ_POLLED`。`iomap_dio_submit_bio` 只在 `IOCB_HIPRI` 且 kiocb 异步时调用 `bio_set_polled`（`fs/iomap/direct-io.c:72`）。同步 `read` 带 HIPRI 不会走轮询。

---

## 17.7 关键函数表

| 文件 | 符号 | 行号 | 和配方的关系 |
|---|---|---|---|
| `block/genhd.c` | `part_stat_show` | 960 | `stat` 的 inflight 和第 11 列 |
| `block/elevator.c` | `elevator_get_default` | 570 | 多队列默认 `none` |
| `block/elevator.c` | `elv_iosched_store` | 748 | 写 `scheduler` 文件 |
| `block/blk-mq.c` | `blk_mq_try_issue_directly` | 2705 | `none` 空闲时没有 insert |
| `block/mq-deadline.c` | `dd_limit_depth` | 642 | 写和异步的浅深度 |
| `block/kyber-iosched.c` | `kyber_timer_fn` | 277 | 按 p99 改令牌 |
| `block/bfq-iosched.c` | `__bfq_dispatch_request` | 5172 | `strict_guarantees` 时只留一个在途 |
| `fs/iomap/direct-io.c` | `iomap_dio_submit_bio` | 64 | HIPRI 才标 `REQ_POLLED` |
| `io_uring/opdef.c` | `IORING_OP_READ` | 228 | `plug` 和 `iopoll` 都打开 |
| `include/trace/events/block.h` | `block_rq_issue` / `block_rq_complete` | 229 / 149 | 设备侧延迟 |
| `include/trace/events/io_uring.h` | `io_uring_cqe_overflow` | 571 | CQ 收割不及 |

---

**导航**：[← 观测](./16-observability.md) · [目录](./README.md)
