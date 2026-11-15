# 5. SWIOTLB

**结论**：SWIOTLB 是一块预先留出的低地址物理内存，切成 2KiB 的槽；`swiotlb_map` 找到连续槽，把原始缓冲拷进去，再返回槽的未加密 DMA 地址。direct 在地址装不下或被强制 bounce 时调用它；IOMMU DMA 域在设备不可信或 kmalloc 对齐需要 bounce 时调用它。池满时返回 `DMA_MAPPING_ERROR`，并在允许告警时打印已用槽数。

> 调用点在 [04](./04-direct-dma.md) 和 [09](./09-dma-iommu.md)。计数器的路径不固定，见 [12](./12-observability.md)。

---

## 5.1 池和槽

```c
/* kernel/dma/swiotlb.c:76 */
struct io_tlb_slot {
	phys_addr_t orig_addr;
	size_t alloc_size;
	unsigned short list;
	unsigned short pad_slots;
};
```

`list` 是从这个下标起还剩多少空闲槽。分配时把占用的槽写成 0，并把前面相邻空闲槽的 `list` 改成新的连续长度（`swiotlb_area_find_slots`，`:1100`）。`orig_addr` 记下拷贝的源，sync 时靠它找回去。`pad_slots` 只在第一片非填充槽上有效，用来满足对齐。

槽大小是 `IO_TLB_SIZE`，`IO_TLB_SHIFT` 为 11，即 2KiB（`include/linux/swiotlb.h:32`）。一次映射最多 `IO_TLB_SEGSIZE`（128）个连续槽（`:26`），所以单次 bounce 的上限量级是 256KiB 减去对齐填充。`swiotlb_max_mapping_size`（`kernel/dma/swiotlb.c:1545`）用这个上限再减去 `min_align_mask` 造成的浪费。

默认池大小 `IO_TLB_DEFAULT_SIZE` 是 64MiB（`swiotlb.h:36`）。启动参数 `swiotlb=` 由 `setup_io_tlb_npages` 解析（`swiotlb.c:186`）：先是槽数（向上对齐到 128），可选区域个数，然后 `force` 把 `swiotlb_force_bounce` 设真，`noforce` 设 `swiotlb_force_disable`。`force` 让 `is_swiotlb_force_bounce` 为真（`swiotlb.h:194`），direct 的每次 `map_page` 都进槽，不管 mask 够不够。

```c
/* include/linux/swiotlb.h:124 */
struct io_tlb_mem {
	struct io_tlb_pool defpool;
	unsigned long nslabs;
	struct dentry *debugfs;
	bool force_bounce;
	bool for_alloc;
	/* CONFIG_SWIOTLB_DYNAMIC 时还有 can_grow、pools、dyn_alloc */
};
```

`CONFIG_SWIOTLB_DYNAMIC` 下池可以不止一块，`swiotlb_find_slots` 先扫已有池，都失败且 `can_grow` 时 `schedule_work` 并尝试 `GFP_NOWAIT` 再分配一块（`swiotlb.c:1176`）。静态配置没有这条增长路径，`swiotlb_find_slots` 的另一份实现直接在默认池里找（`:1249` 起的 `#else`）。设备用 `dev->dma_io_tlb_mem` 指向自己的 `io_tlb_mem`（`include/linux/device.h:784`）。`is_swiotlb_active` 要求这个指针非空且 `nslabs` 非 0（`swiotlb.c:1569`）。

`for_alloc` 为真时，池还被 `dma_direct_alloc` 拿来做 coherent 分配（`is_swiotlb_for_alloc`，`swiotlb.h:266`）。这和 streaming bounce 共用槽，但调用的是 `swiotlb_alloc`（`swiotlb.c:1673`），不是 `swiotlb_map`。

---

## 5.2 map：找槽、拷贝、再翻译

```text
swiotlb_map                         kernel/dma/swiotlb.c:1516
  trace_swiotlb_bounced
  swiotlb_tbl_map_single            :1324
    swiotlb_find_slots              按 area 加锁找连续 list >= nslots
    找不到：dev_warn_ratelimited "swiotlb buffer is full ..."  :1355
            返回 DMA_MAPPING_ERROR
    写上 orig_addr
    swiotlb_bounce(..., DMA_TO_DEVICE)   :1382
      注释：FROM_DEVICE 也先拷出去，避免把槽里的旧内核数据漏给设备
  phys_to_dma_unencrypted(槽地址)
  dma_capable 失败则 unmap 槽并返回 DMA_MAPPING_ERROR
  非 coherent 且没 SKIP_CPU_SYNC：arch_sync_dma_for_device(槽)
```

`swiotlb_bounce`（`:864`）用槽里的 `orig_addr` 做 `memcpy`。`DMA_TO_DEVICE` 从原始页拷到槽的 `vaddr`；反方向从槽拷回。高地址页走 `memcpy_from_page` / `memcpy_to_page`。长度超过 `alloc_size` 时会 `dev_WARN_ONCE` 并截断拷贝（`:898`）。

unmap（`swiotlb_tbl_unmap_single`，`:1480`）在方向是 `DMA_FROM_DEVICE` 或双向、且没有 `DMA_ATTR_SKIP_CPU_SYNC` 时，先 `swiotlb_bounce(..., DMA_FROM_DEVICE)`，再还槽。`swiotlb_sync_single_for_device`（`:1494`）只在 TO 或双向时把 CPU 的新数据拷进槽。`swiotlb_sync_single_for_cpu`（`:1503`）只在 FROM 或双向时把槽拷回原始缓冲。方向不对会 `BUG_ON`。

direct 的 sync 在拷贝之外，还会对非 coherent 设备做 `arch_sync_dma_for_*`（`kernel/dma/direct.h:55`）。槽地址和原始地址都可能要维护 cache，顺序是：对设备 sync 时先拷进槽再维护槽；对 CPU sync 时先维护 cache 再从槽拷回。

---

## 5.3 谁会进来：地址限制和机密计算

direct 的条件在 `dma_direct_map_page`：`is_swiotlb_force_bounce`，或 `!dma_capable`，或 `dma_kmalloc_needs_bounce`，并且池是活的。

x86 上池会不会先被建好，看 `pci_swiotlb_detect`（`arch/x86/kernel/pci-dma.c:44`）：

- `no_iommu == 0` 且物理页超过 32 位 DMA 窗口：打开池，给 mask 不够的设备用。mask 够的映射仍然直接返回 `phys_to_dma`。
- `CC_ATTR_HOST_MEM_ENCRYPT`：打开池，给不能 DMA 到加密内存的设备。
- `CC_ATTR_GUEST_MEM_ENCRYPT`：打开池，并且 `flags |= SWIOTLB_FORCE`。`swiotlb_init_remap` 把 `force_bounce` 设成 `swiotlb_force_bounce || (flags & SWIOTLB_FORCE)`（`swiotlb.c:369`）。客户机加密因此让使用这块默认池的设备每次 streaming map 都 bounce。注释（`pci-dma.c:58`）说明原因：hypervisor 看不到没显式共享的客户机页。

`swiotlb_tbl_map_single` 在 `CC_ATTR_MEM_ENCRYPT` 时还会 `pr_warn_once`，说明加密打开且系统正在用 DMA bounce（`swiotlb.c:1343`）。这是一次启动期的提示，不是每次映射的计数。

IOMMU 路径上的 bounce 不看「物理地址高不高」。`dev_is_untrusted`（`dma-iommu.c:519`）为真的条件是 PCI 设备的 `untrusted` 位。`pci.h:467` 的注释说这类设备可能发起 DMA 攻击，通常接在 Thunderbolt 一类外部口上；IOMMU 打开时它们应得到完整映射，不能访问任意内存。`dev_use_swiotlb` 对它们返回真（`dma-iommu.c:524`）。页对齐的事务把原始物理页映进 IOVA；不对齐的还要把数据放进槽，再映射槽（`iommu_dma_map_page`，`:1046`）。池不活跃时，不对齐的不可信事务失败，并 `dev_warn_once`。

受限池（`CONFIG_DMA_RESTRICTED_POOL`，`swiotlb.c:1671`）是设备自己的 `io_tlb_mem`，`for_alloc` 可以同时为真。它不是默认的 64MiB 池。demo 05 不能假设计数一定出现在某一个全局路径下。

---

## 5.4 池不够时发生什么

`swiotlb_area_find_slots` 在本 area 的空闲槽不够、或转完一圈找不到 `list >= nslots` 的起点时返回 -1（`:1066`、`:1096`）。`swiotlb_tbl_map_single` 收到 -1 后，若属性没有 `DMA_ATTR_NO_WARN`，打印（`:1357`）：

```text
swiotlb buffer is full (sz: %zd bytes), total %lu (slots), used %lu (slots)
```

然后返回 `DMA_MAPPING_ERROR`。动态池会先试着再分配；`GFP_NOWAIT` 失败同样返回 -1（`:1206`），并可能已经 `pr_warn_ratelimited("Failed to allocate new pool")`（`:750`）。调用链上的 `dma_map_page` 把这个错误交给驱动。已经开始的 SG 映射会卸掉前面的段（[02](./02-dma-address.md)）。内核不会为了这次映射去扩大默认静态池。

---

## 5.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct io_tlb_slot` | [`swiotlb.c:76`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L76) | 一个 2KiB 槽的元数据 |
| `struct io_tlb_mem` | [`swiotlb.h:124`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/swiotlb.h#L124) | 池、强制 bounce、debugfs |
| `setup_io_tlb_npages` | [`swiotlb.c:186`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L186) | `swiotlb=` |
| `swiotlb_map` | [`swiotlb.c:1516`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L1516) | 找槽、拷贝、返回 DMA 地址 |
| `swiotlb_bounce` | [`swiotlb.c:864`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L864) | 槽和原始缓冲之间的 memcpy |
| `swiotlb_init_remap` | [`swiotlb.c:355`](https://github.com/gregkh/linux/blob/v6.6.160/kernel/dma/swiotlb.c#L355) | 没有地址压力且不强制时不建池 |
| `dev_use_swiotlb` | [`dma-iommu.c:524`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/dma-iommu.c#L524) | 不可信设备或 kmalloc bounce |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← direct](./04-direct-dma.md) · [目录](./README.md) · [SG 与 IOVA →](./06-sg-and-iova.md)
