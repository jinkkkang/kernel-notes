# 9. DMA-IOMMU 桥

**结论**：`iommu_dma_ops` 是 DMA 域上的 `dma_map_ops`。`iommu_setup_dma_ops` 只在当前 domain 带 `__IOMMU_DOMAIN_DMA_API` 时把它装到设备上；identity 域保持 `dma_ops == NULL`，streaming 映射继续走 direct。DMA 与 DMA-FQ 的差别在 unmap 时 IOTLB 是马上作废还是进 256 项的 per-CPU 队列。

> 页表写入在 [08](./08-page-tables.md)。group 的默认类型在 [07](./07-iommu-core.md)。

---

## 9.1 ops 表与安装条件

```c
/* drivers/iommu/dma-iommu.c:1616 */
static const struct dma_map_ops iommu_dma_ops = {
	.flags			= DMA_F_PCI_P2PDMA_SUPPORTED,
	.alloc			= iommu_dma_alloc,
	.free			= iommu_dma_free,
	.map_page		= iommu_dma_map_page,
	.unmap_page		= iommu_dma_unmap_page,
	.map_sg			= iommu_dma_map_sg,
	.unmap_sg		= iommu_dma_unmap_sg,
	.sync_single_for_cpu	= iommu_dma_sync_single_for_cpu,
	.sync_single_for_device	= iommu_dma_sync_single_for_device,
	.map_resource		= iommu_dma_map_resource,
	.get_merge_boundary	= iommu_dma_get_merge_boundary,
	.max_mapping_size	= iommu_dma_max_mapping_size,
};
```

`iommu_setup_dma_ops`（`:1645`）：

```text
domain = iommu_get_domain_for_dev(dev)
domain 为空 → pr_warn "Failed to set up IOMMU for device %s; retaining platform DMA ops"
iommu_is_dma_domain(domain) 为假 → 直接 return，不改 dma_ops
为真 → iommu_dma_init_domain；失败同样 pr_warn 并保留平台 ops
成功 → dev->dma_ops = &iommu_dma_ops
```

VT-d、AMD 和 virtio-iommu 的 `probe_finalize` 都是先 `set_dma_ops(dev, NULL)` 再调用这个函数（Intel 在 `drivers/iommu/intel/iommu.c:4473`）。identity 设备因此明确留在 direct，即使 IOMMU 驱动已经 probe 过它。`si_domain` 让硬件按物理地址放行，DMA API 不再分配 IOVA。

`iommu_dma_init_domain`（`dma-iommu.c:570`）用最小页长做 IOVA 粒度，`init_iova_domain` 之后 `iova_domain_init_rcaches`。类型是 DMA-FQ 但硬件没有 `IOMMU_CAP_DEFERRED_FLUSH`，或 `iommu_dma_init_fq` 失败时，把类型改回 `IOMMU_DOMAIN_DMA`（`:619`）。注释说 FQ 失败可以退回 strict。然后 `iova_reserve_iommu_regions` 把 MSI、固件保留区占掉，避免 DMA API 把 IOVA 分到那些窗口上。

---

## 9.2 一次 `map_page` 的桥

```text
iommu_dma_map_page                   dma-iommu.c:1030
  prot = dma_info_to_prot(dir, dev_is_dma_coherent, attrs)
  dev_use_swiotlb 且 (phys|size) 没按 IOVA 粒度对齐
    池不活跃 → DMA_MAPPING_ERROR
    swiotlb_tbl_map_single，必要时把填充字节清零
  非 coherent：arch_sync_dma_for_device
  __iommu_dma_map                      :745
    iommu_dma_alloc_iova               :659  alloc_iova_fast
    iommu_map(domain, iova, phys - 页内偏移, 对齐后的 size, prot, GFP_ATOMIC)
    失败则 free IOVA，返回 DMA_MAPPING_ERROR
  若 iova 失败且 phys 是槽：把槽还回去
```

`GFP_ATOMIC` 是因为 map 可能发生在驱动的提交路径上。`iommu_map` 因此不能睡眠等内存（`might_sleep_if` 只在 GFP 允许阻塞时检查，`iommu.c:2525`）。IOVA 分配失败是 `DMA_MAPPING_ERROR`，驱动应按 [02](./02-dma-address.md) 的办法处理，不能重试到把原子分配耗尽还不看返回值。

unmap（`iommu_dma_unmap_page`，`:1087`）先 `iommu_iova_to_phys`。物理地址为 0 就 `WARN_ON` 返回。然后按 coherent 与属性决定要不要 `arch_sync_dma_for_cpu`，`__iommu_dma_unmap` 拆页并还 IOVA，最后如果物理地址落在 SWIOTLB 里就把槽还掉。

`map_resource`（`:1411`）走同一条 `__iommu_dma_map`，权限额外带 `IOMMU_MMIO`，并且 `dma_info_to_prot` 的 coherent 参数传假。MMIO 不经过 SWIOTLB。

---

## 9.3 identity、DMA、DMA-FQ、unmanaged

| `default_domain->type` | `iommu_setup_dma_ops` | 设备 DMA 地址从哪来 |
|---|---|---|
| `IOMMU_DOMAIN_IDENTITY` | 不安装 `iommu_dma_ops` | `dma_direct_map_page` 的 `phys_to_dma`，或 direct 路径上的 SWIOTLB |
| `IOMMU_DOMAIN_DMA` | 安装 | IOVA；unmap 时 `iotlb_sync` |
| `IOMMU_DOMAIN_DMA_FQ` | 安装 | 同样的 IOVA；unmap 把 IOVA 放进 flush queue |
| `IOMMU_DOMAIN_UNMANAGED` | 不安装。这个类型不是 DMA API 的 default，是 `iommu_domain_alloc` 交给 VFIO / iommufd 的 | 调用者用 `iommu_map` 指定 IOVA，见 [10](./10-vfio-boundary.md) |
| `IOMMU_DOMAIN_BLOCKED` | 不安装 | attach 之后硬件应丢弃 DMA。它不是一条映射成功的路径 |

sysfs `type` 打印的是上表第一列的默认域（[07](./07-iommu-core.md)）。当前装上的 `group->domain` 可以是 unmanaged，同时 `type` 文件仍是 `DMA`。所以「IOMMU 开着」和「这台设备的 `dma_map_page` 会进 `iommu_dma_map_page`」要分别看：前者看有没有 IOMMU 驱动和 group，后者看这台设备此刻的 `dma_ops` 是不是 `&iommu_dma_ops`，而 sysfs 没有这个指针。

`iommu_dma_max_mapping_size`（`:1608`）对不可信设备返回 SWIOTLB 的上限，其余返回 `SIZE_MAX`。可信设备的单次 `map_page` 不受 2KiB 槽限制，受 IOVA 空间和页表分配限制。`get_merge_boundary`（`:1596`）返回最小页长减 1，告诉块层怎样合并 bio 才不把一个 DMA 段切在页中间。

---

## 9.4 DMA-FQ 的队列

```c
/* drivers/iommu/dma-iommu.c:94 */
struct iova_fq_entry {
	unsigned long iova_pfn;
	unsigned long pages;
	struct list_head freelist;
	u64 counter;
};

struct iova_fq {
	struct iova_fq_entry entries[IOVA_FQ_SIZE];
	unsigned int head, tail;
	spinlock_t lock;
};
```

`IOVA_FQ_SIZE` 是 256（`:88`），`IOVA_FQ_TIMEOUT` 是 10 毫秒（`:91`）。`__iommu_dma_unmap`（`:722`）把 `gather.queued` 设成 `cookie->fq_domain` 的当前值。非空时 `iommu_dma_free_iova` 调用 `queue_iova` 而不是立刻 `free_iova_fast`（`:713`）。`iommu_unmap_fast` 已经把 PTE 清掉，但 IOTLB 可能还留着旧翻译，所以 IOVA 不能马上再发给另一台设备。

`queue_iova`（`:175`）先 `smp_mb`，和 IOMMU 驱动的页表更新配对。队列满就 `fq_flush_iotlb`：递增开始计数、`ops->flush_iotlb_all`、递增完成计数（`:149`）。定时器 `fq_flush_timeout`（`:156`）做同样的全量作废，再在每个 CPU 上 `fq_ring_free`。`fq_ring_free` 只释放 `counter` 已经小于完成计数的项（`:137`），避免在全量作废完成前把 IOVA 还回去。

strict 域的 `fq_domain` 为空，`gather.queued` 为假，`__iommu_dma_unmap` 在返回前调用 `iommu_iotlb_sync`（`:740`）。`iommu.strict=1` 或 sysfs 把类型留在 `DMA` 时走这条。`iommu_group_store_type` 允许在驱动仍绑定的情况下从 DMA 升到 DMA-FQ（`iommu.c:3092`），其他类型切换要求先解开驱动。那个 store 需要 `CAP_SYS_ADMIN` 和 `CAP_SYS_RAWIO`（`:3073`）。本章的 demo 不写这个文件。

---

## 9.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `iommu_dma_ops` | [`dma-iommu.c:1616`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1616) | DMA 域的 `dma_map_ops` |
| `iommu_setup_dma_ops` | [`dma-iommu.c:1645`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1645) | 只有 DMA 域才安装 ops |
| `iommu_dma_init_domain` | [`dma-iommu.c:570`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L570) | IOVA 域、FQ 回退、保留区 |
| `iommu_dma_map_page` | [`dma-iommu.c:1030`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1030) | bounce 或直接 `__iommu_dma_map` |
| `__iommu_dma_map` | [`dma-iommu.c:745`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L745) | 分配 IOVA 并 `iommu_map` |
| `__iommu_dma_unmap` | [`dma-iommu.c:722`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L722) | fast unmap，FQ 则入队 |
| `fq_flush_iotlb` | [`dma-iommu.c:149`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L149) | `flush_iotlb_all` |
| `intel_iommu_probe_finalize` | [`intel/iommu.c:4473`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.c#L4473) | 先清空 ops 再安装 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 页表](./08-page-tables.md) · [目录](./README.md) · [VFIO 边界 →](./10-vfio-boundary.md)
