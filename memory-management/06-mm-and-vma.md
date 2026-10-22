# 6. `mm_struct` 与 `vm_area_struct`

**结论**：一个进程（或共享地址空间的线程组）有一个 `mm_struct`。它的 VMA 不放链表里，放在 maple tree `mm_mt`。`mmap` / `brk` 改的是这棵树和 VMA 的标志；真正的物理页要等缺页或预读才出现。

> 定义：`include/linux/mm_types.h`。`mm_struct` 从 `:1175`，`vm_area_struct` 从 `:923`。
> 路径：`mm/mmap.c`、`mm/vma.c`。
> 导航：[上一节 vmalloc](./05-vmalloc-and-kernel-va.md) · [目录](./README.md) · [下一节 缺页](./07-page-fault.md)

内核线程的 `task_struct.mm` 是 NULL，`active_mm` 借用上一个用户 mm（lazy TLB）。见进程章 [02-task-struct](../process-management/02-task-struct.md)。

---

## 6.1 `struct mm_struct` 全部成员（`:1175`）

几乎所有字段包在一个匿名 `struct { ... } __randomize_layout` 里。最后一项 `flexible_array` 必须留在结构体末尾：`mm_cpumask` 按 `nr_cpu_ids` 动态加长（注释 `:1424`）。

### 引用与地址空间骨架

| 成员 | 条件 | 中文 |
|---|---|---|
| `atomic_t mm_count` | 始终，单独 cache line | 「有多少东西抓着这个 mm」。`mm_users` 整体算 1。用 `mmgrab` / `mmdrop`。减到 0 就释放 mm |
| `struct maple_tree mm_mt` | 始终 | **VMA 的存储**。按虚拟地址索引。老的 `mm->mmap` 链表在 v7.3 的这个结构里已经不在 |
| `unsigned long mmap_base` | 始终 | mmap 区基址（自顶向下时的起点） |
| `unsigned long mmap_legacy_base` | 始终 | 自底向上布局的 mmap 基址 |
| `unsigned long mmap_compat_base` | `CONFIG_HAVE_ARCH_COMPAT_MMAP_BASES` | 32 位兼容进程的 mmap 基址 |
| `unsigned long mmap_compat_legacy_base` | 同上 | 兼容进程的 legacy 基址 |
| `unsigned long task_size` | 始终 | 用户空间大小。x86-64 4 级页表约 128TB，和文档布局一致 |
| `pgd_t *pgd` | 始终 | 这套地址空间的顶级页表 |
| `atomic_t membarrier_state` | `CONFIG_MEMBARRIER` | membarrier 标志。故意靠近 `pgd`，因为 `switch_mm` 会同时碰它们 |
| `atomic_t mm_users` | 始终 | 用户侧引用：活着的线程、以及临时 `mmget`。减到 0 时再 `mmdrop` 一次 `mm_count` |
| `struct mm_mm_cid mm_cid` | 始终 | 每 mm 的并发 ID 存储，调度器用 |
| `struct sched_cache_group *sched_cache_grp` | 始终 | sched_cache 统计 |
| `atomic_long_t pgtables_bytes` | `CONFIG_MMU` | 本 mm 页表占的字节。`/proc/meminfo` 的 `PageTables` 是全局的，这里是 per-mm |
| `int map_count` | 始终 | VMA 个数。`/proc/pid/maps` 的行数，也受 `vm.max_map_count` 限制 |
| `spinlock_t page_table_lock` | 始终 | 保护页表和一部分计数 |
| `struct rw_semaphore mmap_lock` | 始终 | 地址空间的大锁。读锁给缺页和查找，写锁给 mmap/munmap。注释特意把它放在这个偏移，让 `count` 和 `owner` 落在不同 cache line |
| `struct list_head mmlist` | 始终 | 可能换出过的 mm 串在 `init_mm.mmlist` 上，`mmlist_lock` 保护 |

### per-VMA 锁与统计

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct rcuwait vma_writer_wait` | `CONFIG_PER_VMA_LOCK` | 写者等 per-VMA 读者退出 |
| `seqcount_t mm_lock_seq` | `CONFIG_PER_VMA_LOCK` | 每次 `mmap_lock` 写加锁/解锁都加。奇数表示写锁正被持有。和 `vma->vm_lock_seq` 比较，判断 VMA 锁是不是写锁 |
| `struct futex_mm_data futex` | 始终 | 本 mm 的 futex 私有哈希 |
| `unsigned long hiwater_rss` | 始终 | RSS 高水位（页） |
| `unsigned long hiwater_vm` | 始终 | 虚拟地址高水位 |
| `unsigned long total_vm` | 始终 | 映射的总页数。`maps` 里各 VMA 长度之和 |
| `unsigned long locked_vm` | 始终 | `mlock` 的页 |
| `atomic64_t pinned_vm` | 始终 | 长期 pin（RDMA、vfio）的页。引用被永久加过 |
| `unsigned long data_vm` | 始终 | `VM_WRITE & ~VM_SHARED & ~VM_STACK` |
| `unsigned long exec_vm` | 始终 | `VM_EXEC & ~VM_WRITE & ~VM_STACK` |
| `unsigned long stack_vm` | 始终 | `VM_STACK` |
| `vm_flags_t def_flags` | 与下一字段同 union | 新 VMA 的默认标志。注释说这是 VMA 标志迁移期的临时布局 |
| `vma_flags_t def_vma_flags` | 同 union | 新类型的默认标志 |
| `seqcount_t write_protect_seq` | 始终 | fork 复制页表等路径把映射写成只读、强迫稍后 COW 时持有 |
| `spinlock_t arg_lock` | 始终 | 保护下面这组起止地址 |

### 段、auxv、RSS

| 成员 | 条件 | 中文 |
|---|---|---|
| `start_code` / `end_code` / `start_data` / `end_data` | 始终 | 代码段、数据段。`/proc/pid/stat` 用 |
| `start_brk` / `brk` / `start_stack` | 始终 | 堆的起点、当前 brk、栈起点 |
| `arg_start` / `arg_end` / `env_start` / `env_end` | 始终 | argv、envp |
| `unsigned long saved_auxv[AT_VECTOR_SIZE]` | 始终 | ELF 辅助向量，给 `/proc/pid/auxv` |
| `unsigned long saved_e_flags` | `CONFIG_ARCH_HAS_ELF_CORE_EFLAGS` | ELF 头里的 ABI 标志，写 core dump |
| `struct percpu_counter rss_stat[NR_MM_COUNTERS]` | 始终 | anon / file / shmem 等 RSS。`/proc/pid/status` 的 `RssAnon`、`RssFile`、`RssShmem` |
| `struct linux_binfmt *binfmt` | 始终 | 装载这个进程的 binfmt（ELF 等） |
| `mm_context_t context` | 始终 | 架构私有。x86 上有 LDT、LAM、pkey 之类 |
| `mm_flags_t flags` | 始终 | mm 标志。必须用 `mm_flags_*` 访问，注释写明了 |

### 外围子系统挂在 mm 上的钩子

| 成员 | 条件 | 中文 |
|---|---|---|
| `spinlock_t ioctx_lock` | `CONFIG_AIO` | 保护 AIO 上下文表 |
| `struct kioctx_table __rcu *ioctx_table` | `CONFIG_AIO` | AIO |
| `struct task_struct __rcu *owner` | `CONFIG_MEMCG` | 这个 mm 的「户主」，memcg 归属看它。改它的条件写在注释里：当前必须是 owner、当前的 mm 已经不是这个 mm、新 owner 的 mm 是它、并且拿着新 owner 的 `alloc_lock` |
| `struct file __rcu *exe_file` | 始终 | `/proc/pid/exe` |
| `struct mmu_notifier_subscriptions *notifier_subscriptions` | `CONFIG_MMU_NOTIFIER` | KVM、RDMA 等二级页表的失效通知 |
| `pgtable_t pmd_huge_pte` | THP 且没有 `CONFIG_SPLIT_PMD_PTLOCKS` | 拆 PMD 时暂存的 PTE 页。`page_table_lock` 保护 |
| `unsigned long numa_next_scan` | `CONFIG_NUMA_BALANCING` | 下次把 PTE 改成 PROT_NONE 以触发 NUMA hint fault 的时间 |
| `unsigned long numa_scan_offset` | 同上 | 扫描续点 |
| `int numa_scan_seq` | 同上 | 防止两个线程同时改 PROT_NONE |
| `atomic_t tlb_flush_pending` | 始终 | 有一批 TLB 刷新正在进行。迁移 PROT_NONE 页时必须先刷 |
| `atomic_t tlb_flush_batched` | `CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH` | 批量 unmap TLB。见 `flush_tlb_batched_pending()` |
| `struct uprobes_state uprobes_state` | 始终 | uprobe |
| `struct rcu_head delayed_drop` | `CONFIG_PREEMPT_RT` | RT 上延迟 mmdrop |
| `atomic_long_t hugetlb_usage` | `CONFIG_HUGETLB_PAGE` | 本 mm 的 hugetlb 用量 |
| `struct work_struct async_put_work` | 始终 | 异步 `mmput` |
| `struct iommu_mm_data *iommu_mm` | `CONFIG_IOMMU_MM_DATA` | IOMMU 与这个 mm 的绑定 |
| `unsigned long ksm_merging_pages` | `CONFIG_KSM` | 正在被 KSM 合并的页，不含 zero page |
| `unsigned long ksm_rmap_items` | `CONFIG_KSM` | 检查过的 rmap 项，含没合并成的 |
| `atomic_long_t ksm_zero_pages` | `CONFIG_KSM` | 和内核零页合并的空页（`use_zero_pages`） |
| `struct { ... } lru_gen` | `CONFIG_LRU_GEN_WALKS_MMU` | 见下 |
| `mm_id_t mm_id` | `CONFIG_MM_ID` | 稳定的 mm 标识，folio 的 `_mm_id[]` 用它 |
| `char flexible_array[]` | 始终，在匿名结构体外 | 末尾柔性数组，实际放 `mm_cpumask` |

`lru_gen` 内嵌结构（`:1404`）的成员：

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct list_head list` | 始终 | 这个 mm 挂在 `lru_gen_mm_list` 上，MGLRU 走页表时遍历 |
| `unsigned long bitmap` | 始终 | 切到这个 mm 时置位，提示页表扫描者「自从上次清位以后它被用过」 |
| `struct mem_cgroup *memcg` | `CONFIG_MEMCG` | 指向 owner 的 memcg |

---

## 6.2 `struct vm_area_struct` 全部成员（`:923`）

第一 cache line 给查树用：起止地址和 `vm_mm`。

| 成员 | 条件 | 中文 |
|---|---|---|
| `unsigned long vm_start` | 与 `vm_freeptr` 同 union | VMA 覆盖 `[vm_start, vm_end)` |
| `unsigned long vm_end` | 同上 | 终点，开区间 |
| `freeptr_t vm_freeptr` | 同 union | `SLAB_TYPESAFE_BY_RCU` 回收 VMA 对象时用的空闲指针。活着的 VMA 不使用它 |
| `struct mm_struct *vm_mm` | 始终 | 属于哪个地址空间。RCU 不稳定读者可以读这个字段 |
| `pgprot_t vm_page_prot` | 始终 | 这一段的页保护。由 `vm_flags` 算出来，缺页建 PTE 时用 |
| `const vm_flags_t vm_flags` | 与 `flags` 同 union | `VM_*`。注释要求用 `vm_flags_set/clear/mod` 或 `vma_flags_*` 改，不要直接写 |
| `vma_flags_t flags` | 同 union | 标志类型迁移期的另一视图 |
| `unsigned int vm_lock_seq` | `CONFIG_PER_VMA_LOCK` | 和 `mm->mm_lock_seq` 配对。写它要同时持有 `mmap_lock` 写锁，并且 `vm_refcnt` 处于排除读者的状态。允许溢出 |
| `unsigned int __vm_anon_pgoff_lo` | 始终 | 匿名页偏移的低 32 位。见 `vma_start_anon_pgoff()` |
| `struct list_head anon_vma_chain` | 始终 | 链到 `anon_vma`。`mmap_lock` 和 `page_table_lock` 一起序列化 |
| `struct anon_vma *anon_vma` | 始终 | 匿名页反向映射的根。文件私有映射在 COW 之后也会有。`MAP_SHARED` 文件映射只有 `i_mmap`，没有 anon_vma |
| `const struct vm_operations_struct *vm_ops` | 始终 | `fault` / `huge_fault` / `map_pages` 等。匿名映射可以是 NULL |
| `unsigned long vm_pgoff` | 始终 | 相对 `vm_file` 的页偏移 |
| `struct file *vm_file` | 始终 | 映射的文件。匿名为 NULL |
| `void *vm_private_data` | 始终 | 驱动私有。注释说以前叫 `vm_pte` |
| `atomic_long_t swap_readahead_info` | `CONFIG_SWAP` | swap 预读游标 |
| `struct vm_region *vm_region` | `!CONFIG_MMU` | NOMMU 的区域。有 MMU 的 x86 没有这个字段 |
| `struct mempolicy *vm_policy` | `CONFIG_NUMA` | 这个 VMA 的 NUMA 策略 |
| `struct vma_numab_state *numab_state` | `CONFIG_NUMA_BALANCING` | NUMA balancing 状态 |
| `refcount_t vm_refcnt` | `CONFIG_PER_VMA_LOCK`，cache line 对齐 | 0 表示已脱离，读者不能再加。1 表示挂着且没上读锁（或写锁）。大于 1 是读者数量。`VM_REFCNT_EXCLUDE_READERS_FLAG` 及以上表示正在踢读者，禁止新的读者进入。RCU 读者可以读这个字段。完整状态机在源码注释 `:1007-1044` |
| `struct lockdep_map vmlock_dep_map` | `CONFIG_PER_VMA_LOCK` 且 `CONFIG_DEBUG_LOCK_ALLOC` | lockdep |
| `unsigned int __vm_anon_pgoff_hi` | `CONFIG_64BIT` | 匿名页偏移的高 32 位 |
| `struct rb_node rb` | 嵌在 `shared` 里 | 链进 `address_space->i_mmap` 区间树 |
| `unsigned long rb_subtree_last` | 嵌在 `shared` 里 | 区间树子树的最大结尾 |
| `struct anon_vma_name *anon_name` | `CONFIG_ANON_VMA_NAME` | `[anon:name]`。`mmap_lock` 序列化，用 `anon_vma_name()` 读 |
| `struct vm_userfaultfd_ctx vm_userfaultfd_ctx` | 始终 | userfaultfd 上下文。没注册时是空 |
| `struct pfnmap_track_ctx *pfnmap_track_ctx` | `__HAVE_PFNMAP_TRACKING` | PFN map 跟踪 |

`shared` 这个匿名结构是文件映射进 `i_mmap` 的挂钩。匿名 VMA 的 `vm_file` 为 NULL，一般不进这棵树。

---

## 6.3 maple tree 里的 VMA

`mm->mm_mt` 的 key 是虚拟地址，value 是 `vm_area_struct *`。查找用 `vma_lookup`（`mm.h:4353`）、`find_vma_intersection`（`:4342`）、`lock_mm_and_find_vma`（`:3278`）。

写树要拿 `mmap_lock` 写锁，或者走 vma 迭代器 `struct vma_iterator`（`vma_iter_*`，`mm.h:1665` 一带）。缺页快路径在 `CONFIG_PER_VMA_LOCK` 下可以只锁单个 VMA（`lock_vma_under_rcu`，`mmap_lock.h:473`），树本身用 RCU 读。

`map_count` 和树里的节点数应该一致。`vm.max_map_count` 默认 65530。进程把文件切成几十万个 VMA 时，`mmap` 返回 `ENOMEM`，不是物理内存不够。

---

## 6.4 `mmap` / `munmap` / `brk`

```text
ksys_mmap_pgoff                         # mm/mmap.c:581
  → vm_mmap_pgoff                       # mm/util.c:565
       → do_mmap                        # mm/mmap.c:338
            检查地址、长度、MAP_* 、rlimit
            文件映射问 f_op->mmap
            get_unmapped_area            # 找洞
            mmap_region                  # 建或扩展 VMA，插入 mm_mt
            能合并就 vma_merge_new_range # mm/vma.c:1140

munmap
  → do_munmap                           # mm/mmap.c:1077
       → do_vmi_align_munmap            # mm/vma.c:1663
            拆 VMA、从 maple tree 删除、unmap 页表、释放 folio

brk                                     # SYSCALL_DEFINE1 mm/mmap.c:117
  → do_brk_flags                        # mm/vma.c:3013
       扩大：扩展或新建匿名 VMA
       缩小：拆掉尾部，unmap
```

`do_mmap` 不分配匿名页。`MAP_POPULATE` 才会在返回前碰页。普通 `malloc` / `mmap(ANON)` 只是记下一笔「这段地址将来可以缺页」。

`brk` 是堆。glibc 的小分配走 brk，大分配直接 mmap。两者在 `mm` 里都是 VMA，缺页路径一样。`start_brk` 是起点，`brk` 是当前终点。

合并条件：相邻、同一 `vm_file`、同一标志、`vm_pgoff` 连续、同一 anon_vma / 策略。合并不了，`map_count` 就涨。

---

## 6.5 `/proc/pid/maps` 和 `smaps` 怎么读

`maps` 一行：

```text
start-end perm offset dev inode pathname
```

`perm` 里的 `p` 是私有（`MAP_PRIVATE`），`s` 是共享。pathname 为空、或 `[heap]`、`[stack]`、`[anon:...]` 都是匿名。

`smaps` 比 `maps` 多这些和内存有关的字段：

| 字段 | 含义 |
|---|---|
| `Size` | VMA 长度，不是 RSS |
| `Rss` | 已经在页表里的页 |
| `Pss` | 按映射次数分摊后的 RSS。共享库会被多个进程分 |
| `Shared_Clean/Dirty` | 多个 PTE 指着的页 |
| `Private_Clean/Dirty` | 只有这个进程映射 |
| `Anonymous` | 匿名 folio，含 COW 之后不再和文件共享的页 |
| `LazyFree` | `MADV_FREE`，还在，但回收时可以不写 swap |
| `AnonHugePages` / `FilePmdMapped` | 这段里的 THP |
| `Locked` | mlock |
| `Swap` / `SwapPss` | 换出的 |
| `VmFlags` | 内核 `VM_*` 的缩写 |

`smaps_rollup` 是整个 mm 的合计，比逐段 `smaps` 便宜。

用户态示例：[demos/01_maps_smaps.c](./demos/01_maps_smaps.c)。本机编译运行见 [14](./14-perf-and-dev-practice.md) 的验证记录。

---

## 6.6 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `do_mmap` | `mm/mmap.c:338` | mmap 主体 |
| `ksys_mmap_pgoff` | `mm/mmap.c:581` | 系统调用包装 |
| `vm_mmap_pgoff` | `mm/util.c:565` | 内核里 mmap 一个文件 |
| `do_munmap` | `mm/mmap.c:1077` | munmap |
| `do_vmi_align_munmap` | `mm/vma.c:1663` | 按 VMA 对齐拆 |
| `SYSCALL_DEFINE1(brk)` | `mm/mmap.c:117` | brk |
| `do_brk_flags` | `mm/vma.c:3013` | 扩大或缩小堆 |
| `vma_merge_new_range` | `mm/vma.c:1140` | 新范围并进相邻 VMA |
| `vma_merge_extend` | `mm/vma.c:1857` | mremap / brk 扩展时合并 |
| `vma_lookup` | `mm.h:4353` | 地址 → VMA |
| `find_vma_intersection` | `mm.h:4342` | 和区间相交的 VMA |
| `lock_mm_and_find_vma` | `mm.h:3278` | 缺页时锁 mm 并查找 |
| `lock_vma_under_rcu` | `mmap_lock.h:473` | per-VMA 锁快路径 |
| `vm_area_alloc` / `vm_area_dup` | `mm.h:264` | 分配 / 复制 VMA 对象 |

---

[上一节 vmalloc](./05-vmalloc-and-kernel-va.md) · [目录](./README.md) · [下一节 缺页](./07-page-fault.md)
