# dma-iommu demos

对照 v6.6.160 的 DMA 地址、IOMMU group 和 SWIOTLB。**demo 01 只读，不证明硬件 DMA。** 写作时在虚拟机上跑过脚本（`sampled on 6.12.113+`，退出码 0）：PCI 设备的 `iommu_group` 是 `NONE`，group 目录和 `/sys/class/iommu` 为空所以是 UNKNOWN，dmesg 里能看到 SWIOTLB。那次结果只说明脚本在这台虚拟机上能跑，不作为预期输出，也不作为硬件 DMA 验证。状态是 **待真机验证**。02–05 只有计划，没有脚本。

不要在生产机上跑。脚本不提权、不写 sysfs、不加载模块。保存输出由调用者自己重定向，脚本不创建结果文件。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `01-run.sh` | 只读盘点 `uname`、`/proc/cmdline`、PCI BDF、iommu group、`/sys/class/iommu`、能读到的 dmesg | **待真机验证** |

```text
./01-run.sh
./01-run.sh > inventory.txt
```

不需要 root。没有 `KN_DEMO_RUN` 一类开关：脚本没有会改系统的路径。

## demo 01 读什么

按节打印。每一节缺数据时用 `SKIP:` 或 `UNKNOWN:`，退出码仍是 0。

| 节 | 读 | 缺了怎么说 |
|---|---|---|
| `meta` | `date`、`uname -a`、`uname -r`、uid | `uname -r` 失败则 UNKNOWN。永远打印局限说明 |
| `cmdline` | `/proc/cmdline` | 不可读则 SKIP |
| `pci` | `/sys/bus/pci/devices/<bdf>/{vendor,device}`，`driver` 与 `iommu_group` 的 `readlink` | 目录不存在 SKIP；目录空 UNKNOWN；超过 512 个 BDF 只印前 512 并 SKIP 其余 |
| `iommu_groups` | `/sys/kernel/iommu_groups/<id>/type` 与 `devices/*` 的 `readlink` | 目录不存在 SKIP；没有组 UNKNOWN；`type` 不可读 SKIP 这一项 |
| `iommu_class` | `/sys/class/iommu/` 的名字和 `readlink` | 目录不存在 SKIP；空目录 UNKNOWN |
| `dmesg` | `dmesg` 里含 `DMAR`、`IOMMU`、`SWIOTLB`、`software IO TLB` 的行 | 命令不存在或权限拒绝 SKIP；能读但没有这些行 UNKNOWN，并写明这不表示没有 IOMMU。超过 80 行只印前 80 |

不读 `/sys/class/dma`。那是 dmaengine 的 channel 类。它为空不能当作「设备不能 DMA」。

`type` 文件是 `iommu_group_show_type` 打印的 **default domain**（`drivers/iommu/iommu.c:863`，v6.6.160）：`identity`、`DMA`、`DMA-FQ`、`unmanaged`、`blocked` 或 `unknown`。它不是当前装上的 domain，也不是 `dev->dma_ops`。group 目录存在同样不能推出 ACS 已经挡住对等 DMA。脚本在 `iommu_groups` 节里重复这句，避免把盘点读成结论。

`/sys/class/iommu` 来自 `iommu_class`（`drivers/iommu/iommu-sysfs.c:37`）。它列出注册过的 IOMMU 设备，不表示每台 PCI 设备都走翻译域。

## 各环境的 SKIP / UNKNOWN

| 环境 | 预期 |
|---|---|
| 无 root，`/proc/cmdline` 和 sysfs 可读，dmesg 被拒 | cmdline、PCI、group 有内容或 UNKNOWN；dmesg 为 SKIP。退出码 0 |
| 虚拟机没有 PCI 设备 | `pci` 为 UNKNOWN 或 `pci_count=0` 不会出现；空目录是 UNKNOWN |
| 内核没编 IOMMU，没有 `/sys/kernel/iommu_groups` | 该节 SKIP。不要据此写「DMA 不可用」 |
| 有 group 但 `type` 是 `identity` | 照印。这只说明默认域是 identity，direct 映射仍可能使用 `dma_range_map` 或 SWIOTLB |
| dmesg 能读但没有 DMAR/IOMMU/SWIOTLB | UNKNOWN。早期日志可能被冲掉 |
| 任何一节失败 | 不把别的节改成失败。脚本末尾仍是 `result=ok` |

## demo 01 验收（给以后的 code agent）

在测试机上执行 `./01-run.sh`，把整段 stdout 另存时标 `sampled on <uname -r>`。下面是要核对的事实，不是一份编好的输出：

1. 有 `sampled on` 行，且与当次 `uname -r` 一致。有局限说明：group、`type`、dmesg 不能确定 `dma_map_ops`、当前域或 ACS。
2. 没有 `sudo`、没有对 sysfs 的写、没有 `modprobe` / `insmod`、没有改 cmdline。
3. 缺路径是 `SKIP:`，空目录或「能读但没有匹配行」是 `UNKNOWN:`，进程退出码为 0。
4. PCI 行的 `driver=` 和 `iommu_group=` 来自 `readlink` 的原文，或是 `NONE` / `SKIP` / `UNKNOWN`。不要把相对链接展开成猜测的绝对路径。
5. 输出里不出现「因此该设备使用 direct」或「隔离充分」这类结论句。脚本本身不打印这类句子。
6. 虚拟机上的成功退出只说明脚本能跑。backlog `DMA-IOMMU-INVENTORY` 在贴上真机（或明确标注的虚拟机）stdout 之前保持 `open`。贴的时候写明这不是硬件 DMA 验证。

## 以后再写（只有名字）

| 编号 | 目的 | 前提 | 风险 | 状态 |
|------|------|------|------|------|
| 02 | 同一次传输在 `dma_map_sg` 前后看原始 `nents` 和 `sg_dma_address` | 测试机、能观察驱动或自建只在测试设备上的映射；先读 [06](../06-sg-and-iova.md) 的 unmap 必须用原始 nents | 映射一张正在使用的设备队列会破坏 IO。不要对启动盘或管理网卡做 | RECIPE / 待真机验证。不写脚本 |
| 03 | 对照 direct 与 IOMMU 翻译域各一次 map 的 trace（`swiotlb:swiotlb_bounced` 与 `iommu:map`） | 两台已经处于不同默认域的测试设备，或事先由人在维护窗口改过启动参数 | **不要**由脚本修改 `iommu=` / `iommu.passthrough=` 或重启。域类型写在 sysfs `type` 里，写它会改一组设备 | RECIPE / 待真机验证。不写脚本 |
| 04 | 在已经隔离的测试设备上观察一次 fault 日志从哪条路径出来 | 专用设备、可恢复的实验环境；先读 [11](../11-fault-and-isolation.md) 的三条上报 | 禁止脚本做设备解绑、VFIO 绑定、DMA fault 注入。故障可能把设备打到需要重置 | RECIPE / 待真机验证。不写脚本 |
| 05 | 读 SWIOTLB 使用槽数 | 先在目标机上确认接口在不在。v6.6.160 在 `CONFIG_DEBUG_FS` 且池已建立时，`swiotlb_create_debugfs_files`（`kernel/dma/swiotlb.c:1637`）创建 `io_tlb_nslabs`、`io_tlb_used`、`io_tlb_used_hiwater`。动态池还会有子目录。trace 是 `swiotlb:swiotlb_bounced` | 不承诺路径一定是 `/sys/kernel/debug/swiotlb/io_tlb_used`。`hiwater` 可写，写 0 会清水位，脚本不应写。debugfs 常要 root | RECIPE / 待真机验证。不写脚本 |

[12](../12-observability.md) 里的 trace 命令同样是 RECIPE。
