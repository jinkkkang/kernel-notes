# 14. virtio-blk 走读

**结论**：virtio-blk 把 `struct request` 拆成 virtqueue 上的三段描述符——出头、数据 sg、状态字节——`virtqueue_add_sgs` 排进去，`bd->last` 时 `virtqueue_notify` 踢客户机退出；完成由 virtqueue 回调 `virtblk_done` 或轮询 `virtblk_poll` 取回，再 `blk_mq_end_request`。

> 驱动在 `drivers/block/virtio_blk.c`。和 NVMe 一样挂在 blk-mq 上：`virtio_mq_ops`（`:1288`）的 `queue_rq` 是 `virtio_queue_rq`（`:429`）。云上的虚拟盘走这条，不走 NVMe 的 SQ 门铃。

C 新手把 virtqueue 想成客机和宿主机共用的一个环。客机把「读还是写、哪一扇区、数据页、一个状态字节」排成散列表项。`virtqueue_notify` 通知宿主机。宿主机做完后把用过的描述符放回已用环，并注入中断。驱动在中断里 `virtqueue_get_buf` 取出当初放进去的 `virtblk_req`。状态字节是协议规定的结果，不是 `blk_status_t`；驱动用 `virtblk_result` 翻译。

request 后面的 PDU 是 `struct virtblk_req`（`:89`）。`blk_mq_rq_to_pdu` 得到它，完成时 `blk_mq_rq_from_pdu` 得到 request。一条 `blk_mq_hw_ctx` 对应 `vblk->vqs[hctx->queue_num]` 里的一个 `struct virtio_blk_vq`（`:50`）：一把锁、一个 `virtqueue *`、一个名字。

---

## 14.1 设备和队列

`virtblk_probe`（`:1300`）读 virtio 配置空间里的容量、块大小、是否支持 discard、写零、zoned，然后 `init_vq`（`:1017`）建 virtqueue，再 `blk_mq_alloc_tag_set`。`tag_set.nr_hw_queues` 设成 `vblk->num_vqs`（`:1373`）。`queuedata` 指向 `struct virtio_blk`（`:56`），所以 `hctx->queue->queuedata` 能找回 `vblk`。

```text
init_vq
  有 VIRTIO_BLK_F_MQ：读 num_queues，否则 num_vqs = 1
  和模块参数 num_request_queues、nr_cpu_ids 取最小
  poll_queues 参数决定末尾几条没有回调的 virtqueue
  前半 callbacks[i] = virtblk_done，名字 req.%u
  后半 callbacks[i] = NULL，名字 req_poll.%u
  virtio_find_vqs
virtblk_map_queues                :1221
  普通队列：blk_mq_virtio_map_queues，按 IRQ 亲和
  POLL：blk_mq_map_queues，没有中断可问
```

读队列数是 0（`:1047`）。写和读共用 `HCTX_TYPE_DEFAULT`，除非请求带 `REQ_POLLED` 才进 poll 队列。模块参数和映射的细节在 [08](./08-multiqueue-affinity.md)。

`struct virtio_blk`（`:56`）里和这条路径有关的成员：`vdev`、`disk`、`tag_set`、`num_vqs`、`io_queues[HCTX_MAX_TYPES]`、`vqs`。`struct virtblk_req`（`:89`）：

```c
struct virtblk_req {
	struct virtio_blk_outhdr out_hdr;   /* 类型、ioprio、扇区。客机写、宿主机读 */
	union {
		u8 status;                  /* 普通请求的结果字节 */
		struct {
			__virtio64 sector;  /* ZONE_APPEND 写回的扇区 */
			u8 status;          /* 协议要求状态是 in header 的最后一字节 */
		} zone_append;
	} in_hdr;
	size_t in_hdr_len;
	struct sg_table sg_table;
	struct scatterlist sg[];            /* 柔性数组，跟在 PDU 后面 */
};
```

---

## 14.2 把 request 翻译成 virtio 头

`virtblk_setup_cmd`（`:239`）按 `req_op` 填 `out_hdr`。`out_hdr.ioprio` 来自 `req_get_ioprio`（`:252`），所以 cgroup 和任务 ioprio 会进虚拟盘协议，宿主机能不能用是宿主机的事。

| `req_op` | virtio 类型 | 扇区 |
|---|---|---|
| `REQ_OP_READ` | `VIRTIO_BLK_T_IN` | `blk_rq_pos` |
| `REQ_OP_WRITE` | `VIRTIO_BLK_T_OUT` | `blk_rq_pos` |
| `REQ_OP_FLUSH` | `VIRTIO_BLK_T_FLUSH` | 不填扇区 |
| `REQ_OP_DISCARD` | `VIRTIO_BLK_T_DISCARD` | 另建范围描述 |
| `REQ_OP_WRITE_ZEROES` | `VIRTIO_BLK_T_WRITE_ZEROES` | `REQ_NOUNMAP` 决定是否带 UNMAP |
| `REQ_OP_SECURE_ERASE` | `VIRTIO_BLK_T_SECURE_ERASE` | 范围描述 |
| `REQ_OP_ZONE_APPEND` | `VIRTIO_BLK_T_ZONE_APPEND` | `in_hdr_len` 用扩展头 |
| `REQ_OP_DRV_IN` | 调用者已经填好 | 直接返回 0 |

类型和扇区在 `:313` 写成 virtio 的字节序。discard、写零、安全擦除再调 `virtblk_setup_discard_write_zeroes_erase`（`:162`）。分配范围数组失败返回 `BLK_STS_RESOURCE`（`:319`）。不认识的操作码 `WARN_ON_ONCE` 并返回 `BLK_STS_IOERR`（`:307`）。

数据页在 `virtblk_map_data`（`:214`）：`blk_rq_map_sg` 把 bio 收成 scatterlist，再按 virtio 的 DMA 方向映射。没有数据的 flush 可以没有数据段。

`virtblk_add_req`（`:140`）把最多三段交给 `virtqueue_add_sgs`：

```text
第 1 段：out_hdr，一定是 out（客机 → 宿主机）
第 2 段：有数据时
  type 带 VIRTIO_BLK_T_OUT：也是 out（写）
  否则：in（读，宿主机写回这些页）
第 3 段：in_hdr 的状态字节，长度 in_hdr_len，一定是 in
GFP_ATOMIC：queue_rq 的上下文不能睡觉
```

三段的方向是协议的一部分。状态字节必须由宿主机写回来，所以它是 in。写数据是 out，读数据是 in。

---

## 14.3 `virtio_queue_rq`

```text
virtio_queue_rq                   :429
  qid = hctx->queue_num
  virtblk_prep_rq                 :407
    virtblk_setup_cmd
    virtblk_map_data
      失败：virtblk_fail_to_queue → BLK_STS_RESOURCE（-ENOMEM）
    blk_mq_start_request          超时从这里开始
  spin_lock_irqsave(&vqs[qid].lock)
  virtblk_add_req
    失败：
      virtqueue_kick              先把环里已有的踢出去
      -ENOSPC：blk_mq_stop_hw_queue
      卸映射，virtblk_fail_to_queue
        -ENOSPC → BLK_STS_DEV_RESOURCE
        -ENOMEM → BLK_STS_RESOURCE
        其它 → BLK_STS_IOERR
  bd->last 且 virtqueue_kick_prepare：记下 notify
  spin_unlock
  notify 为真：virtqueue_notify    踢宿主机
  返回 BLK_STS_OK
```

`-ENOSPC` 表示 virtqueue 描述符不够，不是磁盘空间不够。驱动停掉这条硬件队列（`blk_mq_stop_hw_queue`），等完成路径里 `blk_mq_start_stopped_hw_queues` 再打开。注释（`:449`）说 `-ENOMEM` 不停队列：那是 bounce 或全局内存不够，停了也不会因为本队列完成而好转。`BLK_STS_DEV_RESOURCE` 和 `BLK_STS_RESOURCE` 的差别在 [03](./03-request-and-blk-mq.md) 和 [15](./15-errors-timeout-retry.md)。

`bd->last` 为假时不 notify。一批请求只在最后一条踢一次，和 NVMe 攒门铃是同一类优化。批中间失败时，块层会调 `commit_rqs`。`virtio_commit_rqs`（`:380`）对这条 virtqueue `virtqueue_kick_prepare`，需要的话 `virtqueue_notify`。

批量入口 `virtio_queue_rqs`（`:506`）按 virtqueue 分组，`virtblk_prep_rq_batch`（`:468`）成功的放进提交链表，`virtblk_add_req_batch`（`:478`）在同一把 vq 锁里连续 `virtblk_add_req`，最后踢一次。准备失败的请求交回块层，改走单条 `virtio_queue_rq`。

---

## 14.4 完成：中断和轮询

有回调的 virtqueue 由 `virtblk_done`（`:351`）收割。它是 `virtio_find_vqs` 注册的回调，中断或 virtio 的软处理最终调到这里。

```text
virtblk_done
  qid = vq->index
  spin_lock_irqsave(&vqs[qid].lock)
  do {
    virtqueue_disable_cb
    while ((vbr = virtqueue_get_buf(...)) != NULL)
      req = blk_mq_rq_from_pdu(vbr)
      不是 fake timeout：blk_mq_complete_request(req)
    环坏了就跳出
  } while (!virtqueue_enable_cb(vq))
  有完成：blk_mq_start_stopped_hw_queues(..., true)
    刚才因 -ENOSPC 停下的队列可以再跑
  spin_unlock_irqrestore
```

`virtqueue_disable_cb` / `enable_cb` 这一对避免收割的同时丢通知：关掉回调、取空、再打开，若打开时发现又有新缓冲，循环再来一次。

`blk_mq_complete_request` 可能把 `virtblk_request_done` 投到另一颗 CPU 的 `BLOCK_SOFTIRQ`。`virtio_mq_ops.complete` 就是 `virtblk_request_done`（`:1292`）。

```text
virtblk_request_done              :335
  status = virtblk_result(virtblk_vbr_status(vbr))
  virtblk_unmap_data
  virtblk_cleanup_cmd             discard 的范围数组在这里释放
  REQ_OP_ZONE_APPEND：把宿主机写回的 sector 填进 req->__sector
  blk_mq_end_request(req, status)
```

状态字节总是 in header 的最后一字节（`virtblk_vbr_status`，`:330`）。`virtblk_result`（`:114`）：

| virtio 状态 | `blk_status_t` |
|---|---|
| `VIRTIO_BLK_S_OK` | `BLK_STS_OK` |
| `VIRTIO_BLK_S_UNSUPP` | `BLK_STS_NOTSUPP` |
| `VIRTIO_BLK_S_ZONE_OPEN_RESOURCE` | `BLK_STS_ZONE_OPEN_RESOURCE` |
| `VIRTIO_BLK_S_ZONE_ACTIVE_RESOURCE` | `BLK_STS_ZONE_ACTIVE_RESOURCE` |
| `VIRTIO_BLK_S_IOERR`、`VIRTIO_BLK_S_ZONE_UNALIGNED_WP`、其它 | `BLK_STS_IOERR` |

轮询队列的回调是 NULL，不会进 `virtblk_done`。`virtblk_poll`（`:1259`）在 `mq_ops.poll` 上：

```text
持 vq 锁
while virtqueue_get_buf：
  若不必远程完成，且能加入 io_comp_batch：
    批完成函数 virtblk_complete_batch   :1248
      卸映射、cleanup，然后 blk_mq_end_request_batch
  否则 virtblk_request_done
有完成则 blk_mq_start_stopped_hw_queues
```

io_uring 的 `IORING_SETUP_IOPOLL` 通过块层 poll 调到这里，和 NVMe 的 `nvme_poll` 在 blk-mq 看来是同一个钩子。

---

## 14.5 和 NVMe 对照

| | NVMe PCI | virtio-blk |
|---|---|---|
| `queue_rq` | `nvme_queue_rq`（`pci.c:877`） | `virtio_queue_rq`（`virtio_blk.c:429`） |
| 命令放哪 | SQ 内存里的 `nvme_command` | virtqueue 的 out_hdr + sg + status |
| 通知设备 | `nvme_write_sq_db` 写 MMIO 门铃 | `virtqueue_notify` |
| 描述数据 | PRP 或 SGL | `blk_rq_map_sg` + virtio DMA |
| 完成 IRQ | `nvme_irq` 扫 CQ 相位位 | `virtblk_done` 里 `virtqueue_get_buf` |
| 轮询 | `nvme_poll` | `virtblk_poll` |
| 开始计时 | `nvme_start_request` → `blk_mq_start_request` | `virtblk_prep_rq` 里直接 `blk_mq_start_request` |
| 队列满 | `BLK_STS_RESOURCE` / 设备资源 | virtqueue `-ENOSPC` → `BLK_STS_DEV_RESOURCE` 并停队列 |
| 批量 | `nvme_queue_rqs` | `virtio_queue_rqs` |
| 补通知 | `nvme_commit_rqs` | `virtio_commit_rqs` |

两边都在 `blk_mq_start_request` 之后硬件才算「看见」请求，超时都从这一刻算。两边的 `complete` 都进 `blk_mq_end_request`，bio 的 `bi_end_io` 不知道底下是 NVMe 还是 virtio。

---

## 14.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `drivers/block/virtio_blk.c` | `struct virtio_blk` | 56 | 一块虚拟盘 |
| `drivers/block/virtio_blk.c` | `struct virtblk_req` | 89 | request 后面的 PDU |
| `drivers/block/virtio_blk.c` | `virtblk_result` | 114 | virtio 状态字节 → `blk_status_t` |
| `drivers/block/virtio_blk.c` | `virtblk_add_req` | 140 | 三段描述符 `virtqueue_add_sgs` |
| `drivers/block/virtio_blk.c` | `virtblk_map_data` | 214 | bio → sg → DMA |
| `drivers/block/virtio_blk.c` | `virtblk_setup_cmd` | 239 | `req_op` → `VIRTIO_BLK_T_*` |
| `drivers/block/virtio_blk.c` | `virtblk_request_done` | 335 | 卸映射并 `blk_mq_end_request` |
| `drivers/block/virtio_blk.c` | `virtblk_done` | 351 | 中断路径收割已用环 |
| `drivers/block/virtio_blk.c` | `virtio_commit_rqs` | 380 | 补一次 kick |
| `drivers/block/virtio_blk.c` | `virtblk_prep_rq` | 407 | setup、映射、`blk_mq_start_request` |
| `drivers/block/virtio_blk.c` | `virtio_queue_rq` | 429 | `queue_rq` |
| `drivers/block/virtio_blk.c` | `virtio_queue_rqs` | 506 | 按 virtqueue 批量提交 |
| `drivers/block/virtio_blk.c` | `init_vq` | 1017 | MQ 与 poll virtqueue |
| `drivers/block/virtio_blk.c` | `virtblk_map_queues` | 1221 | IRQ 亲和或 CPU 平分 |
| `drivers/block/virtio_blk.c` | `virtblk_poll` | 1259 | 轮询收割 |
| `drivers/block/virtio_blk.c` | `virtio_mq_ops` | 1288 | 交给 blk-mq 的函数表 |
| `drivers/block/virtio_blk.c` | `virtblk_probe` | 1300 | 探测、建队列、注册磁盘 |

---

**导航**：[← NVMe](./13-nvme-walkthrough.md) · [目录](./README.md) · [错误与超时 →](./15-errors-timeout-retry.md)
