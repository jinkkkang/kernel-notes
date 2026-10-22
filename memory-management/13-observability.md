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

本机快照是宿主视角（这台 box 自己的 `/proc`）。`SecPageTables` 为 0，`balloon_*` 却非 0：气球计数在，二级页表记账没有。两者不矛盾，一个是事件累计，一个是当前存量。

---

[上一节 balloon](./12-balloon-fpr-and-overcommit.md) · [目录](./README.md) · [下一节 排障](./14-perf-and-dev-practice.md)
