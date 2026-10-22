# 内存管理大章

> 内核版本：**v7.3**（笔记钉 `c03c1a14507d`；`Makefile` 为 `VERSION=7`、`PATCHLEVEL=3`、`SUBLEVEL=0`）。下文路径、函数、行号均对照该树核对。查不到的符号写成「查不到」，不编。
> 在线对照：[elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)
> 本机/云端 box 观测快照内核是 **6.12.94+**（见 [13](./13-observability.md)）。数字是实测；结构体与调用链以 v7.3 为准。两套内核不要混成一个行号。

**一句话**：物理页从 buddy/SLUB 出来，虚拟地址由 `mm_struct` + VMA + 缺页填进页表；回收、THP、KVM 二维页表、balloon/FPR 都是在这两条线上加策略。

---

## 目录

1. [总览](./01-overview.md)
2. [物理布局：page / folio / node / zone / sparsemem](./02-physical-layout.md)
3. [buddy 与页分配](./03-buddy.md)
4. [SLUB](./04-slab.md)
5. [vmalloc 与内核虚拟地址](./05-vmalloc-and-kernel-va.md)
6. [mm 与 VMA](./06-mm-and-vma.md)
7. [缺页](./07-page-fault.md)
8. [页缓存与回写（只写内存侧）](./08-page-cache-and-writeback.md)
9. [THP 与 hugetlb](./09-thp-and-hugetlb.md)
10. [回收、swap、OOM](./10-reclaim.md)
11. [KVM 内存](./11-kvm-memory.md)
12. [balloon、FPR 与超卖](./12-balloon-fpr-and-overcommit.md)
13. [观测](./13-observability.md)
14. [按症状排障](./14-perf-and-dev-practice.md)

## 建议阅读顺序

```text
01 总览
  → 02 物理布局（page/zone/node，后面所有章的名词都在这）
  → 03 buddy → 04 SLUB → 05 vmalloc
  → 06 mm/VMA → 07 缺页 → 08 页缓存
  → 09 THP/hugetlb
  → 10 回收
  → 11 KVM 二维页表 → 12 balloon/FPR
  → 13 观测 → 14 症状剧本
```

先物理后虚拟。没搞清 `folio` 和 `zone`，缺页和回收都会读成魔法。KVM 章放在缺页之后：EPT violation 是「guest 缺页」和「宿主缺页」叠在一起。

## 与其它章的边界

| 本章覆盖 | 留给其它章 |
|---|---|
| buddy、SLUB、vmalloc、缺页、回收、THP、KVM MMU、balloon/FPR | cgroup memory 的 charge / high / max 接口（[cgroup/06-memory.md](../cgroup/06-memory.md)） |
| memcg 回收在 `shrink_node` 里怎么被叫到 | `mem_cgroup` 对象与 `page_counter` 细节，不在本章重复大段 |
| 页缓存 folio、脏页对回收的压力 | 完整 VFS：inode、dentry、具体文件系统 `a_ops`（以后 `vfs/`） |
| `mmap` / `brk` 的地址空间结果 | `exec` 装载 ELF（[process-management/06-exec.md](../process-management/06-exec.md)） |
| kswapd、直接回收对延迟的影响 | 调度器选谁跑（EEVDF，`scheduler/` 另章；本章只讲内存路径把任务弄睡） |
| PSI memory 数字怎么读 | PSI 聚合实现（[cgroup/08-psi-rstat.md](../cgroup/08-psi-rstat.md)） |

## 可选 demo

[demos/README.md](./demos/README.md)：用户态 `mmap` / smaps / 触发缺页；一个自定义 `kmem_cache` 的模块示例源码（不在本机 insmod）。

## 实测 vs RECIPE

- **实测**：附件快照里已经采到的 `/proc/meminfo`、`vmstat`、`buddyinfo`、`zoneinfo`（DMA 段）、PSI memory。`/proc/pagetypeinfo` 与 `/proc/slabinfo` 当时 `Permission denied`，数字写「采不到」，不编。
- **RECIPE**：perf、bpftrace、KVM nested、balloon inflate/deflate 操作、kmemleak。本环境没有对应权限或没有虚拟化设备，步骤只作配方，标 RECIPE，并写明为什么没跑。

## 未覆盖

刻意不展开：

- DAMON、memory tier、CXL 拓扑的完整策略
- HMM / ZONE_DEVICE / GPU 迁移
- CMA、hugetlb vmemmap 优化（HVO）的位级实现
- swap 槽位、swap cache 的全部状态机（只讲和回收的交界）
- IOMMU、secretmem、mseal 的完整语义
- 具体文件系统的 `address_space_operations`
- 内核 percpu 分配器（`mm/percpu.c`）内部

**前置**：进程章的 `mm` 指针、cgroup 章的 memory 控制器。本章假设你能读 `container_of` 和 `list_head`。
