# 16. 观测

**结论**：每块盘的累计计数在 `/sys/block/<disk>/stat`，由 `part_stat_show` 按固定的 17 列打印；更细的单次请求轨迹用 `block` tracepoint（blktrace 读的就是它们）。io_uring 在 6.6 里没有单独的 `/proc` 全局计数器，能看的是每个环 fd 的 `fdinfo`、两个 sysctl，以及 `io_uring` tracepoint。

> 打印函数是 `block/genhd.c` 的 `part_stat_show`（`:960`）。计数结构是 `struct disk_stats`（`include/linux/part_stat.h:8`）。tracepoint 在 `include/trace/events/block.h` 和 `include/trace/events/io_uring.h`。

下面的命令都是 **RECIPE**。本环境没有对着 v6.6.160 的块设备采集过输出，所以不贴样例数字。若在 6.12 主机上读到了列，列名以 v6.6.160 的 `sprintf` 为准，两套内核的列顺序要单独核对，不要把 6.12 的数字写进 6.6 的行号旁边。

`iostat` 读的就是这个 sysfs 文件。工具自己的列名（`rkB/s` 等）是用户态算出来的，内核只提供下面这些原始列。

---

## 16.1 `struct disk_stats`

```c
/* include/linux/part_stat.h:8
 * NR_STAT_GROUPS 是 4：STAT_READ、STAT_WRITE、STAT_DISCARD、STAT_FLUSH
 * （include/linux/blk_types.h:466）
 */
struct disk_stats {
	u64 nsecs[NR_STAT_GROUPS];          /* 花在这一类 IO 上的纳秒 */
	unsigned long sectors[NR_STAT_GROUPS];
	unsigned long ios[NR_STAT_GROUPS];  /* 完成的次数 */
	unsigned long merges[NR_STAT_GROUPS];
	unsigned long io_ticks;             /* 有 IO 在飞的时间，jiffies 累加 */
	local_t in_flight[2];               /* [0] 读 [1] 写，正在飞的请求数 */
};
```

每个 `block_device` 一份 per-cpu 数组，指针是 `bd_stats`（`blk_types.h:45`）。`part_stat_read`（`part_stat.h:34`）把所有 CPU 加起来。更新计数的一侧要 `part_stat_lock` / `part_stat_unlock`，这两个宏就是 `preempt_disable` / `preempt_enable`（`:25`）。

`in_flight` 只有读和写两格。discard 和 flush 不进这个数组。sysfs 里单独打印的 inflight 来自 `blk_mq_in_flight`（mq 队列）或 `part_in_flight`，见 `part_stat_show` `:968`。

---

## 16.2 `/sys/block/<disk>/stat` 的 17 列

`part_stat_show`（`block/genhd.c:960`）的格式串是四段：

```text
%8lu %8lu %8llu %8u     读：ios、merges、sectors、毫秒
%8lu %8lu %8llu %8u     写：ios、merges、sectors、毫秒
%8u %8u %8u             inflight、io_ticks 毫秒、各类 nsecs 之和的毫秒
%8lu %8lu %8llu %8u     discard：ios、merges、sectors、毫秒
%8lu %8u                flush：ios、毫秒
```

毫秒由纳秒除以 `NSEC_PER_MSEC` 得到（`:989`）。扇区仍是 512 字节。`io_ticks` 用 `jiffies_to_msecs`（`:995`）。第 11 列是读、写、discard、flush 四类 `nsecs` 的和，不是 `io_ticks`。两者不同：`io_ticks` 是「这段时间里有没有 IO」，多请求并行时不会按请求数相乘；第 11 列把每个请求的服务时间加在一起，并行时可以比墙钟更长。

分区有自己的 `stat`，属性挂在 `block/partitions/core.c:209` 和整盘的 `block/genhd.c:1072`，处理函数都是 `part_stat_show`。读分区文件得到的是这个 `block_device` 的 `bd_stats`，不是把整盘再拆一遍。

旁边的 `inflight` 文件是 `part_inflight_show`（`genhd.c:1009`），两列：读、写。它用 `blk_mq_in_flight_rw`（`blk-mq.c:116`）。

```bash
# RECIPE：看列，不要把输出当成 v6.6.160 的实测
cat /sys/block/<disk>/stat
cat /sys/block/<disk>/<part>/stat
cat /sys/block/<disk>/inflight
```

调度器名字：

```bash
# RECIPE
cat /sys/block/<disk>/queue/scheduler
```

读路径是 `elv_iosched_show`（`block/elevator.c:764`）。方括号里的是当前项。`none` 表示 `q->elevator == NULL`。写同一个文件会 `elevator_change`，见 [04](./04-sched-none.md)。

mq-deadline 的 `read_expire` 等在调度器子目录，属性表 `deadline_attrs`（`mq-deadline.c:1036`）。kyber 是 `read_lat_nsec`、`write_lat_nsec`（`kyber-iosched.c:892`）。bfq 是 `slice_idle`、`low_latency` 等（`bfq-iosched.c:7628`）。`none` 没有这个目录。

---

## 16.3 iostat 在读什么

`iostat` 打开的是上面的 `stat`。它用两次读取的差除以时间，得到每秒次数和吞吐。扇区乘 512 再换成 kB 是工具的换算，内核列是扇区。

```bash
# RECIPE：-x 会多打 await、aqu-sz 等派生列
iostat -xz 1 <disk>
```

对照时：

| iostat 里常见的派生量 | 内核列 |
|---|---|
| 读/写 IOPS | 两次 `ios` 的差 |
| 读/写吞吐 | 两次 `sectors` 的差 × 512 |
| 队列里还有多少 | `inflight`，或 `inflight` 文件的两列 |
| await 一类的平均等待 | 工具用第 4、8 列的毫秒差除以 ios 差。第 11 列是四类时间之和，和单类的毫秒不是同一个数 |

`merges` 增加说明 bio 并进了已有 request，少了一次下发。`none` 主要靠 plug 合并，调度器还有排序树里的合并。合并多不多不能单独证明调度器「好坏」，它只说明相邻请求碰到了一起。

---

## 16.4 blktrace 与 tracepoint

`CONFIG_BLK_DEV_IO_TRACE` 打开时，`request_queue` 里有 `struct blk_trace *blk_trace`（`blkdev.h:489`）。blktrace 用户态工具通过 ioctl 打开它。内核侧的事件名就是 ftrace 里的 `block:` 事件，定义在 `include/trace/events/block.h`。

和一次请求生命周期对得上的：

| 事件 | 行 | 何时 |
|---|---|---|
| `block_bio_queue` | 383 | `submit_bio_noacct_nocheck` 入队前 |
| `block_bio_frontmerge` / `block_bio_backmerge` | 372 / 361 | bio 并进已有 request |
| `block_split` | 469 | bio 被拆开 |
| `block_getrq` | 394 | 分配到 request |
| `block_rq_insert` | 215 | 插进调度器或软件队列 |
| `block_rq_issue` | 229 | `blk_mq_start_request` |
| `block_rq_complete` | 149 | request 完成 |
| `block_rq_error` | 165 | 带错误完成 |
| `block_bio_complete` | 284 | `bio_endio` |
| `block_plug` / `block_unplug` | 407 / 452 | 任务暂存与放出 |
| `block_bio_remap` / `block_rq_remap` | 507 / 549 | 分区或叠层改了设备号和扇区 |

```bash
# RECIPE：需要 CONFIG_BLK_DEV_IO_TRACE，并且当前用户能开 trace
blktrace -d /dev/<disk> -o - | blkparse -i -

# RECIPE：不用 blktrace 工具，直接看 ftrace 事件
# 事件名以 include/trace/events/block.h 为准
perf record -e block:block_rq_issue,block:block_rq_complete -a -- sleep 2
perf script
```

`block_rq_issue` 到 `block_rq_complete` 的时间是请求在驱动和设备里的时间，不含在调度器里排队的时间。排队时间要看 `block_rq_insert` 到 `block_rq_issue`。bio 从 `block_bio_queue` 到 `block_bio_complete` 则把调度、拆分、完成都算进去。

kyber 还有自己的 `kyber` 事件（`include/trace/events/kyber.h`，由 `kyber-iosched.c` 里的 `CREATE_TRACE_POINTS` 生成）：`kyber_throttled`、`kyber_adjust`。只有调度器是 kyber 时才会出现。

bpftrace 若要做直方图，思路是在 `block:block_rq_issue` 记下时间戳，在 `block:block_rq_complete` 做差。本环境没有跑过这类脚本，这里不给一份假装执行过的程序。字段以 trace 头文件为准，不要凭记忆写 `args->` 的名字。

---

## 16.5 io_uring：6.6 里实际有的

没有 `/proc/io_uring` 这种全局文件。能看的是：

**每个环 fd 的 fdinfo。** `io_uring_show_fdinfo`（`io_uring/fdinfo.c:53`）往 seq_file 打：

```text
SqMask SqHead SqTail CachedSqHead
CqMask CqHead CqTail CachedCqTail
SQEs:  然后每个未消费 SQE 的 opcode、fd、flags、off
CQEs:  然后 user_data、res、flag
SqThread SqThreadCpu          SQPOLL 线程的 pid 和 CPU
UserFiles UserBufs             固定文件和固定缓冲的个数
```

```bash
# RECIPE：<fd> 是 io_uring_setup 返回的描述符
cat /proc/<pid>/fdinfo/<fd>
```

`SqTail - SqHead` 是还没被内核取走的提交。`CqTail - CqHead` 是用户还没收的完成。两者长时间很大，分别表示提交赶不上、和用户态没有及时收割。

**sysctl。** `io_uring/io_uring.c:157` 登记在 `kernel` 下：

| 文件 | 含义 |
|---|---|
| `/proc/sys/kernel/io_uring_disabled` | 0 允许；1 仅 `CAP_SYS_ADMIN` 或 `io_uring_group`；2 全部拒绝（`io_uring_allowed`，`:4186`） |
| `/proc/sys/kernel/io_uring_group` | `disabled == 1` 时额外允许的组 |

**tracepoint。** `include/trace/events/io_uring.h`：

| 事件 | 行 | 何时 |
|---|---|---|
| `io_uring_create` | 27 | 建环 |
| `io_uring_register` | 69 | register 操作码 |
| `io_uring_submit_req` | 370 | 提交一个请求 |
| `io_uring_complete` | 326 | 产生完成 |
| `io_uring_cqe_overflow` | 571 | CQ 放不下 |
| `io_uring_req_failed` | 496 | prep 或 issue 失败 |
| `io_uring_short_write` | 630 | 短写 |
| `io_uring_poll_arm` | 414 | 武装 poll |
| `io_uring_queue_async_work` | 140 | 丢进 io-wq |
| `io_uring_cqring_wait` | 255 | enter 在等 CQE |
| `io_uring_task_work_run` | 608 | 跑 task_work |
| `io_uring_local_work_run` | 663 | 本地 work |

```bash
# RECIPE
perf record -e io_uring:io_uring_submit_req,io_uring:io_uring_complete -a -- sleep 2
perf script
```

这些事件看的是 io_uring 请求（`io_kiocb`），不是块层 request。一次 `IORING_OP_READ` 若走了 direct，后面还会有 `block:block_rq_issue`。两边的 `user_data` 和 request 指针不是同一个编号，要对时间线而不是对一个 id。

---

## 16.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/part_stat.h` | `struct disk_stats` | 8 | per-cpu 计数 |
| `include/linux/blk_types.h` | `enum stat_group` | 466 | 读、写、discard、flush |
| `block/genhd.c` | `part_stat_show` | 960 | `/sys/block/*/stat` 的 17 列 |
| `block/genhd.c` | `part_inflight_show` | 1009 | `inflight` 文件 |
| `block/partitions/core.c` | `stat` 属性 | 209 | 分区上的同名文件 |
| `block/elevator.c` | `elv_iosched_show` | 764 | `queue/scheduler` |
| `include/trace/events/block.h` | `block_rq_issue` | 229 | 请求交给驱动 |
| `include/trace/events/block.h` | `block_rq_complete` | 149 | 请求完成 |
| `include/trace/events/block.h` | `block_bio_queue` | 383 | bio 进入块层 |
| `io_uring/fdinfo.c` | `io_uring_show_fdinfo` | 53 | `/proc/pid/fdinfo/fd` |
| `io_uring/io_uring.c` | `io_uring_allowed` | 4186 | `io_uring_disabled` 的三个值 |
| `include/trace/events/io_uring.h` | `io_uring_complete` | 326 | CQE 产生 |

---

**导航**：[← 错误与超时](./15-errors-timeout-retry.md) · [目录](./README.md) · [性能配方 →](./17-perf-analysis.md)
