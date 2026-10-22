# 13. 观测

**结论**：先读 `/proc/meminfo` 把「谁占着」分成可用、缓存、匿名、slab、页表，再用 `/proc/vmstat` 看分配和回收是不是正在发生，最后用 `/proc/buddyinfo` 看高阶页还在不在。PSI 只回答「有没有人在等内存」，不回答「等的是哪一种」。

> 本节数字来自本机快照 `uploads/host-mm-snapshot.txt`，内核 **6.12.94+**（`uname`：`Linux ... 6.12.94+ #1 SMP PREEMPT_DYNAMIC ... x86_64`）。
> 笔记的结构体和函数行号是 **v7.3** `c03c1a14507d`。两边计数器名字在 v7.3 的 `mm/vmstat.c` 里仍能对上的，下面会点明；对不上的不拿 v7.3 的行号去解释 6.12 的数字。
> 导航：[上一节 balloon](./12-balloon-fpr-and-overcommit.md) · [目录](./README.md) · [下一节 排障](./14-perf-and-dev-practice.md)

`/proc/pagetypeinfo` 和 `/proc/slabinfo` 快照里是 `Permission denied`。下面不编这两份的数字。

---

## 13.1 `/proc/meminfo`

单位是 kB。这台机器约 16GB。

| 字段 | 快照 | 怎么读 |
|---|---|---|
| `MemTotal` | 16386368 | 物理内存减去内核保留之后，memmap 认为有的。不是 DIMM 标称 |
| `MemFree` | 369288 | buddy 里的空闲。约 360MB。偏低不代表马上 OOM |
| `MemAvailable` | 3421272 | 估计还能给新进程的，含可回收的缓存。约 3.3GB。看这个，不看 `MemFree` |
| `Buffers` | 82432 | 块设备元数据缓存 |
| `Cached` | 3120060 | 页缓存 + tmpfs 等。和 `Active(file)+Inactive(file)` 对得上量级 |
| `SwapCached` | 0 | 曾经换出、又换入、仍挂在 swap cache 的。无 swap |
| `Active` / `Inactive` | 2183736 / 2876092 | LRU 上的。file+anon |
| `Active(anon)` / `Inactive(anon)` | 876 / 1925976 | 匿名页几乎都在 inactive。活跃匿名只有约 876kB |
| `Active(file)` / `Inactive(file)` | 2182860 / 950116 | 文件页是大头，而且多数算 active |
| `Unevictable` / `Mlocked` | 0 / 0 | 没有 mlock |
| `SwapTotal` / `SwapFree` | 0 / 0 | **没有 swap**。匿名页换不走 |
| `Zswap` / `Zswapped` | 0 / 0 | 没有 zswap |
| `Dirty` / `Writeback` | 764 / 0 | 很干净。见 [08](./08-page-cache-and-writeback.md) |
| `AnonPages` | 1857444 | 匿名 RSS 约 1.8GB。和 inactive anon 同量级 |
| `Mapped` | 351576 | 映射进页表的文件页 |
| `Shmem` | 69604 | tmpfs、共享内存 |
| `KReclaimable` | 256936 | 可回收的内核内存，主要是 SReclaimable 加内核 misc |
| `Slab` | 338408 | slab 总页，约 330MB |
| `SReclaimable` / `SUnreclaim` | 256936 / 81472 | 可回收（dentry/inode）和不可回收。两者之和等于 `Slab` |
| `KernelStack` | 12724 | 内核栈 |
| `PageTables` | 53524 | 页表约 52MB |
| `SecPageTables` | 0 | 二级页表（KVM EPT 一类）的记账。0 表示没在记，或没有 |
| `Committed_AS` | 5864400 | 已承诺的虚拟内存约 5.6GB |
| `CommitLimit` | 8193184 | 正好是 `MemTotal/2`。和默认 `overcommit_ratio=50`、`SwapTotal=0` 的启发式一致。`overcommit_memory` 本身没在快照里，这个等式只说明当前 CommitLimit 长得像 ratio 50 |
| `VmallocTotal` | 34359738367 | 约 32TB 虚拟窗口，不是用量 |
| `VmallocUsed` | 15984 | 约 16MB |
| `VmallocChunk` | 0 | 这个字段在新内核里经常是 0，不能用来判断碎片 |
| `Percpu` | 3040 | percpu 分配器 |
| `AnonHugePages` 及 `ShmemHugePages` / `FileHugePages` | 全 0 | 没有 THP |
| `HugePages_*` / `Hugetlb` | 0 | 没有 hugetlb 池 |
| `Hugepagesize` | 2048 | 默认大页 2MB，即使池是空的也会打印 |
| `DirectMap4k` / `2M` / `1G` | 18432 / 3127296 / 15728640 | 直接映射拆成的页大小。约 15GB 用 1GB 页，约 3GB 用 2MB，18MB 用 4KB |

`MemAvailable` 远大于 `MemFree`，差的主要是可回收文件页。把缓存当泄漏是读错了字段。

---

## 13.2 `/proc/vmstat`

单位是页，除非名字明显是次数。和 v7.3 `mm/vmstat.c` 对得上的文本名：`pgscan_kswapd`（`:1298`）、`allocstall` 前缀（`:1338`）、`balloon_inflate`（`:1440`）、`compact_stall`（`:1389`）。

### 当前存量（`nr_*`）

| 项 | 值 | 读法 |
|---|---|---|
| `nr_free_pages` | 92322 | 约 360MB，和 `MemFree` 一致 |
| `nr_anon_pages` | 464361 | 约 1.8GB |
| `nr_file_pages` | 800623 | 文件页约 3.1GB |
| `nr_slab_reclaimable` / `unreclaimable` | 64234 / 20368 | 和 meminfo 的 SReclaimable / SUnreclaim 同量级 |
| `nr_dirty` | 191 | 约 764kB |
| `nr_writeback` | 0 | |
| `nr_isolated_anon` / `file` | 0 / 0 | 此刻没有页正被回收隔离 |
| `nr_zone_inactive_anon` / `active_anon` | 481559 / 219 | 匿名几乎全 inactive |
| `nr_zone_inactive_file` / `active_file` | 237529 / 545715 | 文件页多数 active |
| `workingset_refault_file` | 1132283 | 文件页被挤走后又缺页回来。缓存是热的 |
| `workingset_refault_anon` | 0 | 匿名没有 refault。没有 swap 时这也正常 |
| `nr_anon_transparent_hugepages` | 0 | |

### 累计事件

| 项 | 值 | 读法 |
|---|---|---|
| `pgalloc_dma` / `dma32` / `normal` / `movable` | 0 / 12429729 / 21146499 / 0 | 分配落在 DMA32 和 NORMAL。没有 MOVABLE zone 的分配 |
| `allocstall_dma32` / `normal` / `movable` | 0 / 11 / 373 | 进 slowpath 的次数。movable 的 stall 多，但 `pgalloc_movable` 是 0：stall 计数按分配请求的首选 zone 记，页最后可能从别的 zone 给出，或失败。不要把 stall 当成「从 MOVABLE 分到了多少页」 |
| `pgfault` / `pgmajfault` | 22590804 / 8519 | 缺页很多，真正读盘的少 |
| `pgscan_kswapd` / `pgsteal_kswapd` | 2082453 / 1611280 | 后台回收效率大约 1611280/2082453 ≈ 77% |
| `pgscan_direct` / `pgsteal_direct` | 19314 / 18131 | 直接回收次数不多，但扫到的页大部分被偷走 |
| `pgscan_file` / `pgsteal_file` | 2101767 / 1629411 | |
| `pgscan_anon` / `pgsteal_anon` | 0 / 0 | 没扫匿名。无 swap 时回收匿名没有意义 |
| `pgskip_normal` | 492221 | 扫 NORMAL 时跳过。常见原因是该 zone 的水位或隔离状态让扫描放弃 |
| `compact_stall` / `fail` / `success` | 0 / 0 / 0 | 直接压缩没有发生 |
| `compact_daemon_wake` | 72 | kcompactd 被叫醒过 |
| `compact_daemon_migrate_scanned` / `free_scanned` | 640430 / 2197208 | 后台压缩在扫，但 `compact_success` 仍是 0：daemon 的成功不一定记在 `compact_success`（那个计数偏直接压缩）。看 daemon 自己的扫描量，不要用 success 判断 kcompactd 没干活 |
| `thp_*` | 全 0 | 没装过 THP，也没 fallback |
| `oom_kill` | 0 | |
| `balloon_inflate` / `deflate` / `migrate` | 4718592 / 2097152 / 45672 | 历史累计页数。见 [12](./12-balloon-fpr-and-overcommit.md) |
| `pswpin` / `pswpout` | 0 / 0 | |
| `numa_hit` / `numa_miss` | 31818671 / 0 | 单节点，全部命中 |

`kswapd_low_wmark_hit_quickly` 399、`kswapd_high_wmark_hit_quickly` 46：kswapd 经常刚睡又被 LOW 叫醒，偶尔才是因为到了 HIGH 立刻再睡。水位贴着 LOW 走。

---

## 13.3 `/proc/buddyinfo`

每列是一个 order，从 0 到 10（`MAX_PAGE_ORDER` 默认 10，v7.3 `mmzone.h:31`；6.12 同样是这个列数）。数字是**该阶的空闲块个数**，不是页数。页数 = 个数 × 2^order。

```text
Node 0, zone    DMA     0    0    0    0    0    0    0    0    1    1    3
Node 0, zone  DMA32  9372 5976 2779 1021  603  167   93   15    2    1    2
Node 0, zone Normal   996 1103  625  192  173   36   17    5   13   11    0
```

| zone | 读法 |
|---|---|
| DMA | 只有 order 8、9、10 各几块。DMA 的 managed 只有 3840 页（zoneinfo），本来就小 |
| DMA32 | 低阶很多，order 9 还有 1 块（2MB×512=1GB 量级的一块），order 10 有 2 块 |
| Normal | order 10 是 **0**。要 4MB 连续物理页会失败或触发压缩。order 9 还有 11 块，2MB 还有 |

高阶为 0、低阶几千，是外碎片，不是内存用完。`MemFree` 仍有 360MB。

---

## 13.4 `/proc/zoneinfo`（快照只完整留下了 DMA）

DMA 段：

```text
pages free 3840  min 15  low 18  high 21  promo 24
      spanned 4095  present 3998  managed 3840
      protection: (0, 2953, 15983, 15983)
```

`protection` 就是 `lowmem_reserve[]`。DMA 给更高 zone 留的保护是 0（自己就是最低），后面的数字是更高 zone 的分配不能把 DMA 吃进保留区。DMA 的水位只有十几页，因为 managed 才 3840 页。

per-CPU pageset：`count 0, high 0, batch 1`。DMA 太小，PCP 不囤页。

快照在 `Node 0, zone DMA32` 处截断，**没有** DMA32/Normal 的 min/low/high。不要用 DMA 的水位去推 Normal。

`pagetypeinfo`：`Permission denied`。缺了它就不知道 Normal 的 order 10 为 0 是 UNMOVABLE 钉死的，还是只是暂时没有。要看的话用 root 再读，那是另一台操作，本节不补数。

---

## 13.5 PSI memory

`/proc/pressure/memory`：

```text
some avg10=0.00 avg60=0.00 avg300=0.00 total=2919337
full avg10=0.00 avg60=0.00 avg300=0.00 total=2872235
```

`some`：至少一个任务在等内存（直接回收、refault、被 memcg 节流）。`full`：所有非 idle 任务都在等，整机没有进展。

平均值是最近 10/60/300 秒的百分比，此刻全 0。`total` 是累计微秒。约 2.9 秒的历史等待，相对这台机器的 uptime（快照没写 uptime，不推）只能说明「发生过，现在没有」。

PSI 的实现和 cgroup 聚合见 [cgroup/08-psi-rstat.md](../cgroup/08-psi-rstat.md)。全局文件和某个 cgroup 的 `memory.pressure` 不是同一个数。

---

## 13.6 slabinfo 与 memcg `memory.stat`

`/proc/slabinfo`：`Permission denied`。非 root 只能看 meminfo 的 `Slab` 三项。要对象级别的泄漏，需要 root 读 slabinfo，或 `slabtop`，见 [14](./14-perf-and-dev-practice.md)。

本快照**没有**采集 `/sys/fs/cgroup/.../memory.stat`。cgroup 章的观测笔记里有别的时点的 `memory.current`（例如 agent cgroup），不要和这份 meminfo 加在一起当成同一瞬间。

`memory.stat` 在 v2 里把用量拆成 anon、file、kernel、slab、sock 等。字段以 `Documentation/admin-guide/cgroup-v2.rst` 和运行内核的文件为准。读的时候：

- `anon` + `file` 接近用户页。
- `slab` 再分 reclaimable。内核泄漏看 slab 不看 anon。
- `workingset_refault_*` 是这个 memcg 的，不是全局 vmstat 的同名项。全局那份在 `/proc/vmstat`。

---

## 13.7 关键 tracepoint

打开方式是 ftrace 或 bpftrace。本环境没有把这些 tracepoint 跑一遍，命令放在 [14](./14-perf-and-dev-practice.md) 标 RECIPE。名字核对自 v7.3 `include/trace/events/`。

| 事件 | 文件:行 | 什么时候打 |
|---|---|---|
| `mm_page_alloc` | `kmem.h:180` | 页分配完成 |
| `mm_page_free` | `kmem.h:139` | 页释放 |
| `mm_page_alloc_extfrag` | `kmem.h:269` | 分配造成外碎片（steal/claim） |
| `mm_page_pcpu_drain` | `kmem.h:246` | PCP 倒回 buddy |
| `kmem_cache_alloc` / `kmalloc` / `kfree` | `kmem.h:12` / `:57` / `:97` | slab |
| `mm_vmscan_kswapd_wake` / `kswapd_sleep` | `vmscan.h:76` / `:59` | kswapd 醒/睡 |
| `mm_vmscan_wakeup_kswapd` | `:151` | 谁把它叫醒 |
| `mm_vmscan_direct_reclaim_begin` | `vmscan.h:203`（`DEFINE_EVENT`，模板在 `:177`） | 直接回收开始。同模板还有 `mm_vmscan_memcg_reclaim_begin`（`:211`） |
| `mm_vmscan_direct_reclaim_end` | `vmscan.h:249` | 直接回收结束 |
| `mm_vmscan_lru_shrink_inactive` | `vmscan.h:477` | 收缩 inactive |
| `mm_vmscan_node_reclaim_begin` | `vmscan.h:564` | node reclaim |
| `mm_vmscan_lru_isolate` | `:364` | 从 LRU 隔离 |
| `mm_vmscan_write_folio` | `:409` | 回收路径写脏页 |
| `mm_shrink_slab_start` / `end` | `:272` / `:322` | shrinker |
| `mm_compaction_begin` / `end` | `compaction.h:101` / `:131` | 压缩 |
| `mm_compaction_try_to_compact_pages` | `:165` | 直接压缩入口 |
| `mm_compaction_kcompactd_sleep` | `:293` | kcompactd 睡 |
| `mm_filemap_fault` | `filemap.h:114` | 文件缺页 |
| `mm_khugepaged_scan` / `mm_collapse_huge_page` | `huge_memory.h:251` / `:90` | THP 折叠 |
| `balance_dirty_pages` | `writeback.h:657` | 脏页限速 |
| `mark_victim` | `oom.h:76` | OOM 选中进程 |
| `reclaim_retry_zone` | `oom.h:34` | OOM 前再看水位 |
| `kvm_page_fault` | 在 MMU trace 里，`trace_kvm_page_fault` 的调用点见 [11](./11-kvm-memory.md) | EPT/NPF。通用 `include/trace/events/kvm.h` 里没有同名 `TRACE_EVENT(kvm_page_fault)`，定义在 x86 mmutrace，不在那个文件 |

`include/trace/events/kvm.h` 里和内存近的是 `kvm_age_hva`（`:406`）、`kvm_unmap_hva_range`（`:388`）、`kvm_vm_set_mem_attributes`（`:367`）。

---

## 13.8 guest 和宿主对照

同一时刻，两边的数字回答的是不同的问题。

| 问题 | 在 guest 里看 | 在宿主里看 |
|---|---|---|
| guest 还剩多少空闲 | guest `MemFree` / `MemAvailable` | 没有直接对应。qemu RSS 是宿主记账 |
| 宿主是不是把页给了别的虚拟机 | guest 看不出来，除非 balloon 把 `MemTotal` 或可用页压下去 | qemu 进程的 RSS、`memory.stat`、宿主机 `MemAvailable` |
| 气球 | guest `balloon_*` vmstat、`MemTotal` 是否下降 | 宿主侧 qemu 的 balloon 统计（`info balloon`） |
| FPR | guest 空闲页仍在 `MemFree` 里 | 宿主可能已经把这些 HPA 分走。guest `MemFree` 偏乐观 |
| 缺页延迟 | guest `pgmajfault`、PSI | 宿主上 qemu 的 major fault、EPT violation 的 trace |
| 大页 | guest `AnonHugePages` | 宿主 THP/hugetlb。两边都要看，见 [09](./09-thp-and-hugetlb.md) |
| 页表 | guest `PageTables` | 宿主 `SecPageTables`（EPT）+ qemu 自己的 `PageTables` |
| OOM | guest `dmesg` 和 `oom_kill` | 宿主 OOM killer 可能杀的是 qemu 整个进程，guest 里什么都没有 |

本机快照是宿主视角（这台 box 自己的 `/proc`）。`SecPageTables` 为 0，`balloon_*` 非 0：气球计数在，二级页表记账是 0。一个是事件累计，一个是当前存量。

---

## 13.9 pagemap、soft-dirty、idle page

**结论**：工作集估算用三件套。`/proc/<pid>/pagemap` 看这个进程每一页现在是什么；soft-dirty 看两次清零之间谁被写过；`/sys/kernel/mm/page_idle/bitmap` 按 PFN 看谁被碰过。pagemap 和 clear_refs 的文件权限是 `S_IRUSR` / `S_IWUSR`（`fs/proc/base.c:3325`、`:3322`）。kpageflags 是 `S_IRUSR`。page_idle 的 bitmap 是 `0600`。本机快照没采这三份。

### `/proc/<pid>/pagemap`

`pagemap_read`（`fs/proc/task_mmu.c:2304`）。文件偏移和读长度都必须是 `PM_ENTRY_BYTES`（一项 `u64`，`:1951`）的整数倍，否则 `-EINVAL`。注释（`:2287-2292`）和宏（`:1952-1960`）一致：

| 位 | 宏 | 含义 |
|---|---|---|
| 0–54 | `PM_PFRAME_BITS` 55，`PM_PFRAME_MASK` | 存在时是 PFN。在 swap 里时是 swap 文件号和偏移的编码。没映射是 0 |
| 55 | `PM_SOFT_DIRTY` | soft-dirty |
| 56 | `PM_MMAP_EXCLUSIVE` | 独占映射 |
| 57 | `PM_UFFD_WP` | userfaultfd write-protect |
| 58 | `PM_GUARD_REGION` | guard region |
| 59–60 | 无宏 | 注释写 zero |
| 61 | `PM_FILE` | 文件页或共享匿名 |
| 62 | `PM_SWAP` | 在 swap |
| 63 | `PM_PRESENT` | 在内存里 |

`:2327` 的注释写「do not disclose physical addresses」。下一行 `pm.show_pfn = file_ns_capable(file, &init_user_ns, CAP_SYS_ADMIN)`。没有 `CAP_SYS_ADMIN`，PFN 字段不填真地址。高效读法是先看 `/proc/<pid>/maps`，对空洞 `llseek` 跳过（注释 `:2300`）。

`/proc/kpagecount` 和 `/proc/kpageflags` 在 `fs/proc/page.c`：`kpagecount_read` `:128`，`kpageflags_read` `:255`，`proc_create` 在 `:282` 和 `:283`，权限 `S_IRUSR`。kpagecount 是 mapcount，kpageflags 是页标志。和 pagemap 的 PFN 对上之后才能从「这个进程的虚拟页」走到「这张物理页的标志」。

### soft-dirty

`clear_soft_dirty`（`task_mmu.c:1706`）。`pgtable_supports_soft_dirty()` 为假直接返回。PTE 为空返回。存在的 PTE：

```c
if (pte_is_pinned(vma, addr, ptent))   /* :1690：可写、COW、mm 有 pin、folio 可能被 DMA pin */
    return;                            /* pin 住的页跳过，不清 soft-dirty */
old_pte = ptep_modify_prot_start(vma, addr, pte);
ptent = pte_wrprotect(old_pte);        /* 写成只读，下次写再缺页，才能抓住写 */
ptent = pte_clear_soft_dirty(ptent);
ptep_modify_prot_commit(...);
```

不在内存里的 swap PTE 走 `pte_swp_clear_soft_dirty`（`:1732`）。PMD 大页另有 `clear_soft_dirty_pmd`（`:1738`，要 `CONFIG_TRANSPARENT_HUGEPAGE`）。

清零的用户接口是写 `/proc/<pid>/clear_refs`。`clear_refs_write`（`task_mmu.c:1858`）把写入的整数当成 `enum`（`:1678` 起）：`CLEAR_REFS_SOFT_DIRTY` 的值是 4（`:1681`）。写 4 再读 pagemap 的 bit 55，置位的就是这段时间里被写过的页。pin 住的页清不掉，不要把它们算进「没被写」。

### idle page tracking

`/sys/kernel/mm/page_idle/bitmap`。属性组 `page_idle_attr_group`（`mm/page_idle.c:209`），名字 `"page_idle"`，`subsys_initcall`（`:214`）挂到 `mm_kobj`。

写（`page_idle_bitmap_write`，`:163`）：偏移和长度按 `BITMAP_CHUNK_SIZE` 对齐。某个 bit 为 1 时，`page_idle_get_folio` 拿到 folio，`page_idle_clear_pte_refs`（`:97`）用 `rmap_walk`（`:114`，`rmap_one` 是 `page_idle_clear_pte_refs_one`）清掉 PTE 上的 young，再 `folio_set_idle`（`:189`）。

读（`page_idle_bitmap_read`，`:118`）：同一对齐规则。folio 带 idle 标志时，先 `page_idle_clear_pte_refs` 再查一次。`page_idle_clear_pte_refs_one`（`:52`）若发现 PTE/PMD 的 young 被清掉了，就 `folio_clear_idle`（`:86`）并且这一位不置位；young 本来就是 0、idle 标志还在，输出位才置 1（`:151`）。读一个确实 idle 的页不会把它的 idle 标志清掉。folio 拿不到锁时 `page_idle_clear_pte_refs` 直接返回（`:111`），这一轮 young 可能还在。估算是近似的。

要开一个新的观测窗口，再写一遍 bitmap，把关心的 PFN 标回 idle。

---

## 13.10 DAMON

**结论**：DAMON 用一个内核线程周期采样访问，把地址空间收成若干 region，每个 region 有访问计数。用户态从 sysfs 或 debugfs 配间隔和 scheme。本节钉上下文结构，scheme 的动作表（pageout、migrate、`MR_DAMON`）不展开。

`damon_new_ctx`（`mm/damon/core.c:832`）分配一个 `struct damon_ctx`。`damon_start`（`:1974`）对每个 ctx 先做一次 commit 校验（临时 ctx 上 `__damon_commit_ctx`，完了就 `damon_destroy_ctx`），再在 `damon_lock` 下调用 `__damon_start`（`:1935`）。`exclusive` 为真且已经有运行中的 ctx，返回 `-EBUSY`。

```text
__damon_start(ctx)                        # core.c:1935，调用时已持 damon_lock
  kdamond 已在：-EBUSY
  kthread_run(kdamond_fn, ctx, "kdamond.%d", nr_running_ctxs)
  成功：wait_for_completion(kdamond_started)
```

### `struct damon_attrs`（`include/linux/damon.h:809`）

嵌在 `damon_ctx` 开头。时间单位是微秒。

| 成员 | 中文 |
|---|---|
| `unsigned long sample_interval` | 两次采样的间隔。每次看 region 在上一个间隔里有没有被访问 |
| `unsigned long aggr_interval` | 聚合间隔。这段时间里把访问累加到 `damon_region->nr_accesses`，到点清零 |
| `unsigned long ops_update_interval` | 隔这么久检查监控目标要不要更新（虚拟地址监控时，应用 `mmap` 会改区间） |
| `struct damon_intervals_goal intervals_goal` | 自动调 sample/aggr 的目标。`aggrs == 0` 时整结构被忽略 |
| `unsigned long min_nr_regions` | 自适应 region 数下限 |
| `unsigned long max_nr_regions` | 上限 |
| `unsigned long aggr_samples` | 私有。`aggr_interval / sample_interval`。外部调 `damon_set_attrs` 时这个字段可以不填，函数自己算；kdamond 内部再调时保留调用者填的值 |

`struct damon_intervals_goal`（`:735`）：

| 成员 | 中文 |
|---|---|
| `unsigned long access_bp` | 目标访问事件比例，单位 bp（1/10000） |
| `unsigned long aggrs` | 在多少次聚合里达到这个比例。0 表示关闭自动调节 |
| `unsigned long min_sample_us` | 调出来的采样间隔下限 |
| `unsigned long max_sample_us` | 上限 |

### `struct damon_ctx`（`:854`）

头文件把一部分标了 public，其余是 private。全部成员如下。

| 成员 | 可见性 | 中文 |
|---|---|---|
| `struct damon_attrs attrs` | public（结构开头） | 上面那组间隔和 region 数 |
| `unsigned long passed_sample_intervals` | private | 这个 ctx 启动后已经过去多少个采样间隔 |
| `unsigned long next_aggregation_sis` | private | 下次聚合前还要过多少个采样间隔 |
| `unsigned long next_ops_update_sis` | private | 下次更新监控目标前还要过多少个 |
| `unsigned long next_intervals_tune_sis` | private | 下次自动调间隔前还要过多少个 |
| `struct completion kdamond_started` | private | `__damon_start` 等 kdamond 真正跑起来 |
| `unsigned long *regions_score_histogram` | private | scheme quota 排序用的直方图 |
| `struct list_head call_controls` | private | `damon_call_control` 链表。kdamond 之外的线程用 `damon_call()` 把活交进来 |
| `bool call_controls_obsolete` | private | 上面这张表过期了 |
| `struct mutex call_controls_lock` | private | 保护 call_controls |
| `struct damos_walk_control *walk_control` | private | `damos_walk()` 的控制块 |
| `bool walk_control_obsolete` | private | walk 控制块过期 |
| `struct mutex walk_control_lock` | private | 保护 walk_control |
| `bool maybe_corrupted` | private | 注释写目前只在 `damon_commit_ctx()` 失败时置位 |
| `struct task_struct *kdamond` | private | 这个 ctx 的内核线程。NULL 表示没在跑 |
| `struct mutex kdamond_lock` | private | 保护 `kdamond` |
| `unsigned long addr_unit` | public | core 和 ops 之间的地址换算比例 |
| `unsigned long min_region_sz` | public | region 最小尺寸 |
| `bool pause` | public | 让 kdamond 主循环暂停 |
| `struct damon_operations ops` | private | 这套用例的采样 / 目标有效性回调 |
| `struct list_head adaptive_targets` | private | `damon_target` 链表 |
| `struct list_head probes` | private | `damon_probe` 链表 |
| `struct list_head schemes` | private | `damos` scheme 链表 |
| `struct rnd_state rnd_state` | private | `damon_rand()` 用的 per-ctx PRNG 状态 |

kdamond 跑起来之后，别的线程碰这个 ctx 要走 `damon_call()` 或 `damos_walk()`。直接改链表会和采样线程赛跑。线程在所有 target 都无效，或有人 `damon_stop()` 时退出。pid 用 `damon_kdamond_pid()` 取。

---

## 13.11 shrinker debugfs

**结论**：每个 shrinker 在 `/sys/kernel/debug/shrinker/<名字>-<id>/` 下有 `count` 和 `scan`。`count` 只读，问这个 shrinker 还有多少对象。`scan` 只写，让它立刻扫一批。用来确认「slab 回收不动」是 count 就是 0，还是 scan 了但扫不动。

根目录由 `shrinker_debugfs_init`（`mm/shrinker_debug.c:247`）创建：`debugfs_create_dir("shrinker", NULL)` 在 `:253`，dentry 存 `shrinker_debugfs_root`（`:16`）。每个 shrinker 注册时 `shrinker_debugfs_add`（`:159`）用 ida 拿 `debugfs_id`，目录名 `"%s-%d"`（名字加 id，`:176`）。init 末尾持 `shrinker_mutex` 把启动时已经挂上、还没有 dentry 的 shrinker 补登记（`:260`）。

| 文件 | 权限 | 实现 |
|---|---|---|
| `count` | `0440` | `shrinker_debugfs_count_show`（`:47`）。有 `count_objects` 才创建（`:186`） |
| `scan` | `0220` | `shrinker_debugfs_scan_write`（`:100`）。有 `scan_objects` 才创建（`:189`） |

`count` 的输出：对每个在线 memcg（shrinker 不感知 memcg 时只打 id 0 那一行）一行，先是 memcg id，再每个 node 的对象数。`count_objects` 返回 `SHRINK_EMPTY` 时记 0（`:34`）。非 NUMA-aware 的 shrinker 只问 node 0（`:26`）。

`scan` 的写格式是三个数：`memcg_id nid nr_to_scan`（`sscanf`，`:119`）。`nr_to_scan == 0` 直接返回。不感知 memcg 时 id 必须是 0，否则 `-EINVAL`。感知时用 `mem_cgroup_get_from_id`，memcg 不在线返回 `-ENOENT`。然后填 `shrink_control` 调 `shrinker->scan_objects`（`:146`）。

debugfs 还没挂上时 `shrinker_debugfs_add` 直接返回 0（`:168`），shrinker 先记在 `shrinker_list` 里，等根目录出现再补。本机非 root 读不到 debugfs，这一节是配方，不是本机实测。

---

## 13.12 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `pagemap_read` | `fs/proc/task_mmu.c:2304` | 读 `/proc/<pid>/pagemap` |
| `clear_soft_dirty` | `:1706` | 清 PTE soft-dirty 并写保护 |
| `clear_soft_dirty_pmd` | `:1738` | PMD 大页的同一件事 |
| `kpageflags_read` | `fs/proc/page.c:255` | `/proc/kpageflags` |
| `kpagecount_read` | `:128` | `/proc/kpagecount` |
| `page_idle_bitmap_read` | `mm/page_idle.c:118` | 读 idle bitmap |
| `page_idle_bitmap_write` | `:163` | 把 PFN 标成 idle |
| `page_idle_clear_pte_refs` | `:97` | 标 idle 前清 PTE young |
| `damon_new_ctx` | `mm/damon/core.c:832` | 分配 `damon_ctx` |
| `damon_start` | `:1974` | 启动一组 kdamond |
| `__damon_start` | `:1935` | `kthread_run(kdamond_fn)` |
| `shrinker_debugfs_count_show` | `mm/shrinker_debug.c:47` | debugfs `count` |
| `shrinker_debugfs_scan_write` | `:100` | debugfs `scan` |
| `shrinker_debugfs_add` | `:159` | 给一个 shrinker 建目录 |

---

[上一节 balloon](./12-balloon-fpr-and-overcommit.md) · [目录](./README.md) · [下一节 排障](./14-perf-and-dev-practice.md)
