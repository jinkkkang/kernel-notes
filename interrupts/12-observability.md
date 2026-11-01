# 12. 观测

**结论**：先看 `/proc/interrupts` 和 `/proc/softirqs` 知道计数落在哪颗 CPU，再看 `/proc/irq/N/smp_affinity` 和 `effective_affinity` 知道希望的目标和芯片实际接受的目标，最后用 debugfs 的 VECTOR 矩阵和 tracepoint 看分配与单次处理。

> 本机内核是 6.12，不是笔记对照的 6.6。下面命令都是 **RECIPE**，没有附上采样输出。文件名和字段对照 v6.6.160 源码。

---

## 12.1 `/proc/interrupts`

`fs/proc/interrupts.c:39` 创建 `interrupts`，`show` 指向 `show_interrupts`（`kernel/irq/proc.c:461`）。每一行是一个 Linux IRQ：前面每 CPU 一列，数字来自 `irq_desc.kstat_irqs`（`include/linux/irqdesc.h:58`，读取 `irq_desc_kstat_cpu`，`:119`）。后面是芯片名、`hwirq`、流控名（`desc->name`）、以及各 `irqaction` 的 `name`。

x86 还会在表尾加架构行，`arch_show_interrupts`（`arch/x86/kernel/irq.c:61`）：本地定时器、伪中断、函数调用 IPI、posted interrupt 等。这些是系统向量，没有普通的 `irq_desc` 行。某一行要 `system_vectors` 里对应位被置上才打印，例如 Hypervisor 回调（`irq.c:141`）。

读的时候：

| 看到 | 说明 |
|---|---|
| 某 IRQ 只在一列上涨 | 亲和是单 CPU，或托管掩码里只有一颗在线 CPU |
| 计数涨但业务不涨 | 处理函数在跑，活可能堆在 softirq |
| 行尾有多个名字 | `IRQF_SHARED`，几家串在同一个 `irqaction` 链表上 |
| 架构区的 `RES` 涨得快 | `RESCHEDULE_VECTOR`，去看调度章，不是设备中断风暴 |

`/proc/irq/N/spurious`（`proc.c:387`）给出未处理计数，对应 `irq_desc.irqs_unhandled`。

---

## 12.2 `/proc/softirqs`

`fs/proc/softirqs.c:33` 创建。`show_softirqs`（`:11`）对 `i` 从 0 到 `NR_SOFTIRQS-1`、对每颗 CPU 打印 `kstat_softirqs_cpu(i, j)`。行名是 `softirq_to_name[]`：`HI`、`TIMER`、`NET_TX`、`NET_RX`、`BLOCK`、`IRQ_POLL`、`TASKLET`、`SCHED`、`HRTIMER`、`RCU`（`kernel/softirq.c:63`）。

`NET_RX` 一列很高，同时 `ksoftirqd` 占 CPU，才像软中断饱和。只有计数高、CPU 仍空闲，只说明包多，不说明来不及。

---

## 12.3 `/proc/irq/N/`

目录在第一次挂上处理函数时由 `register_irq_proc`（`kernel/irq/proc.c:339`）创建。`CONFIG_SMP` 下的文件：

| 文件 | 权限 | 行号 | 含义 |
|---|---|---|---|
| `smp_affinity` | 0644 | 367 | 位掩码，写它等于 `irq_set_affinity` |
| `smp_affinity_list` | 0644 | 375 | 同样的掩码，CPU 列表写法 |
| `affinity_hint` | 0444 | 371 | 驱动的提示，只读 |
| `node` | 0444 | 378 | `irq_common_data.node` |
| `effective_affinity` | 0444 | 381 | 要 `CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK` |
| `effective_affinity_list` | 0444 | 383 | 同上，列表写法 |
| `spurious` | 0444 | 387 | 未处理统计 |

x86 向量域置了 `IRQD_SINGLE_TARGET`，有效亲和通常是一颗 CPU。`smp_affinity` 可以是一个集合，`effective_affinity` 是集合里真正写进 APIC 的那颗。两者不一致时，要么迁移还没完成（`prev_vector` 还在），要么发生过 [07](./07-irq-alloc-failure.md) 里的 “Affinity broken”。

默认亲和文件是 `/proc/irq/default_smp_affinity`，由 `register_default_affinity_proc`（`proc.c:423`）创建。命令行 `irqaffinity=` 改的是这块掩码的初值（`irqdesc.c:38`）。

---

## 12.4 debugfs

要 `CONFIG_GENERIC_IRQ_DEBUGFS`。`irq_debugfs_init`（`kernel/irq/debugfs.c:245`）做两级目录：

```text
/sys/kernel/debug/irq/          debugfs_create_dir("irq")     :250
/sys/kernel/debug/irq/irqs/     debugfs_create_dir("irqs")     :254
/sys/kernel/debug/irq/domains/  irqdomain.c:2031
/sys/kernel/debug/irq/domains/default   :2033，模式 0444
```

每个域一个文件，名字是 `irq_domain.name`，模式 0444（`irqdomain.c:2018`）。向量域的名字是 `"VECTOR"`（`arch/x86/kernel/apic/vector.c:797`）。

```bash
# RECIPE
cat /sys/kernel/debug/irq/domains/VECTOR
cat /sys/kernel/debug/irq/irqs/42
```

域文件在没有具体 `irq_data` 时走 `x86_vector_debug_show` → `irq_matrix_debug_show`（`vector.c:624`，`kernel/irq/matrix.c:490`）。列的含义见 [07](./07-irq-alloc-failure.md)：`avl` / `man` / `mac` / `act`。单条 IRQ 文件打印 `Vector`、`Target`、`is_managed`、`can_reserve`、`has_reserved`（`vector.c:644`）。

---

## 12.5 tracepoint

| 事件 | 头文件 | 行号 | 看什么 |
|---|---|---|---|
| `irq:irq_handler_entry` | `include/trace/events/irq.h` | 53 | `irq`、`name`，处理函数开始 |
| `irq:irq_handler_exit` | 同文件 | 83 | `ret`：handled 还是 none |
| `irq:softirq_entry` / `softirq_exit` | 同文件 | 128 / 142 | `vec`：softirq 号 |
| `irq:softirq_raise` | 同文件 | 156 | 谁把位置上 |
| `irq:tasklet_entry` / `tasklet_exit` | 同文件 | 189 / 203 | tasklet 回调 |
| `irq_vectors:local_timer_entry` 等 | `arch/x86/include/asm/trace/irq_vectors.h` | 41 起的 `DEFINE_IRQ_VECTOR_EVENT` | 系统向量进出。名字是 `local_timer`、`reschedule`、`call_function`、`call_function_single`、`irq_work`、`spurious_apic`、`error_apic`，外加配置打开时的 `thermal_apic` 等 |
| `irq_vectors:vector_alloc` | 同文件 | 219 | 分配结果，负数表示失败 |
| `irq_matrix:irq_matrix_alloc` | `include/trace/events/irq_matrix.h` | 181 | `bit` 是向量号，`cpu` 是目标 |

`irq_work_exit` 不能被 perf 采样事件订阅（`irq_vectors.h:75`），否则采样本身又产生 `irq_work`。

```bash
# RECIPE
perf record -e irq:irq_handler_entry,irq:irq_handler_exit -a -- sleep 2
perf script | head
```

`entry` 和 `exit` 配对看持续时间。同一 `irq` 的 `exit` 若经常是未处理，回到共享中断那一节：有一家 `handler` 老是返回 `IRQ_NONE`。

---

## 12.6 irqbalance

irqbalance 不是内核线程。内核里没有它的实现；`Documentation/networking/scaling.rst:85` 把它描述成动态改 IRQ 亲和的用户态守护进程。它写的就是 `/proc/irq/N/smp_affinity`。

内核侧和它有关的几点：

| 机制 | 效果 |
|---|---|
| `IRQF_NOBALANCING` / `IRQ_NO_BALANCING` | 描述符不允许均衡（`include/linux/irq.h:95`） |
| `IRQD_AFFINITY_MANAGED` | 托管中断，用户态改亲和会被核心拒绝或改回掩码 |
| `affinity_hint` | 只是提示。`irq_update_affinity_hint` 不改硬件（`interrupt.h:321`） |
| VT-d posted interrupt | `posted_intr.c:306` 的注释：这类中断要保持单 CPU，irqbalance 把它摊开会和 PI 的通知目标冲突 |

看 irqbalance 是否在起作用：比较写之前和写之后的 `effective_affinity`，而不是只看进程在不在。托管中断上它往往写不进去。

---

**导航**：[← KVM](./11-kvm-irq-virt.md) · [目录](./README.md) · [性能配方 →](./13-perf-analysis.md)
