# 2. DMA 地址与一致 / 流式映射

**结论**：`dma_alloc_coherent` 给出一块设备和 CPU 可以同时看的缓冲，并返回它的 `dma_addr_t`；`dma_map_page` / `dma_map_sg` 只为一次传输建立流式映射，成功之后这段缓冲的所有权在设备侧，CPU 再碰它之前要 `dma_sync_*_for_cpu` 或 unmap。coherent 保证的是 cache 一致，描述符两个字之间的先后仍要靠屏障。

> 分发函数在 [03](./03-dma-api-dispatch.md)。direct 与 SWIOTLB 的地址怎么算在 [04](./04-direct-dma.md) 和 [05](./05-swiotlb.md)。

---

## 2.1 `dma_addr_t` 与失败值

```c
/* include/linux/types.h:149 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
typedef u64 dma_addr_t;
#else
typedef u32 dma_addr_t;
#endif
```

头文件注释（`:140`）把它定义成 DMA API 能返回的任何合法地址。32 位 DMA API 时这个类型可以只有 32 位；PCI BAR 本身可以更宽，但驱动用 `ioremap` 之后的内核虚拟地址去访问 MMIO，不把 BAR 塞进 `dma_addr_t`。

失败值是全 1（`include/linux/dma-mapping.h:75`）：

```c
#define DMA_MAPPING_ERROR		(~(dma_addr_t)0)
```

驱动用 `dma_mapping_error`（`:95`）判断，不要自己和 `0` 比较。它在地址等于 `DMA_MAPPING_ERROR` 时返回 `-ENOMEM`，否则返回 0。`dma_map_sg` 的失败约定不同：返回 0（`:225`）。`dma_map_sgtable` 返回负数，注释列出 `-EINVAL`、`-ENOMEM`、`-EIO`、`-EREMOTEIO`（`:257`）。

方向（`include/linux/dma-direction.h:5`）：

```c
enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3,
};
```

`valid_dma_direction`（`:12`）不接受 `DMA_NONE`。`dma_map_page_attrs` 对非法方向 `BUG_ON`（`kernel/dma/mapping.c:154`）。`DMA_TO_DEVICE` 是 CPU 写完、设备来读；`DMA_FROM_DEVICE` 是设备来写、CPU 之后再读。双向两边都可能写。

---

## 2.2 流式映射：所有权交给设备

`dma_map_single` 是 `dma_map_page` 的薄包装（`include/linux/dma-mapping.h:328`）。它拒绝 vmalloc 地址，然后用 `virt_to_page` 加页内偏移调用 `dma_map_page_attrs`。

`dma_map_sgtable` 的 kerneldoc（`kernel/dma/mapping.c:243`）把所有权写死了：成功之后缓冲属于 DMA 域，CPU 要碰它，必须先 `dma_sync_sgtable_for_cpu` 或 `dma_unmap_sgtable`。sync 回来之后、下一次让设备 DMA 之前，再 `dma_sync_sgtable_for_device`（`:395`）。

direct 实现里，非 coherent 设备会在 map 时做 `arch_sync_dma_for_device`（`kernel/dma/direct.h:110`），除非属性里有 `DMA_ATTR_SKIP_CPU_SYNC`。unmap 默认先 `dma_direct_sync_single_for_cpu`（`:120`）。`DMA_FROM_DEVICE` 的 sync 末尾还有 `arch_dma_mark_clean`（`:80`），用来告诉内核这段是干净的。

coherent 设备上，direct 的 `dma_direct_need_sync`（`kernel/dma/direct.c:600`）在地址不是 SWIOTLB 槽时返回假。`dma_need_sync`（`kernel/dma/mapping.c:822`）走 direct 时用这个结果；走 ops 时只要 ops 挂了 sync 回调就返回真。驱动可以据此省掉 sync，但不能据此省掉描述符之间的屏障。

---

## 2.3 一致映射：cache 一致，顺序仍要写明

```c
/* include/linux/dma-mapping.h:423 */
static inline void *dma_alloc_coherent(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp)
{
	return dma_alloc_attrs(dev, size, dma_handle, gfp,
			(gfp & __GFP_NOWARN) ? DMA_ATTR_NO_WARN : 0);
}
```

它进入 `dma_alloc_attrs`（`kernel/dma/mapping.c:497`）。那里先拒绝 `__GFP_COMP`：DMA 分配不能再变回一个 compound page 指针。然后清掉调用者的 `GFP_DMA` / `GFP_DMA32` / `GFP_HIGHMEM`，改由实现自己挑 zone。`dev->coherent_dma_mask` 为空会 `WARN_ON_ONCE`。

`Documentation/core-api/dma-api.rst`（v6.6.160 该文件约第 36–39 行）对 consistent memory 的定义是：设备和 CPU 写进去的数据，另一边可以立刻读到，不必再做 cache 维护；但在通知设备来读之前，仍可能要把 CPU 的 write buffer 推出去。howto 把同一件事说得更硬（`Documentation/core-api/dma-api-howto.rst:305`）：

```text
desc->word0 = address;
wmb();
desc->word1 = DESC_VALID;
```

注释写明：consistent 映射不取消内存屏障。CPU 可以重排对这块内存的 store，和重排普通内存一样。第一个描述符字必须先于「有效」标志被设备看见时，中间要有屏障。

6.6 提供的设备向屏障在 `include/asm-generic/barrier.h`：架构若定义了 `__dma_wmb` / `__dma_rmb` 就用它们（`:49`、`:45`），否则 `dma_wmb()` 落到 `wmb()`、`dma_rmb()` 落到 `rmb()`（`:80`、`:76`）。x86 把 `__dma_rmb` 和 `__dma_wmb` 定义成编译器 `barrier()`（`arch/x86/include/asm/barrier.h:54`）。这是 x86 的实现选择：对设备可见的 store 顺序由 x86 内存模型加上驱动在门铃处的 MMIO 读回承担。arm64 等架构的 `__dma_wmb` 不是这一个编译器屏障。驱动要可移植，就调用 `dma_wmb` / `wmb`，不要假设「coherent 了就可以连着写两个描述符字」。

`dev_is_dma_coherent`（`include/linux/dma-map-ops.h:262`）在架构需要 sync 时读 `dev->dma_coherent`，否则恒为真。这个位说的是这台设备的 DMA 主设备能否窥探 CPU cache，不说 CPU store 之间的顺序。

IOMMU 侧把「coherent」译成页表标志：`dma_info_to_prot`（`drivers/iommu/dma-iommu.c:639`）在 `coherent` 为真时置 `IOMMU_CACHE`。`IOMMU_CACHE` 的注释是 DMA cache coherency（`include/linux/iommu.h:20`）。方向则变成读/写权限：`DMA_TO_DEVICE` 只给 `IOMMU_READ`（设备读内存），`DMA_FROM_DEVICE` 只给 `IOMMU_WRITE`，双向两者都给（`:647`）。权限位管的是设备能不能写这页，不管 CPU 写描述符的顺序。

---

## 2.4 `dma_map_sg` 的返回值不是「一整段 IOVA」

`dma_map_sg_attrs`（`kernel/dma/mapping.c:231`）的注释：成功时返回映射后的段数，可以小于传入的 `nents`；任何错误返回 0。unmap 必须用当初的 scatterlist 和当初的 `nents`，不能用这个返回值。

`dma_map_sgtable`（`:270`）把成功的段数写进 `sgt->nents`，输入长度留在 `sgt->orig_nents`。`dma_unmap_sgtable`（`include/linux/dma-mapping.h:371`）用的是 `orig_nents`。sync 同样用 `orig_nents`（`:392`、`:409`）。

direct 的 `dma_direct_map_sg`（`kernel/dma/direct.c:465`）对每个输入段各调用一次 `dma_direct_map_page`，成功时返回原来的 `nents`。相邻段不会被收成一个 DMA 地址。IOMMU 的非 bounce 路径会先申请一整段 IOVA，再按段边界拆回驱动可见的输出段，细节在 [06](./06-sg-and-iova.md)。两种实现都允许返回值大于 1。驱动循环必须停在返回的段数上，并用 `sg_dma_address` / `sg_dma_len` 读结果（`include/linux/scatterlist.h:31`）。

---

## 2.5 错误时谁把已经映上的段卸掉

direct：某一段 `dma_direct_map_page` 返回 `DMA_MAPPING_ERROR` 时，`dma_direct_map_sg` 以 `-EIO` 退出（`kernel/dma/direct.c:495`），并对已经成功的前缀调用 `dma_direct_unmap_sg`（`:505`），属性加上 `DMA_ATTR_SKIP_CPU_SYNC`，避免对没完成的映射再做一次 CPU 同步。

`__dma_map_sg_attrs`（`kernel/dma/mapping.c:186`）若 ops 返回的错误不是 `-EINVAL` / `-ENOMEM` / `-EIO` / `-EREMOTEIO`，会 `WARN_ON_ONCE` 并改写成 `-EIO`。`dma_map_sg_attrs` 把负数收成 0。所以 `dma_map_sg` 的调用者只看 0 和正段数；要区分 P2P 拒绝（`-EREMOTEIO`）得用 `dma_map_sgtable`。

`dma_map_page` 失败时没有「半段」可卸，调用者只检查 `dma_mapping_error`。不要把失败地址写进设备描述符。

---

## 2.6 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `dma_addr_t` | [`types.h:149`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/types.h#L149) | 设备地址的宽度 |
| `DMA_MAPPING_ERROR` | [`dma-mapping.h:75`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L75) | 单段映射失败 |
| `dma_mapping_error` | [`dma-mapping.h:95`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L95) | 与全 1 比较 |
| `dma_alloc_coherent` | [`dma-mapping.h:423`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L423) | 转到 `dma_alloc_attrs` |
| `dma_alloc_attrs` | [`mapping.c:497`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L497) | coherent 分配的分发 |
| `dma_map_page_attrs` | [`mapping.c:147`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L147) | 流式单页 |
| `dma_map_sg_attrs` | [`mapping.c:231`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L231) | 返回映射段数，失败为 0 |
| `dma_map_sgtable` | [`mapping.c:270`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L270) | 段数写入 `sgt->nents`，错误码保留 |
| `dma_sync_single_for_cpu` | [`mapping.c:332`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L332) | 所有权回到 CPU |
| `dma_info_to_prot` | [`dma-iommu.c:639`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L639) | 方向和 coherent 译成 IOMMU 权限 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [API 分发 →](./03-dma-api-dispatch.md)
