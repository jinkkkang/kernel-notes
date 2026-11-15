# 7. IOMMU 核心

**结论**：内核能单独改写页表的最小单位是 `struct iommu_group`，不是 PCI 函数号。一组设备共享一个 `default_domain`；`iommu_attach_group` 把一个 domain 装到整组。group 由 PCI 别名和 ACS 沿路能否挡住对等 DMA 来划分，所以它是内核的隔离单位，硬件隔离是否成立还取决于 ACS、固件保留区和有没有人把 domain 换成别的。

> 页表项怎么写、IOTLB 怎么废，在 [08](./08-page-tables.md)。default domain 怎样变成 `dma_ops` 在 [09](./09-dma-iommu.md)。

---

## 7.1 domain 的类型位

```c
/* include/linux/iommu.h:95 */
struct iommu_domain {
	unsigned type;
	const struct iommu_domain_ops *ops;
	unsigned long pgsize_bitmap;
	struct iommu_domain_geometry geometry;
	struct iommu_dma_cookie *iova_cookie;
	enum iommu_page_response_code (*iopf_handler)(struct iommu_fault *fault,
						      void *data);
	void *fault_data;
	union {
		struct {
			iommu_fault_handler_t handler;
			void *handler_token;
		};
		struct {	/* IOMMU_DOMAIN_SVA */
			struct mm_struct *mm;
			int users;
		};
	};
};
```

类型是位组合（`:85`）：

```c
#define IOMMU_DOMAIN_BLOCKED	(0U)
#define IOMMU_DOMAIN_IDENTITY	(__IOMMU_DOMAIN_PT)
#define IOMMU_DOMAIN_UNMANAGED	(__IOMMU_DOMAIN_PAGING)
#define IOMMU_DOMAIN_DMA	(__IOMMU_DOMAIN_PAGING | __IOMMU_DOMAIN_DMA_API)
#define IOMMU_DOMAIN_DMA_FQ	(__IOMMU_DOMAIN_PAGING | __IOMMU_DOMAIN_DMA_API | \
				 __IOMMU_DOMAIN_DMA_FQ)
#define IOMMU_DOMAIN_SVA	(__IOMMU_DOMAIN_SVA)
```

头文件注释（`:70`）：BLOCKED 挡住全部 DMA，用来隔离；IDENTITY 表示 DMA 地址按系统物理地址解释；UNMANAGED 的映射由 IOMMU API 的调用者管理，注释写的用途是虚拟机；DMA / DMA-FQ 给 DMA API 自己用，FQ 表示批量作废 TLB。`iommu_is_dma_domain` 只测试 `__IOMMU_DOMAIN_DMA_API`（`:116`）。identity、blocked、unmanaged 都不是 DMA 域。

`struct iommu_domain_ops`（`:333`）里和映射直接相关的是 `attach_dev`、`map_pages`、`unmap_pages`、`flush_iotlb_all`、`iotlb_sync_map`、`iotlb_sync`、`iova_to_phys`。`attach_dev` 的返回值注释把 `-EINVAL` 当成「这个 domain 和设备不匹配」的软失败，调用者可以换一个 domain 再试。

`struct iommu_ops`（`:264`）是硬件实例的操作：`domain_alloc`、`probe_device`、`device_group`、`def_domain_type`。`def_domain_type` 返回 identity、DMA 或 0。0 表示用系统默认。

---

## 7.2 group：一组设备，一个当前 domain

```c
/* drivers/iommu/iommu.c:50 */
struct iommu_group {
	struct kobject kobj;
	struct kobject *devices_kobj;
	struct list_head devices;
	struct xarray pasid_array;
	struct mutex mutex;
	void *iommu_data;
	char *name;
	int id;
	struct iommu_domain *default_domain;
	struct iommu_domain *blocking_domain;
	struct iommu_domain *domain;
	unsigned int owner_cnt;
	void *owner;
};
```

`iommu_group_alloc` 的注释（`:926`）说 group 表示 IOMMU 的最小粒度。kobject 名字是 id，挂在 `kernel_kobj` 下的 `iommu_groups`（`iommu_init`，`:2731`），所以 sysfs 路径是 `/sys/kernel/iommu_groups/<id>/`。`devices` 子目录里是指向成员设备的链接。

`default_domain` 是 DMA API 用的域。`domain` 是当前装上的域，可以被 VFIO / iommufd 换成 unmanaged。`blocking_domain` 在有 `owner` 时用来挡住 DMA（`__iommu_group_set_core_domain`，`:2035`）。`owner` 表示有人独占了这组的 DMA 所有权。`iommu_group_claim_dma_owner`（`:3264`）的注释写明这是给 VFIO 的兼容接口，一组只能有一个 owner。

`iommu_attach_device`（`:2075`）要求组里恰好一台设备，否则 `-EINVAL`。多设备组要用 `iommu_attach_group`（`:2179`），它把同一个 domain 装到组内每一台。拆开一台设备单独给一个 domain，在组里还有别的设备时不会成功。

`iommu_group_show_type`（`:863`）打印的是 `default_domain->type`，字符串是 `identity`、`DMA`、`DMA-FQ`、`unmanaged`、`blocked`。它不读 `group->domain`。sysfs 的 `type` 因此是默认域的类型。有 owner 把当前域换成别的之后，这个文件仍可能显示 `DMA`。

---

## 7.3 PCI 怎样被放进同一组

VT-d 的 `device_group` 对 PCI 设备调用 `pci_device_group`（`drivers/iommu/intel/iommu.c:4538`）。这个函数（`drivers/iommu/iommu.c:1653`）沿三条规则找已有的组，找不到再 `iommu_group_alloc`：

1. `pci_for_each_dma_alias`：拓扑造成的 DMA 别名必须和别名设备同一组（`:1670`）。
2. 从别名点继续往根走，直到 `pci_acs_path_enabled(..., REQ_ACS_FLAGS)` 为真（`:1681`）。`REQ_ACS_FLAGS` 是 `PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF`（`:1542`）。路径上 ACS 没挡住对等 DMA 的桥，下面的设备会跟桥上面的设备合成一组。
3. 同一槽上 ACS 没打开的多功能设备，以及它们的别名，合成一组（`:1703`）。

`generic_device_group`（`:1643`）则是一台非 PCI 设备一组。平台设备不一定走 ACS 这套规则。

组合成了，只说明内核打算用一个 domain 覆盖这些 RID。ACS 能力位缺失、桥的 ACS 没按 `REQ_ACS_FLAGS` 打开、或者设备有 DMA 别名时，组会变大。组变大之后，组内一台设备的映射对组内其他设备也可见。这不是「每个 BDF 一块硬件防火墙」。

---

## 7.4 默认域怎么选

`iommu_subsys_init`（`:192`）在命令行没指定时：编了 `CONFIG_IOMMU_DEFAULT_PASSTHROUGH` 就用 identity，否则用翻译域。内存加密会关掉默认的 passthrough（`:202`）。翻译域且 `iommu_dma_strict` 为假时，默认类型改成 `IOMMU_DOMAIN_DMA_FQ`（`:208`）。然后打一行 `Default domain type: ...`。

启动参数：

| 参数 | 位置 | 作用 |
|---|---|---|
| `iommu.passthrough=` | `iommu.c:670` | 真则默认 identity，假则翻译 |
| `iommu.strict=` | `iommu.c:680` | 真则同步作废 IOTLB，并把默认从 DMA-FQ 降回 DMA |
| `iommu=pt` / `iommu=nopt` | `arch/x86/kernel/pci-dma.c:161` | x86 上设同一对默认 |
| `iommu=off` | `pci-dma.c:123` | `no_iommu = 1` |

单台设备还可以用 `ops->def_domain_type` 要求 identity 或 DMA。不可信 PCI 设备在 `iommu_get_def_domain_type` 里被强制成 `IOMMU_DOMAIN_DMA`（`:1734`），不跟系统的 passthrough 默认走。一组里多台设备要求的类型冲突时，`iommu_get_default_domain_type`（`:1824`）会失败。`iommu_setup_default_domain`（`:2958`）按这个结果分配 domain，并在 attach 之前映射 `IOMMU_RESV_DIRECT` 区间（`:2994`），避免固件还在用的 1:1 窗口在切换时消失。

驱动不支持 default domain 时，`group->default_domain` 留空（`:2982`）。注释说这种驱动应在 `set_platform_dma_ops` 里把设备留在平台自己的 DMA ops 上。

---

## 7.5 IOVA 分配器

DMA 域的 cookie 里嵌着 `struct iova_domain`（`include/linux/iova.h:28`）：一棵按 PFN 区间排序的红黑树，加上 per-CPU 的 `rcaches`。`struct iova`（`:18`）只有 `pfn_hi`、`pfn_lo` 和树节点。

`alloc_iova_fast`（`drivers/iommu/iova.c:439`）先向 rcache 要。小于缓存上限的长度会被向上取整到 2 的幂，注释说明否则还回去的尺寸对不上。rcache 没有时调用 `alloc_iova`，从 `limit_pfn` 往下找。`flush_rcache` 为真且仍失败，就把各 CPU 缓存刷回树再试一次。失败返回 0。`iommu_dma_alloc_iova` 把 0 变成 `DMA_MAPPING_ERROR`（`dma-iommu.c:761`）。

32 位优先是分配策略，不是硬件限制：`dev->iommu->pci_32bit_workaround` 为真时先在 32 位 IOVA 里试（`dma-iommu.c:690`）。失败就清掉这个标志并 `dev_notice`，然后用设备的 `dma_mask`。`iommu.forcedac` 可以让 PCI 一开始就用 64 位（`dma-iommu.c:77`）。

释放走 `free_iova_fast`，或在 DMA-FQ 上先进 per-CPU 队列，等 IOTLB 作废完成再还（`queue_iova`，`dma-iommu.c:175`）。队列在 [09](./09-dma-iommu.md)。

---

## 7.6 关键函数

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct iommu_domain` | [`iommu.h:95`](https://github.com/gregkh/linux/blob/v6.6.160/include/linux/iommu.h#L95) | 类型、ops、几何、cookie |
| `struct iommu_group` | [`iommu.c:50`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L50) | 设备链表与三个 domain 指针 |
| `pci_device_group` | [`iommu.c:1653`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L1653) | 别名 + ACS + 多功能 |
| `iommu_attach_group` | [`iommu.c:2179`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2179) | 整组装上一个 domain |
| `iommu_setup_default_domain` | [`iommu.c:2958`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L2958) | 分配并切换默认域 |
| `iommu_group_show_type` | [`iommu.c:863`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iommu.c#L863) | sysfs 读的是 default domain |
| `alloc_iova_fast` | [`iova.c:439`](https://github.com/gregkh/linux/blob/v6.6.160/drivers/iommu/iova.c#L439) | rcache，失败再走红黑树 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← SG 与 IOVA](./06-sg-and-iova.md) · [目录](./README.md) · [页表 →](./08-page-tables.md)
