# 8. PIO、MMIO 与通知

**结论**：客户机对端口或 MMIO 寄存器的一次访问是 trap。内核 IO 总线若有设备认领，这次 trap 在 `KVM_RUN` 内部结束。没有设备时，ioctl 返回，用户态看到 `KVM_EXIT_IO` 或 `KVM_EXIT_MMIO`。virtio 的 kick 常常被登记成 ioeventfd：trap 仍发生，但内核只 `eventfd_signal`，不把退出交给 QEMU。eventfd 的值不是描述符内容。后端做完之后另写 used ring，再敲 irqfd，那才是完成通知。

---

## 8.1 内核总线先于用户态

`kvm_io_bus_write`（`virt/kvm/kvm_main.c:5464`）在对应的 `kvm->buses[bus_idx]` 上按地址找设备。`__kvm_io_bus_write`（`:5443`）对每个重叠的设备调用 `kvm_iodevice_write`。设备返回 0 表示认领，函数把下标变成 0 返回。没有任何设备返回 0 时，返回 `-EOPNOTSUPP`。

`emulator_pio_in_out`（`arch/x86/kvm/x86.c:7917`）把端口访问交给 `KVM_PIO_BUS`。`kvm_io_bus_write` 返回非 0 才进入 `userspace_io`（`:7931`）。返回 0 表示内核设备已经完成，函数返回 1，`kvm_fast_pio` 跳过指令，用户态看不到 `KVM_EXIT_IO`。

没有设备时从 `userspace_io`（`x86.c:7948`）起填充，第一项赋值在 `:7949`：

- 记下 `port`、`in`、`count`、`size`。
- IN：把 `pio_data` 清 0，等用户态在下一次 `KVM_RUN` 之前写上结果。
- OUT：把客户机要写出的字节 `memcpy` 进 `pio_data`。
- `exit_reason = KVM_EXIT_IO`，`direction` 用 `KVM_EXIT_IO_IN`（0）或 `KVM_EXIT_IO_OUT`（1），`data_offset = KVM_PIO_PAGE_OFFSET * PAGE_SIZE`。

`trace_kvm_pio` 在 OUT 的路径上于总线尝试之前触发（`emulator_pio_out`，`:8011`），IN 则在内核完成时或用户态完成之后。事件字段是 `rw`、`port`、`size`、`count`、`val`（`arch/x86/kvm/trace.h:161`）。`KVM_PIO_OUT` 是 1（`:159`），打印成 `pio_write`。一次用户态退出因此可以既有 `kvm_pio` 又有稍后的 `kvm_userspace_exit`。前者是指令模拟碰到了端口，后者是 ioctl 真的返回了。

MMIO 的用户态缓冲区在 `kvm_run->mmio`，最多 8 字节（`kvm.h:334` 起，`:333` 是 `KVM_EXIT_MMIO` 注释）。更长的访问由 `complete_emulated_mmio` 拆成多次退出（`x86.c:11164`）。coalesced MMIO 用 vCPU 映射的另一页（`KVM_COALESCED_MMIO_PAGE_OFFSET` 是 2，`asm/kvm.h:15`）把多次写攒起来，本章不展开环的槽格式。

---

## 8.2 ioeventfd：trap 变成 notify

`KVM_IOEVENTFD`（`kvm.h:1518`）登记：

```c
/* include/uapi/linux/kvm.h:827 */
struct kvm_ioeventfd {
	__u64 datamatch;   /* FLAG_DATAMATCH 时要和写入值相等 */
	__u64 addr;        /* PIO 端口或 MMIO 的 GPA */
	__u32 len;         /* 1、2、4、8；0 表示只比地址 */
	__s32 fd;
	__u32 flags;       /* DATAMATCH、PIO、DEASSIGN、VIRTIO_CCW_NOTIFY */
	__u8  pad[36];
};
```

`KVM_IOEVENTFD_FLAG_PIO` 把总线选成 PIO，否则是 MMIO（`ioeventfd_bus_from_flags`，`virt/kvm/eventfd.c:830`）。注册之后，设备的 `write` 是 `ioeventfd_write`（`:782`）。`ioeventfd_in_range`（`:740`）要求地址相等；`len` 非 0 时长度也要相等；没有 wildcard 时还要比对 `datamatch`。命中则 `eventfd_signal(p->eventfd)` 并返回 0。返回 0 被 IO 总线当成「已经认领」，`emulator_pio_in_out` 不会走向 `userspace_io`。

virtio-pci 的 kick 是客户机对通知寄存器的一次写。modern 设备的 `vp_notify`（`drivers/virtio/virtio_pci_common.c:41`）执行 `iowrite16(vq->index, vq->priv)`，把队列号写进通知寄存器。对客户机这是一次 MMIO。QEMU 若把这个地址登记成 ioeventfd，并且 `datamatch` 等于队列号（或 `len == 0` 只比地址），这次 VM-exit 在内核里变成 eventfd 计数加一。描述符仍然在客户机内存的 avail ring 里。eventfd 的计数只表示「来看队列」，不携带 `vring_desc`。

地址或数据对不上时 `ioeventfd_write` 返回 `-EOPNOTSUPP`，总线继续问下一个设备。都没有认领，用户态才看到 `KVM_EXIT_MMIO` 或 `KVM_EXIT_IO`。所以同一个通知寄存器，在登记 ioeventfd 之前和之后，`kvm_userspace_exit` 的次数可以差一个数量级，而客户机驱动的 `virtqueue_notify` 代码没有变。

---

## 8.3 三件事不要记成一次退出

| 步骤 | 谁执行 | 内核里的标志 |
|---|---|---|
| trap | 客户机指令访问端口或 MMIO，VM-exit | `kvm_exit` / `kvm_pio` / `kvm_mmio`。处理函数可能返回 1 |
| notify | ioeventfd 或用户态在 `KVM_EXIT_*` 之后自己写 kick fd | `ioeventfd_write` 的 `eventfd_signal`。vhost 则在 `VHOST_SET_VRING_KICK` 的 fd 上 poll |
| 完成 | 后端写 used ring，再信号 call fd | `vhost_signal` 或用户态写 call eventfd。irqfd 的 `irqfd_wakeup` 再注入客户机 |

完成通知走 [07](./07-irq-clock.md) 的 irqfd。客户机里 `vring_interrupt` 调用驱动登记的 `callback`。virtio-blk 的 callback 是 `virtblk_done`。从 kick 到 callback 之间，vCPU 线程可能一直停在下一次 `KVM_RUN` 里，也可能因为 HLT 睡在 `kvm_vcpu_block`，要等 irqfd 的 kick 把它唤醒。用户态后端若在自己的线程里做完 IO，这段等待不在 vCPU 线程的 `KVM_RUN` 里面，而在后端线程的 read/poll 里面。观测时要把这两条线程分开，见 [14](./14-observability.md)。

demo 不登记 ioeventfd，也不创建 irqfd。它的 `OUT` 因此一定是用户态退出，`HLT` 在无 irqchip 时也是用户态退出。这不能用来预测一个已经配置好 virtio 设备的虚拟机。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 中断与时钟](./07-irq-clock.md) · [目录](./README.md) · [virtio 协商 →](./09-virtio-negotiate.md)
