# 1. 全景与职责

**结论**：一次云上磁盘读要穿过三套代码。客户机内核的 virtio-blk 把请求放进 virtqueue；宿主机内核的 KVM 负责让客户机 CPU 真正执行，并在 PIO/MMIO 或注入中断时插手；QEMU（或另一个用户态进程）或者内核 vhost 负责把队列里的描述符变成真实 IO。这三套代码用 fd、共享内存和 eventfd 对接，不共用一个函数调用栈。

> 内存章已经写了 GPA 怎样经过 EPT 变成 HPA。本章从「谁创建 VM、谁执行客户机指令、谁消费 virtqueue」写起，地址翻译只在 [06](./06-guest-memory.md) 接到那一章。

C 新手把虚拟机看成两个进程视角加一块共享内存。QEMU 是宿主机上的普通进程，它打开 `/dev/kvm`。客户机内核是另一套地址空间里的内核，它看见的 PCI 设备和 APIC 多数是 KVM 或 QEMU 做出来的。两边同时看见的，是 QEMU `mmap` 出来、又用 `KVM_SET_USER_MEMORY_REGION` 登记成客户机物理内存的那几段。

---

## 1.1 五个角色

| 角色 | 跑在哪 | 6.6 里能对上的代码 |
|---|---|---|
| QEMU 或别的 VMM | 宿主机用户态 | 本章不读它的源码。它必须使用的接口在 `include/uapi/linux/kvm.h` |
| KVM | 宿主机内核 | `virt/kvm/kvm_main.c`，x86 在 `arch/x86/kvm/` |
| 客户机内核 | 客户机里 | virtio 驱动在 `drivers/virtio/`、`drivers/block/virtio_blk.c` |
| virtqueue | 客户机物理内存里的环 | 布局在 `include/uapi/linux/virtio_ring.h`，客户机操作在 `drivers/virtio/virtio_ring.c` |
| 后端 | 宿主机用户态，或宿主机内核的 vhost | 内核后端在 `drivers/vhost/`。v6.6.160 有 net、scsi、vsock、vdpa，没有通用 blk 后端 |

QEMU 创建 VM 和 vCPU，把客户机内存登记进 KVM，然后在一个线程里循环 `KVM_RUN`。这个线程就是该 vCPU 的宿主机线程。`kvm_vcpu_ioctl` 在第一次 `KVM_RUN` 时把 `vcpu->pid` 记成当前线程（`virt/kvm/kvm_main.c:4148`）。

客户机驱动不调用 `KVM_RUN`。它执行普通的 PIO、MMIO 和内存访问。这些访问在 VMX/SVM 上会按配置产生 VM-exit，退出处理在宿主机内核里决定：自己做完再进客户机，还是把 `kvm_run->exit_reason` 填好后从 ioctl 返回。

---

## 1.2 两条边界

```text
宿主机用户态 QEMU
    ioctl / mmap / eventfd
宿主机内核 KVM 与 vhost
    VM-entry / VM-exit，客户机物理内存
客户机内核 virtio 驱动
```

上面那条边界是 Linux 的用户态 ABI：`KVM_*` ioctl、vhost 的 `VHOST_*` ioctl、eventfd。下面那条边界是 CPU 的虚拟化指令加上客户机看见的设备寄存器。客户机内核的系统调用不会直接进入 QEMU。它先在客户机里执行，VM-exit 之后才进 KVM。

因此有两句要分开记：

1. 线程执行 `ioctl(vcpu_fd, KVM_RUN)` 时，已经在宿主机内核。`vcpu_enter_guest` 把 `vcpu->mode` 写成 `IN_GUEST_MODE`（`arch/x86/kvm/x86.c:10795`）之后，才会调用 `vmx_vcpu_run` 或 `svm_vcpu_run`。在此之前的请求处理和 `complete_userspace_io` 都还是宿主机内核逻辑。
2. VM-exit 返回到 `vcpu_enter_guest` 里 `static_call(kvm_x86_vcpu_run)` 的下一行（`x86.c:10883`）。退出处理函数返回 1 时，`vcpu_run` 的循环继续，QEMU 仍堵在同一次 ioctl 里。返回 0 时，ioctl 才结束，`trace_kvm_userspace_exit` 在 `kvm_main.c:4167` 记下这次回到用户态。

---

## 1.3 virtio 接在哪一层

virtio 设备对客户机来说是一个 PCI（或 MMIO）设备加若干队列。协商在客户机驱动的 `virtio_dev_probe`（`drivers/virtio/virtio.c:237`）。数据面是 virtqueue：客户机把描述符放进环，再写通知寄存器。这个写操作是一次客户机 MMIO 或 PIO。

KVM 看到这次访问时有三条去向，[08](./08-pio-mmio-notify.md) 按代码分开：

- 内核里注册的 ioeventfd 命中，`eventfd_signal` 之后不再回到 QEMU 的这次 `KVM_RUN`。
- 没有设备认领，`kvm_run` 填 `KVM_EXIT_IO` 或 `KVM_EXIT_MMIO`，ioctl 返回。
- 访问的是客户机 RAM，根本不是一次设备退出，走内存章的 EPT/NPT。

后端被 eventfd 叫醒之后去读 virtqueue。用户态后端读的是自己映射的 HVA。内核 vhost 用 `vhost_get_vq_desc` 把同一段 GPA 翻译成 `struct iovec`。做完之后后端写 used ring，再敲另一只 eventfd；那只 fd 如果登记成 irqfd，就变成客户机中断。中断注入的 LAPIC 细节在 [interrupts/11](../interrupts/11-kvm-irq-virt.md)。

---

## 1.4 本章写到哪

写：三类 fd、`KVM_RUN` 在返回用户态之前做的事、VMX 上一次进入和退出、vCPU 线程怎样因为 HLT 睡觉、virtqueue 的 split/packed 布局、virtio-blk 在客户机里怎样提交和收割、vhost 怎样配置队列和访问描述符、reset 与 dirty log 停在哪个 ioctl。

不写：EPT 页表项的格式、virtio-net 的 skb 与 tap、IRTE 的硬件格式、VFIO 的设备绑定、QEMU 迁移的全流程。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[目录](./README.md) · [用户态 API →](./02-userspace-api.md)
