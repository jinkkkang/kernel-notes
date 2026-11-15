# 13. reset、在途请求与 dirty log

**结论**：客户机拆设备时，`virtio_reset_device` 保证正在跑的队列回调已经停下，然后传输层 `reset`。调用者还得保证没有别的上下文正在 `virtqueue_notify`。已经放进环、后端还没写 used 的请求是在途请求：reset 之后后端再写 used ring，客户机已经不认这笔 `data` 指针。迁移时内核负责的是 memslot 的 dirty bitmap（或 dirty ring），不是 QEMU 怎样把设备状态写进迁移流。

---

## 13.1 设备 reset 与队列

`virtio_reset_device`（`drivers/virtio/virtio.c:220`）的注释写了三件事：阻止设备再发中断和再访问内存；调用时不能有正在进行的 `virtqueue_notify` / `virtqueue_kick`；函数保证队列回调不在进行，但系统调用、工作队列和下半部要调用者自己停。`CONFIG_VIRTIO_HARDEN_NOTIFICATION` 下它先 `virtio_break_device` 再 `virtio_synchronize_cbs`，然后 `dev->config->reset`。没有这个选项时直接 reset。broken 标志让随后的 `virtqueue_notify` 返回假（`virtio_ring.c:2377`）。

virtio-blk 的拆除顺序在 `virtblk_remove`（`drivers/block/virtio_blk.c:1597`）：`flush_work` 停掉配置空间工作项，`del_gendisk`，释放 tag set，然后在 `vdev_mutex` 里 `virtio_reset_device`，把 `vblk->vdev` 置 NULL，再 `del_vqs`。注释写明队列停了之后不再有人使用 `vblk->vdev`。

休眠路径 `virtblk_freeze`（`:1624`，`CONFIG_PM_SLEEP`）先 `blk_mq_freeze_queue` 和 `blk_mq_quiesce_queue_nowait`，注释要求删队列之前 virtqueue 里不再有请求，然后才 `virtio_reset_device` 和 `del_vqs`。`virtblk_restore` 再把队列建回来。

单条队列的 `virtqueue_reset`（`virtio_ring.c:2848`）要求调用时没有并发的其他队列操作。它 `virtqueue_disable_and_recycle`，对每个还没取走的缓冲调用 `recycle`，再按 packed 或 split `reinit`，最后 `virtqueue_enable_after_reset`。失败时 `-EBUSY` 表示没能和设备同步，注释写明队列可能已经不能用。`-ENOENT` 表示传输或设备不支持。`VIRTIO_F_RING_RESET`（`virtio_config.h:111`，bit 40）是这条按队列重置的 feature。vhost-net 的 feature 掩码包含这一位（`drivers/vhost/net.c:73`），vhost 共用的 `VHOST_FEATURES` 不包含它。

---

## 13.2 在途请求

一笔 virtio-blk 请求在 `virtqueue_add_sgs` 成功之后、`virtqueue_get_buf` 把它交回之前，描述符在环上，`desc_state[].data` 仍指向 `virtblk_req`。后端可能已经在读数据页。reset 若发生在这中间：

- 客户机侧 `recycle` 或 `del_vqs` 会释放或回收这块 PDU。后端若还把状态字节写进那块客户机内存，写的是 GPA，页可能还在；但客户机不再把这块内存当 in 头。
- 后端若在 reset 之后 `vhost_add_used` 或用户态写 used ring，客户机的回调已经被 synchronize 掉，或者队列已 broken，这次 used 更新没有驱动来取。
- kick eventfd 和 call eventfd 如果还挂在旧队列上，复位之后的第一次通知会打进已经拆掉的队列。vhost 的 `VHOST_SET_VRING_KICK` 用 `VHOST_FILE_UNBIND`（`include/uapi/linux/vhost.h:18`，值 -1）解开 fd。用户态后端要在设备 reset 的同一临界区里解开 ioeventfd / irqfd。

内核没有一个「取消在途 virtio-blk」的通用 ioctl。客户机能做的是冻住 blk-mq、reset、再保证后端不再碰旧环。后端协议上的取消属于用户态设备实现，本章不写 QEMU 的请求引用计数。

---

## 13.3 dirty log 停在内核 bitmap

迁移要知道客户机写过哪些 GPA。用户态在 `KVM_SET_USER_MEMORY_REGION` 时把 `KVM_MEM_LOG_DIRTY_PAGES` 放进 flags（`include/uapi/linux/kvm.h:104`）。之后 `KVM_GET_DIRTY_LOG`（`kvm.h:1470`）传入：

```c
/* kvm.h:624 */
struct kvm_dirty_log {
	__u32 slot;          /* 低 16 位是 slot id，高 16 位是地址空间 */
	__u32 padding1;
	union {
		void __user *dirty_bitmap; /* 每一页一位 */
		__u64 padding2;
	};
};
```

x86 打开了 `KVM_GENERIC_DIRTYLOG_READ_PROTECT`（`arch/x86/kvm/Kconfig:47`）。因此 VM ioctl 走 `kvm_vm_ioctl_get_dirty_log`（`virt/kvm/kvm_main.c:2236`），里面调用 `kvm_get_dirty_log_protect`（`:2145`），不走同文件 `#ifndef` 里那份不带写保护的 `kvm_get_dirty_log`。`kvm_get_dirty_log_protect` 的注释把顺序写成：对 bitmap 做快照并按需要清位，把对应页改回只写保护，把快照拷给用户态，返回后由调用者刷新 TLB。快照和写保护之间，客户机仍可能靠还没失效的 TLB 写页；这一笔算在本次快照里，刷新之后的写留给下一次读取。

`slot` 的高 16 位是地址空间号（`:2159`）。没有 bitmap（没开日志，或 slot 不存在）返回 `-ENOENT`。若当前 VM 用 dirty ring 而不是 bitmap，`kvm_use_dirty_bitmap` 为假，返回 `-ENXIO`（`:2156`）。dirty ring 满时 vCPU 退出原因是 `KVM_EXIT_DIRTY_RING_FULL`（`kvm.h:261`）。那是另一条把脏页交给用户态的路径，同样停在内核记录「哪一页被写过」。

用户态拿到 bitmap 之后怎样传给目的机器、怎样在目标上重建 virtio 设备，是 QEMU 迁移流的事。内核不保存 virtqueue 的 avail idx。迁移恢复时，设备状态（特征、队列地址、last avail idx）要由用户态重新 `VHOST_SET_VRING_BASE` 或重新协商。本章不写那条用户态状态机。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← vhost](./12-vhost.md) · [目录](./README.md) · [观测 →](./14-observability.md)
