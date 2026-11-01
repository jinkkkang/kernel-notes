# 块层与 IO 大章

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`，tag 提交 `0aebf2870d53`）。下文路径、函数、结构体字段与行号对照 `/tmp/linux` 的该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.94+**（与 [vfs/12](../vfs/12-observability.md)、[memory-management/13](../memory-management/13-observability.md) 同一套主机）。数字若来自这台机器会标明 sampled on 6.12。结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。本章的命令默认是 **RECIPE**，没有对着 v6.6.160 跑出过输出。

**一句话**：VFS 或回写把一次传输收成 `struct bio`，blk-mq 再把它变成 `struct request`，调度器（或 `none`）挑出请求，驱动的 `queue_rq` 交给设备；完成从中断或轮询走回 `bio_endio`。

面向 **C 语言新手**：`struct bio` 描述「内存里哪几页、设备上哪一段扇区」。`struct request` 是块层发给驱动的一张工单，里面可以串着一个或多个 `bio`。`request_queue` 是这块盘的队列。软件队列（`blk_mq_ctx`）按提交 CPU 分，硬件队列（`blk_mq_hw_ctx`）按设备能并行处理的通道分。函数表 `blk_mq_ops` 是驱动填的一组函数指针，块层只按名字去调。

## 阅读顺序

```text
01 总览（VFS → bio → request → 调度器 → 驱动）
  → 02 bio / bvec / submit_bio
  → 03 request 与 blk-mq
  → 04 none → 05 mq-deadline → 06 kyber → 07 bfq
  → 08 多队列与 CPU/IRQ 亲和
  → 09 ioprio 与 cgroup io
  → 10 回写与 fsync → 11 O_DIRECT
  → 12 io_uring
  → 13 NVMe 走读 → 14 virtio-blk 走读
  → 15 错误、超时、failfast
  → 16 观测 → 17 性能配方
```

先分清三层对象：`bio` 是提交者的语言，`request` 是驱动的语言，调度器站在两者中间。云上常见的盘是 NVMe 和 virtio-blk，13 和 14 厚度对齐。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | VFS 到设备的总图，和 VFS / MM / cgroup / 中断的交界 | 中 |
| [02-bio-and-bvec.md](./02-bio-and-bvec.md) | `struct bio`、`bio_vec`、拆分合并、`submit_bio` | **厚** |
| [03-request-and-blk-mq.md](./03-request-and-blk-mq.md) | `struct request`、`request_queue`、`blk_mq_hw_ctx`、tag、`blk_mq_ops` | **厚** |
| [04-sched-none.md](./04-sched-none.md) | 名字是 `none`：没有 elevator，从软件队列直接派发 | **厚** |
| [05-sched-mq-deadline.md](./05-sched-mq-deadline.md) | `mq-deadline`：按扇区排序，按到期时间兜底 | **厚** |
| [06-sched-kyber.md](./06-sched-kyber.md) | `kyber`：按读/写/丢弃域发令牌，用延迟直方图调深度 | **厚** |
| [07-sched-bfq.md](./07-sched-bfq.md) | `bfq`：按权重的 `bfq_queue`，预算与空转 | **厚** |
| [08-multiqueue-affinity.md](./08-multiqueue-affinity.md) | 硬件/软件队列、CPU 映射、IRQ 亲和 | **厚** |
| [09-ioprio-and-cgroup-io.md](./09-ioprio-and-cgroup-io.md) | `ioprio`；cgroup io 控制器交到 blk-cgroup | **厚** |
| [10-writeback-and-fsync.md](./10-writeback-and-fsync.md) | 回写线程到 `submit_bio`；fsync 到设备 flush | **厚** |
| [11-direct-io.md](./11-direct-io.md) | `O_DIRECT`：`iov_iter` 到 bio，对照缓冲 IO | **厚** |
| [12-io-uring.md](./12-io-uring.md) | SQ/CQ、SQE/CQE、固定缓冲与文件、poll、`uring_cmd`，对照经典 AIO | **厚** |
| [13-nvme-walkthrough.md](./13-nvme-walkthrough.md) | `nvme_queue_rq` 到完成 IRQ | **厚** |
| [14-virtio-blk-walkthrough.md](./14-virtio-blk-walkthrough.md) | virtio-blk 从 `virtio_queue_rq` 到 `virtblk_done` | **厚** |
| [15-errors-timeout-retry.md](./15-errors-timeout-retry.md) | `blk_status_t`、超时、failfast | **厚** |
| [16-observability.md](./16-observability.md) | `/sys/block/*/stat`、iostat、blktrace、io_uring 的 fdinfo | **厚** |
| [17-perf-analysis.md](./17-perf-analysis.md) | 队列深度、延迟、调度器选择、io_uring 与同步/libaio。RECIPE | **厚** |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `submit_bio` 之前，缓冲读已经在页缓存里，脏页已经标脏 | 页缓存、脏页限速、flusher 何时醒来：[memory-management/08-page-cache-and-writeback.md](../memory-management/08-page-cache-and-writeback.md) §8.2–§8.3 |
| `read_iter` / `write_iter` 在 `IOCB_DIRECT` 处分叉，VFS 的 `kiocb` 与 `iov_iter` | [vfs/05-read-write.md](../vfs/05-read-write.md) |
| `vfs_fsync_range` 调到 `f_op->fsync` | 同上 §5.6。本章从 `file_write_and_wait_range` 和 `blkdev_issue_flush` 往下写 |
| cgroup v2 的 `io.*` 文件由谁登记 | [cgroup/07-io-pids-misc.md](../cgroup/07-io-pids-misc.md) §7.1。本章写 bio 上的 `bi_blkg`、限流和 ioprio 策略怎么吃到请求 |
| 完成 IRQ 进 IDT、`request_threaded_irq`、MSI 向量 | [interrupts/](../interrupts/README.md)。本章只写 NVMe / virtio 的处理函数里做了什么 |
| `spin_lock_irqsave` 的锁字 | [locks-and-ds/05-spinlock.md](../locks-and-ds/05-spinlock.md) |

块层不认识文件名。`inode`、`dentry`、路径查找停在 VFS。tmpfs 没有 bio，见 [vfs/10](../vfs/10-tmpfs-walkthrough.md)。

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/blk_types.h` | `struct bio`、`blk_opf_t`、`blk_status_t`、`req_op` |
| `include/linux/bvec.h` | `struct bio_vec`、`struct bvec_iter` |
| `include/linux/blkdev.h` | `struct request_queue`、`struct gendisk`、`struct blk_plug` |
| `include/linux/blk-mq.h` | `struct request`、`blk_mq_hw_ctx`、`blk_mq_ops`、`blk_mq_tags` |
| `block/blk-mq.h` | `struct blk_mq_ctx`（软件队列，不在公开头文件里） |
| `block/bio.c`、`block/blk-merge.c` | 分配、拆分、合并 |
| `block/blk-core.c` | `submit_bio`、`blk_status_t` 与 errno |
| `block/blk-mq.c`、`block/blk-mq-sched.c` | 把 bio 变成 request，派发到驱动 |
| `block/elevator.c` | 调度器注册，名字 `none` / 切换 |
| `block/mq-deadline.c`、`kyber-iosched.c`、`bfq-iosched.c` | 三个调度器 |
| `block/blk-cgroup.c`、`blk-throttle.c`、`blk-iocost.c`、`blk-iolatency.c`、`blk-ioprio.c` | cgroup io |
| `block/blk-flush.c` | `REQ_PREFLUSH` / `REQ_FUA` |
| `block/fops.c` | 块设备文件的 read/write/fsync/direct IO |
| `fs/mpage.c`、`fs/iomap/buffered-io.c`、`fs/iomap/direct-io.c`、`fs/direct-io.c` | 回写与 Direct IO 组 bio |
| `io_uring/` | 6.6 的 io_uring 实现（不在 `fs/io_uring.c`） |
| `drivers/nvme/host/pci.c` | NVMe PCI |
| `drivers/block/virtio_blk.c` | virtio-blk |
| `include/trace/events/block.h` | `block:*` tracepoint |

## 实测 vs RECIPE

- 本章不编 `/sys/block/*/stat` 或 `iostat` 的数字。字段含义按 v6.6.160 的 `part_stat_show` 写。
- [17](./17-perf-analysis.md) 的 perf、blktrace、bpftrace 都标 **RECIPE**。本环境没有对着 v6.6.160 的块设备跑过这些命令。

## 未覆盖

- 设备映射（dm-linear / dm-crypt）和 md 的条带如何克隆 request
- SCSI 中间层、`sd` 驱动的命令翻译
- zoned（SMR / ZNS）写指针的完整状态机。只在 bio 操作码和 BFQ/deadline 碰到的地方点到
- `blk-crypto`、integrity（T10 PI）的载荷格式
- NVMe over Fabrics（tcp / rdma / fc）。13 只走 PCI
- io_uring 的网络操作码（send/recv/accept）。12 只写到会进块层的读写、poll、`uring_cmd`

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
