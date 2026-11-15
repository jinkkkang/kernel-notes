# 10. VFIO 边界

**结论**：6.6 里 VFIO type1 和 iommufd 都在内核里持有 `struct iommu_domain`。用户态提交的是进程虚拟地址、长度、权限和期望的 IOVA；内核钉住页面后调用 `iommu_map`。用户态拿不到 domain 指针，也不能自己改页表。

> KVM 怎样消费这些 IOVA、virtio 队列怎样建，不在本章。group 的 owner 语义在 [07](./07-iommu-core.md)。

---

## 10.1 type1：`VFIO_IOMMU_MAP_DMA`

ioctl 号和参数在 `include/uapi/linux/vfio.h:1544`：

```c
struct vfio_iommu_type1_dma_map {
	__u32	argsz;
	__u32	flags;
	__u64	vaddr;		/* Process virtual address */
	__u64	iova;		/* IO virtual address */
	__u64	size;
};
#define VFIO_IOMMU_MAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 13)
```

注释（`:1522`）说它把进程虚拟地址映射到 IO 虚拟地址。内核入口是 `vfio_dma_do_map`（`drivers/vfio/vfio_iommu_type1.c:1564`）。它检查 size、iova、vaddr 按 domain 的页长对齐，拒绝回绕，拒绝和已有映射重叠（`-EEXIST`），再检查 IOVA 落在允许的孔径里。

真正写页表的是 `vfio_iommu_map`（`:1437`）：对 `iommu->domain_list` 里的每个 `vfio_domain` 调用 `iommu_map(d->domain, iova, pfn << PAGE_SHIFT, ..., prot | IOMMU_CACHE, GFP_KERNEL)`。pfn 来自内核钉住用户页的路径（`vfio_pin_map_dma`，`:1464`）。domain 是 `vfio_iommu_type1_attach_group`（`:2159`）里 `iommu_domain_alloc` 得到的，类型按分配标志是 unmanaged（`__iommu_domain_alloc` 会把类型写回，`drivers/iommu/iommu.c:1998`）。`iommu_attach_group` 把这个 domain 装到整组（`vfio_iommu_type1.c:2224`）。

组里若还有别的设备，它们共享这次 attach。隔离边界仍是 group，不是容器里的某一个 fd。`VFIO_EMULATED_IOMMU` 分支只把组记下来，不分配 domain（`:2189`）。

---

## 10.2 iommufd：`IOMMU_IOAS_MAP`

6.6 树里有 `drivers/iommu/iommufd/`。`iommufd_ioas_map`（`ioas.c:200`）处理 `IOMMU_IOAS_MAP`：检查 flags 只能是 FIXED_IOVA、WRITEABLE、READABLE，取出 ioas 对象，调用 `iopt_map_user_pages`。

`iopt_map_user_pages` 的注释（`io_pagetable.c:396`）写明：对有 domain 的页表，它会钉住用户页并装进 domain 的 IOVA；调用者以后用 `iopt_unmap_iova` 撤销。返回给 ioctl 的是选中的 IOVA（`ioas.c:232` 写回 `cmd->iova`），不是内核指针。

设备挂上硬件页表时，`iommufd_hw_pagetable_attach`（`device.c:328`）调用 `iommu_attach_group(hwpt->domain, idev->igroup->group)`（`:364`）。`iommufd_device_attach` 的注释（`:634`）把这件事叫做把设备连到一个 iommu_domain。domain 存在 `iommufd_hw_pagetable` 里。

`vfio_compat.c` 提供 `IOMMUFD_VFIO` 这一组符号，让 VFIO 容器可以改走 iommufd 的 ioas，而不是 type1 自己的 `vfio_domains`。两条路径在 6.6 同时存在。哪一条在某次运行里生效，取决于 VFIO 设备绑定时用的是 type1 容器还是 iommufd 上下文，sysfs 的 group `type` 文件不会写成「用户态持有 domain」。

---

## 10.3 和 DMA API 的分界

unmanaged domain 没有 `__IOMMU_DOMAIN_DMA_API` 位，`iommu_setup_dma_ops` 不会给它装 `iommu_dma_ops`。设备被 VFIO 占用期间，内核驱动不应再对它调用 `dma_map_page`。所有权由 `iommu_group_claim_dma_owner` 记在 group 上（[07](./07-iommu-core.md)）。本章不写 claim 的调用顺序，也不写 device fd 的 read/write。

用户态看见的 IOVA 是否等于客户机物理地址，是 QEMU 或同类程序的约定。内核只保证 `iommu_map` 把指定的 IOVA 指到被钉住的物理页，并在 unmap 时作废 IOTLB。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← DMA-IOMMU 桥](./09-dma-iommu.md) · [目录](./README.md) · [fault 与隔离 →](./11-fault-and-isolation.md)
