# 7. 中断分配失败

**结论**：`pci_alloc_irq_vectors` 返回 `-ENOSPC` 表示「在 `min_vecs` 和硬件/内核能给的数量之间对不上」；真正把向量分完的是每 CPU 的 `irq_matrix`，把 IRTE 分完的是 IOMMU 表。两者的 dmesg 不同，errno 也不总是 `-ENOSPC`。

> 路径对照 v6.6.160。本机没有在 6.6 上复现这些打印。下面命令都是 **RECIPE**，不附带输出。

---

## 7.1 调用链

```text
pci_alloc_irq_vectors                         drivers/pci/msi/api.c:234
  pci_alloc_irq_vectors_affinity              :254
    PCI_IRQ_MSIX → __pci_enable_msix_range    drivers/pci/msi/msi.c:789
    PCI_IRQ_MSI  → __pci_enable_msi_range     :411
    PCI_IRQ_LEGACY → 若 min_vecs==1 且 dev->irq，返回 1
    msix_capability_init / msi_capability_init
      pci_msi_setup_msi_irqs                  drivers/pci/msi/irqdomain.c:11
        msi_domain_alloc_irqs_all_locked      kernel/irq/msi.c:1422
          __msi_domain_alloc_irqs             :1247
            __irq_domain_alloc_irqs           kernel/irq/irqdomain.c:1598
              irq_domain_alloc_irqs_locked    :1530
                irq_domain_alloc_descs        :1144
                  __irq_alloc_descs           kernel/irq/irqdesc.c:814
                层次 alloc：
                  intel_irq_remapping_alloc   drivers/iommu/intel/irq_remapping.c:1309
                    或 AMD 分配（iommu.c 里 index < 0 的那段）
                    再父域 x86_vector_alloc_irqs   arch/x86/kernel/apic/vector.c:542
                      irq_matrix_reserve_managed / irq_matrix_alloc
                      kernel/irq/matrix.c:210 / :377
```

v6.6.160 里没有名为 `msi_domain_alloc_irqs` 的函数。PCI 层次域走 `msi_domain_alloc_irqs_all_locked`。若芯片提供了 `ops->domain_alloc_irqs`，`__msi_domain_alloc_locked`（`msi.c:1345`）会改走那一个；默认才是 `__msi_domain_alloc_irqs`。

`pci_alloc_irq_vectors_affinity` 把 `nvecs` 初值设成 `-ENOSPC`（`api.c:259`）。MSI-X 和 MSI 都没成功、又不允许 INTx 时，原样返回这个 `-ENOSPC`。

---

## 7.2 PCI 层每一个 `-ENOSPC` / `-ENOMEM`

### MSI-X：`__pci_enable_msix_range`（`msi.c:789`）

| 条件 | 返回 | 行号 |
|---|---|---|
| `maxvec < minvec` | `-ERANGE` | 795 |
| 设备已经开了 MSI | `-EINVAL`，并 `pci_info`：`can't enable MSI-X (MSI already enabled)` | 798 |
| 已经开了 MSI-X | `-EINVAL` | 803 |
| 域不支持 `MSI_FLAG_PCI_MSIX` | `-ENOTSUPP` | 807 |
| `pci_msi_supported` 失败或设备不是 D0 | `-EINVAL` | 810 |
| `pci_msix_vec_count` < 0 | 把这个负数返回（无能力时是 `-EINVAL`，`api.c:76`） | 814 |
| `entries` 有重复下标，或域要求连续而下标不连续 | `-EINVAL` | 817 |
| 表比 `nvec` 小，且没有 `PCI_IRQ_VIRTUAL` | 把 `nvec` 降成表大小 | 819 |
| 降完仍 `< minvec` | `-ENOSPC` | 828 |
| `pci_setup_msi_context` 失败 | 它的返回值 | 831 |
| 建不起设备域 | `-ENODEV` | 835 |
| 有 `affd` 且 `irq_calc_affinity_vectors` 的结果 `< minvec` | `-ENOSPC` | 841 |
| `msix_capability_init` 返回负数 | 原样返回 | 849 |
| 返回值是正数但 `< minvec`（部分成功、不够最小条数） | `-ENOSPC` | 851 |

`msix_capability_init`（`:712`）里和内存有关的一点：`msix_map_region` 失败返回 `-ENOMEM`（`:734`）。它映射的是 MSI-X 表所在的 BAR，不是向量矩阵。

### MSI：`__pci_enable_msi_range`（`:411`）

| 条件 | 返回 | 行号 |
|---|---|---|
| 不支持或不是 D0 | `-EINVAL` | 418 |
| 已经开了 MSI-X | `-EINVAL`，`can't enable MSI (MSI-X already enabled)` | 422 |
| `maxvec < minvec` | `-ERANGE` | 427 |
| 已经开了 MSI | `-EINVAL` | 430 |
| 域不支持 | `-ENOTSUPP` | 434 |
| `pci_msi_vec_count` < 0 | 原样返回 | 438 |
| 设备能发的条数 `< minvec` | `-ENOSPC` | 440 |
| 亲和计算后 `< minvec` | `-ENOSPC` | 456 |
| 下层返回正数但 `< minvec` | `-ENOSPC` | 466 |

`pci_msi_vec_count`（`:482`）读 Multiple Message Capable，结果是 1、2、4、…、32。设备只声明了 1 条而 `minvec` 是 8，在这里就 `-ENOSPC`，还没碰到向量矩阵。

### 亲和掩码分配失败不会变成 errno

`irq_create_affinity_masks`（`kernel/irq/affinity.c:26`）在 `kcalloc` 失败（`:61`）或 `group_cpus_evenly` 失败（`:77`）时返回 `NULL`。`msix_setup_interrupts`（`msi.c:674`）不检查这个 `NULL`：`msix_setup_msi_descs` 看到 `masks == NULL` 就把 `desc.affinity` 留空（`:631`），分配继续。所以「掩码没申请到」不会单独给出 `-ENOMEM`，表现是这批中断没有托管亲和。

---

## 7.3 描述符与域

`irq_domain_alloc_irqs_locked`（`irqdomain.c:1530`）：

| 步骤 | 失败 |
|---|---|
| `irq_domain_alloc_descs` → `__irq_alloc_descs` | 见下 |
| `irq_domain_alloc_irq_data` 失败 | `-ENOMEM`（`:1550`），并 `pr_debug("cannot allocate memory for IRQ%d\n")` |
| `domain->ops->alloc` 返回负数 | 原样返回（`:1555`） |
| `irq_domain_trim_hierarchy` 失败 | 它的返回值 |

`irq_find_free_area`（`irqdesc.c:139`）在 maple tree 里找不到一段空号时返回 `-ENOSPC`（`:144`）。`__irq_alloc_descs`（`:814`）**没有**写 `if (start < 0) return start`。MSI 这条路通过 `irq_domain_alloc_descs`（`irqdomain.c:1156`）传入 `irq = -1`。此时：

- `irq >= 0` 不成立，`-EEXIST` 那支（`irqdesc.c:839`）不走。
- `start + cnt` 里 `cnt` 是 `unsigned int`，负的 `start` 被转成很大的无符号数。
- `irq_expand_nr_irqs`（`:541`）看到 `nr > MAX_SPARSE_IRQS` 就返回 `-ENOMEM`（`:544`）。`CONFIG_SPARSE_IRQ` 下 `MAX_SPARSE_IRQS` 是 `INT_MAX`（`kernel/irq/internals.h:15`）。

因此「IRQ 号空间找不到洞」在动态分配路径上以 **`-ENOMEM`** 离开 `__irq_alloc_descs`，dmesg 里不一定有 `-ENOSPC` 这几个字符。指定了正的 `irq` 且找到的号对不上时，返回的是 `-EEXIST`（`:838`）。

`alloc_descs` 里 `alloc_masks` 申请亲和 cpumask 失败返回 `-ENOMEM`（`irqdesc.c:58`、`:64`、`:74`），整段失败时 `alloc_descs` 返回 `-ENOMEM`（`:538`）。

`__msi_domain_alloc_irqs`（`msi.c:1247`）对每个 `msi_desc` 调 `__irq_domain_alloc_irqs`。`virq < 0` 时不把这个 errno 直接抛出，而走 `msi_handle_pci_fail`（`:1182`）：

| 情况 | 返回 |
|---|---|
| 不是 PCI/VMD 这几类 `bus_token` | `-ENOSPC`（`:1194`） |
| PCI 且 `nvec_used > 1`（多向量 MSI，不是 MSI-X） | `1`，让上层缩小后再试 |
| PCI 且已经成功分到一些 | 返回已成功的条数（正数） |
| PCI 且一条都没有 | `-ENOSPC`（`:1202`） |

上层 `__pci_enable_msi_range` / `__pci_enable_msix_range` 看到正数会缩小 `nvec` 重试；小于 `minvec` 再变成 `-ENOSPC`。

---

## 7.4 重映射：IRTE

有 VT-d 或 AMD 重映射时，域的 `alloc` 先向父域（向量域）要 IRQ，再占 IRTE。父域失败则原样返回父域的 errno（Intel：`irq_remapping.c:1326`）。

Intel `alloc_irte`（`:106`）：

- `count` 为 0 或指针空：返回 `-1`。
- 连续项的对齐掩码超过 IOMMU 能力：`pr_err("Requested mask %x exceeds ...")` 后返回 `-1`（`:122`）。
- `bitmap_find_free_region` 在 `INTR_REMAP_TABLE_ENTRIES`（65536，`drivers/iommu/intel/iommu.h:505`）里失败：`pr_warn("IR%d: can't allocate an IRTE\n", iommu->seq_id)`（`:133`），返回负的 `index`。

`intel_irq_remapping_alloc` 在 `index < 0` 时再打 `pr_warn("Failed to allocate IRTE\n")`（`:1336`），然后 `return ret`。这里的 `ret` 在 `:1329` 被写成 `-ENOMEM`，**IRTE 耗尽时调用方看到的是 `-ENOMEM`，不是 `-ENOSPC`**。认这条失败要看 dmesg 里的 `IR%d:`，不要只看 errno。

AMD `alloc_irq_index` 把扫完表仍不够的情况写成 `index = -ENOSPC`（`drivers/iommu/amd/iommu.c:3108`）。表长 `MAX_IRQS_PER_TABLE` 是 `1 << 9`，即 512（`amd_iommu_types.h:300`）。IOAPIC 类型若 `alloc_irq_table` 失败则 `index = -ENOMEM`（`iommu.c:3447`）。`index < 0` 时 `pr_warn("Failed to allocate IRTE\n")`（`:3460`），`ret = index`，所以 AMD 这条可以把 `-ENOSPC` 传上去。

同一句 `Failed to allocate IRTE` 两处都有。Intel 那句前面通常还有 `IR%d: can't allocate an IRTE`。

---

## 7.5 向量矩阵

`x86_vector_alloc_irqs`（`vector.c:542`）：

| 条件 | 返回 | 行号 |
|---|---|---|
| `apic_is_disabled` | `-ENXIO` | 551 |
| 遗留级联 IRQ | `-EINVAL` | 559 |
| `alloc_apic_chip_data` 失败 | `-ENOMEM` | 568 |
| `assign_irq_vector_policy` 失败 | 矩阵的返回值 | 599 |

`assign_irq_vector_policy`（`:310`）三条：

| 中断种类 | 调用 | 矩阵函数 | `-ENOSPC` 行 |
|---|---|---|---|
| 托管 | `reserve_managed_vector` | `irq_matrix_reserve_managed` | `matrix.c:236`（掩码里某 CPU 的窗口没有空位可预留） |
| 带了 `info->mask` | `assign_irq_vector` | `irq_matrix_alloc` | `:392` 没有在线 CPU，或 `:397` 位图没有空位 |
| 其余 | `reserve_irq_vector` | `irq_matrix_reserve` | **不返回错误**。只增加 `global_reserved`。超额时 `pr_warn("Interrupt reservation exceeds available resources\n")`（`:349`） |

预约成功只说明「记账上还没超」，激活时才占具体向量。`irq_matrix_alloc_managed` 的 `-ENOSPC` 在 `:297`（掩码里没有在线 CPU）和 `:305`（预留位已经被占完）。激活失败的打印：

```text
Managed startup for irq %u, but no CPU
Managed startup irq %u, no vector available
irq %u: Affinity broken due to vector space exhaustion.
```

分别在 `vector.c:439`、`:449`、`:424`。第三句是普通（可预约）中断被放宽到亲和掩码之外时的警告，分配本身可能仍然成功。

空 cpumask 时矩阵返回的是 `-EINVAL`（`matrix.c:388` 和 `:293`），不是 `-ENOSPC`。

---

## 7.6 `nr_irqs` 上限

```c
/* arch/x86/kernel/apic/vector.c:711 */
int __init arch_probe_nr_irqs(void)
{
	int nr;

	if (nr_irqs > (NR_VECTORS * nr_cpu_ids))
		nr_irqs = NR_VECTORS * nr_cpu_ids;

	nr = (gsi_top + nr_legacy_irqs()) + 8 * nr_cpu_ids;
#ifdef CONFIG_PCI_MSI
	if (gsi_top <= NR_IRQS_LEGACY)
		nr += 8 * nr_cpu_ids;
	else
		nr += gsi_top * 16;
#endif
	if (nr < nr_irqs)
		nr_irqs = nr;

	return legacy_pic->probe();
}
```

`NR_VECTORS` 是 256（`irq_vectors.h:102`）。`nr_cpu_ids` 是启动后的 CPU 个数上限，受 `maxcpus` 影响（`kernel/smp.c:955` 的 `early_param("maxcpus", maxcpus)`）。第一刀是 `256 * nr_cpu_ids`。第二刀用 GSI 顶和每 CPU 8 条（MSI 时再加一截）把 `nr_irqs` 再降到不超过这个估算。

`early_irq_init` 打印（`irqdesc.c:558`）：

```text
NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d
```

`NR_IRQS` 是编译期宏（[03](./03-apic-and-vectors.md)），`nr_irqs` 是运行时这个被砍过的值。描述符不够时走上一节的扩展；扩展越过 `MAX_SPARSE_IRQS` 得到 `-ENOMEM`。这个上限和「某颗 CPU 的向量窗口」不是同一个数：IRQ 号可以远多于 256，因为不同 CPU 可以复用同一个向量号。

---

## 7.7 每 CPU 还能分多少：用宏算，不要背一个数

矩阵搜索窗（`vector.c:810`）：

```text
alloc_start = FIRST_EXTERNAL_VECTOR          /* 0x20 */
alloc_end   = FIRST_SYSTEM_VECTOR            /* CONFIG_X86_LOCAL_APIC 下 = LOCAL_TIMER_VECTOR = 0xec */
alloc_size  = alloc_end - alloc_start
            = NR_EXTERNAL_VECTORS            /* irq_vectors.h:110 */
            = 0xec - 0x20
            = 0xcc                           /* 十进制 204 */
```

CPU 第一次 `irq_matrix_online` 时（`matrix.c:84`）：

```text
cpumap.available = alloc_size - cpumap.managed - systembits_inalloc
```

`systembits_inalloc` 只统计 **落在 `[alloc_start, alloc_end)` 里** 的系统位（`matrix.c:195`）。`apic_idts[]` 里的定时器、IPI、伪中断都 ≥ `0xec`，不在这个窗里，不占这 204。会占窗的是：

| 来源 | 什么时候扣 |
|---|---|
| `IA32_SYSCALL_VECTOR`（`0x80`） | 仅当它被装进 `def_idts[]` 并 `set_bit(system_vectors)`。`idt.c:119` 要求 `CONFIG_IA32_EMULATION`（64 位）或 `CONFIG_X86_32` |
| `PIC_CASCADE_IR`（IRQ 2）的 `ISA_IRQ_VECTOR(2)` = `0x32` | `nr_legacy_irqs() > 1` 时 `lapic_assign_system_vectors`（`vector.c:775`） |
| 其余 ISA 向量 `0x30`–`0x3f`（除了 `0x32`） | 在**当时的当前 CPU** 上 `irq_matrix_assign`（`vector.c:782`），减的是这颗 CPU 的 `available`，不是 `systembits_inalloc`。有 IO-APIC 时 `lapic_update_legacy_vectors`（`:748`）直接返回，不会再把它们改成系统位 |

之后每分出一个普通向量，`available` 再减 1。托管预留在对应 CPU 的 `managed` 上加 1，在线 CPU 同时减 `available`（`irq_matrix_reserve_managed`，`:221`）。

所以「还能用的向量」= debugfs 里该 CPU 的 `avl` 列，不是 204，也不是 `204 - 某个固定系统向量数`。204 只是窗宽。

向量为什么会先被用完：

| 原因 | 代码里的机制 |
|---|---|
| 窗是每 CPU 的，而且只有 `[0x20, 0xec)` | `irq_alloc_matrix` 的后两个参数 |
| 托管中断在掩码里的每颗 CPU 上预留 1 位，即使还没激活 | `irq_matrix_reserve_managed` |
| 在线 CPU 少 | `maxcpus=`（`kernel/smp.c:955`）让 `nr_cpu_ids` / 在线集合变小。矩阵选 CPU 时还要和 `cpu_online_mask` 相交（`assign_vector_locked`，`vector.c:275`） |
| 掩码被收得很窄 | `irq_create_affinity_masks` 或用户写的亲和。`isolcpus=`（`kernel/sched/isolation.c:246`）改的是调度 housekeeping；`group_cpus_evenly` 拷贝的是 `cpu_present_mask`（`lib/group_cpus.c:386`），`kernel/irq/affinity.c` 不读 housekeeping。所以 `isolcpus` 不会自动把 CPU 从托管中断掩码里删掉，这些 CPU 只要还在线就仍占向量 |
| 大量 SR-IOV VF、每 VF 许多队列 | 每个队列一条 MSI-X，每条在某颗 CPU 上占 1 个向量。设备表本身还有上限，见下 |
| 设备 MSI-X 表不够 | `pci_msix_vec_count` = `msix_table_size`（`msi.h:6`），小于 `minvec` 时 `-ENOSPC`（`msi.c:828`），与矩阵无关 |
| IRTE 表不够 | Intel 65536 / AMD 每表 512，见 7.4 |
| CPU 下线迁不走 | `lapic_can_unplug_cpu`（`vector.c:1179`）打印 `Cannot disable CPU` 并返回 `-ENOSPC` |

---

## 7.8 症状 → 原因 → 查什么

| 症状 | 更可能的原因 | 先看 |
|---|---|---|
| 驱动打印分配失败，errno `-ENOSPC`，dmesg 没有 `IRTE` | `min_vecs` 大于设备 MSI/MSI-X 能力，或亲和算完不够，或矩阵 `-ENOSPC` 被 `msi_handle_pci_fail` 收成 `-ENOSPC` | `pci_msix_vec_count` 对应的配置字；VECTOR 域 debugfs 的 `avl` |
| dmesg 有 `IR%d: can't allocate an IRTE` | Intel IRTE 位图满。errno 仍可能是 `-ENOMEM`（7.4） | 这句里的 IOMMU 序号；是不是开了大量 VF |
| 只有 `Failed to allocate IRTE`，没有 `IR%d:` | AMD 路径（或 Intel 在打印第二句）。AMD 的 errno 可以是 `-ENOSPC` | `iommu.c:3460` 与 `index` |
| `Managed startup irq %u, no vector available` | 托管预留和激活时的位图不一致，或预留位被占完 | 该 irq 的 `is_managed` / `Vector`（debugfs `irqs/N`） |
| `Affinity broken due to vector space exhaustion` | 亲和掩码里分不出向量，核心换了一颗掩码外的 CPU。中断可能已经起来 | 该 irq 的 `effective_affinity` 是否还在预期核上 |
| `Interrupt reservation exceeds available resources` | 全局预约数追上 `global_available`。预约本身仍成功 | 矩阵的 `Global reserved` 与 `Global available` |
| `CPU %u has %u vectors, %u available. Cannot disable CPU` | 下线时剩余空位装不下要迁走的向量 | 不要先下线这颗 CPU；先减队列或把中断迁走 |
| `No irq handler for vector` | 向量已经送到 CPU，但 `vector_irq[]` 是 `VECTOR_UNUSED` | 这是到来路径，不是分配路径（`irq.c:264`） |
| 启动早的 `NR_IRQS:` 那一行里 `nr_irqs` 很小 | `arch_probe_nr_irqs` 按 CPU 数和 GSI 砍过 | 和单 CPU 向量窗对照，别混成一个数 |

---

## 7.9 诊断（RECIPE）

这些文件的名字在 v6.6.160 源码里。下面命令没有在本仓库的机器上对着 6.6 跑过。

```bash
# RECIPE
dmesg | grep -E 'IRTE|vector space exhaustion|Cannot disable CPU|NR_IRQS:'
```

源码里真实存在、和本路径有关的格式串：

| 格式串 | 位置 |
|---|---|
| `IR%d: can't allocate an IRTE` | `drivers/iommu/intel/irq_remapping.c:133` |
| `Failed to allocate IRTE` | 同文件 `:1336`；`drivers/iommu/amd/iommu.c:3460` |
| `irq %u: Affinity broken due to vector space exhaustion.` | `arch/x86/kernel/apic/vector.c:424` |
| `Managed startup for irq %u, but no CPU` | `:439` |
| `Managed startup irq %u, no vector available` | `:449` |
| `Interrupt reservation exceeds available resources` | `kernel/irq/matrix.c:349` |
| `CPU %u has %u vectors, %u available. Cannot disable CPU` | `vector.c:1179` |
| `Reserved vectors %u > available %u. IRQ request may fail` | `:1186` |
| `NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d` | `kernel/irq/irqdesc.c:558` |
| `can't enable MSI-X (MSI already enabled)` | `drivers/pci/msi/msi.c:798` |
| `can't enable MSI (MSI-X already enabled)` | `:422` |
| `%s: %d.%u No irq handler for vector` | `arch/x86/kernel/irq.c:264` |

```bash
# RECIPE：哪条中断打在哪颗 CPU 上。计数来自 irq_desc.kstat_irqs
cat /proc/interrupts
```

`/proc/interrupts` 由 `fs/proc/interrupts.c:39` 的 `proc_create_seq("interrupts", ...)` 创建，内容来自 `show_interrupts`（`kernel/irq/proc.c:461`）。

```bash
# RECIPE：需要 CONFIG_GENERIC_IRQ_DEBUGFS。根目录在 kernel/irq/debugfs.c:250
ls /sys/kernel/debug/irq/domains/
# 向量域的名字是创建时传入的 "VECTOR"（vector.c:797）
# 域级 debug_show 在 irqd==NULL 时调用 irq_matrix_debug_show（vector.c:624）
cat /sys/kernel/debug/irq/domains/VECTOR
cat /sys/kernel/debug/irq/irqs/<irq>
```

`irq_matrix_debug_show`（`matrix.c:490`）的格式是：

```text
Online bitmaps:   %6u
Global available: %6u
Global reserved:  %6u
Total allocated:  %6u
System: %u: %*pbl
| CPU | avl | man | mac | act | vectors
```

列：`avl` = `cpumap.available`，`man` = `managed`，`mac` = `managed_allocated`，`act` = `allocated`，最后是 `alloc_map` 的位图。单条中断的 `x86_vector_debug_show`（`vector.c:644` 起）打印 `Vector`、`Target`、`is_managed`、`can_reserve`、`has_reserved`。

tracepoint 在 `include/trace/events/irq_matrix.h`：`irq_matrix_alloc`（`:181`）、`irq_matrix_alloc_managed`（`:165`）、`irq_matrix_reserve_managed`（`:149`）、`irq_matrix_reserve`（`:120`）、`irq_matrix_online`（`:106`）。向量域还有 `irq_vectors:vector_alloc`、`vector_alloc_managed`、`vector_setup`（`arch/x86/include/asm/trace/irq_vectors.h:219`、`:245`、`:325`）。

```bash
# RECIPE：trace-cmd 或 perf 二选一。事件名以上面头文件为准
perf record -e irq_matrix:irq_matrix_alloc,irq_matrix:irq_matrix_alloc_managed \
  -e irq_vectors:vector_alloc,irq_vectors:vector_setup -a -- sleep 5
```

怎么读：`irq_matrix_alloc` 的 `bit` 是分到的向量号，`cpu` 是目标 CPU。返回前内核已经在 `vector < 0` 时不会打成功这条；失败要结合函数返回值（`-ENOSPC`）和上面的 `pr_warn`，trace 的 `vector_alloc` 里 `vector` 字段可以是负数（`trace_vector_alloc` 在 `vector.c:260`，分配结果直接传入）。

```bash
# RECIPE：MSI-X Count 与 pci_msix_vec_count 是同一个配置字
lspci -vv -s <BDF> | grep -A2 MSI-X
```

---

## 7.10 缓解

驱动已经按这个顺序降级（`pci_alloc_irq_vectors_affinity`，`api.c:269`）：先 MSI-X，再 MSI，再 INTx。INTx 只在 `min_vecs == 1` 且 `dev->irq` 非 0 时返回 1（`:284`）。标志：

| 宏 | 值 | 位置 |
|---|---|---|
| `PCI_IRQ_LEGACY` | `1 << 0` | `include/linux/pci.h:1083` |
| `PCI_IRQ_MSI` | `1 << 1` | `:1084` |
| `PCI_IRQ_MSIX` | `1 << 2` | `:1085` |
| `PCI_IRQ_AFFINITY` | `1 << 3` | `:1086` |
| `PCI_IRQ_VIRTUAL` | `1 << 4` | `:1652` |

`PCI_IRQ_VIRTUAL` 让 MSI-X 在表比请求短时仍按请求条数走（`msi.c:821`），多出来的是虚拟项，不增加硬件表项。它不制造 APIC 向量。

把 `min_vecs` 设得比 `max_vecs` 小，PCI 层可以在部分成功时返回一个中间值，驱动少开队列。这是 API 合同（`api.c:230` 的注释：成功时返回值可以小于 `max_vecs`）。

队列个数是驱动自己的策略。用户态常用：

```bash
# RECIPE：减少网卡队列，从而少要 MSI-X。ethtool 不在内核树里
ethtool -L <ifname> combined <N>
```

内核命令行（都在 v6.6.160 里，不是模块参数）：

| 参数 | 位置 | 和向量的关系 |
|---|---|---|
| `maxcpus=` | `kernel/smp.c:955` | 在线 CPU 变少，每条托管中断的掩码和 `matrix_find_best_cpu` 的选择面变小 |
| `isolcpus=` | `kernel/sched/isolation.c:246` | 没写标志时加上 `HK_FLAG_DOMAIN`（`:242`），收的是调度域。托管中断掩码仍按 `cpu_present_mask` 摊（见 7.7），不会因此少占向量 |
| `irqaffinity=` | `kernel/irq/irqdesc.c:38` | 默认亲和 |
| `pci=nomsi` | `drivers/pci/pci.c:7029` | 关掉 MSI，设备退回 INTx。整机中断变成共享引脚 |
| `noapic` | `arch/x86/kernel/apic/io_apic.c:201` | 关掉 IO-APIC。不是 MSI 向量耗尽时该用的开关 |

本路径上没有另找「某个 `.ko` 的 `module_param`」来放大向量窗。向量窗宽由 `FIRST_EXTERNAL_VECTOR` 和 `FIRST_SYSTEM_VECTOR` 定死，不能靠模块参数改成大于 `NR_EXTERNAL_VECTORS`。

---

**导航**：[← 注册](./06-request-and-flow.md) · [目录](./README.md) · [softirq →](./08-softirq-tasklet.md)
