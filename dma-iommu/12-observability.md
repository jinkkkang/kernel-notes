# 12. 观测与性能

**结论**：6.6 能对上 map/unmap 的是 tracepoint `iommu:map`、`iommu:unmap` 和 `swiotlb:swiotlb_bounced`；能对上 VT-d IOTLB 描述符提交的是 `intel_iommu:qi_submit`（编了 `CONFIG_DMAR_TABLE` 时）。debugfs 和 sysfs 都取决于配置和权限，路径不存在就是没有这个接口，不是计数为零。下面的命令都是 **RECIPE**，本章没有贴过输出。

> group 的 `type` 是默认域，不是当前 `dma_ops`。读法见 [07](./07-iommu-core.md) 和 [09](./09-dma-iommu.md)。demo 01 只做只读盘点。

---

## 12.1 启动参数（只读核对，不在这里改）

改这些参数要重启。demo 和这篇笔记都不改 `/proc/cmdline` 以外的任何启动配置。

| 参数 | 源码 | 日志或行为 |
|---|---|---|
| `iommu=off` | `arch/x86/kernel/pci-dma.c:123` | `no_iommu = 1`。高于 32 位内存不再因此打开 SWIOTLB |
| `iommu=pt` / `nopt` | `arch/x86/kernel/pci-dma.c:161` | 默认 identity 或翻译域 |
| `iommu=soft` | `arch/x86/kernel/pci-dma.c:158` | 打开 x86 SWIOTLB |
| `iommu.passthrough=` | `drivers/iommu/iommu.c:670` | 同上一对默认 |
| `iommu.strict=` | `drivers/iommu/iommu.c:680` | 为真则 `iommu_subsys_init` 不把默认域改成 DMA-FQ（`:208`） |
| `iommu.forcedac=` | `drivers/iommu/dma-iommu.c:77` | 为真时打 `Forcing DAC for PCI devices` |
| `swiotlb=` | `kernel/dma/swiotlb.c:186` | 槽数、area 数、`force` 或 `noforce` |
| `swiotlb=force` | 同上 | `swiotlb_force_bounce`，direct 的 streaming map 都进槽 |

`iommu_subsys_init` 会打（`drivers/iommu/iommu.c:211`）：

```text
Default domain type: %s
DMA domain TLB invalidation policy: strict|lazy mode
```

`swiotlb_print_info` 在池建好时打 `mapped [mem %pa-%pa] (%luMB)`（`kernel/dma/swiotlb.c:239`）。x86 还会打 `PCI-DMA: Using software bounce buffering for IO (SWIOTLB)`（`arch/x86/kernel/pci-dma.c:183`）。这些行出现，说明池被建过；没出现，要先区分 dmesg 权限、`swiotlb=noforce`、以及 `iommu=off` 导致的不建池。

---

## 12.2 sysfs：group 与 IOMMU 设备类

`/sys/kernel/iommu_groups/<id>/` 来自 `kset_create_and_add("iommu_groups", NULL, kernel_kobj)`（`drivers/iommu/iommu.c:2731`）。每个组有：

| 文件 | 谁填 | 能说明什么 |
|---|---|---|
| `devices/` | 组成员的符号链接 | 内核把哪些设备放进同一隔离单位 |
| `type` | `iommu_group_show_type`（`:863`） | **默认**域：`identity`、`DMA`、`DMA-FQ`、`unmanaged`、`blocked`、或 `unknown` |
| `reserved_regions` | `iommu_group_show_resv_regions`（`:841`） | `start end 类型`，类型字符串在 `:85`：`direct`、`direct-relaxable`、`reserved`、`msi`，以及 MSI 窗口的软件翻译项 |
| `name` | 可选 | 驱动起的名字 |

`type` 不读 `group->domain`。VFIO 换成 unmanaged 之后，这个文件仍可能是 `DMA`。它也不含 `dma_ops` 指针。

写 `type` 的函数是 `iommu_group_store_type`（`:3067`），需要 `CAP_SYS_ADMIN` 和 `CAP_SYS_RAWIO`，并且除 DMA→DMA-FQ 外要先解开驱动。**RECIPE 不包含这条写。** 写错会改一组设备的翻译方式。

`/sys/class/iommu/` 是 `iommu_class`（`drivers/iommu/iommu-sysfs.c:37`，`name = "iommu"`）。下面的名字是注册过的 IOMMU 设备实例。实例存在只说明驱动调用了 `iommu_device_sysfs_add`。`/sys/class/dma` 是 dmaengine 通道，本章不用它判断设备 DMA 能不能用。

PCI 设备上的 `iommu_group` 符号链接指向组目录。没有这个链接，表示这台设备没进组；有链接，仍不表示它的 streaming map 走 `iommu_dma_ops`。

---

## 12.3 tracepoint

事件定义都在 v6.6.160 里。tracefs 里看不看得见，还取决于对应的 `.o` 有没有编进正在跑的内核。下面不编造计数。

| tracefs 名（`events/` 下） | 定义 | 何时触发 |
|---|---|---|
| `iommu/map` | `include/trace/events/iommu.h:79` | `iommu_map` 成功。字段 iova、paddr、size |
| `iommu/unmap` | `:103` | `__iommu_unmap` 返回前。另有 unmapped_size |
| `iommu/io_page_fault` | `:153` | `report_iommu_fault`。不是 VT-d `dmar_fault` 的那条 `pr_err` |
| `iommu/attach_device_to_domain` | `:72` | `__iommu_attach_device` |
| `iommu/add_device_to_group` | `:39` | 设备进组 |
| `swiotlb/swiotlb_bounced` | `include/trace/events/swiotlb.h:10` | `swiotlb_map` 以及 dma-iommu 的不对齐 bounce。打印里的 `FORCE` 来自 `is_swiotlb_force_bounce` |
| `intel_iommu/qi_submit` | `drivers/iommu/intel/trace.h:21` | 无效化队列提交。`CONFIG_DMAR_TABLE` 编入 `trace.o` |
| `intel_iommu/prq_report` | 同文件 `:58` | 页请求，不是每次 map |

`iommu:map` 的一次 size 是 `iommu_map` 的参数。SG 的非 bounce 路径可能对物理连续的几段合成一次 map，对不连续的段多次 map（`iommu_map_sg`，[06](./06-sg-and-iova.md)）。把 trace 条数当成 `dma_map_sg` 的返回段数会对不上：返回段数还经过 `__finalise_sg` 的段边界。

direct 成功且没进 SWIOTLB 时，没有 `iommu:map`，也没有 `swiotlb_bounced`。这条静默是 direct 的正常结果，不是 tracepoint 坏了。

RECIPE，先看事件在不在，再短时间打开：

```bash
# 事件目录不存在就停，不要当成 0 次映射
ls /sys/kernel/tracing/events/iommu/map
ls /sys/kernel/tracing/events/swiotlb/swiotlb_bounced

# 需要能写 tracefs。做完把 enable 设回 0
# echo 1 > /sys/kernel/tracing/events/iommu/map/enable
# echo 1 > /sys/kernel/tracing/events/iommu/unmap/enable
# 短负载之后
# cat /sys/kernel/tracing/trace
# echo 0 > /sys/kernel/tracing/events/iommu/map/enable
# echo 0 > /sys/kernel/tracing/events/iommu/unmap/enable
```

比较 strict 和 DMA-FQ 时，看的是 `unmap` 与 `intel_iommu:qi_submit`（或 SMMU 命令队列，本章没有对应的通用 trace 名）是否被合成。FQ 的定时器是 10ms、队列 256 项（`dma-iommu.c:88`）。没有前后两组 trace 时，不要估计「FQ 快多少」。

---

## 12.4 debugfs：有配置才有目录

| 路径 | 条件 | 内容 |
|---|---|---|
| `/sys/kernel/debug/swiotlb/io_tlb_nslabs` | `CONFIG_SWIOTLB` 且 `CONFIG_DEBUG_FS`，`swiotlb_create_debugfs_files`（`swiotlb.c:1637`） | 槽总数，只读 |
| `.../io_tlb_used` | 同上 | 当前使用槽数 |
| `.../io_tlb_used_hiwater` | 同上，写只接受 0 | 高水位。读它可以看峰值；写 0 会清水位，demo 不写 |
| `/sys/kernel/debug/dma-api/` | `CONFIG_DMA_API_DEBUG`，`debug.c:819` 建目录 `dma-api` | `error_count`、`dump`、`driver_filter` 等。这是 API 误用检查，不是性能计数 |
| `/sys/kernel/debug/iommu/` | `CONFIG_IOMMU_DEBUGFS`。没有这个选项时 `iommu_debugfs_setup` 是空函数（`include/linux/iommu.h:1126`） | 厂商子目录的父节点。打开时内核会打一段 NOTICE |
| `/sys/kernel/debug/iommu/intel/` | 再加 `CONFIG_INTEL_IOMMU_DEBUGFS`，且 `no_iommu` 为假（`intel/iommu.c:3855`） | `iommu_regset`、`dmar_translation_struct`、`invalidation_queue` 等（`intel/debugfs.c:661`） |

动态 SWIOTLB 的每块池还会在默认 debugfs 目录下再挂子目录（`swiotlb_create_debugfs_files` 的 `dirname`）。所以「使用中的槽」不一定只在 `swiotlb/io_tlb_used` 一个文件里。demo 05 如果要做，先在目标机上 `find` debugfs 里实际存在的 `io_tlb_*`，再读；本章不承诺唯一路径。

debugfs 通常要 root。非 root 读失败是权限，不是接口不存在。先看目录在不在，再谈数字。

`dma-api/dump` 和 `driver_filter` 的写会改变调试器行为。RECIPE 只建议在测试机上短时间打开，并在结束时恢复。不要在生产机上为了看一条 map 去加载带 `CONFIG_DMA_API_DEBUG` 的另一套内核。

---

## 12.5 开销落在哪

一次翻译域上的 `dma_map_page` 至少做：`alloc_iova_fast`（rcache 命中则不到红黑树）、`iommu_map` 写 PTE、以及 `iotlb_sync_map`（VT-d caching mode 才刷 IOTLB，见 [08](./08-page-tables.md)）。unmap 在 strict 域上同步 `iommu_flush_iotlb_psi` 或 SMMU 的 TLBI 命令；在 DMA-FQ 上把 IOVA 放进队列，满或 10ms 到了才 `flush_iotlb_all`。

direct 且地址落在 mask 里时，`dma_direct_map_page` 是 `phys_to_dma` 加可能的 cache sync，没有 IOVA，没有 IOTLB。SWIOTLB 加上一次或两次 `memcpy`，长度是映射长度。把 direct 的延迟和 DMA-FQ 的延迟放在一张表里比较之前，要先确认两边的 `dma_ops` 和有没有 `swiotlb_bounced`。本章没有这组数字。

`dmar_perf_latency`（`intel/debugfs.c:679`）在 Intel debugfs 打开时存在，写它会改延迟统计的开关。那是另一个 RECIPE，而且会动调试状态，不放进 demo 01。

---

## 12.6 和 demo 的分工

[demos/01-run.sh](./demos/01-run.sh) 只读 `uname`、`/proc/cmdline`、PCI 的 vendor/device/driver 链接、`/sys/kernel/iommu_groups`、可读的 dmesg 过滤行，以及 `/sys/class/iommu` 的链接。它打印 SKIP 或 UNKNOWN，不下结论。

本节的 trace 和 debugfs 命令留在 backlog `DMA-IOMMU-INVENTORY` 之外的后续工作里也可以做，但做的时候要单独标 `sampled on <uname -r>`，并且不要把虚拟机上的 group 列表写成「硬件隔离已验证」。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← fault 与隔离](./11-fault-and-isolation.md) · [目录](./README.md)
