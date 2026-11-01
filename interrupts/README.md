# 中断（x86-64）

> 内核版本：**v6.6.160**（stable 6.6 LTS）。下文路径、函数、结构体字段与行号对照 `/tmp/linux` 的该版本核对（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)

**结论**：设备中断从 APIC 向量进 IDT，落到每 CPU 的 `vector_irq[]` 里的 `irq_desc`，再由 genirq 的流控函数调用 `irqaction`；能推迟的工作进 softirq、中断线程、工作队列或 `irq_work`。x86-64 上真正稀缺的资源是**每 CPU 的外部向量窗口**，不是 Linux IRQ 号本身。

面向 **C 语言新手**：`irq` 是内核里的中断号（一个整数），`vector` 是 CPU 收到的 8 位中断向量（0–255）。二者不是同一个数。`struct` 里的指针成员是「指向另一块对象」，嵌进去的成员是「对象自己的一部分」。

## 阅读顺序

`01 → 02 → 03 → 04 → 05 → 06 → 07`

然后按需要：`08 → 09`（推迟执行）、`10`（定时器）、`11`（KVM）、`12 → 13`（观测与排障配方）。

分配失败先读 [07](./07-irq-alloc-failure.md)，不必从 01 重读整章。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 总图：设备 → APIC → 向量 → `irq_desc` → 处理函数 → softirq/线程 | 中 |
| [02-x86-entry.md](./02-x86-entry.md) | IDT、`idtentry`、`entry_64.S`、`common_interrupt`、中断栈、`irqentry_enter` | **厚** |
| [03-apic-and-vectors.md](./03-apic-and-vectors.md) | LAPIC/x2APIC、IO-APIC、MSI/MSI-X、256 向量布局 | **厚** |
| [04-genirq-objects.md](./04-genirq-objects.md) | `irq_desc` / `irq_data` / `irq_chip` / `irq_domain` / `irqaction` | **厚** |
| [05-irqdomain-hierarchy-and-matrix.md](./05-irqdomain-hierarchy-and-matrix.md) | 向量域、VT-d/AMD 重映射、PCI-MSI 域、`irq_matrix` | **厚** |
| [06-request-and-flow.md](./06-request-and-flow.md) | `request_threaded_irq`、流控、共享、亲和、热插拔 | **厚** |
| [07-irq-alloc-failure.md](./07-irq-alloc-failure.md) | `-ENOSPC`/`-ENOMEM` 全路径、向量耗尽、诊断与缓解 | **厚** |
| [08-softirq-tasklet.md](./08-softirq-tasklet.md) | softirq、`ksoftirqd`、tasklet、`local_bh_disable` | 中 |
| [09-deferred-work-choice.md](./09-deferred-work-choice.md) | 工作队列、中断线程、`irq_work` 怎么选 | 中 |
| [10-timers.md](./10-timers.md) | 时间轮、hrtimer、NO_HZ、clockevent、LAPIC 定时器 | **厚** |
| [11-kvm-irq-virt.md](./11-kvm-irq-virt.md) | 客户机 LAPIC、irqfd、posted interrupt | **厚** |
| [12-observability.md](./12-observability.md) | `/proc`、debugfs、tracepoint、irqbalance | **厚** |
| [13-perf-analysis.md](./13-perf-analysis.md) | 风暴、softirq 饱和、亲和、perf/ftrace 配方 | **厚** |

## 边界（本章不覆盖）

| 留给别章 | 原因 |
|----------|------|
| `spin_lock_irqsave` 的锁字实现 | [locks-and-ds/05-spinlock.md](../locks-and-ds/05-spinlock.md) |
| `__schedule` / EEVDF 选谁跑 | [scheduler/](../scheduler/)；本章只写到 `TIF_NEED_RESCHED` 和 `SCHED_SOFTIRQ` |
| 缺页、`vmalloc` 栈 | [memory-management/](../memory-management/)；中断栈大小只给宏 |
| 信号从哪投递到用户态 | [process-management/09-signals.md](../process-management/09-signals.md) |
| NAPI 协议栈内部 | 只点到 `NET_RX_SOFTIRQ` 怎么被拉起 |
| 完整 IOMMU DMA 重映射 | 只写中断重映射表（IRTE） |

## 源码地图（先认门牌）

| 路径 | 角色 |
|------|------|
| `arch/x86/entry/entry_64.S` | IDT 入口宏展开处；含 `irq_entries_start` |
| `arch/x86/include/asm/idtentry.h` | `DEFINE_IDTENTRY_IRQ`、向量桩 |
| `arch/x86/include/asm/irq_vectors.h` | 256 向量常量 |
| `arch/x86/kernel/irq.c` | `common_interrupt` |
| `arch/x86/kernel/apic/vector.c` | x86 向量域与分配 |
| `arch/x86/kernel/apic/apic.c` | LAPIC、本地定时器 |
| `arch/x86/kernel/apic/io_apic.c` | IO-APIC |
| `kernel/irq/` | genirq：描述符、域、矩阵、`/proc`、debugfs |
| `drivers/pci/msi/` | `pci_alloc_irq_vectors` |
| `drivers/iommu/intel/irq_remapping.c` | VT-d 中断重映射 |
| `drivers/iommu/amd/iommu.c` | AMD 中断重映射 |
| `kernel/softirq.c` | softirq 与 `irq_enter_rcu` |
| `kernel/time/` | 时间轮、hrtimer、tick |
| `arch/x86/kvm/lapic.c` | KVM 内核 LAPIC |
| `virt/kvm/eventfd.c` | irqfd |
| `include/trace/events/irq.h` | `irq_handler_*`、`softirq_*` |
| `include/trace/events/irq_matrix.h` | 矩阵分配 tracepoint |
| `arch/x86/include/asm/trace/irq_vectors.h` | `irq_vectors:*` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
