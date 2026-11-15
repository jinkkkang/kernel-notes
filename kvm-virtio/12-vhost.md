# 12. vhost

**结论**：内核 vhost 把 virtqueue 的消费从 QEMU 线程挪到 `vhost_worker`。用户态用 `VHOST_SET_OWNER`、内存表、`VHOST_SET_VRING_*` 和两只 eventfd 把队列交进来。worker 在 kick fd 上醒来，`vhost_get_vq_desc` 按 **split** 描述符把 GPA 收成 `iovec`，设备做完后 `vhost_add_used` 写 used ring，`vhost_signal` 敲 call eventfd。`VHOST_FEATURES` 不包含 `VIRTIO_F_RING_PACKED`。`vhost_get_vq_desc` 读的是 `vring_desc` 和 avail ring。客户机 `virtio_ring.c` 能建 packed 队列，不能据此说 vhost-net 能解析 packed 描述符。

> `vhost_net` 怎样对 tap `sendmsg` / `recvmsg`，网络章 [14](../networking/14-virtio-net-host-vhost.md) 已经写了。这里写所有 `vhost_*` 共用的队列配置和描述符访问，并用 vhost-net、vhost-scsi、vhost-vsock 作为树里真实存在的后端。

---

## 12.1 树里有哪些后端

`drivers/vhost/` 在 v6.6.160 有 `vhost.c`（共用层）、`net.c`、`scsi.c`、`vsock.c`、`vdpa.c`、`test.c`。没有 `blk.c`。用户态头 `include/uapi/linux/vhost.h` 从 `:126` 起分别给出 net、scsi、vsock、vdpa 的 ioctl，没有 blk 一组。

| 后端 | 客户机设备 | 数据面入口 |
|---|---|---|
| `vhost_net` | virtio-net | `handle_tx_kick`（`net.c:1273`）走到 `vhost_net_tx_get_vq_desc` 里的 `vhost_get_vq_desc`（`:593`）；RX 侧调用在 `:1081` |
| `vhost_scsi` | virtio-scsi | `vhost_get_vq_desc`（`scsi.c:466`、`:954`） |
| `vhost_vsock` | virtio-vsock | `vhost_get_vq_desc`（`vsock.c:127`、`:499`） |
| `vhost_vdpa` | 由 vDPA 父设备决定 | 下面单独说。它不把自己的数据面做成 `vhost_get_vq_desc` |
| `vhost_test` | 测试模块 | 同样调用 `vhost_get_vq_desc`（`test.c:61`） |

virtio-blk 的用户态后端和这些不是同一个驱动。vDPA 父设备可以是块设备，那是父驱动的队列，不是 `drivers/vhost/blk.c`。

---

## 12.2 配置 ioctl 与 worker

共用 ioctl 在 `include/uapi/linux/vhost.h`：

| ioctl | 行 | 作用 |
|---|---|---|
| `VHOST_SET_OWNER` | 32 | 当前进程成为 fd 的所有者。之后的命令要由它发出 |
| `VHOST_SET_MEM_TABLE` | 38 | 把 GPA 区间登记成用户地址 |
| `VHOST_SET_VRING_NUM` | 71 | 描述符个数。队列已经绑后端时不能改 |
| `VHOST_SET_VRING_ADDR` | 73 | desc / avail / used 三个用户指针 |
| `VHOST_SET_VRING_BASE` | 75 | split 时是 last avail idx；见下节 packed 分支 |
| `VHOST_SET_VRING_KICK` | 109 | 客户机 kick 用的 eventfd |
| `VHOST_SET_VRING_CALL` | 111 | 写 used 之后用来通知客户机的 eventfd |
| `VHOST_NEW_WORKER` | 61 | 额外创建一个 worker，再 `VHOST_ATTACH_VRING_WORKER` 绑到队列 |

`vhost_worker_create`（`drivers/vhost/vhost.c:675`）用 `vhost_task_create(vhost_run_work_list, ...)` 起线程，名字是 `vhost-%d`，`%d` 是创建时的 `current->pid`（`:688`）。`vhost_run_work_list`（`:414`）取出 llist 里的 `vhost_work`，逐个调用 `work->fn`。kick fd 可读时，poll 回调把 `vq->handle_kick` 排进这个链表。worker 与 vCPU 线程不是同一条：vCPU 线程可以继续 `KVM_RUN`，worker 在自己的 kick 上阻塞。

`struct vhost_virtqueue` 的前半（`vhost.h:81`）和访问有关的成员：

```c
struct vhost_virtqueue {
	struct vhost_dev *dev;
	struct vhost_worker __rcu *worker;
	struct mutex mutex;
	unsigned int num;
	vring_desc_t __user *desc;     /* 客户机地址空间里的 split 描述符 */
	vring_avail_t __user *avail;
	vring_used_t __user *used;
	const struct vhost_iotlb_map *meta_iotlb[VHOST_NUM_ADDRS];
	struct file *kick;
	struct vhost_vring_call call_ctx;
	/* last_avail_idx 的注释：数值限制在 0x7fff，
	 * 高位在 VIRTIO_F_RING_PACKED 时用作 wrap。见 :102。 */
	u16 last_avail_idx;
	u16 avail_idx;
	u16 last_used_idx;
	/* ... */
};
```

`vhost_vring_ioctl` 的 `VHOST_SET_VRING_KICK` 分支从 `vhost.c:2060` 起，`eventfd_fget` 在 `:2065`，然后换上 `vq->kick` 并启停 poll。`VHOST_SET_VRING_CALL` 从 `:2076` 起，`:2087` 把 ctx 换进 `vq->call_ctx.ctx`。

---

## 12.3 split 描述符怎样被读

`vhost_get_vq_desc`（`vhost.c:2662`）的注释说明返回值是描述符头下标；没有新缓冲时返回 `vq->num`；错误是负数。步骤：

1. `avail_idx == last_avail_idx` 时 `vhost_get_avail_idx`。仍没有新项就返回 `vq->num`（`:2673`）。
2. `vhost_get_avail_head` 读 `avail->ring[last_avail_idx % num]`（`:2684`）。这是 split 的下标环，不是 packed 的标志位。
3. 沿着 `vring_desc` 走。`VRING_DESC_F_INDIRECT` 交给 `get_indirect`（`:2561`），后者按 `VRING_DESC_F_NEXT` 链接，并拒绝嵌套 indirect（`:2612`）。
4. `VRING_DESC_F_WRITE` 的段用 `VHOST_ACCESS_WO`，其余用 `VHOST_ACCESS_RO`。`translate_desc`（`:2500`）在 IOTLB 里把 GPA 收成 `iov_base` 指向的用户地址。
5. out 段必须全部出现在 in 段之前，否则 `-EINVAL`（`:2761`）。
6. 成功后 `last_avail_idx++`（`:2773`）。函数假定 `used_flags` 已经含 `VRING_USED_F_NO_NOTIFY`，否则 `BUG_ON`（`:2777`）。调用方要在取描述符之前关掉客户机 kick，取完一批再按 event idx 打开。

`vhost_add_used`（`:2791`）把 id 和长度写进 used ring。`vhost_signal`（`:2912`）在 `call_ctx.ctx` 非空且 `vhost_notify` 为真时 `eventfd_signal`。`vhost_notify` 在没有 `VIRTIO_RING_F_EVENT_IDX` 时看 avail flags 的 `NO_INTERRUPT`（`:2888`），有 event idx 时用 `vring_need_event`（`:2908`）。`vhost_add_used_and_signal`（`:2921`）把两步接起来。call fd 之后若是 irqfd，注入不再经过 vhost。

和用户态后端相比：用户态在自己的线程里读同一块 HVA，内核 vhost 用 worker 和 `copy_from_user` 这一类访问。两边都依赖用户态事先把 GPA 和 HVA 的对照登记对。vhost 少一次「VM-exit 回到 QEMU 再读环」。kick 仍是一次客户机通知寄存器写，只是 ioeventfd 的对端从 QEMU 换成了 vhost 的 poll。

---

## 12.4 packed 的边界，以及 vhost-user / vDPA

`VHOST_FEATURES`（`vhost.h:259`）包含 `VIRTIO_F_NOTIFY_ON_EMPTY`、`VIRTIO_RING_F_INDIRECT_DESC`、`VIRTIO_RING_F_EVENT_IDX`、`VHOST_F_LOG_ALL`、`VIRTIO_F_ANY_LAYOUT`、`VIRTIO_F_VERSION_1`。没有 `VIRTIO_F_RING_PACKED`。`VHOST_NET_FEATURES`（`net.c:73`）在此之上加了 `VHOST_NET_F_VIRTIO_NET_HDR`、`VIRTIO_NET_F_MRG_RXBUF`、`VIRTIO_F_ACCESS_PLATFORM`、`VIRTIO_F_RING_RESET`，仍然没有 packed。

`vhost_vring_ioctl` 的 `VHOST_SET_VRING_BASE` 在 `vhost_has_feature(vq, VIRTIO_F_RING_PACKED)` 时把 `s.num` 的低 16 位和高 16 位分别存进 `last_avail_idx` 与 `last_used_idx`（`vhost.c:2038`）。这是索引的打包格式，函数下面没有 packed 描述符循环。`vhost_get_vq_desc` 仍然读 `vq->avail->ring`。因此即使用户态把 packed 位写进了某个 vq 的 `acked_features`，这条取描述符的函数也不按 `vring_packed_desc` 解析。

`vhost_vdpa` 在 `vdpa.c:700` 和 `:733` 按同一 feature 位复制 `vq_state.packed` 的 last avail/used idx 和 counter。数据面在 vDPA 父设备里。例如 `drivers/vdpa/mlx5/net/mlx5_vnet.c` 的 feature 掩码包含 `VIRTIO_F_RING_PACKED`。那是设备自己的队列，不是 `vhost_get_vq_desc` 学会了 packed。

vhost-user 把同一套「内存表 + kick + call + 环地址」换成用户态进程之间的套接字消息。Linux 6.6 的 `drivers/vhost/` 不实现那个协议。网络章 14.1 只点名这条路径。本章同样不读 QEMU 或 DPDK 的源码。

`vhost_net` 从客户机 TX 队列取出描述符之后对 tap `sendmsg`，RX 方向相反。报文进 `sk_buff` 之后的协议栈不在本章，从 [networking/14](../networking/14-virtio-net-host-vhost.md) 的 `handle_tx` 接下去。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← virtio-blk](./11-virtio-blk.md) · [目录](./README.md) · [reset →](./13-reset-migration.md)
