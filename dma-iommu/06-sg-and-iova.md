# 6. SG 与 IOVA

**结论**：`dma_map_sg` 的输入段数是调用者传入的 `nents`，返回值是映射之后驱动该遍历的段数；unmap 和 sync 用的是原来的段数（`sg_table` 上则是 `orig_nents`）。direct 给每一段各自的 `phys_to_dma` 结果。IOMMU 的非 bounce 路径先占一整段 IOVA，再按设备的段边界把驱动可见的输出拆开，物理页不需要连续。两种路径都不保证整个输入只变成一个 DMA 段。

> bio 到 scatterlist 的转换停在块层的 `blk_rq_map_sg`。IOVA 红黑树在 [07](./07-iommu-core.md)。

---

## 6.1 `scatterlist` 里有两套长度

```c
/* include/linux/scatterlist.h:11 */
struct scatterlist {
	unsigned long	page_link;
	unsigned int	offset;
	unsigned int	length;
	dma_addr_t	dma_address;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
	unsigned int	dma_length;
#endif
};
```

`page_link` 的低两位是链表标志：`SG_CHAIN`（0x1）指向下一段表，`SG_END`（0x2）表示这是最后一项（`:67`）。页指针要掩掉这两位。map 之前，`offset` 和 `length` 描述 CPU 页里的哪一段。map 之后，设备地址在 `dma_address`，设备可见长度用 `sg_dma_len`。

`CONFIG_NEED_SG_DMA_LENGTH` 打开时 `sg_dma_len` 读 `dma_length`，否则读 `length`（`:33`）。头文件注释（`:25`）要求调用者只使用 `dma_map_sg` 返回的那段，或者停在第一个 `sg_dma_len == 0` 的项。

```c
/* include/linux/scatterlist.h:39 */
struct sg_table {
	struct scatterlist *sgl;
	unsigned int nents;		/* number of mapped entries */
	unsigned int orig_nents;	/* original size of list */
};
```

`dma_map_sgtable` 成功后把返回值写入 `nents`（`kernel/dma/mapping.c:278`）。`dma_unmap_sgtable` 把 `orig_nents` 传给 `dma_unmap_sg_attrs`（`dma-mapping.h:374`）。把 `nents` 传进 unmap，会在合并发生时少卸，或在实现把 DMA 字段写回原始 offset 时看错长度。

---

## 6.2 direct：一段输入，一段输出

`dma_direct_map_sg`（`kernel/dma/direct.c:465`）对 `for_each_sg` 的每一项调用 `dma_direct_map_page`，把结果写进 `sg->dma_address`，并把 `sg_dma_len` 设成 `sg->length`。成功返回原来的 `nents`。某一项失败就 unmap 已经写过的前缀并返回负数，外层 `dma_map_sg` 再收成 0。

P2P 段可能被标成总线地址并 `continue`（`:477`），那种段不经过 `phys_to_dma`。其余段的 DMA 地址各自独立。两段物理页相邻，也不表示两个 `dma_address` 相差正好是第一段的长度：`dma_range_map` 的区间边界、bounce 进槽，都会打断这种算术。

---

## 6.3 IOMMU：先占 IOVA，再决定驱动能看见几段

非 bounce 的 `iommu_dma_map_sg`（`drivers/iommu/dma-iommu.c:1241`）做三步。

第一步，把每段的页内不对齐部分暂存在尚未使用的 DMA 字段里，把 `s->offset` / `s->length` 调成 IOVA 粒度的整数（`:1275`）。同时按 `dma_get_seg_boundary` 计算填充，避免一段 IOVA 跨过设备不能跨越的边界。累加得到 `iova_len`。

第二步，`iommu_dma_alloc_iova` 为这整段长度分配一个 IOVA（`:1335`）。然后 `iommu_map_sg`（`drivers/iommu/iommu.c:2629`）沿着这个 IOVA 往下映。它只在相邻 scatterlist 项的物理地址也连续时把它们并成一次 `iommu_map`（`:2649` 的 `s_phys != start + len`）。物理上不连续的页变成同一条 IOVA 上的多段页表项。函数注释说物理页的合并留给 IOMMU 驱动（`dma-iommu.c:1342`）。

第三步，`__finalise_sg`（`:1113`）把暂存的偏移恢复，并决定输出段数。能拼进上一段的条件是（`:1154`）：已经有一段输出、这一段的 IOVA 偏移为 0、没有踩到 `dma_get_seg_boundary`、并且加上之后不超过 `dma_get_max_seg_size`。否则 `count++`，新开一段 `sg_dma_address`。返回值是 `count`，可以小于输入 `nents`，也可以等于。默认 `dma_get_max_seg_size` 在没有 `dma_parms` 时是 64KiB（`dma-mapping.h:486`）。一个很长的 bio 因此经常变成多个 DMA 段，即使它们落在一次 IOVA 分配里。

unmap 依赖这个布局：`iommu_dma_unmap_sg`（`:1361`）的注释写明这些段来自同一次连续 IOVA 分配，于是取第一个和最后一个 `sg_dma_address` 做一次 `__iommu_dma_unmap`。这是实现内部的契约。驱动看到的仍是 `sg_dma_address(sg)` 组成的链表，不能假设 `nents == 1`。

bounce 路径更直接：`dev_use_sg_swiotlb` 为真时（不可信设备，或某段 kmalloc 长度不安全），`iommu_dma_map_sg_swiotlb`（`:1211`）对每一段调用 `iommu_dma_map_page`，返回原来的 `nents`。每一段有自己的 IOVA。unmap 走 `iommu_dma_unmap_sg_swiotlb`，不能用「一整段 IOVA」的算法。入口用 `sg_dma_mark_swiotlb` 做标记（`:1217`）。

`iommu_dma_alloc_iova`（`:659`）向 `alloc_iova_fast` 要的是页帧个数。IOVA 的粒度是 domain 支持的最小页（`iommu_dma_init_domain` 里的 `__ffs(domain->pgsize_bitmap)`，`:584`）。分配器在 [07](./07-iommu-core.md)。

---

## 6.4 和 bio 的接缝

块层的 `blk_rq_map_sg`（`include/linux/blk-mq.h:1149`）把 request 的 `bio` / `bio_vec` 收成一张 scatterlist：每一项的 `page`、`offset`、`length` 来自 bio。它不调用 DMA API，也不分配 IOVA。

NVMe PCI 在 `nvme_map_data` 里拿到这张表之后调用 `dma_map_sgtable`，再用映射段数填 PRP 或 SGL。那条调用链和错误码怎样变成 `blk_status_t`，写在 [block-io/13](../block-io/13-nvme-walkthrough.md) §13.2。本章停在 `dma_map_sgtable` 返回之后 `sg_dma_address` 已经可被驱动读取。

网络驱动通常对 skb 的每个 frag 调用 `dma_map_page` 或 `dma_map_single`，不经过 bio。描述符环怎样组织，留在 [networking/](../networking/README.md)。

---

## 6.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct scatterlist` | [`scatterlist.h:11`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/scatterlist.h#L11) | CPU 段与 DMA 地址 |
| `struct sg_table` | [`scatterlist.h:39`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/scatterlist.h#L39) | `nents` 与 `orig_nents` |
| `dma_unmap_sgtable` | [`dma-mapping.h:371`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L371) | unmap 使用 `orig_nents` |
| `dma_direct_map_sg` | [`direct.c:465`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.c#L465) | 逐段，不合并 |
| `iommu_dma_map_sg` | [`dma-iommu.c:1241`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1241) | 一次 IOVA，再 `__finalise_sg` |
| `__finalise_sg` | [`dma-iommu.c:1113`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1113) | 按段边界决定输出段数 |
| `iommu_map_sg` | [`iommu.c:2629`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2629) | 物理连续才并成一次 map |
| `blk_rq_map_sg` | [`blk-mq.h:1149`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/blk-mq.h#L1149) | bio 到 scatterlist，尚未 DMA map |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← SWIOTLB](./05-swiotlb.md) · [目录](./README.md) · [IOMMU 核心 →](./07-iommu-core.md)
