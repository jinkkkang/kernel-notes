# 11. virtio-blk 请求

**结论**：客户机 blk-mq 的 `virtio_queue_rq` 把 `struct request` 收成三段描述符：设备读的 `virtio_blk_outhdr`、数据页、设备写回的状态字节。`bd->last` 时 `virtqueue_notify` 踢一下通知寄存器。完成时 `virtblk_done` 从 used ring 取回 `virtblk_req`，`blk_mq_complete_request` 最终进 `virtblk_request_done`，用状态字节调用 `blk_mq_end_request`。后端在宿主机上读这些 GPA。v6.6.160 的内核没有 virtio-blk 的通用 vhost 设备，所以这条路径的后端是用户态（或 vDPA 父设备），本章只写它必须对上的描述符和 eventfd。

> `bio` 如何变成 `request`、调度器如何选出 `bd`，块层章 [14](../block-io/14-virtio-blk-walkthrough.md) 已按同一版本写过 `virtio_queue_rq`。这里沿同一函数把通知和完成接到 KVM，不把 QEMU 的块设备状态机打开。

---

## 11.1 请求对象

```c
/* drivers/block/virtio_blk.c:89
 * 跟在 request 的 PDU 后面。blk_mq_rq_to_pdu 得到它。
 */
struct virtblk_req {
	struct virtio_blk_outhdr out_hdr;   /* 设备读：类型、优先级、扇区 */
	union {
		u8 status;                  /* 普通请求：设备写回的一个字节 */
		struct {
			__virtio64 sector;  /* ZONE_APPEND 多写回的扇区 */
			u8 status;          /* 协议要求状态是 in 头的最后一字节 */
		} zone_append;
	} in_hdr;
	size_t in_hdr_len;
	struct sg_table sg_table;
	struct scatterlist sg[];
};
```

头的布局在 `include/uapi/linux/virtio_blk.h:219`：

```c
struct virtio_blk_outhdr {
	__virtio32 type;     /* VIRTIO_BLK_T_IN=0 是读，T_OUT 是写 */
	__virtio32 ioprio;
	__virtio64 sector;   /* 512 字节为单位的偏移 */
};
```

`virtblk_vbr_status`（`virtio_blk.c:330`）不假设 `status` 的偏移，而是取 in 头最后一个字节。`virtblk_result`（`:114`）把 `VIRTIO_BLK_S_OK`（0，`virtio_blk.h:317`）译成 `BLK_STS_OK`，把不支持译成 `BLK_STS_NOTSUPP`，其余译成 `BLK_STS_IOERR`。这个字节是 virtio 协议的结果，不是 `blk_status_t` 的数值。

---

## 11.2 提交

`virtio_mq_ops.queue_rq` 是 `virtio_queue_rq`（`:1288` 与 `:429`）。

`virtblk_prep_rq`（`:407`）先 `virtblk_setup_cmd`（`:239`）按 `req_op` 填 `out_hdr`，再 `virtblk_map_data`（`:214`）把数据页收成 scatterlist。没有数据的 flush 可以没有中间段。然后 `blk_mq_start_request`。

`virtblk_add_req`（`:140`）组最多三段：

```text
out：out_hdr，设备读
out 或 in：sg_table。type 带 VIRTIO_BLK_T_OUT 时算 out（写盘），否则算 in（读盘）
in：in_hdr，长度 in_hdr_len，设备写
```

三段交给 `virtqueue_add_sgs(..., GFP_ATOMIC)`。`queue_rq` 持有队列自旋锁，不能在这里睡眠。`virtqueue_add_split` 怎样发布 `avail->idx` 见 [10](./10-virtqueue.md)。

`virtio_queue_rq` 在加描述符失败时仍 `virtqueue_kick` 一次（`:448`），让后端把已经进环的请求取走；`-ENOSPC` 才 `blk_mq_stop_hw_queue`。成功时只有 `bd->last` 为真且 `virtqueue_kick_prepare` 为真才把 `notify` 记下，放锁之后再 `virtqueue_notify`（`:459`）。一次 blk-mq 派发里的最后一个请求才可能产生 kick。前面的请求只更新了 avail ring。后端若只在 kick 时醒来，它醒来后应该把 avail idx 之后所有新描述符都取走，而不是只取一个。

`virtqueue_notify` 变成 `vp_notify` 的 `iowrite16`。从这里起离开 virtio-blk，进入 [08](./08-pio-mmio-notify.md) 的 trap。ioeventfd 命中时 vCPU 线程不会把 `KVM_EXIT_MMIO` 交回 QEMU；用户态后端线程在 eventfd 上醒来。

---

## 11.3 客户机侧完成

中断路径是 `virtblk_done`（`:351`），它在 `init_vq` 里被登记成普通队列的 callback。函数持队列锁，`virtqueue_disable_cb` 之后循环 `virtqueue_get_buf`。每个取回的 `virtblk_req` 用 `blk_mq_rq_from_pdu` 找回 `request`，再 `blk_mq_complete_request`（`:367`）。`virtio_mq_ops.complete` 指向 `virtblk_request_done`（`:1292`）。后者 `virtblk_unmap_data`，用 `virtblk_result(virtblk_vbr_status(vbr))` 得到 `blk_status_t`，`blk_mq_end_request`（`:335`）。

轮询队列的 `virtblk_poll`（`:1259`）同样 `virtqueue_get_buf`，若 `blk_mq_complete_request_remote` 为假且没有放进 batch，就直接 `virtblk_request_done`。batch 的收尾是 `virtblk_complete_batch`（`:1248`），最后 `blk_mq_end_request_batch`。两条路径都依赖 used ring 里的 id 还能对上 `desc_state`。后端若在客户机 reset 之后继续写 used ring，驱动已经把队列标成 broken 或已经释放 PDU，这次写就是在途请求的生命周期问题，见 [13](./13-reset-migration.md)。

---

## 11.4 后端只到 API 边界

用户态后端被 kick eventfd 叫醒之后，要做的事情和内核 vhost 的 split 路径同构，但代码不在 Linux 树里：

1. 读客户机 avail ring 的 `idx` 和 `ring[]`。这些 GPA 通过 memslot 的 HVA 访问，见 [06](./06-guest-memory.md)。
2. 按 `vring_desc` 的 `addr`/`len`/`flags` 读 out 头和数据，把 in 段写回。状态字节写在 in 头的最后一字节。
3. 把描述符头下标和写入长度放进 used ring，屏障之后增加 `used->idx`。
4. 按 event idx 或 `NO_INTERRUPT` 决定要不要写 call eventfd。call fd 若登记为 `KVM_IRQFD`，注入走 [07](./07-irq-clock.md)，客户机随后进入 `virtblk_done`。

本章不引用 QEMU 的函数名，也不把 v6.6.160 这个 Linux 标签当成 QEMU 的版本。QEMU 的源码版本要单独钉。内核侧能核对的是：没有 `drivers/vhost/blk.c`，`include/uapi/linux/vhost.h` 里的后端专用 ioctl 是 `VHOST_NET_*`、`VHOST_SCSI_*`、`VHOST_VSOCK_*` 和 `VHOST_VDPA_*`，没有 `VHOST_BLK`。把 virtio-blk 说成「走内核 vhost-blk」在这个版本里没有对应文件。vhost 的通用队列代码和实际存在的后端在 [12](./12-vhost.md)。vhost-scsi 服务的是 virtio-scsi，不是这条 `virtio_blk_outhdr`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← virtqueue](./10-virtqueue.md) · [目录](./README.md) · [vhost →](./12-vhost.md)
