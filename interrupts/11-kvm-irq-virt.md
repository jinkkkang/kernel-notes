# 11. KVM 里的中断

**结论**：客户机看到的 LAPIC / IO-APIC / PIC 是 KVM 的软件模型；宿主机把 MSI 或 irqfd 的 eventfd 翻译成 `kvm_set_irq`，再写进客户机 LAPIC 的 IRR。APICv / posted interrupt 让这条路径不必每次都 VM-Exit。

> 路由：`virt/kvm/irqchip.c` 的 `kvm_set_irq`（`:70`）。x86 的 MSI 解码在 `arch/x86/kvm/irq_comm.c`。LAPIC 模型在 `arch/x86/kvm/lapic.c`。posted interrupt 在 `arch/x86/kvm/vmx/posted_intr.c`。

---

## 11.1 客户机 LAPIC

每个 vCPU 一份。寄存器页的布局和客户机看到的 APIC 寄存器一一对应，因为 VMX 微码会直接碰这一页（注释在结构体里）。

```c
/* arch/x86/kvm/lapic.h:46 */
struct kvm_timer {
	struct hrtimer timer;          /* 没有硬件虚拟定时器时用宿主机 hrtimer */
	s64 period;                    /* 纳秒 */
	ktime_t target_expiration;
	u32 timer_mode;
	u32 timer_mode_mask;
	u64 tscdeadline;               /* TSC-deadline 模式的目标 */
	u64 expired_tscdeadline;
	u32 timer_advance_ns;
	atomic_t pending;              /* 已经触发、还没注入的次数 */
	bool hv_timer_in_use;          /* 正在用 VMX 预emption timer 一类的硬件定时器 */
};

/* arch/x86/kvm/lapic.h:59 */
struct kvm_lapic {
	unsigned long base_address;
	struct kvm_io_device dev;      /* 客户机 MMIO 访问落到这里 */
	struct kvm_timer lapic_timer;
	u32 divide_count;
	struct kvm_vcpu *vcpu;
	bool apicv_active;
	bool sw_enabled;
	bool irr_pending;
	bool lvt0_in_nmi_mode;
	/* ISR 里置位的个数 */
	s16 isr_count;
	/* ISR 里最高的向量；-1 表示缓存无效，必须再扫 */
	int highest_isr_cache;
	/**
	 * APIC 寄存器页。布局与客户机看到的一致，因为 VMX 微码会访问它。
	 * 微码只用其中的 TPR。
	 */
	void *regs;
	gpa_t vapic_addr;
	struct gfn_to_hva_cache vapic_cache;
	unsigned long pending_events;
	unsigned int sipi_vector;
	int nr_lvt_entries;
};
```

注入一条边沿中断：`kvm_apic_set_irq`（`lapic.c:832`）把向量写进 IRR。客户机下一次能接收中断时，`kvm_get_apic_interrupt`（声明在 `lapic.h:93`）从 IRR 搬到 ISR，把向量号交给 VM-Entry。

定时器到期：`apic_timer_fn`（`lapic.c:2869`）是 `hrtimer` 的回调，里面 `kvm_apic_local_deliver(apic, APIC_LVTT)`（交付点在 `:1895` 一带）。改用硬件定时器时走 `kvm_lapic_expired_hv_timer`（`:2201`）。模式和宿主机的 `lapic-deadline` 是两层：客户机写自己的 TSC-deadline 寄存器，KVM 再决定用 hrtimer 还是 VMX 的预emption timer 去对准那个时刻。

`kvm_apic_accept_pic_intr`（`:2946`）决定 ExtINT（老 PIC）能不能进这颗 vCPU。LVT0 配成 ExtINT 时，PIC 的输出才从这里进来。

---

## 11.2 客户机 IO-APIC 与 PIC

```c
/* arch/x86/kvm/ioapic.h:74
 * IOAPIC_NUM_PINS 即 KVM_IOAPIC_NUM_PINS，uapi 里是 24
 * （arch/x86/include/uapi/asm/kvm.h:77）。
 */
struct kvm_ioapic {
	u64 base_address;
	u32 ioregsel;
	u32 id;
	u32 irr;
	u32 pad;
	union kvm_ioapic_redirect_entry redirtbl[IOAPIC_NUM_PINS];
	unsigned long irq_states[IOAPIC_NUM_PINS];
	struct kvm_io_device dev;
	struct kvm *kvm;
	spinlock_t lock;
	struct rtc_status rtc_status;
	struct delayed_work eoi_inject;
	u32 irq_eoi[IOAPIC_NUM_PINS];
	u32 irr_delivered;
};
```

`kvm_ioapic_set_irq`（`arch/x86/kvm/ioapic.c:485`）改引脚电平，查 `redirtbl[pin]`，再变成一条 LAPIC 中断（向量、目标、投递模式都在重定向项里）。

PIC 在 `arch/x86/kvm/i8259.c`。`kvm_pic_set_irq`（`:188`）更新 IRR/IMR。路由表把「芯片 + 引脚」对上函数：`irq_comm.c:35` 调 PIC，`:43` 调 IO-APIC。

---

## 11.3 路由与 `kvm_set_msi`

`kvm_set_irq`（`virt/kvm/irqchip.c:70`）按 GSI 查路由表，对每一项调用它的 `set`。路由由 `kvm_set_irq_routing`（`:168`）安装。一项可以是 IRQCHIP（PIC/IO-APIC 引脚）、MSI，或 Hyper-V 的 SINT。

x86 的 MSI 项：

```c
/* arch/x86/kvm/irq_comm.c:131 */
int kvm_set_msi(...)
{
	if (kvm_msi_route_invalid(kvm, e))   /* x2APIC 格式下 address_hi 低 8 位必须是 0 */
		return -EINVAL;
	if (!level)
		return -1;
	kvm_set_msi_irq(kvm, e, &irq);       /* :104，拆 address/data */
	return kvm_irq_delivery_to_apic(kvm, NULL, &irq, NULL);
}
```

`kvm_set_msi_irq` 用 `x86_msi_msg_get_destid` 取目标 APIC ID（`:114`），向量在 `msg.arch_data.vector`。这和宿主机写设备 MSI 消息是同一套地址格式，只是写进的是客户机 LAPIC，不是宿主机 LAPIC。

`kvm_irq_delivery_to_apic` 按物理/逻辑目标找到 vCPU，再 `kvm_apic_set_irq`。

---

## 11.4 irqfd

irqfd 把一个 eventfd 接到上面的 `kvm_set_irq`。用户态（或 vhost）对 eventfd `write` 一次，就等于对某个 GSI 打一个沿。

`kvm_irqfd`（`virt/kvm/eventfd.c:593`）分到 `kvm_irqfd_assign`（`:318`）或 `kvm_irqfd_deassign`（`:563`）。assign 时 `INIT_WORK(&irqfd->inject, irqfd_inject)`（`:340`），并把等待队列挂到 eventfd 上。

唤醒函数 `irqfd_wakeup`（`:202`）在 `EPOLLIN` 时先试 `kvm_arch_set_irq_inatomic`。返回 `-EWOULDBLOCK`（原子上下文做不完，例如要走复杂路由）就 `schedule_work(&irqfd->inject)`。

```c
/* virt/kvm/eventfd.c:43 */
static void irqfd_inject(struct work_struct *work)
{
	/* 没有 resampler：拉高再拉低，合成一个沿 */
	kvm_set_irq(..., irqfd->gsi, 1, false);
	kvm_set_irq(..., irqfd->gsi, 0, false);
	/* 有 resampler：只拉高，等客户机 EOI 再采样通知 */
}
```

vhost 不直接调用 `kvm_set_irq`。`vhost_signal`（`drivers/vhost/vhost.c:2912`）在客户机需要通知时 `eventfd_signal(vq->call_ctx.ctx)`（`:2916`）。QEMU 把这个 eventfd 注册成 irqfd 之后，信号就进了上一节的路径。注释写的是 “This actually signals the guest, using eventfd.”（`:2911`）。

---

## 11.5 APICv 与 posted interrupt

APICv 打开时（`kvm_lapic.apicv_active`），虚拟 APIC 寄存器页由处理器在 VM-Entry/VM-Exit 时同步，客户机写 EOI、TPR 可以不退出。

posted interrupt 更进一步：宿主机把向量写进 vCPU 的 posted-interrupt 描述符（PIR），再向该 vCPU 正在跑的物理 CPU 发一条 IPI。IPI 的向量是 `POSTED_INTR_VECTOR`（`0xf2`）或唤醒用的 `POSTED_INTR_WAKEUP_VECTOR`（`0xf1`），见 [03](./03-apic-and-vectors.md)。处理器硬件把 PIR 并进虚拟 APIC 的 IRR，不必 VM-Exit 进 `kvm_apic_set_irq`。

| 函数 | 行号 | 作用 |
|---|---|---|
| `vmx_deliver_posted_interrupt` | `arch/x86/kvm/vmx/vmx.c:4259` | 把向量投进 PIR，必要时发 posted-interrupt IPI |
| `vmx_vcpu_pi_load` / `vmx_vcpu_pi_put` | `posted_intr.c:53` / `:196` | vCPU 被调度上/离开 CPU 时改 NDST（通知目标） |
| `pi_wakeup_handler` | `:218` | `POSTED_INTR_WAKEUP_VECTOR` 上把阻塞的 vCPU 叫醒 |
| `pi_has_pending_interrupt` | `:240` | PIR 或虚拟 APIC 里是否还有未处理向量 |
| `vmx_pi_update_irte` | `:272` | 把宿主机 IRQ 的 IRTE 改成 posted 模式，直接投到 vCPU |

`vmx_can_use_vtd_pi`（`:135`）判断能不能用 VT-d posted interrupt。可以的话，设备 MSI 在 IOMMU 的 IRTE 里写成「posted」，中断从设备到 vCPU 不再经过宿主机的 `common_interrupt` 处理函数。`posted_intr.c:306` 附近的注释提到 irqbalance 若把这类中断摊到多 CPU，会破坏「单 CPU 通知目标」这个前提。

嵌套虚拟化还有 `POSTED_INTR_NESTED_VECTOR`（`0xf0`），给 L2 用。本章不展开 L2 的描述符格式。

---

## 11.6 和宿主机中断章的边界

| 宿主机（本章 01–07） | 客户机（本节） |
|---|---|
| `irq_desc` / 向量矩阵 | `kvm_lapic.regs` 里的 IRR/ISR |
| `pci_alloc_irq_vectors` | 客户机驱动在客户机内核里再申请一次 |
| VT-d IRTE 分配失败 | `vmx_pi_update_irte` 改的是已经存在的宿主机 IRQ |

客户机里的「向量用完了」不会打印宿主机的 `can't allocate an IRTE`。那句只属于宿主机 IOMMU。客户机自己的分配失败出现在客户机的 dmesg 里。

---

**导航**：[← 定时器](./10-timers.md) · [目录](./README.md) · [观测 →](./12-observability.md)
