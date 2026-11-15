# KVM 与 virtio

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION = 6`、`PATCHLEVEL = 6`、`SUBLEVEL = 160`，gregkh/linux 标签 `v6.6.160`，提交 `0aebf2870d53a8d97e02a99e2103ea0b38950c1b`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[git.kernel.org stable v6.6.160](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/?h=v6.6.160) · [gregkh/linux v6.6.160](https://github.com/gregkh/linux/tree/v6.6.160)
> 行号链接用 gregkh 的 blob（`#L` 能跳到行）。主线是 x86：Intel VMX 写完整进出，AMD SVM 只点差异。
> demo 01 能在没有 `/dev/kvm` 权限时以 77 退出。写作时没有跑通 `KVM_RUN`。观测命令默认是 **RECIPE**。

**一句话**：QEMU 用 `/dev/kvm` 把客户机的执行交进宿主机内核；客户机里的 virtio 驱动把请求放进 virtqueue，宿主机要么在用户态读这块共享内存，要么用 `vhost_*` 在内核里读。`KVM_RUN` 进入宿主机内核，不等于 CPU 已经在执行客户机。VM-exit 回到 KVM 的退出处理，不等于 ioctl 已经返回 QEMU。

面向 **C 语言新手**：`/dev/kvm`、VM fd、vCPU fd 是三个文件描述符。`struct kvm_run` 是 vCPU fd 上 `mmap` 出来的一块共享内存，内核把退出原因写在里面。virtqueue 是客户机和后端共用的一块客户机内存，加上一次「有新描述符」的通知。

## 阅读顺序

```text
01 全景（QEMU / KVM / 客户机驱动 / virtio / vhost）
  → 02 /dev/kvm 与三类 fd
  → 03 KVM_RUN 在宿主机内核里走到哪
  → 04 VM-entry 与 VM-exit（VMX 主线）
  → 05 vCPU 线程、HLT、halt poll、steal
  → 06 客户机执行和 PIO 用到的地址（EPT 不展开）
  → 07 虚拟中断与时钟
  → 08 PIO / MMIO / eventfd 通知
  → 09 virtio 协商
  → 10 virtqueue
  → 11 virtio-blk 请求
  → 12 vhost
  → 13 reset、在途请求、dirty log
  → 14 观测分层
```

先分清一次 ioctl 停在宿主机内核的哪一层，再读 virtqueue。队列答的是「描述符在客户机内存的哪一段」，KVM 答的是「谁把 CPU 送进客户机、谁在 VM-exit 之后决定还要不要回到 QEMU」。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | QEMU、KVM、客户机驱动、virtio、vhost 的职责和用户态 / 宿主机内核 / 客户机内核边界 | 中 |
| [02-userspace-api.md](./02-userspace-api.md) | `/dev/kvm`、VM fd、vCPU fd、memslot、寄存器、`mmap` `kvm_run` | 中 |
| [03-kvm-run.md](./03-kvm-run.md) | `KVM_RUN` 到 x86 `vcpu_run`：请求、信号、`complete_userspace_io` | **厚** |
| [04-vm-entry-exit.md](./04-vm-entry-exit.md) | Intel VMX 进入与退出；哪些退出留在内核，哪些写 `kvm_run`；SVM 只点差异 | **厚** |
| [05-vcpu-schedule.md](./05-vcpu-schedule.md) | vCPU 线程、HLT、halt poll、`kvm_vcpu_block`、抢占与 steal | **厚** |
| [06-guest-memory.md](./06-guest-memory.md) | GVA / GPA / HVA / HPA 接到取指和 PIO；EPT 与 `mmu_notifier` 交给内存章 | 中 |
| [07-irq-clock.md](./07-irq-clock.md) | irqchip、irqfd、注入与唤醒、APICv / posted interrupt 的条件、pvclock 与 steal time | **厚** |
| [08-pio-mmio-notify.md](./08-pio-mmio-notify.md) | 内核设备与用户态退出、ioeventfd、irqfd；trap、notify、完成是三件事 | **厚** |
| [09-virtio-negotiate.md](./09-virtio-negotiate.md) | virtio bus、feature、status；modern virtio-pci 主线，legacy 与 MMIO 薄写 | 中 |
| [10-virtqueue.md](./10-virtqueue.md) | split 与 packed、indirect、屏障、event idx、kick 与 callback | **厚** |
| [11-virtio-blk.md](./11-virtio-blk.md) | 客户机 blk-mq 到 virtqueue，再到后端完成；QEMU 只写到 ioctl / eventfd 边界 | **厚** |
| [12-vhost.md](./12-vhost.md) | 内核 vhost 的队列、eventfd、worker、描述符访问；vhost-user / vDPA 只写边界 | **厚** |
| [13-reset-migration.md](./13-reset-migration.md) | 设备 reset、队列拆掉、在途请求、dirty log 的内核职责 | 中 |
| [14-observability.md](./14-observability.md) | exit、调度、halt poll、通知、后端等待分层。事件名和字段按源码核对。命令是 RECIPE | **厚** |
| [demos/](./demos/) | demo 01 可编译。**KVM_RUN 成功路径待真机验证**。02–05 只有计划 | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| memslot 把 GPA 对上 HVA；取指和 PIO 数据页用到这个对照 | EPT / NPT、TDP MMU、`mmu_notifier` 的填表和拆表：[memory-management/11](../memory-management/11-kvm-memory.md) |
| irqfd 调到 `kvm_set_irq`，posted interrupt 在 `apicv_active` 时写 PIR | 客户机 LAPIC 的 IRR/ISR、IO-APIC、IRTE 的分配：[interrupts/11](../interrupts/11-kvm-irq-virt.md)、[interrupts/05](../interrupts/05-irqdomain-hierarchy-and-matrix.md) |
| `vhost_net` 怎样被 kick、怎样 `vhost_signal` | tap 与 `handle_tx` / `handle_rx` 的报文路径：[networking/14](../networking/14-virtio-net-host-vhost.md)。客户机 virtio-net 的 NAPI：[networking/13](../networking/13-virtio-net-guest.md) |
| virtio-blk 的头、状态字节和 `virtqueue_add_sgs` | `bio` / `request` 的通用生命周期仍以 [block-io/14](../block-io/14-virtio-blk-walkthrough.md) 为块层那一侧。本章把同一条调用接到 KVM 通知和后端 |
| vhost 访问客户机页时用到的 HVA | 设备 DMA 的 `dma_addr_t` 与 IOMMU 域：[dma-iommu/](../dma-iommu/README.md)。本章不写 VFIO 容器，不写 vIOMMU 页表 |
| vCPU 线程睡在 `TASK_INTERRUPTIBLE`，steal 读 `sched_info.run_delay` | 宿主机调度类和 EEVDF：[scheduler/](../scheduler/README.md) |

QEMU 的迁移状态机、qcow2、客户机固件、嵌套 VMCS 的字段逐项、SEV-ES 的加密保存区，都不在本章。引用 QEMU 时只描述它必须对上的 Linux ioctl 和 eventfd；不把某个 Linux tag 当成 QEMU 的版本。

## 源码地图

| 路径 | 角色 |
|------|------|
| [`include/uapi/linux/kvm.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/uapi/linux/kvm.h#L18) | `KVM_API_VERSION`、`struct kvm_run`、退出原因 |
| [`arch/x86/include/uapi/asm/kvm.h`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/include/uapi/asm/kvm.h#L111) | `kvm_regs`、`kvm_sregs`、`KVM_PIO_PAGE_OFFSET` |
| [`virt/kvm/kvm_main.c`](https://github.com/gregkh/linux/blob/v6.6.160/virt/kvm/kvm_main.c#L4117) | `KVM_RUN`、memslot、`kvm_vcpu_block`、mmap 大小 |
| [`arch/x86/kvm/x86.c`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/kvm/x86.c#L11227) | `kvm_arch_vcpu_ioctl_run`、`vcpu_run`、PIO、HLT |
| [`arch/x86/kvm/vmx/vmx.c`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/kvm/vmx/vmx.c#L7328) | `vmx_vcpu_run` 与 VMX 退出表 |
| [`arch/x86/kvm/svm/svm.c`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/kvm/svm/svm.c#L4260) | `svm_vcpu_run` |
| [`kernel/entry/kvm.c`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/entry/kvm.c#L6) | 信号与 `KVM_EXIT_INTR` |
| [`virt/kvm/eventfd.c`](https://github.com/gregkh/linux/blob/v6.6.160/virt/kvm/eventfd.c#L782) | ioeventfd、irqfd |
| [`drivers/virtio/virtio.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/virtio/virtio.c#L237) | 探测与 feature |
| [`drivers/virtio/virtio_ring.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/virtio/virtio_ring.c#L544) | split / packed virtqueue |
| [`drivers/block/virtio_blk.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/block/virtio_blk.c#L429) | 客户机 virtio-blk |
| [`drivers/vhost/vhost.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/vhost/vhost.c#L2662) | `vhost_get_vq_desc`（split 布局） |
| [`drivers/vhost/vhost.h`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/vhost/vhost.h#L259) | `VHOST_FEATURES` |
| [`include/trace/events/kvm.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/trace/events/kvm.h#L22) | `kvm_userspace_exit`、`kvm_vcpu_wakeup`、`kvm_halt_poll_ns` |
| [`arch/x86/kvm/trace.h`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/kvm/trace.h#L17) | `kvm_entry`、`kvm_exit`、`kvm_pio` |

## 实测 vs RECIPE

- **实测**：demo 01 在写作环境编译通过（`gcc -std=c11 -Wall -Wextra -Werror`）。同一环境跑了 [demos/01-run.sh](./demos/01-run.sh)：`KN_KVM_TEST_SIGNAL=1` 退出 0（不打开 `/dev/kvm`），缺文件的设备路径退出 77，`/dev/null` 退出 1，真实 `/dev/kvm` 因权限退出 77。`runner_virt=kvm`，`uname -r` 是 `6.12.113+`。没有 `KVM_EXIT_IO` 或 `KVM_EXIT_HLT` 行。这次采样不能当作 v6.6.160 的行号依据，也不能当作 nested 上跑通了客户机指令。
- **RECIPE / 待真机验证**：`KVM_RUN` 打出 IO 再打出 HLT 的验收在 [demos/README.md](./demos/README.md)。backlog ID 是 `KVM-MINIMAL-RUN`。[14](./14-observability.md) 的 `perf` / trace 命令同样是 RECIPE。demo 02–05 不实现。

## 未覆盖

- aarch64 的 `KVM_RUN` 与 GICv3
- 嵌套 VMCS / VMCB 的字段逐项，以及 SEV、TDX
- QEMU 的机器模型、迁移协议和磁盘格式
- 客户机 virtio-net 的 NAPI 与宿主机 tap 的报文内部
- IRTE 的格式、VFIO 设备 fd、vIOMMU
- 通用的内核 vhost-blk 后端（v6.6.160 的树里没有这份实现，见 [12](./12-vhost.md)）

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
