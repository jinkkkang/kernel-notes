# 6. 注册、流控、亲和

**结论**：`request_threaded_irq` 只负责把 `irqaction` 挂到已经存在的 `irq_desc` 上并打开中断；流控函数（`handle_*_irq`）决定 mask、调用驱动、eoi 的顺序；亲和要么由用户写 `smp_affinity`，要么由 `PCI_IRQ_AFFINITY` 在分配时做成托管掩码。

> 注册：`kernel/irq/manage.c`。流控：`kernel/irq/chip.c`。托管掩码：`kernel/irq/affinity.c`。CPU 下线：`kernel/irq/cpuhotplug.c` 与 `arch/x86/kernel/apic/vector.c`。

---

## 6.1 `request_threaded_irq` → `__setup_irq`

驱动最常见的 `request_irq` 是内联函数（`include/linux/interrupt.h:164`），它把 `thread_fn` 传成 `NULL`，转到 `request_threaded_irq`（`kernel/irq/manage.c:2145`）。

调用链：

```text
request_threaded_irq                         manage.c:2145
  irq == IRQ_NOTCONNECTED                    -> -ENOTCONN（interrupt.h:144 的 1U<<31）
  共享却没有 dev_id / 标志组合非法           -> -EINVAL（:2169）
  irq_to_desc 失败                           -> -EINVAL
  handler 和 thread_fn 都是 NULL             -> -EINVAL
  只有 thread_fn                           handler = irq_default_primary_handler（:1035）
  kzalloc(irqaction) 失败                    -> -ENOMEM（:2191）
  __setup_irq                                :1509
```

`__setup_irq`（`:1509`）在 `request_mutex` 之外还要拿 `desc->lock`。和新手有关的几步：

1. 芯片仍是 `no_irq_chip` → `-ENOSYS`（`:1518`）。说明描述符还没被域装上芯片。
2. 调用方没带 `IRQF_TRIGGER_*` 时，用 `irqd_get_trigger_type` 补上（`:1529`）。
3. 嵌套线程中断（`IRQ_NESTED_THREAD`）必须有 `thread_fn`。
4. 已有 `action` 时检查共享：双方都要 `IRQF_SHARED`，触发类型不能打架（`:1648` 一带）。
5. 需要线程时 `kthread_create` 出一个名字像 `irq/N-name` 的内核线程，入口是 `irq_thread`（`:1298`）。
6. 成功且没有 `IRQF_NO_AUTOEN` 时，`startup` / `enable` 把 `depth` 减到 0 并 unmask。

`IRQF_ONESHOT`：主处理函数返回后流控先 mask，等线程跑完 `irq_finalize_oneshot`（`:1086`）才 unmask。共享的 oneshot 用 `thread_mask` 记「哪几家还没跑完」，全部清掉才打开线。

强制线程化：`CONFIG_IRQ_FORCED_THREADING` 下，非 RT 看静态键 `force_irqthreads()`（`interrupt.h:509`），命令行在 `manage.c:30` 的 `setup_forced_irqthreads`。`IRQF_NO_THREAD` 和 `IRQF_PERCPU` 的中断不会被拉进线程。RT 上 `force_irqthreads()` 恒为真（`:511`）。

---

## 6.2 流控函数

`desc->handle_irq` 在域分配或 `irq_set_handler` 时装上。x86 PCI MSI 设备域用 `handle_edge_irq`（`msi.c:257`）。IO-APIC 电平引脚多用 `handle_fasteoi_irq` 或带 ack 的变体。

它们都在拿着 `desc->lock` 时更新状态，然后调用 `handle_irq_event`（`kernel/irq/handle.c:202`）→ `__handle_irq_event_percpu`（`:139`）顺着 `action` 链表调用 `handler`。

| 函数 | 行号 | 典型硬件 | 核心动作 |
|---|---|---|---|
| `handle_simple_irq` | `chip.c:538` | 没有 mask 概念的简单源 | 直接 `handle_irq_event` |
| `handle_level_irq` | `:628` | 传统电平 | mask → 处理 → 条件 unmask |
| `handle_fasteoi_irq` | `:687` | 只需最后 eoi 一次 | 处理完 `chip->irq_eoi` |
| `handle_edge_irq` | `:787` | 边沿、MSI | 处理期间若又 pending 就循环；ack 在 mask_ack |
| `handle_edge_eoi_irq` | `:849` | 边沿但仍要 eoi | 边沿循环 + eoi |
| `handle_fasteoi_ack_irq` | `:1212` | 先 ack 再 eoi | |
| `handle_fasteoi_mask_irq` | `:1263` | 电平且要 mask | |
| `handle_percpu_irq` | `:894` | 每 CPU，无全局锁那条慢路径 | 不走共享链表的重量锁 |
| `handle_nested_irq` | `:461` | 嵌套在线程里 | 没有硬中断上下文 |

`handle_fasteoi_irq` 开头若 `!irq_may_run` 且置了 `IRQD_RESEND_WHEN_IN_PROGRESS`，把 `IRQS_PENDING` 记下，留给下一次（`chip.c:698`）。这是亲和迁移和「新 CPU 上的下一次中断」撞车时用的。

`handle_edge_irq`（`:787`）在处理函数跑的时候不长期 mask。若处理过程中同一条线又来了，硬件或核心把 `IRQS_PENDING` 置上，循环再跑一遍，避免边沿丢中断。

主处理函数的返回值：

| 返回 | 后果 |
|---|---|
| `IRQ_NONE` | 记入未处理统计，共享时换下一家 |
| `IRQ_HANDLED` | 这条算处理过 |
| `IRQ_WAKE_THREAD` | `wake_up_process` 那条中断线程（`handle.c:176`） |

线程函数跑完若是 oneshot，由 `irq_thread`（`manage.c:1298`）收尾 unmask。

---

## 6.3 共享中断

多个 `irqaction` 用 `next` 串起来，`for_each_action_of_desc` 逐个调用。规则在 `__setup_irq`：

- 后来者必须和已有者一样带 `IRQF_SHARED`，否则拒绝（`manage.c:1648` 附近）。
- 触发类型（`IRQF_TRIGGER_MASK`）必须兼容。
- `dev_id` 不能是 `NULL`（`request_threaded_irq` 已经挡过）。
- `/proc/interrupts` 的最后一列会把各家 `name` 拼在一起。每家还能有 `/proc/irq/N/<name>/`（`kernel/irq/proc.c:320` 的 `register_handler_proc`）。

共享线的驱动必须能从 `dev_id` 判断「这次是不是我的设备」。认不出来就返回 `IRQ_NONE`，不要误清别人的硬件状态。

---

## 6.4 亲和与托管亲和

用户态写 `/proc/irq/N/smp_affinity` 最终进 `irq_set_affinity`（`manage.c:478`）→ `irq_set_affinity_locked`（`:352`）。x86 向量芯片的 `apic_set_affinity`（`vector.c:873`）在新目标上 `assign_irq_vector`。若旧 CPU 上还有未完成的处理，向量先记在 `prev_vector`，等那颗 CPU 上的清理定时器收掉。

托管亲和是另一条路，给多队列网卡用。驱动把 `PCI_IRQ_AFFINITY` 传给 `pci_alloc_irq_vectors`（标志在 `include/linux/pci.h:1086`）。PCI 层调用：

```c
/* kernel/irq/affinity.c:26 */
struct irq_affinity_desc *
irq_create_affinity_masks(unsigned int nvecs, struct irq_affinity *affd);
```

```c
/* include/linux/interrupt.h:285 */
struct irq_affinity {
	unsigned int	pre_vectors;     /* 开头几条不参与摊开，例如管理队列 */
	unsigned int	post_vectors;    /* 末尾几条同样排除 */
	unsigned int	nr_sets;
	unsigned int	set_size[IRQ_AFFINITY_MAX_SETS]; /* 最多 4 组，:269 */
	void		(*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void		*priv;
};
```

没有 `calc_sets` 时用 `default_calc_sets`（`affinity.c:12`）：一组，大小等于需要摊开的向量数。然后 `group_cpus_evenly` 把在线 CPU 分成几堆，每条中断拿到一个掩码，`is_managed` 置 1（`irq_affinity_desc`，`interrupt.h:299`）。

```c
struct irq_affinity_desc {
	struct cpumask	mask;            /* 这条中断允许的 CPU */
	unsigned int	is_managed : 1;  /* 1：内核托管，用户改亲和会受限制 */
};
```

向量数不够时 `irq_calc_affinity_vectors`（`affinity.c:110`）把希望值压到机器吃得下的范围。压完仍小于 `minvec` 时，PCI 层返回 `-ENOSPC`（`drivers/pci/msi/msi.c:841` 和 `:455`）。细节在 [07](./07-irq-alloc-failure.md)。

托管中断的掩码是分配时定的。掩码里的 CPU 都下线后，`msi_init_virq`（`kernel/irq/msi.c:1208`）把描述符标成 `IRQD_MANAGED_SHUTDOWN` 并返回 0，而不是失败。等 CPU 回来再启动。向量域侧用 `MANAGED_IRQ_SHUTDOWN_VECTOR` 占位（[05](./05-irqdomain-hierarchy-and-matrix.md)）。

`irq_update_affinity_hint`（`interrupt.h:325`）只更新 `affinity_hint`，不改硬件。`irq_set_affinity_and_hint` 才会顺手应用。irqbalance 读的是 hint 和 `smp_affinity`，见 [12](./12-observability.md)。

默认亲和掩码来自命令行 `irqaffinity=`（`kernel/irq/irqdesc.c:38`）。解析失败时至少把启动 CPU 放进掩码（`:35`）。

---

## 6.5 CPU 热插拔

下线一颗 CPU 时，指向它的中断必须迁走，否则向量还占着那张已经离线的 `cpumap`。

| 函数 | 行号 | 作用 |
|---|---|---|
| `irq_migrate_all_off_this_cpu` | `kernel/irq/cpuhotplug.c:171` | 遍历这颗 CPU 上的中断，改亲和 |
| `fixup_irqs` | `arch/x86/kernel/irq.c:342` | 架构侧收尾 |
| `lapic_can_unplug_cpu` | `vector.c:1170` | 下线前检查向量账是否够迁 |
| `irq_force_complete_move` | `vector.c:1075` | 热插拔路径上把迁了一半的向量做完 |
| `vector_schedule_cleanup` | `vector.c:1045` | 旧向量挂到 per-CPU 定时器上稍后再释放 |

`lapic_can_unplug_cpu` 在 `vector_lock` 下比较「这颗 CPU 已分配的向量数」和「矩阵里还能用的空位」（`irq_matrix_allocated` / `irq_matrix_available`）。空位不够：

```text
CPU %u has %u vectors, %u available. Cannot disable CPU
```

（`vector.c:1179`），返回 `-ENOSPC`。预约数大于空位时只警告、不失败（`:1186`）：

```text
Reserved vectors %u > available %u. IRQ request may fail
```

迁亲和失败时 `cpuhotplug.c:150`：

```text
IRQ%u: set affinity failed(%d).
```

有效亲和里只剩离线 CPU 时（`:45`）：

```text
Eff. affinity ... of IRQ %u contains only offline CPUs after offlining CPU %u
```

托管中断只在自己的掩码里挑 CPU。`maxcpus=` 让在线集合变小之后，掩码和 `cpu_online_mask` 的交集变窄，热插拔更容易走到上面两句。`isolcpus=` 本身不改这张掩码，见 [07](./07-irq-alloc-failure.md)。

---

## 6.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/interrupt.h` | `request_irq` | 164 | 无线程的包装 |
| `kernel/irq/manage.c` | `request_threaded_irq` | 2145 | 注册 |
| | `__setup_irq` | 1509 | 挂 action、建线程、启动 |
| | `irq_thread` | 1298 | 中断线程主体 |
| | `irq_set_affinity` | 478 | 改亲和 |
| `kernel/irq/chip.c` | `handle_fasteoi_irq` | 687 | 电平 / 只 eoi |
| | `handle_edge_irq` | 787 | 边沿 / MSI |
| `kernel/irq/handle.c` | `__handle_irq_event_percpu` | 139 | 调用每个 handler |
| `kernel/irq/affinity.c` | `irq_create_affinity_masks` | 26 | 托管掩码 |
| | `irq_calc_affinity_vectors` | 110 | 按 CPU 数收缩向量数 |
| `kernel/irq/cpuhotplug.c` | `irq_migrate_all_off_this_cpu` | 171 | 下线迁移 |
| `arch/x86/kernel/apic/vector.c` | `lapic_can_unplug_cpu` | 1170 | 下线前的向量检查 |

---

**导航**：[← 矩阵](./05-irqdomain-hierarchy-and-matrix.md) · [目录](./README.md) · [分配失败 →](./07-irq-alloc-failure.md)
