# 15. 错误、超时与 failfast

**结论**：块层不用负的 errno 在驱动和 bio 之间传递结果，而用 `blk_status_t`；`blk_status_to_errno` 只在要回到文件系统或用户态时才查表。超时从 `blk_mq_start_request` 开始算，到期后调用驱动的 `timeout`，驱动可以结束请求或把计时器再拨一次。

> 状态码在 `include/linux/blk_types.h:94` 起。对照表 `blk_errors[]` 在 `block/blk-core.c:152`。计时在 `block/blk-timeout.c` 的 `blk_add_timer`（`:128`），扫描在 `blk_mq_timeout_work`（`block/blk-mq.c:1637`）。

C 新手把 `blk_status_t` 想成一个很小的整数，0 表示成功。它不是 `-EIO`。`-EIO` 是 `BLK_STS_IOERR` 查表之后的结果。在 `bio->bi_status` 和 `queue_rq` 的返回值里看到的是前者。

---

## 15.1 `blk_status_t` 的值

类型在非 Alpha 上是 `u8`（`blk_types.h:97`）。`BLK_STS_OK` 是 0（`:100`）。其余：

| 宏 | 值 | 头文件在说什么 |
|---|---|---|
| `BLK_STS_NOTSUPP` | 1 | 操作不支持 |
| `BLK_STS_TIMEOUT` | 2 | 超时 |
| `BLK_STS_NOSPC` | 3 | 空间不够 |
| `BLK_STS_TRANSPORT` | 4 | 传输可恢复 |
| `BLK_STS_TARGET` | 5 | 目标端错误 |
| `BLK_STS_RESV_CONFLICT` | 6 | 预留冲突 |
| `BLK_STS_MEDIUM` | 7 | 介质错误 |
| `BLK_STS_PROTECTION` | 8 | 数据保护（PI） |
| `BLK_STS_RESOURCE` | 9 | 内核资源不够，例如内存、DMA 映射。注释 `:122`：不一定有在途 IO 会把资源还回来 |
| `BLK_STS_IOERR` | 10 | 其余 I/O 错误 |
| `BLK_STS_DM_REQUEUE` | 11 | 只给 device mapper，注释写明别的地方不要用 |
| `BLK_STS_AGAIN` | 12 | 仅当 `RQF_NOWAIT` 且这次会阻塞。见 `bio_wouldblock_error` |
| `BLK_STS_DEV_RESOURCE` | 13 | 设备自己的槽满了。注释 `:137`：在途 IO 完成就会还槽，块层可以再跑队列 |
| `BLK_STS_ZONE_RESOURCE` | 14 | zone 资源，别的 zone 仍可服务 |
| `BLK_STS_ZONE_OPEN_RESOURCE` | 15 | 打开的 zone 太多 |
| `BLK_STS_ZONE_ACTIVE_RESOURCE` | 16 | 活跃的 zone 太多 |
| `BLK_STS_OFFLINE` | 17 | 设备离线或正在下线 |
| `BLK_STS_DURATION_LIMIT` | 18 | 设备因 Command Duration Limit 中止命令 |

`BLK_STS_RESOURCE` 和 `BLK_STS_DEV_RESOURCE` 都表示「现在送不进去」，但块层对它们的预期不同。`RESOURCE` 可能是系统范围的分配失败，不能假设马上会有完成来解套。`DEV_RESOURCE` 明确说设备资源会被在途请求的完成释放。`blk_mq_dispatch_rq_list` 两种都会把请求放回 `hctx->dispatch` 并安排再跑；驱动自己停队列（virtio 的 `-ENOSPC`）是额外一步。

`blk_path_error`（`blk_types.h:194`）用来判断多路径该不该换一条路。下面这些返回 false，换路也没用：`NOTSUPP`、`NOSPC`、`TARGET`、`RESV_CONFLICT`、`MEDIUM`、`PROTECTION`。其余返回 true，包括超时、传输、资源、IOERR。

---

## 15.2 和 errno 的对照表

`blk_errors[]`（`blk-core.c:152`）用指定下标初始化。`blk_status_to_errno`（`:194`）把状态下标当数组下标。越界时 `WARN_ON_ONCE` 并返回 `-EIO`。

| 状态 | errno | `name` 字符串 |
|---|---|---|
| `OK` | 0 | `""` |
| `NOTSUPP` | `-EOPNOTSUPP` | `"operation not supported"` |
| `TIMEOUT` | `-ETIMEDOUT` | `"timeout"` |
| `NOSPC` | `-ENOSPC` | `"critical space allocation"` |
| `TRANSPORT` | `-ENOLINK` | `"recoverable transport"` |
| `TARGET` | `-EREMOTEIO` | `"critical target"` |
| `RESV_CONFLICT` | `-EBADE` | `"reservation conflict"` |
| `MEDIUM` | `-ENODATA` | `"critical medium"` |
| `PROTECTION` | `-EILSEQ` | `"protection"` |
| `RESOURCE` | `-ENOMEM` | `"kernel resource"` |
| `IOERR` | `-EIO` | `"I/O"` |
| `DM_REQUEUE` | `-EREMCHG` | `"dm internal retry"` |
| `AGAIN` | `-EAGAIN` | `"nonblocking retry"` |
| `DEV_RESOURCE` | `-EBUSY` | `"device resource"` |
| `ZONE_OPEN_RESOURCE` | `-ETOOMANYREFS` | `"open zones exceeded"` |
| `ZONE_ACTIVE_RESOURCE` | `-EOVERFLOW` | `"active zones exceeded"` |
| `OFFLINE` | `-ENODEV` | `"device offline"` |
| `DURATION_LIMIT` | `-ETIME` | `"duration limit exceeded"` |

这张表没有给 `BLK_STS_ZONE_RESOURCE`（14）写指定初始化项。数组长度由最大下标 `DURATION_LIMIT`（18）决定，下标 14 因此是零值：`errno` 为 0，`name` 为 `NULL`。`blk_status_to_errno(BLK_STS_ZONE_RESOURCE)` 会返回 0，而不是一个负的错误码。zone 打开过多和活跃过多有专门的项，不要和这一条混用。

`errno_to_blk_status`（`:181`）按 errno 反查。对不上就返回 `BLK_STS_IOERR`。iomap 在提交前已经失败时用它填 `bi_status`（`iomap_submit_ioend`，见 [10](./10-writeback-and-fsync.md)）。

`blk_status_to_str`（`:204`）返回上表的 `name`。打印前要确认状态下标在表里有名字。`blk_print_req_error`（`blk-mq.c:800`）走的就是这条。

mpage 和 iomap 的完成回调用 `blk_status_to_errno(bio->bi_status)` 得到负 errno，再 `mapping_set_error`。用户态 `read` / `write` 最终看到的是这个 errno，不是 `blk_status_t` 的数字。

---

## 15.3 failfast

三个标志在 `enum req_flag_bits`（`blk_types.h:404`），从操作码占用的 8 位之后开始：

| 宏 | 位名 | 注释 |
|---|---|---|
| `REQ_FAILFAST_DEV` | `__REQ_FAILFAST_DEV` | 设备错误不再由驱动重试 |
| `REQ_FAILFAST_TRANSPORT` | `__REQ_FAILFAST_TRANSPORT` | 传输错误不重试 |
| `REQ_FAILFAST_DRIVER` | `__REQ_FAILFAST_DRIVER` | 驱动错误不重试 |

`REQ_FAILFAST_MASK` 是三者的或（`:460`）。`blk_noretry_request`（`blkdev.h:603`）为真表示 request 带了其中任意一个。

这些位不让块层自己重试。它们告诉驱动：这类错误不要在驱动内部再发一遍命令。多路径软件（dm-multipath）更希望尽快拿到 `blk_path_error` 为真的状态，自己换路，而不是等驱动把同一条坏路重试完。

合并会把问题变复杂。两个 bio 的 failfast 不一样却并进了同一个 request 时，请求被标成 `RQF_MIXED_MERGE`（`blk-mq.h:34`）。`blk_rq_set_mixed_merge`（`blk-merge.c:741`）做这件事。完成后要按各自的 failfast 分开报错，不能让一个 bio 的「不许重试」盖住另一个。

`REQ_RAHEAD`（预读）是另一种「可以失败」：预读 bio 失败不应该弄脏文件的错误状态。那是文件系统完成回调里的判断，不是 failfast 位本身。

---

## 15.4 超时怎么计

每个 request 有自己的到期时刻 `rq->deadline`（jiffies），队列有一个共享的 `timer_list timeout` 和一个 `timeout_work`。

```text
blk_mq_start_request              blk-mq.c:1252
  blk_add_timer                   blk-timeout.c:128
    req->timeout 为 0：抄 q->rq_timeout
    清掉 RQF_TIMED_OUT
    deadline = jiffies + req->timeout
    若队列定时器还没挂、或这次更早：mod_timer，向上取整到下一秒附近
```

`blk_rq_timeout`（`blk-timeout.c:109`）把到期时刻限制在 `jiffies + BLK_MAX_TIMEOUT` 以内。`BLK_MAX_TIMEOUT` 是 `5 * HZ`（`block/blk.h:13`）。注释说这只是粗略上限，单次请求的 `req->timeout` 可以更长，但队列定时器不会一次拨到那么远；到点再看谁还活着，把定时器拨向剩下的最早到期。

NVMe 把 tag set 的 `timeout` 设成 `nvme_io_timeout * HZ`，模块参数默认 30 秒（`drivers/nvme/host/core.c:50`、`:4400`）。这个值在分配 request 时进 `rq->timeout`。virtio-blk 用队列的 `rq_timeout`，没有自己的 30 秒常量。

定时器触发的是 `q->timeout`，工作函数是 `blk_mq_timeout_work`（`blk-mq.c:1637`），在 `blk_mq_init_allocated_queue` 里挂上（`:4477`）。

```text
percpu_ref_tryget(&q->q_usage_counter)
  失败就返回。注释 :1647：冻队列的窗口里不能用 blk_queue_enter，
  否则超时处理和冻队列互相等
blk_mq_queue_tag_busy_iter(..., blk_mq_check_expired)
  有到期请求：
    blk_mq_wait_quiesce_done     等已经开始的提交走完
    再迭代 blk_mq_handle_expired
      blk_mq_rq_timed_out        :1559
没有更早的下一次到期：
  每条 hctx blk_mq_tag_idle
否则 mod_timer 到 expired.next
```

`blk_mq_req_expired`（`:1580`）要求 `state == MQ_RQ_IN_FLIGHT`，并且还没标 `RQF_TIMED_OUT`。`deadline` 用 `READ_ONCE` 读。已经完成或还没 `start_request` 的请求不会被当成超时。

```text
blk_mq_rq_timed_out
  rq_flags |= RQF_TIMED_OUT
  驱动提供了 timeout：
    BLK_EH_DONE：返回。驱动已经或将要完成这条请求
    BLK_EH_RESET_TIMER：再 blk_add_timer
    其它值：WARN_ON_ONCE
  没有 timeout 回调：直接 blk_add_timer
```

`enum blk_eh_timer_return`（`include/linux/blk-mq.h:276`）只有这两个值。`BLK_EH_DONE` 的注释说驱动已经完成命令，或稍后会完成。`BLK_EH_RESET_TIMER` 把 `deadline` 再拨一轮。

NVMe 的 `nvme_timeout`（`pci.c:1321`）在复位之前先 poll CQ，避免漏中断被当成真超时。PCI 通道离线时它返回 `BLK_EH_RESET_TIMER`。细节在 [13](./13-nvme-walkthrough.md)。

`RQF_TIMED_OUT` 置上之后，同一条请求不会再次进入 `blk_mq_rq_timed_out`，除非 `blk_add_timer` 把它清掉（`blk-timeout.c:140` 在重新计时时清这个标志）。

---

## 15.5 谁重试

块层核心不把失败的 bio 自动再 `submit_bio` 一遍。重试发生在三处，彼此独立：

```text
驱动内部
  failfast 位置位时，驱动不该对那一类错误再发命令
  NVMe 的 nvme_complete_rq 还可以按 nvme_decide_disposition 选择 RETRY
    nvme_retry_req（core.c:293）会延迟后再提交，和 failfast 不是同一个开关

多路径
  blk_path_error 为真时，dm-multipath 可以换一条路径再提交
  BLK_STS_DM_REQUEUE 是 dm 自己的内部重试，注释要求不要漏出驱动

文件系统
  缓冲写的错误记在 address_space 上，下次 fsync 报告给用户
  Direct IO 把 errno 交回 kiocb，由调用者决定要不要再写
  REQ_NOWAIT / BLK_STS_AGAIN 对 io_uring 是「现在做不了」，不是永久失败
```

`submit_bio_noacct` 在进入驱动前就会结束的错误（只读、越界、不支持的操作码）直接 `bio->bi_status = ...; bio_endio(bio)`（`blk-core.c:833`）。这种 bio 没有 request，也没有超时。

假超时用来测试这条路径。`QUEUE_FLAG_FAIL_IO`（`blkdev.h:546`）置位时 `blk_should_fake_timeout`（`include/linux/blk-mq.h:916`）返回真。virtio 的 `virtblk_done` 见到它就不调用 `blk_mq_complete_request`（`virtio_blk.c:366`），让请求一直挂到真的超时。这是调试开关，不是设备返回的状态。

---

## 15.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/blk_types.h` | `blk_status_t` 与 `BLK_STS_*` | 94 | 状态码定义 |
| `include/linux/blk_types.h` | `blk_path_error` | 194 | 多路径该不该换路 |
| `include/linux/blk_types.h` | `REQ_FAILFAST_*` | 435 | 三个不重试标志 |
| `block/blk-core.c` | `blk_errors` | 152 | 状态到 errno 和短字符串 |
| `block/blk-core.c` | `errno_to_blk_status` | 181 | errno 反查，失败则 `IOERR` |
| `block/blk-core.c` | `blk_status_to_errno` | 194 | 回到文件系统时用 |
| `block/blk-core.c` | `blk_status_to_str` | 204 | `name` 字符串 |
| `block/blk.h` | `BLK_MAX_TIMEOUT` | 13 | 队列定时器最多往前看 `5 * HZ` |
| `block/blk-timeout.c` | `blk_add_timer` | 128 | 写下 `rq->deadline` |
| `block/blk-mq.c` | `blk_mq_start_request` | 1252 | 调用 `blk_add_timer` 的地方 |
| `block/blk-mq.c` | `blk_mq_rq_timed_out` | 1559 | 调驱动 `timeout` |
| `block/blk-mq.c` | `blk_mq_timeout_work` | 1637 | 扫描到期 tag |
| `include/linux/blk-mq.h` | `enum blk_eh_timer_return` | 276 | `DONE` 或 `RESET_TIMER` |
| `include/linux/blkdev.h` | `blk_noretry_request` | 603 | 是否带了任一 failfast |
| `block/blk-merge.c` | `blk_rq_set_mixed_merge` | 741 | 合并了不同的 failfast |
| `drivers/nvme/host/core.c` | `nvme_retry_req` | 293 | NVMe 决定重试时的延迟重提 |

---

**导航**：[← virtio-blk](./14-virtio-blk-walkthrough.md) · [目录](./README.md) · [观测 →](./16-observability.md)
