# 3. DMA API 分发

**结论**：每次 map / alloc 都从 `get_dma_ops(dev)` 取出 `dev->dma_ops`；指针为空就走 direct。指针非空时，只有 `CONFIG_DMA_OPS_BYPASS` 且 `dev->dma_ops_bypass` 被置上、并且 mask 盖住 `dma_direct_get_required_mask`，`dma_go_direct` 才返回真。streaming 用 `*dev->dma_mask` 做这个比较，coherent 分配用 `dev->coherent_dma_mask`。

> direct 的换算在 [04](./04-direct-dma.md)。哪类 IOMMU domain 会把 ops 设成 `iommu_dma_ops` 在 [09](./09-dma-iommu.md)。

---

## 3.1 `struct dma_map_ops`

```c
/* include/linux/dma-map-ops.h:23 */
struct dma_map_ops {
	unsigned int flags;

	void *(*alloc)(struct device *dev, size_t size,
			dma_addr_t *dma_handle, gfp_t gfp,
			unsigned long attrs);
	void (*free)(struct device *dev, size_t size, void *vaddr,
			dma_addr_t dma_handle, unsigned long attrs);
	/* ... alloc_pages / mmap / get_sgtable ... */
	dma_addr_t (*map_page)(struct device *dev, struct page *page,
			unsigned long offset, size_t size,
			enum dma_data_direction dir, unsigned long attrs);
	void (*unmap_page)(struct device *dev, dma_addr_t dma_handle,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs);
	int (*map_sg)(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs);
	void (*unmap_sg)(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs);
	void (*sync_single_for_cpu)(struct device *dev, dma_addr_t dma_handle,
			size_t size, enum dma_data_direction dir);
	void (*sync_single_for_device)(struct device *dev,
			dma_addr_t dma_handle, size_t size,
			enum dma_data_direction dir);
	int (*dma_supported)(struct device *dev, u64 mask);
	u64 (*get_required_mask)(struct device *dev);
	size_t (*max_mapping_size)(struct device *dev);
	unsigned long (*get_merge_boundary)(struct device *dev);
};
```

`map_sg` 的注释（`:54`）要求失败时返回负数，合法的错误码列在 `dma_map_sgtable` 的 kerneldoc 里。`flags` 里 6.6 定义了 `DMA_F_PCI_P2PDMA_SUPPORTED`（`:21`）。`iommu_dma_ops` 带这个标志（`drivers/iommu/dma-iommu.c:1617`）。direct 没有 ops 结构，`dma_pci_p2pdma_supported` 在 `ops == NULL` 时直接返回真（`kernel/dma/mapping.c:749`），并写明不看 `dma_ops_bypass`：bypass 也不能让一套不支持 P2P 的 ops 突然支持 P2P。

`get_dma_ops`（`include/linux/dma-map-ops.h:90`）：

```c
static inline const struct dma_map_ops *get_dma_ops(struct device *dev)
{
	if (dev->dma_ops)
		return dev->dma_ops;
	return get_arch_dma_ops();
}
```

这是 `CONFIG_DMA_OPS` 的版本。没编这个选项时函数恒返回 `NULL`（`:103`），所有设备走 direct。`dev->dma_ops` 字段本身也包在 `CONFIG_DMA_OPS` 里（`include/linux/device.h:760`）。

---

## 3.2 `dma_go_direct`

```c
/* kernel/dma/mapping.c:116 */
static bool dma_go_direct(struct device *dev, dma_addr_t mask,
		const struct dma_map_ops *ops)
{
	if (likely(!ops))
		return true;
#ifdef CONFIG_DMA_OPS_BYPASS
	if (dev->dma_ops_bypass)
		return min_not_zero(mask, dev->bus_dma_limit) >=
			    dma_direct_get_required_mask(dev);
#endif
	return false;
}
```

`dma_alloc_direct` 把 mask 换成 `dev->coherent_dma_mask`（`:135`）。`dma_map_direct` 把 mask 换成 `*dev->dma_mask`（`:141`）。注释（`:130`）说明 bypass 是给 IOMMU 驱动用的：streaming 的 mask 够大时可以不进 ops。`dma_ops_bypass` 的设备字段注释（`include/linux/device.h:709`）写明它由 dma ops 在 `dma_supported` 里维护，streaming 的 map/unmap/sync 会跳过 ops，coherent 分配则还要 coherent mask 也够大。

因此「有 ops」和「这次调用走 ops」是两件事。没有 bypass 配置时，非空 ops 一直返回假，调用进入 `ops->map_page`。

`dma_map_page_attrs` 还有第二条旁路（`kernel/dma/mapping.c:159`）：`arch_dma_map_page_direct(dev, page_to_phys(page) + offset + size)` 为真时也走 direct。这是架构钩子，默认不改变上面的选择。unmap / map_sg 有对应的 `arch_dma_unmap_page_direct`、`arch_dma_map_sg_direct`。

---

## 3.3 三套限制：streaming mask、coherent mask、总线限制

```c
/* include/linux/device.h:763，字段紧挨着 */
	u64		*dma_mask;	/* dma mask (if dma'able device) */
	u64		coherent_dma_mask;
	u64		bus_dma_limit;	/* upstream dma constraint */
	const struct bus_dma_region *dma_range_map;
```

`dma_mask` 是指针。`dma_get_mask`（`include/linux/dma-mapping.h:437`）在指针为空或指向的值为 0 时返回 `DMA_BIT_MASK(32)`，否则返回 `*dev->dma_mask`。`dma_map_page_attrs` 在 `dev->dma_mask` 为空时直接 `DMA_MAPPING_ERROR`（`kernel/dma/mapping.c:156`），不会用这个 32 位默认值去真的映射。

`coherent_dma_mask` 是值，不是指针。`dma_set_mask`（`kernel/dma/mapping.c:763`）先把 mask 截断成 `dma_addr_t`，再问 `dma_supported`。通过之后写入 `*dev->dma_mask`。`dma_set_coherent_mask`（`:780`）同样问 `dma_supported`，然后写 `dev->coherent_dma_mask`。`dma_set_mask_and_coherent`（`dma-mapping.h:450`）先设 streaming，成功再设 coherent；头文件注释说 API 保证 coherent mask 可以设成不大于 streaming mask。

`dma_supported`（`mapping.c:730`）在没有 ops 时调用 `dma_direct_supported`。有 ops 但没有 `dma_supported` 回调时返回 1。注释强调必须调用 ops 的方法，因为 `->dma_supported` 会设置 bypass 标志。

`bus_dma_limit` 是上游桥的额外上限。`dma_capable`（`include/linux/dma-direct.h:99`）要求区间末端 `end <= min_not_zero(*dev->dma_mask, dev->bus_dma_limit)`。`dma_addressing_limited`（`dma-mapping.h:476`）在 `min(dma_get_mask, bus_dma_limit)` 小于 `dma_get_required_mask` 时返回真。它的注释说地址位不够是 bounce 的首要原因，但不是唯一原因。

`dma_get_required_mask`（`mapping.c:476`）走 direct 时用 `dma_direct_get_required_mask`：取 `max_pfn` 对应的物理地址，经 `phys_to_dma_direct`，再铺成覆盖这个地址的掩码（`kernel/dma/direct.c:39`）。走 ops 且 ops 没提供回调时，返回 32 位掩码。注释（`:485`）说这是给多数 IOMMU 的默认值：IOMMU 可以把高物理页映进 32 位 IOVA，所以「设备需要看到全部物理内存」这件事对 DMA 域不成立。

---

## 3.4 一次 `dma_map_page` 的分发

```text
dma_map_page                         include/linux/dma-mapping.h:416
  dma_map_page_attrs                 kernel/dma/mapping.c:147
    ops = get_dma_ops(dev)           include/linux/dma-map-ops.h:90
    BUG_ON 非法方向
    dma_mask 为空 → DMA_MAPPING_ERROR
    dma_map_direct(dev, ops)         :141  比较 *dma_mask
      真 → dma_direct_map_page       kernel/dma/direct.h:84
      假 → ops->map_page
    debug_dma_map_page
```

`dma_alloc_attrs` 用 `dma_alloc_direct`，比较的是 coherent mask（`mapping.c:519`）。设备声明的一致内存（`dma_alloc_from_dev_coherent`）优先于这条分发（`:513`）。

sync 的分发同样先看 `dma_map_direct`。direct 为假且 ops 没有对应 sync 回调时，sync 函数什么都不做，只打 debug（`mapping.c:338`）。调用者不能从「函数返回了」推出 cache 已经维护过。

---

## 3.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct dma_map_ops` | [`dma-map-ops.h:23`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-map-ops.h#L23) | 设备的 DMA 函数表 |
| `get_dma_ops` | [`dma-map-ops.h:90`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-map-ops.h#L90) | 读 `dev->dma_ops` |
| `dma_go_direct` | [`mapping.c:116`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L116) | 空 ops 或 bypass |
| `dma_map_direct` | [`mapping.c:141`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L141) | streaming，用 `dma_mask` |
| `dma_alloc_direct` | [`mapping.c:135`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L135) | coherent，用 `coherent_dma_mask` |
| `dma_set_mask` | [`mapping.c:763`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L763) | 截断并检查后写入 `*dma_mask` |
| `dma_capable` | [`dma-direct.h:99`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-direct.h#L99) | mask 与 `bus_dma_limit` 的交集 |
| `dma_addressing_limited` | [`dma-mapping.h:476`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L476) | 设备盖不住系统要求的掩码 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 一致与流式](./02-dma-address.md) · [目录](./README.md) · [direct →](./04-direct-dma.md)
