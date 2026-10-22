# 9. THP 与 hugetlb

**结论**：THP 是把普通匿名页或文件页在缺页或 `khugepaged` 里合成更大的 folio（常见 2MB，也就是 PMD）。hugetlb 是预先留出来的另一种大页，有自己的池子和缺页函数，不进普通 buddy。数据库和 DPDK 要的通常是 hugetlb 或明确的 `madvise`，不是全局 `always`。

> THP：`mm/huge_memory.c`、`mm/khugepaged.c`、`include/linux/huge_mm.h`。
> hugetlb：`mm/hugetlb.c`。
> 导航：[上一节 页缓存](./08-page-cache-and-writeback.md) · [目录](./README.md) · [下一节 回收](./10-reclaim.md)

---

## 9.1 三种「大页」不要混

| | THP（PMD，2MB） | mTHP | hugetlb |
|---|---|---|---|
| 谁分配 | 缺页 `create_huge_pmd`（`memory.c:6379`）或 `khugepaged` | 更小阶的透明大页，sysfs 按 order 开 | `alloc_hugetlb_folio`（`hugetlb.c:2950`），从预留池 |
| 失败了 | `VM_FAULT_FALLBACK`，退回 4KB。用户无感 | 同样可退回 | 池子空了就是失败，`mmap` 或缺页得到错误，不会偷偷改成 4KB |
| 回收 | 可以拆、可以整个回收 | 同 THP | 不进普通 LRU。内存压力不会把 hugetlb 页拿去给别的进程 |
| 看计数 | `AnonHugePages`、`/proc/vmstat` 的 `thp_*` | sysfs `stats` 按 order | `HugePages_*`、`Hugetlb` |

mTHP 是同一套透明大页机制按 order 拆开的控制面，不是第三种分配器。v7.3 的 sysfs 在 `mm/huge_memory.c` 里按 `thpsize` 建组（`sysfs_add_group` 出现在 `:935` 一带）。某个 order 在不在，看 `/sys/kernel/mm/transparent_hugepage/` 下面实际有哪些目录，不要凭旧文档背 `hugepages-64kB` 一定存在。

---

## 9.2 缺页路径上的 THP

[07](./07-page-fault.md) 里 `__handle_mm_fault` 在 PMD 为空且 `thp_vma_allowable_order(..., PMD_ORDER)` 时调用 `create_huge_pmd`。装不成就 `VM_FAULT_FALLBACK`，继续 PTE。

允许与否看 VMA 标志和全局策略：

- `VM_HUGEPAGE`（`madvise(MADV_HUGEPAGE)`）：即使全局是 `madvise` 模式也尝试。
- `VM_NOHUGEPAGE`：不尝试。
- 全局 `always`：匿名 VMA 默认尝试。
- 文件 THP 还要文件系统和挂载选项配合。读侧大页和写侧不是一回事。

`khugepaged`（线程函数 `khugepaged`，`khugepaged.c:3039`）在后台扫 `MADV_HUGEPAGE` 的 VMA，把已经分散的 4KB 页折叠成 2MB。它分配失败记在 `thp_collapse_alloc_failed`。折叠会拿 `mmap_lock` 写锁，这是延迟毛刺的来源，不是缺页本身。

相关 tracepoint（`include/trace/events/huge_memory.h`）：`mm_khugepaged_scan_pmd`（`:56`）、`mm_collapse_huge_page`（`:90`）、`mm_khugepaged_scan`（`:251`）。

拆分：`shrinker` 或 `madvise(MADV_NOHUGEPAGE)`、或者页缓存截断，把大 folio 拆成小页。`_deferred_list`（folio 第三页槽，见 [02](./02-physical-layout.md)）是延迟拆分队列。`thp_split_pmd`、`thp_deferred_split_page` 在 vmstat 里。

---

## 9.3 sysfs 与 vmstat

全局开关（名字以 v7.3 `Documentation/admin-guide/mm/transhuge.rst` 为准，目录在运行中的内核上确认）：

```text
/sys/kernel/mm/transparent_hugepage/enabled       # always / madvise / never
/sys/kernel/mm/transparent_hugepage/defrag        # 缺页时允不允许同步压缩
/sys/kernel/mm/transparent_hugepage/khugepaged/   # 扫描间隔、分配时的水位
/sys/kernel/mm/transparent_hugepage/hugepages-*/  # mTHP：按 order 的 enabled
```

`defrag` 设成 `always` 时，缺页可以在分配者上下文里 compact。这是 THP 延迟尖刺的经典原因。`defer` 或 `madvise` 把压缩留给 kcompactd 或只对 `MADV_HUGEPAGE` 同步做。

hugetlb：

```text
/proc/sys/vm/nr_hugepages
/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# 挂载 hugetlbfs 后应用 mmap
mount -t hugetlbfs none /mnt/huge
```

预留要在内存还连续的时候做。机器跑久了，`nr_hugepages` 写下去可能只得到一部分，`/proc/meminfo` 的 `HugePages_Total` 小于请求值。先 compact 或重启再预留，比在线上硬写靠谱。

本机 6.12.94+ 快照：`AnonHugePages 0`、`HugePages_Total 0`、`thp_fault_alloc 0`、`thp_collapse_alloc 0`。这台 box 上 THP 没有成功装过，也没有 hugetlb 池。解释计数时以这组 0 为准，不要假设云主机默认 `always`。

`thp_fault_fallback` 也是 0。fallback 为 0 且 alloc 为 0，通常是根本没走 THP（策略是 `never`，或没有合适的 VMA），不是「尝试了但全失败」。失败会让 `thp_fault_fallback` 增加。

---

## 9.4 数据库 / DPDK 怎么选

| 需求 | 用什么 | 原因 |
|---|---|---|
| 延迟稳定，页要钉死 | hugetlb，启动时预留，应用 `mmap` hugetlbfs 或 `MAP_HUGETLB` | 不进 LRU，不被 khugepaged 事后折叠，也不被别的进程偷走 |
| 大块匿名内存，能接受偶尔退回 4KB | `madvise(MADV_HUGEPAGE)`，全局保持 `madvise` | 只有这一段尝试大页，不让无关进程在缺页里 compact |
| 包处理要物理连续的大页给 DMA / 二级页表 | hugetlb 2MB 或 1GB | 1GB 要内核命令行预留，运行时往往留不出 |
| 怕 stall | 不要全局 `enabled=always` 加 `defrag=always` | 缺页同步 compact 的延迟是毫秒到几十毫秒级，和业务抖动对得上 |

DPDK 的 `virtio-user` / 物理网卡驱动要的是宿主机大页。guest 里再开 THP 不能代替宿主把 virtio ring 放进大页。guest 看见的「物理连续」还要经过 EPT，宿主如果用 4KB 映射，guest 的 2MB 大页在 EPT 里仍是 512 个 4KB。见 [11](./11-kvm-memory.md) 和 [12](./12-balloon-fpr-and-overcommit.md)。

`mlock` 不会把 4KB 页合成大页。它只是把已经有的页锁在内存里。

---

## 9.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `transparent_hugepage_flags` | `mm/huge_memory.c:61` | 全局 THP 标志位 |
| `create_huge_pmd` | `mm/memory.c:6379` | 缺页装 PMD |
| `khugepaged` | `mm/khugepaged.c:3039` | 后台折叠线程 |
| `khugepaged_enter_vma` | `khugepaged.c:588` | VMA 加入扫描 |
| `alloc_hugetlb_folio` | `mm/hugetlb.c:2950` | 从 hugetlb 池拿 folio |
| `hugetlb_fault` | `mm/hugetlb.c:6013` | hugetlb 缺页 |
| `mm_get_huge_zero_folio` | `include/linux/huge_mm.h:575` | THP 用的零页 |

---

[上一节 页缓存](./08-page-cache-and-writeback.md) · [目录](./README.md) · [下一节 回收](./10-reclaim.md)
