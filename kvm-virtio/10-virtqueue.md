# 10. virtqueue 与 vring

**结论**：客户机驱动通过 `virtqueue_add` 在描述符里写下 GPA、长度和方向，再把描述符头下标放进 avail ring，用 `virtio_wmb` 之后才增加 `avail->idx`。`virtqueue_kick_prepare` 用另一道屏障读 event idx，决定要不要调用传输层的 `notify`。后端完成后把 id 和长度写进 used ring，客户机在回调里 `virtqueue_get_buf` 取回当时传入的 `data` 指针。split 和 packed 是两套环，`vring_create_virtqueue` 按 `VIRTIO_F_RING_PACKED` 二选一。

---

## 10.1 驱动看见的队列对象

```c
/* include/linux/virtio.h:30
 * 这是驱动持有的头。描述符环在 priv 指向的 vring_virtqueue 里。
 */
struct virtqueue {
	struct list_head list;
	void (*callback)(struct virtqueue *vq); /* 完成时调用，可空（轮询队列） */
	const char *name;
	struct virtio_device *vdev;
	unsigned int index;
	unsigned int num_free;
	unsigned int num_max;
	bool reset;
	void *priv;
};
```

`virtqueue_add_sgs`（`virtio_ring.c:2243`，声明在 `virtio.h:58`）把若干 scatterlist 分成 out 段和 in 段，调 `virtqueue_add`（`:2212`）。`virtqueue_add` 看 `vq->packed_ring`：真则 `virtqueue_add_packed`（`:1401`），假则 `virtqueue_add_split`（`:544`）。`packed_ring` 在 `vring_create_virtqueue_packed` 里置真（`:2105`），在 split 创建路径置假（`:2637`）。选择发生在 `vring_create_virtqueue`（`:2680`）：`virtio_has_feature(vdev, VIRTIO_F_RING_PACKED)` 才走 packed。

---

## 10.2 split：desc、avail、used

布局类型在 `include/uapi/linux/virtio_ring.h`：

```c
/* :107。addr 是客户机物理地址，不是宿主机虚拟地址。 */
struct vring_desc {
	__virtio64 addr;
	__virtio32 len;
	__virtio16 flags;   /* NEXT=1，WRITE=2，INDIRECT=4 */
	__virtio16 next;
};

/* :114 */
struct vring_avail {
	__virtio16 flags;   /* NO_INTERRUPT=1 时，后端可以不敲中断 */
	__virtio16 idx;     /* 客户机发布到哪 */
	__virtio16 ring[];  /* 每项是 desc 的头下标 */
};

/* :121 与 :131 */
struct vring_used_elem {
	__virtio32 id;
	__virtio32 len;     /* 后端写入的总长度 */
};
struct vring_used {
	__virtio16 flags;   /* NO_NOTIFY=1 时，客户机可以不 kick */
	__virtio16 idx;
	vring_used_elem_t ring[];
};
```

`virtqueue_add_split` 从 `free_head` 取描述符，按 out 然后 in 的顺序填 `addr`/`len`/`flags`。链的最后一项清掉 `VRING_DESC_F_NEXT`（`:645`）。需要 indirect 时，这些描述符放在单独分配的表里，环上只放一条 `VRING_DESC_F_INDIRECT`（`:650`）。indirect 表本身用 `vring_map_single` 映射；`use_dma_api` 为真时这是一次 DMA API 映射，地址含义见 dma-iommu 章，本章不展开 `dma_map_ops`。

发布的顺序在 `:685`：

1. 把头下标写入 `avail->ring[avail_idx_shadow & (num - 1)]`。
2. `virtio_wmb(vq->weak_barriers)`。注释写明描述符和 avail 槽必须在暴露新的 idx 之前对后端可见。
3. `avail_idx_shadow++`，再把它写进 `avail->idx`。
4. `num_added++`。若它加到 `65535`，立刻 `virtqueue_kick`，避免 16 位计数绕回时丢掉通知（`:703`）。

`data` 指针存在 `vq->split.desc_state[head].data`（`:679`）。完成时 `virtqueue_get_buf` 用 used ring 的 id 找到它，交还给驱动。后端看不到这个内核指针；它只看到描述符里的 GPA。

`VIRTIO_RING_F_EVENT_IDX`（bit 29，`virtio_ring.h:87`）协商之后，used ring 末尾的 `avail_event` 和 avail ring 末尾的 `used_event` 代替 `NO_NOTIFY` / `NO_INTERRUPT` 标志。`vring_need_event`（`:222`）用 16 位回绕比较：`(new_idx - event_idx - 1) < (new_idx - old)` 时才需要通知。

---

## 10.3 kick：屏障，然后才是一次 MMIO

`virtqueue_kick_prepare_split`（`virtio_ring.c:734`）先 `virtio_mb`，注释要求 avail 槽已经可见再读 event idx。然后若 `vq->event` 为真，用 `vring_need_event` 比较 `vring_avail_event`；否则看 used flags 里有没有 `VRING_USED_F_NO_NOTIFY`（`:752`）。`virtqueue_kick_prepare`（`:2360`）把 split 和 packed 分出去。`virtqueue_notify`（`:2377`）在队列没被标 broken 时调用 `vq->notify`。PCI modern 的 `notify` 是 `vp_notify`：`iowrite16(vq->index, notify_addr)`（`virtio_pci_common.c:41`）。`virtqueue_kick`（`:2405`）就是 prepare 为真才 notify。

virtio-blk 在 `bd->last` 时自己调用 prepare，把 notify 挪到释放队列锁之后（`virtio_blk.c:459`）。这是同一套函数，只是把 MMIO 移出自旋锁。MMIO 之后发生的 trap 或 ioeventfd 在 [08](./08-pio-mmio-notify.md)。

packed 的 prepare 是 `virtqueue_kick_prepare_packed`（`:1568`），同样先 `virtio_mb`（`:1587`），event idx 仍调用 `vring_need_event`（`:1611`），但事件结构是 `vring_packed_desc_event`（`virtio_ring.h:232`），不是 split 的 `avail->flags`。

---

## 10.4 packed 与 indirect

packed 把 avail 和 used 合成一个描述符环。每一项：

```c
/* include/uapi/linux/virtio_ring.h:239 */
struct vring_packed_desc {
	__le64 addr;
	__le32 len;
	__le16 id;       /* 缓冲的 id，相当于 split used 元素的 id */
	__le16 flags;    /* AVAIL 在 bit 7，USED 在 bit 15，定义是移位不是掩码 */
};
```

`VRING_PACKED_DESC_F_AVAIL` 是 7，`VRING_PACKED_DESC_F_USED` 是 15（`virtio_ring.h:51`）。驱动发布时翻转 avail 位并保持 used 位与上一圈不同；后端使用后再把 used 位置成和 avail 相同。wrap counter 因此在标志位里，而不是一个单独的 `idx`。`virtqueue_add_packed`（`virtio_ring.c:1401`）沿这条环填描述符。indirect 在 packed 路径里同样是一条指向描述符表的描述符；split 的 `get_indirect` 那种 `next` 链是 vhost 在宿主机上读 split 环时用的，不能拿来读 packed 标志位。

客户机两边都实现了。`vring_create_virtqueue` 的分支就是证据。宿主机 vhost 的 `vhost_get_vq_desc` 只按 split 的 `vring_desc` 和 `avail->ring` 走，见 [12](./12-vhost.md)。客户机支持 packed，不能写成「vhost-net 也支持 packed」。

---

## 10.5 回调里取回缓冲

`virtqueue_get_buf`（`virtio_ring.c:2440`）调用 `virtqueue_get_buf_ctx`（`:2430`），后者按 `packed_ring` 分到 packed 或 split 实现。驱动在中断或轮询里调用它，传入的 `len` 接收 used 元素的长度。返回的指针是 `virtqueue_add_*` 时的 `data`。队列被 `virtio_break_device` 标成 broken 之后，`virtqueue_notify` 返回假，`virtqueue_get_buf` 不再交出新缓冲。reset 时要先停掉仍在调用 notify 的上下文，见 [13](./13-reset-migration.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 协商](./09-virtio-negotiate.md) · [目录](./README.md) · [virtio-blk →](./11-virtio-blk.md)
