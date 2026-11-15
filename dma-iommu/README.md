# DMA 地址、IOMMU 与 SWIOTLB

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION = 6`、`PATCHLEVEL = 6`、`SUBLEVEL = 160`，gregkh/linux 标签 `v6.6.160`，提交 `0aebf2870d53a8d97e02a99e2103ea0b38950c1b`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[git.kernel.org stable v6.6.160](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/?h=v6.6.160) · [gregkh/linux v6.6.160](https://github.com/gregkh/linux/tree/v6.6.160)
> 行号链接用 gregkh 的 blob（`#L` 能跳到行）。mips / powerpc 另有 `asm/dma-direct.h`，本章的地址换算按 x86 与 arm64 实际走的通用实现来写。
> 本章没有把任何数字当成硬件 DMA 验证。demo 01 是只读盘点，状态是 **待真机验证**。观测命令默认是 **RECIPE**。

**一句话**：设备 DMA 用的是 `dma_addr_t`，它由 `dma_map_ops` 选路之后从物理页变出来；`get_dma_ops()` 为空就进 `kernel/dma/direct.c` 的直接映射，非空且没有 bypass 就进那套 ops（x86 VT-d / ARM SMMU 的 DMA 域是 `drivers/iommu/dma-iommu.c` 的 `iommu_dma_ops`）；SWIOTLB 是这两条路上的 bounce 助手，不是第三条必选 ops。

面向 **C 语言新手**：`struct device` 上的 `dma_ops` 是一张函数表。驱动调用 `dma_map_page` 时，通用代码先看这张表要不要被跳过，再调用具体函数。`dma_addr_t` 是交给设备的地址，宽度由 `CONFIG_ARCH_DMA_ADDR_T_64BIT` 决定（`include/linux/types.h:149`）。CPU 虚拟地址、CPU 物理地址、设备看到的 DMA/总线地址是三套数。

## 阅读顺序

```text
01 总览（VA / PA / DMA 地址，三条路径，和块层 / 网络 / MM 的边界）
  → 02 一致映射与流式映射
  → 03 DMA API 怎样选 ops
  → 04 direct
  → 05 SWIOTLB
  → 06 scatterlist 与 IOVA
  → 07 IOMMU 核心：domain / group
  → 08 页表与 IOTLB（VT-d 主线，SMMU 只点差异）
  → 09 dma-iommu 桥
  → 10 VFIO / iommufd 边界
  → 11 fault、ACS、group
  → 12 观测与开销定位
```

先分清地址从哪来，再读 IOMMU 页表。页表答的是「这个 IOVA 落到哪页物理内存」，DMA API 答的是「驱动这次拿到的 `dma_addr_t` 是谁填的」。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | CPU VA、PA、DMA/bus 地址；direct / bounce / IOMMU 怎样组合；和 block-io、networking、mm 的边界 | 中 |
| [02-dma-address.md](./02-dma-address.md) | `dma_addr_t`，`dma_alloc_coherent` / `dma_map_page` / `dma_map_sg` / `dma_sync_*`，方向，所有权，cache 与 ordering，错误 | **厚** |
| [03-dma-api-dispatch.md](./03-dma-api-dispatch.md) | `dma_map_ops` / `get_dma_ops`，`dma_go_direct`，`dma_mask` / `coherent_dma_mask` / `bus_dma_limit` | **厚** |
| [04-direct-dma.md](./04-direct-dma.md) | `kernel/dma/direct.c`：地址换算、可寻址范围、连续页、何时 bounce | 中 |
| [05-swiotlb.md](./05-swiotlb.md) | 池、slot、拷贝、设备地址限制、机密计算、池耗尽 | 中 |
| [06-sg-and-iova.md](./06-sg-and-iova.md) | `scatterlist`，原始 nents 与 mapped nents，`sg_dma_address`，unmap/sync 的参数；IOVA 怎样盖住物理页；bio 只接到 `blk_rq_map_sg` | 中 |
| [07-iommu-core.md](./07-iommu-core.md) | domain / ops / group，attach/detach，隔离粒度，IOVA allocator | **厚** |
| [08-page-tables.md](./08-page-tables.md) | `iommu_map` / `iommu_unmap`，IOTLB，x86 VT-d 主线，ARM SMMU 差异 | **厚** |
| [09-dma-iommu.md](./09-dma-iommu.md) | `dma-iommu.c`：DMA API 到 IOVA，default domain 的 identity / DMA / DMA-FQ / unmanaged | **厚** |
| [10-vfio-boundary.md](./10-vfio-boundary.md) | 6.6 的 VFIO type1 与 iommufd：用户态提交映射，domain 留在内核 | 薄 |
| [11-fault-and-isolation.md](./11-fault-and-isolation.md) | 硬件 fault 怎样报到 dmesg / trace / `struct iommu_fault`；越界能看见什么；ACS 与 group | 中 |
| [12-observability.md](./12-observability.md) | 6.6 里真实存在的 tracepoint、debugfs、sysfs、启动参数；map/unmap 与 IOTLB 开销怎么对上。RECIPE | **厚** |
| [demos/](./demos/) | demo 01 只读盘点。**待真机验证**。02–05 只有计划 | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `dma_map_sgtable` 把 bio 收成的 scatterlist 变成设备地址 | `bio` / `blk_rq_map_sg` / NVMe PRP 见 [block-io/02](../block-io/02-bio-and-bvec.md)、[block-io/13](../block-io/13-nvme-walkthrough.md) |
| 网卡驱动拿到的 `dma_addr_t` 从哪条 API 来 | `sk_buff`、NAPI、队列见 [networking/](../networking/README.md)。本章不写 ring 的描述符布局 |
| CPU 页表、`vmalloc`、`VM_DMA_COHERENT` 这个标志位 | [memory-management/02](../memory-management/02-physical-layout.md)、[memory-management/05](../memory-management/05-vmalloc-and-kernel-va.md) |
| VT-d 的中断重映射（IRTE） | [interrupts/05](../interrupts/05-irqdomain-hierarchy-and-matrix.md)。那是 MSI，不是设备数据 DMA 的页表 |
| KVM 的 EPT / mmu_notifier | [memory-management/11](../memory-management/11-kvm-memory.md)。本章不写 vIOMMU，不写 `KVM_RUN` |

dmaengine 的 memcpy / slave channel（`/sys/class/dma`）、GPU、HMM、完整的 ACPI DMAR 表解析和设备树 `iommu-map`、完整的 VFIO 容器状态机、virtio 队列内部，都不在本章。

## 源码地图

| 路径 | 角色 |
|------|------|
| [`include/linux/types.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/types.h#L149) | `dma_addr_t` |
| [`include/linux/dma-mapping.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-mapping.h#L75) | `DMA_MAPPING_ERROR`、`dma_alloc_coherent`、`dma_map_sg` |
| [`include/linux/dma-map-ops.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-map-ops.h#L23) | `struct dma_map_ops`、`get_dma_ops` |
| [`include/linux/dma-direct.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/dma-direct.h#L20) | `bus_dma_region`、`phys_to_dma`、`dma_capable` |
| [`kernel/dma/mapping.c`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/mapping.c#L116) | `dma_go_direct` 与各 API 的分发 |
| [`kernel/dma/direct.c`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.c#L25) | 直接分配；bounce 条件在 [`direct.h`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/direct.h#L84) |
| [`kernel/dma/swiotlb.c`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L76) | 池、slot、拷贝 |
| [`include/linux/scatterlist.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/scatterlist.h#L11) | `scatterlist`、`sg_table` |
| [`include/linux/iommu.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/iommu.h#L95) | domain、ops、fault 回调类型 |
| [`drivers/iommu/iommu.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L50) | group、default domain、`iommu_map` |
| [`drivers/iommu/dma-iommu.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L1616) | `iommu_dma_ops` |
| [`drivers/iommu/iova.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iova.c#L439) | `alloc_iova_fast` |
| [`drivers/iommu/intel/iommu.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.c#L4205) | VT-d `map_pages` 与 IOTLB |
| [`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c#L2504) | SMMUv3 把 map 交给 io-pgtable，失效走命令队列 |
| [`drivers/vfio/vfio_iommu_type1.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/vfio/vfio_iommu_type1.c#L1564) | `VFIO_IOMMU_MAP_DMA` |
| [`drivers/iommu/iommufd/ioas.c`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommufd/ioas.c#L200) | `IOMMU_IOAS_MAP` |
| [`include/uapi/linux/iommu.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/uapi/linux/iommu.h#L115) | `struct iommu_fault` |
| [`include/trace/events/iommu.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/trace/events/iommu.h#L79) | `iommu:map` / `unmap` / `io_page_fault` |
| [`include/trace/events/swiotlb.h`](https://github.com/gregkh/linux/blob/v6.6.160/include/trace/events/swiotlb.h#L10) | `swiotlb:swiotlb_bounced` |

## 实测 vs RECIPE

- **实测**：本章没有硬件 DMA 实测。写作时在虚拟机上跑过 [demos/01-run.sh](./demos/01-run.sh)（`sampled on 6.12.113+`，uid 1000，退出码 0）。那次只确认脚本能跑：PCI 有设备但 `iommu_group=NONE`，`/sys/kernel/iommu_groups` 与 `/sys/class/iommu` 是空目录所以打印 UNKNOWN，dmesg 能读到 SWIOTLB 的 `software IO TLB` 行。这次采样不能当作 v6.6.160 的行号依据，也不能当作硬件 DMA、bounce 是否发生或 ACS 隔离的验证。完整 stdout 不贴进正文。
- **RECIPE / 待真机验证**：demo 01 的验收在 [demos/README.md](./demos/README.md)。backlog ID 是 `DMA-IOMMU-INVENTORY`。[12](./12-observability.md) 的 trace / debugfs 命令同样是 RECIPE。demo 02–05 不实现。

## 未覆盖

- dmaengine 的 `dma_async_tx_descriptor`、memcpy / slave channel
- GPU、HMM、SVA 的 `iommu_sva_bind_device` 只在 domain 类型里点到
- ACPI DMAR 表的逐字段、设备树 `iommus` / `iommu-map` 的完整解析
- AMD IOMMU 页表、GART
- vIOMMU、`KVM_RUN`、virtio 描述符环内部
- VFIO 的容器、group fd、device fd 状态机全文
- 网络多队列和块层调度器

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
