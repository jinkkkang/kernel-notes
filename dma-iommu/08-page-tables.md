# 8. 页表翻译

**结论**：`iommu_map` 按 domain 的最小页长对齐之后，循环调用 `map_pages`，成功再 `iotlb_sync_map`；`iommu_unmap` 拆页之后调用 `iotlb_sync` 把搜集到的 IOVA 范围从 IOTLB 丢掉。x86 VT-d 自己写 `struct dma_pte`，再按能力用寄存器或无效化队列作废。ARM SMMUv3 把页表交给 `io_pgtable_ops`，作废时往命令队列提交 `CMDQ_OP_TLBI_*`。

> 谁决定要映射哪一段 IOVA，在 [09](./09-dma-iommu.md)。fault 在映射不存在时怎样上报，在 [11](./11-fault-and-isolation.md)。

---

## 8.1 通用的 map / unmap

`iommu_map`（`drivers/iommu/iommu.c:2519`）把 GFP 里的 `GFP_COMP`、`GFP_DMA`、`GFP_DMA32`、`GFP_HIGHMEM` 当成调用错误。然后 `__iommu_map`：

```text
__iommu_map                         drivers/iommu/iommu.c:2459
  domain 必须带 __IOMMU_DOMAIN_PAGING，否则 -EINVAL
  iova、paddr、size 都按最小页长对齐
  while (size)                      :2492
    __iommu_map_pages → ops->map_pages 或 ops->map
    记下已经 mapped 的字节；失败则 iommu_unmap 把已映的部分卸掉
  成功：trace_map(iova, paddr, size)
iommu_map 在 ret == 0 且 ops->iotlb_sync_map 存在时再调用它
```

`trace_map` 的格式在 `include/trace/events/iommu.h:79`，tracefs 名字是 `iommu:map`。它记录的是这次 API 调用的 IOVA 和物理地址，不是硬件 IOTLB 命中。

`iommu_unmap`（`:2607`）初始化一个 `iommu_iotlb_gather`，调用 `__iommu_unmap`，再 `iommu_iotlb_sync`。`__iommu_unmap`（`:2553`）对没有 paging 位的 domain 直接返回 0：identity 域不会在这里拆页表。循环里 `ops->unmap_pages` 返回 0 就停止，所以卸到一个本来就没有映射的洞会提前结束。`trace_unmap`（`include/trace/events/iommu.h:103`）同时打出请求的 size 和实际 unmapped。

`iommu_unmap_fast`（`:2621`）只做拆页，把范围留在调用者的 `iommu_iotlb_gather` 里，不同步 IOTLB。DMA-FQ 用它把作废推迟到队列满或定时器（[09](./09-dma-iommu.md)）。

`struct iommu_iotlb_gather`（`include/linux/iommu.h:222`）记下 start、end、pgsize、要释放的页表页 `freelist`，以及 `queued`。注释说驱动可以在 `iotlb_sync` 或 `flush_iotlb_all` 之后再释放 `freelist` 里的页，避免硬件还缓存着指向它们的 PTE。

---

## 8.2 VT-d：`dma_pte` 与 `__domain_mapping`

```c
/* drivers/iommu/intel/iommu.h:762 */
/*
 * 0: readable
 * 1: writable
 * 2-6: reserved
 * 7: super page
 * 8-10: available
 * 11: snoop behavior
 * 12-63: Host physical address
 */
struct dma_pte {
	u64 val;
};
```

页长 `VTD_PAGE_SHIFT` 是 12（`drivers/iommu/intel/iommu.h:32`）。`struct dmar_domain`（同文件 `:582`）把通用 `iommu_domain` 嵌在末尾，自己持有 `pgd`、`agaw`、`gaw`，以及 `use_first_level`：为真时 DMA 走第一级页表（和 CPU 页表格式更接近的那套），否则走第二级。`iommu_superpage` 记录硬件能用的大页级别，注释从 4KiB 列到 1TiB。

`intel_iommu_map_pages`（`drivers/iommu/intel/iommu.c:4205`）只接受 4KiB、2MiB、1GiB，且 IOVA 与物理地址按这个页长对齐。然后调用 `intel_iommu_map`（同文件 `:4169`），把 `IOMMU_READ` / `IOMMU_WRITE` 译成 `DMA_PTE_READ` / `DMA_PTE_WRITE`，需要时再加 `DMA_PTE_SNP`。地址宽度不够返回 `-EFAULT`。真正写 PTE 的是 `__domain_mapping`（同文件 `:2218`）。

`__domain_mapping` 先拒绝 domain 不支持的 PFN，再拒绝既不可读也不可写的 prot。属性里放上 present 位。`use_first_level` 时再加 XD、user/supervisor、access，写权限还加 dirty。然后按 `hardware_largepage_caps` 能合并的大页去 `pfn_to_dma_pte`，把 `phys_pfn << 12` 和属性写进 `pte->val`。页表页不够时 `pfn_to_dma_pte` 返回空，函数返回 `-ENOMEM`。

`intel_iommu_domain_alloc`（`:4068`）对 `IOMMU_DOMAIN_DMA` 和 `IOMMU_DOMAIN_UNMANAGED` 都 `alloc_domain` 再 `md_domain_init`。identity 返回全局的 `si_domain->domain`，不新建页表。blocked 返回一个只实现 `attach_dev = blocking_domain_attach_dev` 的静态 domain（`:4061`），attach 时 `device_block_translation`。

`intel_iommu_attach_device`（`:4153`）如果设备已经有 domain，先 block，再 `prepare_domain_attach_device`：检查 snooping、把 domain 的地址宽度收成这颗 IOMMU 的 AGAW，必要时拆掉多余的页表级。然后 `dmar_domain_attach_device` 把设备的上下文项指到这个 domain。

---

## 8.3 VT-d：IOTLB 怎样作废

映射建立之后，`__mapping_notify_one`（`:1608`）只在 caching mode 且第二级页表时调用 `iommu_flush_iotlb_psi`。注释说明非 caching mode 的「从缺页变成存在」不必刷设备 IOTLB，但要 `iommu_flush_write_buffer`。

`iommu_flush_iotlb_psi`（`:1546`）把页数向上取整到 2 的幂，得到 PSI 的 mask。硬件没有 page-selective invalidation，或范围大过 `cap_max_amask_val`，就退化成 domain-selective flush（`DMA_TLB_DSI_FLUSH`）。否则 `DMA_TLB_PSI_FLUSH`，地址里可以带 IH 位。第一级页表走 `domain_flush_pasid_iotlb`。

刷新函数指针在初始化时二选一：队列不可用时 `iommu->flush.flush_iotlb = __iommu_flush_iotlb`（`drivers/iommu/intel/iommu.c:2617`），否则 `qi_flush_iotlb`（`:2622`）。寄存器版本（`:1326`）按类型填全局、按 domain 或按页的描述，写到 ECAP 给出的 IOTLB 寄存器，然后 `IOMMU_WAIT_OP` 等到硬件清掉 IVT 位。IAIG 为 0 时 `pr_err("Flush IOTLB failed\n")`（`:1369`）。`qi_flush_iotlb` 的原型在 `drivers/iommu/intel/iommu.h:817`。描述符提交可以在 `intel_iommu:qi_submit` 上看到（`drivers/iommu/intel/trace.h:21`），这个事件随 `CONFIG_DMAR_TABLE` 编进 `trace.o`。

unmap 侧，`intel_iommu_tlb_sync`（`drivers/iommu/intel/iommu.c:4273`）对 domain 挂过的每颗 IOMMU 调用 `iommu_flush_iotlb_psi`，然后 `put_pages_list` 释放 gather 里的页。`intel_flush_iotlb_all`（同文件 `:1622`）用于 FQ 把整域刷掉的情况。

DMA-FQ 把多次 unmap 收成一次 `flush_iotlb_all`，延迟上限是队列深度和 10ms 定时器（[09](./09-dma-iommu.md)）。strict 模式每次 unmap 都走 `iotlb_sync`。开销差在这里，不在 `dma_map_page` 的符号名上。

---

## 8.4 ARM SMMUv3：只看和 VT-d 不同的两处

`arm_smmu_map_pages`（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2504`）不自己解析 PTE。它取出 `to_smmu_domain(domain)->pgtbl_ops`，调用 `ops->map_pages`。页表格式在 `io-pgtable`（ARM LPAE 那一套），不在 SMMU 驱动的这个函数里。`pgtbl_ops` 为空返回 `-ENODEV`。

`arm_smmu_unmap_pages`（`:2516`）同样转发。`arm_smmu_iotlb_sync`（`:2537`）在 `gather->pgsize` 非 0 时调用 `arm_smmu_tlb_inv_range_domain`。那个函数按 stage 选择 `CMDQ_OP_TLBI_NH_VA`、`CMDQ_OP_TLBI_EL2_VA` 或 `CMDQ_OP_TLBI_S2_IPA`（`:1986`），再 `__arm_smmu_tlb_inv_range` 把命令写进 SMMU 的命令队列。

和 VT-d 并列时只有这两点需要记住：页表项的位定义在 io-pgtable，不在 `dma_pte`；作废的硬件接口是命令队列里的 TLBI，不是 VT-d 的 IOTLB 寄存器或 QI 描述符。`iommu_map` / `iommu_unmap` 上面的 gather 与 trace 两边共用。SMMU 的 STE、CD、二级嵌套不在本章展开。

---

## 8.5 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `iommu_map` | [`iommu.c:2519`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2519) | 对齐、循环 map、`iotlb_sync_map` |
| `iommu_unmap` | [`iommu.c:2607`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2607) | 拆页后 `iotlb_sync` |
| `iommu_unmap_fast` | [`iommu.c:2621`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2621) | 只拆页，留给 FQ |
| `struct dma_pte` | [`intel/iommu.h:762`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.h#L762) | VT-d 的 8 字节 PTE |
| `__domain_mapping` | [`intel/iommu.c:2218`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.c#L2218) | 写 PTE，可合并大页 |
| `iommu_flush_iotlb_psi` | [`intel/iommu.c:1546`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.c#L1546) | PSI，不够则 DSI |
| `__iommu_flush_iotlb` | [`intel/iommu.c:1326`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/iommu.c#L1326) | 寄存器作废并等待 |
| `arm_smmu_map_pages` | [`arm-smmu-v3.c:2504`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c#L2504) | 转发 io-pgtable |
| `arm_smmu_tlb_inv_range_domain` | [`arm-smmu-v3.c:1974`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c#L1974) | 提交 TLBI 命令 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← IOMMU 核心](./07-iommu-core.md) · [目录](./README.md) · [DMA-IOMMU 桥 →](./09-dma-iommu.md)
