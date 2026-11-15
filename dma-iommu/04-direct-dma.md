# 4. direct DMA

**结论**：`dma_direct_map_page` 先算 `phys_to_dma`；强制 bounce、算出来的地址装不进 mask 与 `bus_dma_limit`、或 kmalloc 缓冲在非 coherent 设备上不够对齐时，才改走 `swiotlb_map`。装得下就返回这个 DMA 地址，它等于物理地址只是偏移和加密位都为零的特例。

> 池本身在 [05](./05-swiotlb.md)。分发条件在 [03](./03-dma-api-dispatch.md)。

---

## 4.1 物理地址怎样变成 DMA 地址

```c
/* include/linux/dma-direct.h:20 */
struct bus_dma_region {
	phys_addr_t	cpu_start;
	dma_addr_t	dma_start;
	u64		size;
	u64		offset;
};
```

`translate_phys_to_dma`（`:27`）沿 `dev->dma_range_map` 找包含 `paddr` 的区间，返回 `(dma_addr_t)paddr - m->offset`。没有任何区间盖住它时返回 `DMA_MAPPING_ERROR`，好让后面的 `dma_capable` 失败。反向的 `translate_dma_to_phys`（`:40`）把 DMA 地址加回 `offset`。

通用 `phys_to_dma`（`:72`）在翻译之后调用 `__sme_set`。注释（`:66`）说明加密位会进设备地址，`phys_to_dma_unencrypted` 留给本来就没加密的内存，SWIOTLB 槽就是这种。`dma_to_phys`（`:77`）用 `__sme_clr` 把加密位清掉再当物理地址用。

`dma_direct_set_offset`（`kernel/dma/direct.c:622`）可以给没有固件 `dma-ranges` 的简单设备装一张只有一段的表。注释写明驱动自己调用它是 bug，映射应由设备树或其他固件描述。偏移为 0 时函数直接返回 0，不分配表。

`dma_direct_map_resource`（`:509`）不走这张表：它把传入的 `phys_addr_t` 原样当作 `dma_addr_t`，只做 `dma_capable(..., false)`。这是 MMIO 资源，不是 RAM。

---

## 4.2 `dma_direct_map_page` 的三个出口

```c
/* kernel/dma/direct.h:84 */
static inline dma_addr_t dma_direct_map_page(struct device *dev,
		struct page *page, unsigned long offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t dma_addr = phys_to_dma(dev, phys);

	if (is_swiotlb_force_bounce(dev)) {
		if (is_pci_p2pdma_page(page))
			return DMA_MAPPING_ERROR;
		return swiotlb_map(dev, phys, size, dir, attrs);
	}

	if (unlikely(!dma_capable(dev, dma_addr, size, true)) ||
	    dma_kmalloc_needs_bounce(dev, size, dir)) {
		if (is_pci_p2pdma_page(page))
			return DMA_MAPPING_ERROR;
		if (is_swiotlb_active(dev))
			return swiotlb_map(dev, phys, size, dir, attrs);
		/* warn: mask / bus limit，然后 DMA_MAPPING_ERROR */
	}

	if (!dev_is_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		arch_sync_dma_for_device(phys, size, dir);
	return dma_addr;
}
```

`dma_capable`（`include/linux/dma-direct.h:99`）在地址已经是 `DMA_MAPPING_ERROR` 时失败；RAM 映射在 32 位 `dma_addr_t` 上还会拒绝低于 `min_low_pfn` 的结果；最后比较 `addr + size - 1` 和 `min_not_zero(*dma_mask, bus_dma_limit)`。

`dma_kmalloc_needs_bounce`（`include/linux/dma-map-ops.h:329`）在 `CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC` 关闭时，因为 `dma_kmalloc_safe` 直接返回真（`:285`），所以不会 bounce。打开之后，coherent 设备或 `DMA_TO_DEVICE` 仍视为安全（`:293`）；其余方向还要看长度是否达到 `ARCH_DMA_MINALIGN` 的两倍，或 `kmalloc_size_roundup` 之后是否对齐 cache line（`:309`）。注释承认这个判断有假阳性，假阴性在已知构造里没有。

P2P 页在需要 bounce 时直接失败。direct 不会把对端设备的 MMIO 拷进 SWIOTLB。

池没激活（`is_swiotlb_active` 为假）且地址装不下时，函数打印 mask 与 `bus_dma_limit` 并返回 `DMA_MAPPING_ERROR`（`:104`）。这时没有静默截断。

---

## 4.3 分配：连续页，以及放不进 mask 时的退路

`dma_direct_alloc`（`kernel/dma/direct.c:206`）把 `size` 调成页对齐。非 coherent、且没有 SWIOTLB-for-alloc 时，可以走架构的 `arch_dma_alloc`、全局 coherent 池，或者把直接映射改成 uncached / 再 vmap 一块（`:221`）。coherent 设备则 `__dma_direct_alloc_pages`。

`__dma_direct_alloc_pages`（`:118`）先按 coherent mask 和 `bus_dma_limit` 选 `GFP_DMA` 或 `GFP_DMA32`（`dma_direct_optimal_gfp_mask`，`:47`）。`dma_coherent_ok`（`:69`）用 `phys_to_dma_direct` 检查整段是否落在 `min(coherent_dma_mask, bus_dma_limit)` 里。CMA 或 `alloc_pages_node` 给出的页不合格就放开，再带着 `GFP_DMA32`、然后 `GFP_DMA` 重试（`:146`）。重试用尽就返回 `NULL`。调用者看到的是分配失败，不是一个越界的 `dma_handle`。

`is_swiotlb_for_alloc(dev)` 为真时，分配直接从 SWIOTLB 池拿页（`:127`），并且仍要过 `dma_coherent_ok`。这是「池被当成分配器」的设备，和 streaming bounce 不是同一个开关。`dma_range_map` 的偏移让物理上连续的页在 DMA 地址上连续：`dma_handle` 来自第一页的 `phys_to_dma_direct`（`:301`），长度是调用者请求的连续大小。

`dma_direct_max_mapping_size`（`:591`）在 SWIOTLB 活跃、并且 `dma_addressing_limited` 或强制 bounce 时，返回 `swiotlb_max_mapping_size`。否则返回 `SIZE_MAX`。direct 本身不把多次 `map_page` 合成一段更大的 DMA 地址。

---

## 4.4 和 IOMMU 关闭的关系

x86 `pci_swiotlb_detect`（`arch/x86/kernel/pci-dma.c:44`）在 `no_iommu` 为 0 且 `max_possible_pfn > MAX_DMA32_PFN` 时把 `x86_swiotlb_enable` 设真。主机或客户机内存加密也会打开它；客户机加密还会带上 `SWIOTLB_FORCE`（`:62`）。`pci_iommu_alloc` 随后 `swiotlb_init(x86_swiotlb_enable, ...)`（`:108`）。`swiotlb_init_remap` 在 `addressing_limit` 和 `swiotlb_force_bounce` 都为假时直接返回（`kernel/dma/swiotlb.c:364`），不建池。

`iommu=off` 把 `no_iommu` 设为 1（`pci-dma.c:123`），于是「内存高于 32 位」这条不会再打开池。加密路径不受这个条件保护。池后来被 `swiotlb_exit` 丢掉时（`pci_iommu_init`，`:181`），direct 映射没有槽可用，装不下的地址以 `DMA_MAPPING_ERROR` 失败。

所以 direct 设备的一次成功 `dma_map_page` 只说明 `phys_to_dma` 的结果落在 mask 里，或者这次用了槽。它不说明 IOMMU 硬件不存在。

---

## 4.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `phys_to_dma` | [`dma-direct.h:72`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-direct.h#L72) | 区间偏移加 SME 位 |
| `dma_capable` | [`dma-direct.h:99`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-direct.h#L99) | mask 与总线上限 |
| `dma_direct_map_page` | [`direct.h:84`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.h#L84) | 直接映射或转入 SWIOTLB |
| `dma_direct_alloc` | [`direct.c:206`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.c#L206) | coherent 分配 |
| `dma_coherent_ok` | [`direct.c:69`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.c#L69) | 整段落在 coherent mask 内 |
| `dma_direct_map_sg` | [`direct.c:465`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.c#L465) | 逐段 map，返回原 nents |
| `pci_swiotlb_detect` | [`pci-dma.c:44`](https://github.com/gregkh/linux/blob/v6.6.160/arch/x86/kernel/pci-dma.c#L44) | x86 何时准备池 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← API 分发](./03-dma-api-dispatch.md) · [目录](./README.md) · [SWIOTLB →](./05-swiotlb.md)
