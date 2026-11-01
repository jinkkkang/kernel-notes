# 13. NVMe PCI 走读

**结论**：NVMe PCI 驱动把 `struct request` 填成一条 64 字节的 `nvme_command`，拷进提交队列（SQ）内存，再写门铃；完成队列（CQ）由 MSI-X 中断或轮询扫相位位，用 command id 找回 request，最后 `blk_mq_end_request`。

> 文件是 `drivers/nvme/host/pci.c`。命令字和状态码转换在 `drivers/nvme/host/core.c`。这一节只走 PCI 传输。tcp / rdma / fc 用别的 `queue_rq`，不在这里。

C 新手把 NVMe 的队列想成两块设备能 DMA 的环形数组。SQ 里每格是一条命令，由驱动写、控制器读。CQ 里每格是一条完成，由控制器写、驱动读。门铃是 MMIO 寄存器：驱动写新的尾下标，控制器才知道有新命令。没有门铃，控制器不会去看刚拷进内存的命令。

一条硬件队列 `blk_mq_hw_ctx` 对应一个 `struct nvme_queue`，指针放在 `hctx->driver_data`（`nvme_queue_rq` 第一行，`:880`）。request 后面的 PDU 是 `struct nvme_iod`，用 `blk_mq_rq_to_pdu` 取出。

---

## 13.1 队列和中断怎么建

管理队列和 I/O 队列分开。管理队列用 `nvme_mq_admin_ops`（`:1715`），I/O 队列用 `nvme_mq_ops`（`:1724`）。两张表的 `queue_rq` 都是 `nvme_queue_rq`，`complete` 都是 `nvme_pci_complete_rq`，`timeout` 都是 `nvme_timeout`。只有 I/O 这张填了 `queue_rqs`、`map_queues` 和 `poll`。

```text
nvme_setup_irqs                     :2296
  pre_vectors = 1                   管理队列占一条向量
  poll 队列不申请中断
  pci_alloc_irq_vectors_affinity    PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY
nvme_pci_map_queues                 :440
  非轮询：blk_mq_pci_map_queues，offset 跳过管理队列
  轮询：blk_mq_map_queues
建每条 I/O 队列时：
  非轮询才 queue_request_irq        :1697
    pci_request_irq(..., nvme_irq_check, nvme_irq, ...)  :1618
    或没有线程化时只注册 nvme_irq    :1621
  set_bit(NVMEQ_ENABLED)
```

CPU 和队列的对应关系在 [08](./08-multiqueue-affinity.md)。中断进 IDT 的过程在 [interrupts/02](../interrupts/02-x86-entry.md)。这里从 `nvme_irq` 被调用写起。

tag set 的超时在 `drivers/nvme/host/core.c:4400` 设成 `NVME_IO_TIMEOUT`，即 `nvme_io_timeout * HZ`（`nvme.h:25`）。模块参数 `io_timeout` 的初值是 30（`core.c:50`），单位秒。管理队列用 `NVME_ADMIN_TIMEOUT`（`core.c:4326`）。

---

## 13.2 `nvme_queue_rq`

`blk_mq_ops.queue_rq` 指向这里（`:877`）。参数 `bd->rq` 是块层已经填好扇区、bio 链和 tag 的 request。`bd->last` 表示这一批的最后一条，可以敲门铃。

```text
nvmeq = hctx->driver_data
NVMEQ_ENABLED 没置：返回 BLK_STS_IOERR          :890
nvme_check_ready 失败：nvme_fail_nonready_command
nvme_prep_rq                                      :839
  iod->aborted = false，清 sg 计数
  nvme_setup_cmd                                  core 里把 request 译成 nvme_command
  有数据段：nvme_map_data                         :770
  有 integrity：nvme_map_metadata                 :826
  nvme_start_request                              nvme.h:1085
    blk_mq_start_request                          启动超时，state = IN_FLIGHT
spin_lock(&nvmeq->sq_lock)
nvme_sq_copy_cmd                                  :489
nvme_write_sq_db(nvmeq, bd->last)                 :472
spin_unlock
返回 BLK_STS_OK
```

`nvme_sq_copy_cmd` 把 64 字节命令 `memcpy` 到 `sq_cmds[sq_tail]`，尾下标加一，到 `q_depth` 就绕回 0。控制器还没被通知。

`nvme_write_sq_db`（`:472`）在 `write_sq` 为假时先不写 MMIO，除非下一条会绕回队列头。`bd->last` 为真才把尾下标写进门铃。一批里前面的命令只待在内存里，最后一条（或 `commit_rqs`）才敲一次。`nvme_commit_rqs`（`:498`）就是块层在「本该 last 的那条没送成」时补的这一脚：`sq_tail != last_sq_tail` 则 `nvme_write_sq_db(..., true)`。

`nvme_map_data`（`:770`）尽量走捷径：

```text
只有一个物理段，且不是 P2P 页，长度落在两个控制器页以内：
  nvme_setup_prp_simple             PRP 直接指向这一段
否则若队列不是管理队列、打开了 sgl、控制器支持 SGL：
  nvme_setup_sgl_simple
否则：
  从 iod_mempool 拿 scatterlist
  blk_rq_map_sg                     把 bio_vec 收成 sg
  dma_map_sgtable
  段数够多且允许 SGL：nvme_pci_setup_sgls
  否则 nvme_pci_setup_prps
DMA 映射返回 -EREMOTEIO：BLK_STS_TARGET
mempool 失败：BLK_STS_RESOURCE
```

PRP 是 NVMe 的物理区域页表，SGL 是散列聚集表。驱动二选一填进命令字，控制器按它 DMA。失败时 `nvme_prep_rq` 会 `nvme_unmap_data` 并 `nvme_cleanup_cmd`，把 `blk_status_t` 交回 `blk_mq_dispatch_rq_list`。`BLK_STS_RESOURCE` 会让请求回到 `hctx->dispatch`，而不是立刻失败给文件系统。

---

## 13.3 批量的 `nvme_queue_rqs`

`nvme_mq_ops.queue_rqs`（`:1726`）指向 `nvme_queue_rqs`（`:935`）。块层可以把同一条硬件队列上的多个 request 一次交来。

```text
按 hctx 分组（driver_data 变了就先把上一组提交）
nvme_prep_rq_batch                :920
  队列没开或控制器没就绪：失败，放回 requeue_list
  否则 nvme_prep_rq，成功则放进 submit_list
nvme_submit_cmds                  :906
  持 sq_lock，逐条 nvme_sq_copy_cmd
  最后 nvme_write_sq_db(..., true) 只敲一次门铃
没准备好的请求通过 *rqlist 交回块层，改走一条条的 queue_rq
```

这和 `bd->last` 是同一件事的批处理版本：内存里可以先排很多命令，门铃只写一次。

---

## 13.4 完成：相位位、IRQ、poll

CQ 的每一项是 `struct nvme_completion`。驱动不靠「长度」判断有没有新项，而靠状态字里的相位位。`nvme_cqe_pending`（`:987`）：

```text
hcqe = &nvmeq->cqes[cq_head]
(le16_to_cpu(READ_ONCE(hcqe->status)) & 1) == nvmeq->cq_phase
```

相位和驱动记住的 `cq_phase` 一致，说明控制器写过这一格。绕一圈后 `nvme_update_cq_head`（`:1044`）把 `cq_phase` 翻转，这样旧项不会被当成新完成。

```text
nvme_poll_cq                        :1056
  while (nvme_cqe_pending)：
    dma_rmb                         相位和其余字段之间的读屏障，注释 :1064
    nvme_handle_cqe                 :1010
    nvme_update_cq_head
  有完成则 nvme_ring_cq_doorbell    :994    告诉控制器 CQ 头移到哪了

nvme_handle_cqe
  command_id 是 AEN：nvme_complete_async_event，没有对应 request
  否则 nvme_find_rq(tagset, command_id)
    找不到：dev_warn "invalid id %d completed on queue %d"
  trace_nvme_sq
  nvme_try_complete_req 成功：特殊完成路径自己处理了
  否则尽量 blk_mq_add_to_batch，批的完成函数是 nvme_pci_complete_batch
  批不上：nvme_pci_complete_rq      :975
```

`nvme_pci_complete_rq` 先 `nvme_pci_unmap_rq`（`:959`，卸 DMA），再 `nvme_complete_rq`（`core.c:392`）。后者按 `nvme_decide_disposition` 选择完成、重试或多路径切换。普通成功走 `nvme_end_req`（`core.c:379`）：`nvme_error_status` 把 NVMe 状态码变成 `blk_status_t`，然后 `blk_mq_end_request`。

`nvme_error_status`（`core.c:251`）的一部分对应：

| NVMe 状态 | `blk_status_t` |
|---|---|
| `NVME_SC_SUCCESS` | `BLK_STS_OK` |
| `NVME_SC_CAP_EXCEEDED` | `BLK_STS_NOSPC` |
| `NVME_SC_INVALID_OPCODE` 等 | `BLK_STS_NOTSUPP` |
| `NVME_SC_READ_ERROR` / `WRITE_FAULT` | `BLK_STS_MEDIUM` |
| `NVME_SC_GUARD_CHECK` 等 PI 错误 | `BLK_STS_PROTECTION` |
| `NVME_SC_RESERVATION_CONFLICT` | `BLK_STS_RESV_CONFLICT` |
| `NVME_SC_ZONE_TOO_MANY_OPEN` | `BLK_STS_ZONE_OPEN_RESOURCE` |
| 其余 | `BLK_STS_IOERR` |

中断：

```text
nvme_irq                          pci.c:1077
  DEFINE_IO_COMP_BATCH(iob)
  nvme_poll_cq(nvmeq, &iob)
  批里还有请求：nvme_pci_complete_batch
  扫到完成：IRQ_HANDLED，否则 IRQ_NONE

nvme_irq_check                    :1090
  只看 nvme_cqe_pending
  有：IRQ_WAKE_THREAD，把 nvme_irq 放到线程里
  没有：IRQ_NONE
```

`queue_request_irq` 在支持线程化 IRQ 时把 `nvme_irq_check` 当主处理函数、`nvme_irq` 当线程（`:1618`）。硬中断里只读一个相位位。线程化 IRQ 的通用规则在 [interrupts/06](../interrupts/06-request-and-flow.md)。

轮询队列没有这条 IRQ。`nvme_poll`（`:1118`）由 io_uring 的 `io_do_iopoll` 或块层 `blk_mq_poll` 调用，里面同样是 `nvme_poll_cq`。

---

## 13.5 超时

`nvme_timeout`（`:1321`）是 `mq_ops->timeout`。块层在 `blk_mq_rq_timed_out` 里调用它，见 [15](./15-errors-timeout-retry.md)。

```text
读控制器 CSTS
PCI 设备已断开：把控制器状态改成 DELETING
已经是终态：去 disable
PCI channel offline：返回 BLK_EH_RESET_TIMER，这次先不复位
nvme_should_reset：警告并 disable
轮询队列：nvme_poll
否则 nvme_poll_irqdisable           关中断再扫一遍 CQ，防止漏中断
请求已经不在 IN_FLIGHT：
  打印 "timeout, completion polled"，说明完成其实到了
否则继续中止命令或复位控制器（函数后半）
```

漏中断是这条函数前半段专门处理的情况：完成项已经在 CQ 里，只是 IRQ 没送到。先 poll 再决定要不要复位。

---

## 13.6 从 bio 到 IRQ 的整条线

```text
submit_bio
  blk_mq_submit_bio
    blk_mq_dispatch_rq_list
      nvme_queue_rq                 pci.c:877
        nvme_prep_rq
          nvme_setup_cmd
          nvme_map_data             PRP 或 SGL
          blk_mq_start_request      超时开始
        nvme_sq_copy_cmd
        nvme_write_sq_db            门铃
控制器 DMA 完成，写 CQ，发 MSI-X
  nvme_irq 或 nvme_irq 的线程       :1077
    nvme_poll_cq
      nvme_handle_cqe
        nvme_pci_complete_rq
          nvme_complete_rq          core.c:392
            blk_mq_end_request
              bio_endio
```

轮询把「MSI-X → nvme_irq」换成「`nvme_poll` → `nvme_poll_cq`」，后面的 `nvme_handle_cqe` 相同。

---

## 13.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `drivers/nvme/host/pci.c` | `nvme_mq_ops` | 1724 | I/O 队列的 `blk_mq_ops` |
| `drivers/nvme/host/pci.c` | `nvme_queue_rq` | 877 | `queue_rq` |
| `drivers/nvme/host/pci.c` | `nvme_prep_rq` | 839 | 命令字、DMA、`nvme_start_request` |
| `drivers/nvme/host/pci.c` | `nvme_map_data` | 770 | PRP 或 SGL |
| `drivers/nvme/host/pci.c` | `nvme_sq_copy_cmd` | 489 | 命令拷进 SQ |
| `drivers/nvme/host/pci.c` | `nvme_write_sq_db` | 472 | 写门铃 |
| `drivers/nvme/host/pci.c` | `nvme_queue_rqs` | 935 | 一批命令一次门铃 |
| `drivers/nvme/host/pci.c` | `nvme_commit_rqs` | 498 | 补敲门铃 |
| `drivers/nvme/host/pci.c` | `nvme_cqe_pending` | 987 | 看相位位 |
| `drivers/nvme/host/pci.c` | `nvme_poll_cq` | 1056 | 扫 CQ |
| `drivers/nvme/host/pci.c` | `nvme_handle_cqe` | 1010 | command id → request |
| `drivers/nvme/host/pci.c` | `nvme_irq` | 1077 | 完成中断 |
| `drivers/nvme/host/pci.c` | `nvme_irq_check` | 1090 | 线程化 IRQ 的快检 |
| `drivers/nvme/host/pci.c` | `nvme_poll` | 1118 | 轮询完成 |
| `drivers/nvme/host/pci.c` | `nvme_timeout` | 1321 | 超时：先 poll，再考虑复位 |
| `drivers/nvme/host/pci.c` | `nvme_setup_irqs` | 2296 | 分配 MSI-X |
| `drivers/nvme/host/nvme.h` | `nvme_start_request` | 1085 | 调 `blk_mq_start_request` |
| `drivers/nvme/host/core.c` | `nvme_error_status` | 251 | NVMe 状态 → `blk_status_t` |
| `drivers/nvme/host/core.c` | `nvme_complete_rq` | 392 | 完成、重试或切换路径 |
| `drivers/nvme/host/core.c` | `nvme_io_timeout` | 50 | 默认 30 秒 |

---

**导航**：[← io_uring](./12-io-uring.md) · [目录](./README.md) · [virtio-blk →](./14-virtio-blk-walkthrough.md)
