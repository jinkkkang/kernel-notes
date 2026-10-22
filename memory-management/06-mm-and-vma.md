# 6. `mm_struct` 与 `vm_area_struct`

**结论**：一个进程（或共享地址空间的线程组）有一个 `mm_struct`。它的 VMA 不放链表里，放在 maple tree `mm_mt`。`mmap` / `brk` 改的是这棵树和 VMA 的标志；真正的物理页要等缺页或预读才出现。

> 定义：`include/linux/mm_types.h`。`mm_struct` 从 `:1175`，`vm_area_struct` 从 `:923`。
> 路径：`mm/mmap.c`、`mm/vma.c`。
> 导航：[上一节 vmalloc](./05-vmalloc-and-kernel-va.md) · [目录](./README.md) · [下一节 缺页](./07-page-fault.md)

内核线程的 `task_struct.mm` 是 NULL，`active_mm` 借用上一个用户 mm（lazy TLB）。机制在 [6.8](#68-mm-与-active_mm)。进程章 [02-task-struct](../process-management/02-task-struct.md) 只登记这两个指针。

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

写树要拿 `mmap_lock` 写锁，或者走 vma 迭代器 `struct vma_iterator`（`vma_iter_*`，`mm.h:1665` 一带）。缺页快路径在 `CONFIG_PER_VMA_LOCK` 下可以只锁单个 VMA（`lock_vma_under_rcu`，定义在 `mm/mmap_lock.c:296`，声明在 `mmap_lock.h:473`），树本身用 RCU 读。三把锁谁包着谁，见 [6.11](#611-锁层次mmap_lock--per-vma-lock--ptl)。

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

## 6.6 页表层级：pgd → p4d → pud → pmd → pte

**结论**：用户虚拟地址不直接等于物理页。CPU 从 `mm->pgd` 出发，按地址的若干位一级级往下走。x86-64 编译进 5 级页表（`CONFIG_PGTABLE_LEVELS > 4`），4 级机器上把 p4d 折进 pgd，运行时看 `X86_FEATURE_LA57`。

每一级的下标（`include/linux/pgtable.h`）：

| 级 | 下标 | 在 x86-64 上覆盖 |
|---|---|---|
| pte | `pte_index`，`:48`，`address >> PAGE_SHIFT` | 一条 PTE 是 4KB。一页 PTE（512 项）盖 2MB |
| pmd | `pmd_index`，`:54`，`>> PMD_SHIFT`（21） | 一条 PMD 可以是 2MB 大页，或指向一页 PTE |
| pud | `pud_index`，`:62`，`>> PUD_SHIFT`（30） | 一条 PUD 可以是 1GB 大页，或指向一页 PMD |
| p4d | `p4d_index`，`arch/x86/include/asm/pgtable.h:1099`，`>> P4D_SHIFT`（39） | 一条 P4D 盖 512GB。4 级时 `PTRS_PER_P4D == 1`，下标恒为 0 |
| pgd | `pgd_index`，`pgtable.h:71`，`>> PGDIR_SHIFT` | 4 级时 shift 是 39，一条 PGD 盖 512GB；5 级时 shift 是 48，一条盖 256TB |

`PTRS_PER_PGD/PUD/PMD/PTE` 在 x86-64 都是 512（`arch/x86/include/asm/pgtable_64_types.h:51-80`）。4 级用户空间约 128TB（`__VIRTUAL_MASK_SHIFT` 47），5 级约 64PB（shift 56）。布局表在 [02](./02-physical-layout.md) 的 2.7，这里只讲级。

`pgdir_shift` 和 `ptrs_per_p4d` 是运行时变量，不是常量。默认值在 `arch/x86/kernel/head64.c:56-59`：`pgdir_shift = 39`，`ptrs_per_p4d = 1`，`__pgtable_l5_enabled = 0`。解压阶段如果 CPUID leaf 7 的 bit 16（LA57）在、命令行又没有 `no5lvl`，`arch/x86/boot/compressed/pgtable_64.c:122-129` 把三者改成 1 / 48 / 512。已经在 5 级模式里启动时，`arch/x86/boot/startup/map_kernel.c:26-28` 再写一遍。

`pgtable_l5_enabled()`（`pgtable_64_types.h:31`）早期读 `__pgtable_l5_enabled`（声明在 `:24`），之后是 `cpu_feature_enabled(X86_FEATURE_LA57)`（`:36`）。`/proc/cpuinfo` 的 `la57` 就是这个特性位（`cpufeatures.h:417`）。

折叠：某一级只有一个槽时，这一级不单独占一页页表。x86 的 `p4d_offset`（`asm/pgtable.h:1124`）：

```c
static inline p4d_t *p4d_offset(pgd_t *pgd, unsigned long address)
{
	if (!pgtable_l5_enabled())
		return (p4d_t *)pgd;          /* 4 级：p4d 就是 pgd 自己 */
	return (p4d_t *)pgd_page_vaddr(*pgd) + p4d_index(address);
}
```

通用的「这一级不存在」头文件是 `include/asm-generic/pgtable-nop4d.h`（`p4d_offset` 在 `:35`）、`pgtable-nopud.h`、`pgtable-nopmd.h`。折叠级的 `*_index` 注释写明返回 0，因为 `PTRS_PER_PxD == 1`（`pgtable.h:42-45`）。

查找一条表项：

```c
pgd_t *pgd_offset_pgd(pgd_t *pgd, unsigned long address) /* pgtable.h:140 */
{
	return pgd + pgd_index(address);
}
#define pgd_offset(mm, address)  pgd_offset_pgd((mm)->pgd, (address))  /* :149 */
/* pmd_offset：pud_pgtable(*pud) + pmd_index(address)，:125 */
/* pud_offset：p4d_pgtable(*p4d) + pud_index(address)，:133 */
/* pte_offset_kernel：pmd_page_vaddr(*pmd) + pte_index(address)，:96 */
```

`pmd_off`（`:165`）把四级调用串成一行，给「已知映射在 PMD」的内核路径用。`pgd_offset_k`（`:156`）走 `init_mm`，是内核自己的页表，不是当前进程的。

缺页时的分配式遍历在 `__handle_mm_fault`（`mm/memory.c:6604`）。进来时已经拿着 VMA 锁或 `mmap_lock`：

```text
pgd = pgd_offset(mm, address)          # :6621，顶级一定有，mm->pgd
p4d = p4d_alloc(mm, pgd, address)      # 没有这一级就分配一页页表
pud = pud_alloc(mm, p4d, address)
  PUD 空且允许 PUD 大页 → create_huge_pud
pmd = pmd_alloc(mm, pud, address)      # :6656
  PMD 空且允许 THP → create_huge_pmd
  否则落到 handle_pte_fault            # 见 07
```

`p4d_alloc` / `pud_alloc` / `pmd_alloc` 在折叠的那一级是空操作：表项「已经在」，不用再分配一页。

### page table check

`Documentation/mm/page_table_check.rst`。在 PTE/PMD 变成用户可访问的那一刻多做一次计数，抓「同一页又匿名又文件」这一类双映射。检测到就崩，而不是等以后用错数据。默认关，`page_table_check=` 打开（`early_param` 在 `mm/page_table_check.c:32`，处理函数 `:27`）。`CONFIG_PAGE_TABLE_CHECK_ENFORCED` 则默认开。运行时开关是静态键 `page_table_check_disabled`（`:24`）。

每个被检查的 `struct page` 在 page_ext 里挂一份 `struct page_table_check`（`:16`）：

| 成员 | 中文 |
|---|---|
| `atomic_t anon_map_count` | 作为匿名页被映射的次数 |
| `atomic_t file_map_count` | 作为文件页被映射的次数 |

装 PTE 走 `__page_table_check_ptes_set`（`:210`），拆走 `__page_table_check_pte_clear`（`:148`）。PMD/PUD 有对应的 `_pmds_set` / `_puds_set`。它不代替 mapcount，只在打开时多一道断言。

---

## 6.7 TLB、PCID 与 `mmu_gather`

**结论**：改完页表，CPU 上已经缓存的翻译不会自己消失。x86 用 PCID 给每个 mm 一个地址空间编号，切换时不必整表刷掉。一次 unmap 要拆很多 PTE 时，先把页收进 `struct mmu_gather`，最后一次 `tlb_finish_mmu` 再射向别的 CPU。

PCID 的说明在 `arch/x86/mm/tlb.c:65` 起。硬件字段叫 PCID，内核里的编号叫 ASID。PCID 0 留给不认识 PCID 的用户，所以写进 CR3 的 kPCID 是 ASID+1（`:83-85`）。开了 PTI 时用户页表再用一段 uPCID，等于 kPCID + 2048（`:87-90`）。`switch_mm_irqs_off`（`:783`）在 `loaded_mm` 没变、又不是从 lazy 回来时，可以不重写 CR3。

`struct mmu_gather`（`include/asm-generic/tlb.h:325`）是 unmap 期间的暂存。成员：

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct mm_struct *mm` | 始终 | 哪套地址空间 |
| `struct mmu_table_batch *batch` | `CONFIG_MMU_GATHER_TABLE_FREE` | 要延后释放的页表页 |
| `unsigned long start` / `end` | 始终 | 这次要刷的虚拟区间 |
| `unsigned int fullmm : 1` | 始终 | 正在拆整个 mm，可以整表刷 |
| `unsigned int need_flush_all : 1` | 始终 | 必须全刷，不能只刷区间 |
| `unsigned int freed_tables : 1` | 始终 | 拆掉了页表页，不只是 PTE |
| `unsigned int delayed_rmap : 1` | 始终 | 还有推迟的 rmap 拆除 |
| `unsigned int cleared_ptes : 1` | 始终 | 清过 PTE |
| `unsigned int cleared_pmds : 1` | 始终 | 清过 PMD |
| `unsigned int cleared_puds : 1` | 始终 | 清过 PUD |
| `unsigned int cleared_p4ds : 1` | 始终 | 清过 P4D |
| `unsigned int vma_exec : 1` | 始终 | 区间里有 `VM_EXEC`，I-TLB 也要刷 |
| `unsigned int vma_huge : 1` | 始终 | 区间里有 hugetlb |
| `unsigned int vma_pfn : 1` | 始终 | 区间里有 `VM_PFNMAP` / `VM_MIXEDMAP` |
| `unsigned int unshared_tables : 1` | 始终 | 取消了 hugetlb PMD 共享 |
| `unsigned int fully_unshared_tables : 1` | 始终 | 共享表已经变成某一边独占，新主人可能改它 |
| `unsigned int batch_count` | 始终 | 已经挂了多少个 batch |
| `struct mmu_gather_batch *active` | 没有 `CONFIG_MMU_GATHER_NO_GATHER` | 当前正在填的 batch |
| `struct mmu_gather_batch local` | 同上 | 栈上的第一批，分配失败时也能收几页 |
| `struct page *__pages[MMU_GATHER_BUNDLE]` | 同上 | `local` 的内联页数组，`MMU_GATHER_BUNDLE` 是 8（`:274`） |
| `unsigned int page_size` | 同上且 `CONFIG_MMU_GATHER_PAGE_SIZE` | 这一批页的尺寸。换尺寸要先刷 |

`struct mmu_gather_batch`（`:276`）：`next`、`nr`（已填个数）、`max`、柔性数组 `struct encoded_page *encoded_pages[]`。单批上限 `MAX_GATHER_BATCH`（`:283`）是一页减去 batch 头、再除以指针大小。批次数上限 `MAX_GATHER_BATCH_COUNT`（`:292`）大约是 10000 页，避免不可抢占内核在一次 unmap 里软锁死。

```text
tlb_gather_mmu(tlb, mm)                 # mmu_gather.c:462
  或 tlb_gather_mmu_fullmm               # :478，退出整个 mm
  或 tlb_gather_mmu_vma                  # :499，记下 VM_EXEC / hugetlb
拆 PTE 时 tlb_remove_page / tlb_remove_folio_pages
  批次满了就 tlb_flush_mmu               # :421
tlb_finish_mmu(tlb)                     # :515
  发现 mm_tlb_flush_nested：改成 fullmm，避免和并行的批量 flush 互相看见旧 PTE
  tlb_flush_mmu
  tlb_batch_list_free                    # 没有 NO_GATHER 时
  dec_tlb_flush_pending
```

x86 真正射出去的是 `flush_tlb_mm_range`（`arch/x86/mm/tlb.c:1401`）：

```text
inc_mm_tlb_gen(mm)                       # 和 switch_mm 同步的代际
区间太长（超过 tlb_single_page_flush_ceiling）就升级成整 mm
本 CPU 的 loaded_mm 就是它：关中断，flush_tlb_func 本地刷
mm_cpumask 里还有别的 CPU：flush_tlb_multi，IPI
mm 用了 global ASID：broadcast_tlb_flush
最后 mmu_notifier_arch_invalidate_secondary_tlbs   # KVM 的 EPT，见 11
```

`INVLPGB`（`X86_FEATURE_INVLPGB`）存在时，`flush_tlb_all`（`:1448`）可以不发 IPI，走 `invlpgb_flush_all`。没有这个特性就 `on_each_cpu`。

回收路径上还有另一套批量，不要和 `mmu_gather` 混。`try_to_unmap_flush`（`mm/rmap.c:715`）刷的是 `current->tlb_ubc`，类型是 `struct tlbflush_unmap_batch`（`mm_types_task.h:68`）。只在 `CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH` 下有成员：`arch`（架构的待刷集合）、`flush_required`、`writable`（拆的时候 PTE 是脏的，写 IO 之前必须刷掉，否则旧 TLB 还能把页写脏）。`try_to_unmap_flush_dirty`（`:728`）只在 `writable` 时才刷。

---

## 6.8 mm 与 active_mm

**结论**：`mm` 是这个任务自己的地址空间。`active_mm` 是 CPU 上此刻真正装着的那套页表。内核线程没有自己的 `mm`，它借用上一个用户任务的地址空间，靠 lazy TLB 不去切 CR3。

两个指针都在 `task_struct` 里（`include/linux/sched.h:980-981`）：`struct mm_struct *mm`，`struct mm_struct *active_mm`。用户任务两者指向同一个 mm。内核线程 `mm == NULL`，`active_mm` 指向它借来的那个。

切换在 `kernel/sched/core.c` 上下文切换里（`:5494` 的注释把四种情况写死了）：

```c
if (!next->mm) {                         /* 切到内核线程，:5494 */
	enter_lazy_tlb(prev->active_mm, next);   /* 调用点 :5495 */
	next->active_mm = prev->active_mm;       /* 把地址空间借走 */
	if (prev->mm)
		mmgrab_lazy_tlb(prev->active_mm); /* 用户 → 内核：抓住这个 mm */
	else
		prev->active_mm = NULL;           /* 内核 → 内核：引用已经在 next 上 */
} else {                                 /* 切到用户任务 */
	switch_mm_irqs_off(prev->active_mm, next->mm, next);  /* :5512 */
	if (!prev->mm) {
		rq->prev_mm = prev->active_mm;    /* 内核 → 用户：等 finish_task_switch 里 mmdrop_lazy_tlb */
		prev->active_mm = NULL;
	}
}
```

审计材料里把 `enter_lazy_tlb` 记在 `core.c:5495`。那是调用点。x86 的函数在 `arch/x86/include/asm/tlbflush.h:190`：

```c
static __always_inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == &init_mm)
		return;
	this_cpu_write(cpu_tlbstate_shared.is_lazy, true);  /* 只做标记，不写 CR3 */
}
```

注释（`:176-187`）写明：这个名字应该理解成 `switch_to_kernel_thread`。调度器可以连续调好几次，回到真正的用户 mm 时靠 `switch_mm_irqs_off` 把 lazy 清掉（`tlb.c:826-827`）。`cpu_tlbstate.loaded_mm` 才是这颗 CPU 页表的事实来源，`current->active_mm` 不一定随时更新（`switch_mm_irqs_off` 开头的注释，`:778-781`）。

借来的 mm 引用由 `mmgrab_lazy_tlb` / `mmdrop_lazy_tlb` 管。内核线程在这个 mm 上跑的时候，用户进程可以退出，但页表要留到最后一个借用者放下。读内核线程的 `/proc/pid/maps` 是空的，因为它没有 VMA；它的缺页如果发生在用户地址上，是 bug，不是这个借用机制要处理的情况。

---

## 6.9 反向映射：anon_vma / anon_vma_chain / i_mmap

**结论**：页表是从虚拟地址找到页。回收、迁移、mlock 要反过来：拿着 folio 找到所有指着它的 VMA 和 PTE。匿名页走 `anon_vma` 这棵树，文件页走 `address_space->i_mmap` 区间树。

匿名页不能把 `folio->mapping` 直接指到某个 VMA。VMA 会拆、会合、fork 之后一份页同时属于父子。所以指到 `anon_vma`，再由它找到相关的 VMA。`FOLIO_MAPPING_ANON` 这个低位标记在 [02](./02-physical-layout.md) 已经写过。

`struct anon_vma`（`include/linux/rmap.h:32`）：

| 成员 | 中文 |
|---|---|
| `struct anon_vma *root` | 这棵树的根。锁加在根的 `rwsem` 上 |
| `struct rw_semaphore rwsem` | 写：改树。读：`rmap_walk` 扫 |
| `atomic_t refcount` | 没有「页表一定还在」的保证时拿的引用。最后一个放下的人负责释放 |
| `unsigned long num_children` | 把 `parent` 指过来的 anon_vma 个数，含自己。`anon_vma_clone` 用它决定复用还是新建 |
| `unsigned long num_active_vmas` | `->anon_vma` 指着这个对象的 VMA 个数 |
| `struct anon_vma *parent` | 父。fork 出来的子 anon_vma 指回父，这样父的页还能找到子进程的 VMA |
| `struct rb_root_cached rb_root` | 相关 VMA 的区间树。`rb_node` 的最低位会被 `mm_take_all_locks` 占用，读之前必须先拿 `rwsem`（注释 `:58-63`） |

`struct anon_vma_chain`（`:83`）是 VMA 和 anon_vma 之间的边：

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct vm_area_struct *vma` | 始终 | 这一边的 VMA |
| `struct anon_vma *anon_vma` | 始终 | 这一边的 anon_vma |
| `struct list_head same_vma` | 始终 | 同一个 VMA 上的全部 anon_vma。锁是 `mmap_lock` 加 `page_table_lock` |
| `struct rb_node rb` | 始终 | 挂进 `anon_vma->rb_root`。锁是 `anon_vma->rwsem` |
| `unsigned long rb_subtree_last` | 始终 | 区间树子树的最大结尾 |
| `unsigned long cached_vma_start` / `cached_vma_last` | `CONFIG_DEBUG_VM_RB` | 调试用的区间缓存 |

VMA 上的 `anon_vma_chain` 链表头和 `anon_vma` 指针在 [6.2](#62-struct-vm_area_struct-全部成员923)。`MAP_SHARED` 的文件映射没有 anon_vma，只有 `i_mmap`。私有文件映射在 COW 之后会有。

```text
fork
  dup_mmap(mm, oldmm)                     # mm/mmap.c:1708
    → anon_vma_fork(tmp, mpnt)            # :1781，失败走 fail_nomem_anon_vma_fork
         anon_vma_clone                   # rmap.c:324，把父的链复制过来
         新建子 anon_vma，parent/root 指回父
新的匿名页挂在子进程自己的 anon_vma 上
父进程 COW 之前的页仍挂在父 anon_vma 上，子 VMA 通过链还能被找到
```

`anon_vma_fork` 的定义在 `mm/rmap.c:382`。

文件页的反向是 `address_space->i_mmap`。VMA 用 `shared.rb` / `rb_subtree_last` 挂进去（[6.2](#62-struct-vm_area_struct-全部成员923)）。`rmap_walk_file`（`rmap.c:3139`）在 folio 锁下拿 `i_mmap_rwsem`，按 `folio->index` 找出所有覆盖这段文件偏移的 VMA。

统一入口：

```c
void rmap_walk(struct folio *folio, struct rmap_walk_control *rwc) /* rmap.c:3157 */
{
	if (unlikely(folio_test_ksm(folio)))
		rmap_walk_ksm(folio, rwc);
	else if (folio_test_anon(folio))
		rmap_walk_anon(folio, rwc, false);
	else
		rmap_walk_file(folio, rwc, false);
}
```

`struct rmap_walk_control`（`rmap.h:953`）：

| 成员 | 中文 |
|---|---|
| `void *arg` | 交给 `rmap_one` 的私有指针 |
| `bool try_lock` | 拿不到 anon_vma 锁就放弃，而不是睡 |
| `bool contended` | 上面那次 trylock 失败过 |
| `bool (*rmap_one)(folio, vma, addr, arg)` | 对一个 VMA 里的一条映射做事。返回 false 就停 |
| `int (*done)(folio)` | 可选。返回真就不再往下扫 |
| `struct anon_vma *(*anon_lock)(folio, rwc)` | 匿名页怎么锁。常用 `folio_lock_anon_vma_read`（`rmap.c:637`） |
| `bool (*invalid_vma)(vma, arg)` | 这个 VMA 不算数，跳过 |

`folio_referenced`（`rmap.c:1072`）是回收用的：`rmap_one = folio_referenced_one`，`try_lock = true`。返回值是还被访问着的映射数。mapcount 已经是 0，或者没有 mapping，直接返回 0。

`rmap_walk_locked`（`:3168`）是调用者已经拿着 rmap 锁的版本，不支持 KSM。

---

## 6.10 overcommit 记账

**结论**：`mmap` 成功不等于有那么多物理页。`vm.overcommit_memory` 决定内核在记账时有多较真。`/proc/meminfo` 的 `Committed_AS` 是已经答应出去的虚拟量，`CommitLimit` 只在模式 2 里当上限。

三个值和默认（`include/uapi/linux/mman.h:13-15`，`mm/util.c:781-783`）：

| 值 | 宏 | 默认？ | 行为 |
|---|---|---|---|
| 0 | `OVERCOMMIT_GUESS` | 是，`sysctl_overcommit_memory` 的初值 | 这次申请的页数超过 `totalram_pages + total_swap_pages` 才拒绝。不把历史上已经答应的加总拿来比 |
| 1 | `OVERCOMMIT_ALWAYS` | | 记账之后直接成功 |
| 2 | `OVERCOMMIT_NEVER` | | `vm_committed_as` 不得超过 `CommitLimit`，再扣 root 和单进程的保留 |

`CommitLimit` 由 `vm_commit_limit`（`util.c:904`）算：

```c
if (sysctl_overcommit_kbytes)
	allowed = sysctl_overcommit_kbytes >> (PAGE_SHIFT - 10); /* kB → 页 */
else
	allowed = (totalram_pages() - hugetlb_total_pages())
		  * sysctl_overcommit_ratio / 100;               /* 默认 ratio = 50 */
allowed += total_swap_pages;
```

`/proc/meminfo` 把这个数印成 `CommitLimit`（`fs/proc/meminfo.c:125`），把 `vm_memory_committed()`（`util.c:937`，读 `vm_committed_as`）印成 `Committed_AS`（`:46`、`:126`）。模式 0 和 1 也会印 `CommitLimit`，只是 `__vm_enough_memory` 不用它做判决。本机快照里 `CommitLimit == MemTotal/2`，和默认 ratio 50、swap 为 0 对得上，不能据此认为机器跑在模式 2。

```c
int __vm_enough_memory(const struct mm_struct *mm, long pages, int cap_sys_admin)
{
	vm_acct_memory(pages);                       /* 先记上，:964 */
	if (sysctl_overcommit_memory == OVERCOMMIT_ALWAYS)
		return 0;
	if (sysctl_overcommit_memory == OVERCOMMIT_GUESS) {
		if (pages > totalram_pages() + total_swap_pages)
			goto error;
		return 0;
	}
	allowed = vm_commit_limit();
	if (!cap_sys_admin)
		allowed -= sysctl_admin_reserve_kbytes >> (PAGE_SHIFT - 10); /* 默认 8MB */
	if (mm)
		allowed -= min(mm->total_vm / 32, user_reserve); /* user_reserve 默认 128MB */
	if (percpu_counter_read_positive(&vm_committed_as) < allowed)
		return 0;
error:
	vm_unacct_memory(pages);                     /* 拒绝就把刚才记的减回去 */
	return -ENOMEM;
}
```

`sysctl_admin_reserve_kbytes` 默认 `1 << 13`（8MB），`sysctl_user_reserve_kbytes` 默认 `1 << 17`（128MB）（`:785-786`）。hugetlb 有自己的预留，不进这本账，见 [09](./09-thp-and-hugetlb.md)。KVM 把客户机内存超卖是另一层，见 [12](./12-balloon-fpr-and-overcommit.md)。

`vm_committed_as`（`util.c:922`）独占一条 cache line。读 `/proc/meminfo` 时 `vm_memory_committed` 做的是 `percpu_counter_sum_positive`，大机器上会去对一下各 CPU 的计数。

---

## 6.11 锁层次：mmap_lock / per-VMA lock / PTL

**结论**：改 VMA 树拿 `mmap_lock` 写锁。缺页的快路径只拿目标 VMA 的 per-VMA 锁。改某一张页表里的 PTE 再拿这张表的页表锁（PTL）。锁从外到内是 mmap_lock（或 per-VMA 锁），然后才是 PTL。拿着 PTL 再去睡 `mmap_lock`，就是锁顺序反了。

`mm->mmap_lock` 是 `rw_semaphore`（[6.1](#61-struct-mm_struct-全部成员1175)）。`mmap` / `munmap` / `brk` / `mprotect` / `mlock` 走写锁。很多缺页和 `/proc/pid/maps` 走读锁。`mm->page_table_lock` 是一把自旋锁，没拆分时所有页表共用它。

per-VMA 锁在 `CONFIG_PER_VMA_LOCK` 下是 `vma->vm_refcnt`，不是另一把 rwsem。状态机在 `mm_types.h:1007-1044`，[6.2](#62-struct-vm_area_struct-全部成员923) 已经列过。`mm->mm_lock_seq` 在每次 `mmap_lock` 写加锁/解锁时增加，奇数表示写锁正被持有。`lock_vma_under_rcu`（`mm/mmap_lock.c:296`）：

```text
rcu_read_lock
mas_walk(&mm->mm_mt)                     # 在 maple tree 里找 VMA
vma_start_read                           # 把 vm_refcnt 从 1 抬高；写者正在踢读者就返回 -EAGAIN
地址不在 [vm_start, vm_end) 就 vma_end_read，返回 NULL
```

返回 NULL 时缺页退回 `lock_mm_and_find_vma`，拿 `mmap_lock` 读锁再查一次。写者改 VMA 之前要把读者赶出去，等在 `mm->vma_writer_wait` 上。

页表锁看 `CONFIG_SPLIT_PTE_PTLOCKS`。`include/linux/mm.h`：

```c
/* 拆分，:3862。锁在 PTE 页自己的 ptdesc 里 */
static inline spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return ptlock_ptr(page_ptdesc(pmd_page(*pmd)));
}
/* 不拆分，:3894。整个 mm 一把 */
static inline spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return &mm->page_table_lock;
}
```

PMD 同样有两套：`pmd_lockptr` 在 `:3986`（`CONFIG_SPLIT_PMD_PTLOCKS`，锁在 PMD 页上）和 `:4003`（退回 `page_table_lock`）。自旋锁比一个 `unsigned long` 还大时，`ALLOC_SPLIT_PTLOCKS`（`mm_types_task.h:20`）为真，锁不嵌在 ptdesc 里，`ptlock_alloc` 另分配（`mm.h:3847` 一带）。

`pte_offset_map_lock` 的定义在 `mm/pgtable-generic.c:393`，不是 `memory.c:2051`。`memory.c:2051` 是调用点之一。定义做的事：

```c
pte = __pte_offset_map(pmd, addr, &pmdval);   /* 映射 PTE 页，HIGHPTE 上是 kmap_local */
ptl = pte_lockptr(mm, &pmdval);
spin_lock(ptl);
if (likely(pmd_same(pmdval, pmdp_get_lockless(pmd))))
	return pte;                            /* PMD 没被拆成大页或换掉 */
pte_unmap_unlock(pte, ptl);
goto again;
```

先看 PMD 再锁，是因为 THP 拆分会把 PMD 换成 PTE 页。锁完再对一次，对不上就重来。`pte_unmap_unlock`（`mm.h:3955`）先放锁再 `pte_unmap`。

缺页快路径的顺序：`lock_vma_under_rcu` 或 `mmap_read_lock`，然后 `pte_offset_map_lock`。`munmap` 是 `mmap_write_lock`，再在 `mmu_gather` 里拆页表。hugetlb 的 PMD 共享另有 `hugetlb_vma_lock`（`hugetlb.h:104`），不在这三层里面。

---

## 6.12 没有 MMU，以及 `remap_file_pages`

`Documentation/admin-guide/mm/nommu-mmap.rst` 写的是 uClinux 一类没有页表的配置。x86-64 不走这条。`mm/nommu.c` 里有自己的 `do_mmap`（`:1012`）和 `dup_mmap`（`:1927`）。没有 MMU 时 VMA 多一个 `vm_region`（[6.2](#62-struct-vm_area_struct-全部成员923) 的条件字段）。匿名 `MAP_PRIVATE` 要的是物理连续的页，不是 COW。没有 `fork`，`clone` 必须带 `CLONE_VM`。`mmap` 的系统调用入口仍是 `SYSCALL_DEFINE6(mmap_pgoff)`（`nommu.c:1295`），语义比有 MMU 时窄：共享映射要求文件支持，私有映射不能靠缺页一页页补。

`remap_file_pages`（`Documentation/mm/remap_file_pages.rst`）曾经用非线性映射把一个文件的页打乱塞进同一个 VMA，靠 PTE 里的 file 位记下文件偏移。v7.3 这个系统调用还在，`mm/mmap.c:1101`，第一件事是 `pr_warn_once` 告诉你它过时了。`prot` 非 0 直接 `-EINVAL`。真正的工作是拼出 `MAP_SHARED | MAP_FIXED | MAP_POPULATE`，调用 `do_mmap`（`:1208`）。非线性 PTE 没有了。代价是可能多出 VMA，`map_count` 会涨。新代码用普通 `mmap`。

---

## 6.13 关键函数表

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
| `pgd_offset` / `pgd_offset_pgd` | `pgtable.h:149` / `:140` | 顶级页表项 |
| `p4d_offset` | `asm/pgtable.h:1124` | x86：4 级时返回 pgd 本身 |
| `pmd_offset` / `pud_offset` | `pgtable.h:125` / `:133` | 中间级 |
| `__handle_mm_fault` | `mm/memory.c:6604` | 分配式走完 pgd→pmd |
| `tlb_gather_mmu` / `tlb_finish_mmu` | `mm/mmu_gather.c:462` / `:515` | 批量拆页表 |
| `flush_tlb_mm_range` | `arch/x86/mm/tlb.c:1401` | x86 射 TLB |
| `try_to_unmap_flush` | `mm/rmap.c:715` | rmap 那条批量 TLB |
| `enter_lazy_tlb` | `asm/tlbflush.h:190` | x86：置 lazy，不写 CR3 |
| `switch_mm_irqs_off` | `arch/x86/mm/tlb.c:783` | 真正换 mm |
| `anon_vma_fork` / `anon_vma_clone` | `mm/rmap.c:382` / `:324` | fork 时接上反向映射 |
| `dup_mmap` | `mm/mmap.c:1708` | 复制 VMA，里面调 `anon_vma_fork` |
| `rmap_walk` | `mm/rmap.c:3157` | 按 anon / file / ksm 分发 |
| `folio_referenced` | `mm/rmap.c:1072` | 回收时数访问过的映射 |
| `__vm_enough_memory` | `mm/util.c:959` | overcommit 判决 |
| `vm_commit_limit` | `mm/util.c:904` | `CommitLimit` |
| `pte_lockptr` | `mm.h:3862` 或 `:3894` | 拆分 PTL，或整 mm 一把 |
| `pmd_lockptr` | `mm.h:3986` 或 `:4003` | PMD 那一层 |
| `pte_offset_map_lock` | `mm/pgtable-generic.c:393` | 映射 PTE 并加 PTL，对不上就重试 |
| `lock_vma_under_rcu` | `mm/mmap_lock.c:296` | 缺页快路径的 per-VMA 锁 |
| `SYSCALL_DEFINE5(remap_file_pages)` | `mm/mmap.c:1101` | 过时系统调用，转成 `do_mmap` |

---

[上一节 vmalloc](./05-vmalloc-and-kernel-va.md) · [目录](./README.md) · [下一节 缺页](./07-page-fault.md)
