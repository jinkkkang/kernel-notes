# 11. fault 与隔离

**结论**：6.6 没有一个名叫 `IOMMU_FAULT` 的统一入口。VT-d 的不可恢复 DMA fault 由 `dmar_fault` 读故障寄存器并 `pr_err`；通用层另外有 `report_iommu_fault`（带 `IOMMU_FAULT_READ` / `IOMMU_FAULT_WRITE` 标志并打 `iommu:io_page_fault`）和 `iommu_report_device_fault`（参数是 `struct iommu_fault`，类型是 `IOMMU_FAULT_DMA_UNRECOV` 或 `IOMMU_FAULT_PAGE_REQ`）。group 能限制内核把哪些设备放进同一个 domain，挡不住 ACS 没生效时的对等 DMA。

> 页表缺项怎样产生 fault，取决于 [08](./08-page-tables.md) 的翻译。demo 不注入 fault。

---

## 11.1 三条上报，各走各的结构

`include/linux/iommu.h:46`：

```c
#define IOMMU_FAULT_READ	0x0
#define IOMMU_FAULT_WRITE	0x1

typedef int (*iommu_fault_handler_t)(struct iommu_domain *,
			struct device *, unsigned long, int, void *);
```

`report_iommu_fault`（`drivers/iommu/iommu.c:2711`）在 domain 上装了 `handler` 时调用它，然后无条件 `trace_io_page_fault(dev, iova, flags)`。没有 handler 时返回 `-ENOSYS`。trace 事件的格式在 `include/trace/events/iommu.h:153`，tracefs 名是 `iommu:io_page_fault`。flags 是上面的 0 或 1，不是 `struct iommu_fault`。

`iommu_set_fault_handler`（`:1974`）把这个回调和 token 放进 domain。它服务的是「有人关心这颗 domain 的 fault」的内核用户，不是 procfs 里的一个固定文件。

第二条是可恢复的页请求。`struct iommu_fault` 在用户 API 头里（`include/uapi/linux/iommu.h:115`）：

```c
enum iommu_fault_type {
	IOMMU_FAULT_DMA_UNRECOV = 1,
	IOMMU_FAULT_PAGE_REQ,
};

struct iommu_fault {
	__u32	type;
	__u32	padding;
	union {
		struct iommu_fault_unrecoverable event;
		struct iommu_fault_page_request prm;
		__u8 padding2[56];
	};
};
```

`iommu_report_device_fault`（`iommu.c:1411`）要求设备已经注册 `iommu_fault_param.handler`。页请求且带 `IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE` 时，事件会挂到 `faults` 链表上，等 `iommu_page_response`。没有 handler 返回 `-EINVAL`。VT-d 的 SVM 路径会调用它（`drivers/iommu/intel/svm.c:610`）。普通的 DMA 缺页不经过这个函数。

第三条是 VT-d 主线上的硬件 IRQ。`dmar_fault`（`drivers/iommu/intel/dmar.c:2014`）读故障记录，交给 `dmar_fault_do_one`（`:1977`）。中断重映射 fault 打印 `[INTR-REMAP]`。DMA fault 在 PASID 无效时打印：

```text
[%s NO_PASID] Request device [%02x:%02x.%d] fault addr 0x%llx [fault reason 0x%02x] %s
```

`%s` 由 `type` 选成 `DMA Read` 或 `DMA Write`（`:1996`）。然后 `dmar_fault_dump_ptes`。这条 `pr_err` 进内核日志，不经过 `report_iommu_fault`，也不会自动变成 `struct iommu_fault`。SMMU 的事件队列是另一套驱动代码，本章不把它说成同一个函数。

因此「看到一次 DMA fault」要先分清日志从哪来：`dmesg` 里的 `DMAR` / `Request device` 是 VT-d IRQ；`iommu:io_page_fault` 是有人调用了 `report_iommu_fault`；`IOMMU_FAULT_PAGE_REQ` 只出现在注册了设备 fault handler 的 PRI/Stall 路径上。

---

## 11.2 越界时内核能看见什么

翻译域里，设备发出的 IOVA 没有页表项，或权限和 `dma_info_to_prot` 写下的读/写位不符，硬件可以产生上面的 fault。`DMA_TO_DEVICE` 只有 `IOMMU_READ`（设备读内存）。设备若对这块 IOVA 写，VT-d 的 fault reason 会走权限类原因，具体字符串来自 `dmar_get_fault_reason`，不要凭记忆编原因码。

direct 和 identity 没有这层页表检查。设备可以发到 `dma_capable` 允许的任何总线地址。SWIOTLB 只是把数据拷到槽里，槽的 DMA 地址仍然可被设备访问；它不构成地址空间隔离。`swiotlb_bounce` 对拷贝长度超过分配大小会告警并截断（[05](./05-swiotlb.md)），那是 CPU 侧的 memcpy 边界，不是设备被硬件拦住。

`dma_mapping_error` 能拦住的是驱动没拿到合法 `dma_addr_t`。设备如果仍然使用上一次的地址，或驱动把失败值写进描述符，内核的 map 函数不会再被调用，也就不会有第二次返回值。fault 日志是事后的硬件报告，不是 `dma_map_page` 的错误码。

---

## 11.3 ACS 与 group 的限度

`pci_device_group` 沿上游走，直到 `pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS)`（`drivers/iommu/iommu.c:1685`）。四个标志是 Source Validation、Request Redirection、Completion Redirection、Upstream Forwarding（`:1542`）。路径上做不到这一点的设备会被放进同一组，然后 `iommu_attach_group` 给它们同一个 domain。

这带来两层限制：

1. 组内设备共享映射。给其中一台 `dma_map_page` 得到的 IOVA，同组其他设备的 DMA 也能用，只要事务的 RID 被硬件归进这个 domain。
2. ACS 没按这些标志生效时，对等事务可能根本不经过 IOMMU。group 文件仍然存在，因为驱动还是建了组。`/sys/kernel/iommu_groups/` 里有目录，不能推出对等 DMA 已经被挡住，也不能推出 `dma_ops` 指向 `iommu_dma_ops`。

不可信设备（`pci_dev->untrusted`，`include/linux/pci.h:473`）在默认域选择时被强制成 DMA 域（`iommu.c:1734`），并在 dma-iommu 里走 SWIOTLB 对齐策略（[05](./05-swiotlb.md)）。这缩小了它能看见的物理页，仍然依赖它所在的组没有被 ACS 漏洞放大。

`iommu_group_store_type` 可以在权限足够时改默认域（`iommu.c:3067`），但要求除了 DMA 到 DMA-FQ 之外先解开驱动。改类型不是补上 ACS。demo 01 只读 `type`，不写。

---

## 11.4 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `IOMMU_FAULT_READ` / `WRITE` | [`iommu.h:46`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/iommu.h#L46) | `report_iommu_fault` 的标志，值为 0 和 1 |
| `report_iommu_fault` | [`iommu.c:2711`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2711) | 可选 handler，然后 trace |
| `struct iommu_fault` | [`uapi/linux/iommu.h:115`](https://github.com/gregkh/linux/blob/v6.6.160/include/uapi/linux/iommu.h#L115) | 不可恢复事件或页请求 |
| `iommu_report_device_fault` | [`iommu.c:1411`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L1411) | 设备级 handler，PRI 会入队 |
| `dmar_fault_do_one` | [`dmar.c:1977`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/intel/dmar.c#L1977) | VT-d 故障记录的 `pr_err` |
| `REQ_ACS_FLAGS` | [`iommu.c:1542`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L1542) | 分组时要求的 ACS 位 |
| `pci_device_group` | [`iommu.c:1653`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L1653) | 没有 ACS 就并组 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← VFIO 边界](./10-vfio-boundary.md) · [目录](./README.md) · [观测 →](./12-observability.md)
