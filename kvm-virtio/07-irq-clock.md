# 7. 虚拟中断与时钟

**结论**：客户机能看见的中断，是 KVM 在下一次 VM-entry 之前写进 LAPIC 的 IRR，或者在 APICv 打开时由硬件从 posted-interrupt 描述符里取走。irqfd 是宿主机 eventfd 到 `kvm_set_irq` 的一条捷径。客户机的 kvmclock 和 steal time 是 KVM 写进客户机内存的结构，由 `KVM_REQ_CLOCK_UPDATE` 和 `KVM_REQ_STEAL_UPDATE` 在进入客户机之前更新。它们不是宿主机的 `clocksource`。

> LAPIC 寄存器页、IO-APIC 和 posted interrupt 的投递细节已经在 [interrupts/11](../interrupts/11-kvm-irq-virt.md)。IRTE 的格式和分配失败在 [interrupts/05](../interrupts/05-irqdomain-hierarchy-and-matrix.md)。本节只写「什么条件下走加速路径，以及醒来的是哪条线程」。

---

## 7.1 irqchip 决定谁拥有 LAPIC

`KVM_CREATE_IRQCHIP`（`include/uapi/linux/kvm.h:1489`）把 `irqchip_mode` 设成 `KVM_IRQCHIP_KERNEL`。`KVM_CAP_SPLIT_IRQCHIP` 设成 `KVM_IRQCHIP_SPLIT`。两种模式下 `irqchip_in_kernel` 都为真（`arch/x86/kvm/irq.h:90`），`kvm_arch_vcpu_create` 会 `kvm_create_lapic`（`x86.c:12001`）。模式仍是 `KVM_IRQCHIP_NONE` 时没有 `vcpu->arch.apic`，本章的注入路径不存在，HLT 也不会在内核里等中断。

有 LAPIC 时，边沿注入的公共入口是 `kvm_set_irq`（`virt/kvm/irqchip.c:70`）。它按 GSI 查路由，再进架构的投递函数。x86 上最终可以到达 `kvm_apic_set_irq`，把向量放进 IRR。客户机能收中断时，VM-entry 的事件注入把向量交给客户机。这一段的 IRR/ISR 在中断章 11.1，这里不重复结构体。

用户态也可以不用 irqfd，直接 `KVM_IRQ_LINE`。那条路径同样进 `kvm_set_irq`，但是在 vCPU 线程之外的 ioctl 里。vCPU 若正堵在 `kvm_vcpu_block`，投递一侧要 `kvm_vcpu_kick` 才能把它唤醒。

---

## 7.2 irqfd：eventfd 变成一次注入

`KVM_IRQFD`（`kvm.h:1515`）的参数：

```c
/* include/uapi/linux/kvm.h:1308 */
struct kvm_irqfd {
	__u32 fd;          /* 已经打开的 eventfd */
	__u32 gsi;
	__u32 flags;       /* DEASSIGN、RESAMPLE */
	__u32 resamplefd;
	__u8  pad[16];
};
```

内核把这个 eventfd 挂进等待队列，回调是 `irqfd_wakeup`（`virt/kvm/eventfd.c:202`）。`EPOLLIN` 时先 `eventfd_ctx_do_read`，再在 `irq_srcu` 下调用 `kvm_arch_set_irq_inatomic`（`:230`）。返回 `-EWOULDBLOCK`（路由不能在原子上下文做完，或者 irqfd 正在拆除）时，`schedule_work(&irqfd->inject)`。工作函数 `irqfd_inject`（`:42`）对没有 resampler 的 fd 连续做两次 `kvm_set_irq`：level 1 然后 level 0（`:50`）。这是边沿。带 `KVM_IRQFD_FLAG_RESAMPLE` 时只拉高，等客户机 EOI 再从 `resamplefd` 通知用户态（`irqfd_resampler_notify`，`:59`）。

`kvm_set_irq` 若发现目标 vCPU 在 `kvm_vcpu_block` 里，唤醒路径是 `kvm_vcpu_kick` → `kvm_vcpu_wake_up`（`kvm_main.c:3647`）。vCPU 若在客户机内部，kick 把 `mode` 改成 `EXITING_GUEST_MODE`，`vcpu_enter_guest` 在下一次进入前的检查里会退出来处理 `KVM_REQ_EVENT`。所以「后端写了 call eventfd」之后，客户机驱动的中断回调要等：irqfd 注入成功，并且该 vCPU 线程重新进入客户机、客户机开中断。写 eventfd 本身不调用客户机的 `virtblk_done`。

---

## 7.3 APICv 与 posted interrupt 的条件

`enable_apicv` 在 `arch/x86/kvm/x86.c:235` 默认是真，VMX 模块参数在 `vmx/vmx.c:110`。`kvm_apicv_init`（`x86.c:9848`）在 `:9854` 置上 `APICV_INHIBIT_REASON_ABSENT`。`enable_apicv` 为假时在 `:9856` 再置 `APICV_INHIBIT_REASON_DISABLE`。`kvm_apicv_activated`（`:9822`）要求 `apicv_inhibit_reasons` 全 0。`kvm_vcpu_apicv_activated`（`:9828`）把 VM 的原因和 `kvm_x86_vcpu_get_apicv_inhibit_reasons` 的结果或在一起，仍要全 0。

VMX 要求处理的抑制位（`vmx.c:8266`）包括 `DISABLE`、`ABSENT`、`HYPERV`、`BLOCKIRQ`、`PHYSICAL_ID_ALIASED`、`APIC_ID_MODIFIED`、`APIC_BASE_MODIFIED`。创建 vCPU 时如果 `enable_apicv` 为真，代码先把 `apic->apicv_active` 设成真并挂上 `KVM_REQ_APICV_UPDATE`（`x86.c:12016`），真正的值在第一次进入前按抑制位改正。`vcpu_enter_guest` 里有一条 `WARN_ON_ONCE`，要求 vCPU 和 VM 的 APICv 状态一致（`x86.c:10880`）。

posted interrupt 的投递在 `vmx_deliver_posted_interrupt`（`vmx.c:4259`）。函数注释写明调用方已经在内核 LAPIC 上。`apic->apicv_active` 为假时返回 `-1`。为真时 `pi_test_and_set_pir` 把向量放进 PIR；若 `pi_test_and_set_on` 发现 ON 已经是 1，说明通知已经发出，直接返回 0。否则 `kvm_vcpu_trigger_posted_interrupt`。`vmx_deliver_interrupt`（`:4289`）在这个函数返回非 0 时退回软件路径：`kvm_lapic_set_irr`、`KVM_REQ_EVENT`、`kvm_vcpu_kick`。

因此 APICv 打开并不保证每次中断都没有 VM-exit。抑制位任一置位、`apicv_active` 为假、或 posted 通知时 vCPU 不在客户机里，都会回到软件 IRR 加 kick。分配设备中断时 PIR 还要和 IRTE 对上；那张表的布局不在这里，中断章写 IRTE。trace `kvm_pi_irte_update`（`arch/x86/kvm/trace.h:1079`）只说明 KVM 更新了这层对应关系。

SVM 的对应物是 AVIC，不是这套 PIR/ON。`kvm_avic_*` 事件从 `trace.h:1426` 起。本章不把 AVIC 的 doorbell 展开。

---

## 7.4 pvclock 与 steal time

客户机写 `MSR_KVM_SYSTEM_TIME_NEW`（`kvm_para.h:53`，`0x4b564d01`）启用每个 vCPU 一份 `struct pvclock_vcpu_time_info`。布局在 `arch/x86/include/asm/pvclock-abi.h:26`：

```c
struct pvclock_vcpu_time_info {
	u32   version;             /* 奇数：正在写；偶数：稳定 */
	u32   pad0;
	u64   tsc_timestamp;
	u64   system_time;
	u32   tsc_to_system_mul;
	s8    tsc_shift;
	u8    flags;               /* PVCLOCK_TSC_STABLE_BIT、PVCLOCK_GUEST_STOPPED */
	u8    pad[2];
} __attribute__((__packed__));
```

`vcpu_enter_guest` 处理 `KVM_REQ_CLOCK_UPDATE` 时调用 `kvm_guest_time_update`（`x86.c:10629`，函数在 `:3169`）。它先填 `vcpu->hv_clock`，再由 `kvm_setup_guest_pvclock`（`:3120`）写进客户机那一页：先把 `version` 变成奇数（`:3147` 的 `| 1`），写完字段后加一变成偶数（`:3161`）。客户机用 TSC 和这组系数自己算时间，避免每次读时钟都 VM-exit。`PVCLOCK_GUEST_STOPPED` 用来告诉客户机时间曾经停过（例如宿主机挂起），不是 steal 的累加器。

steal 的结构和更新点在 [05](./05-vcpu-schedule.md)：`MSR_KVM_STEAL_TIME`，`record_steal_time` 加的是宿主机线程的 `sched_info.run_delay` 增量。时钟请求和 steal 请求都在 `IN_GUEST_MODE` 之前处理。它们写的是客户机 GPA 后面的 HVA，写完可以再进入客户机，不一定让 `KVM_RUN` 返回。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 客户机内存](./06-guest-memory.md) · [目录](./README.md) · [PIO / MMIO →](./08-pio-mmio-notify.md)
