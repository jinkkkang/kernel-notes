# 11. KVM 内存

**结论**：guest 里的物理地址（GPA）不是宿主物理地址（HPA）。x86 上 EPT（Intel）或 NPT（AMD）是第二张页表，把 GPA 翻译成 HPA。这张表由 KVM 建，不是 guest 内核建的。guest 缺页先走 guest 自己的页表；EPT violation 是第二张表没有映射，处理在宿主。

> 通用 memslot：`include/linux/kvm_host.h`。x86 MMU：`arch/x86/include/asm/kvm_host.h`、`arch/x86/kvm/mmu/`。
> 导航：[上一篇 swap table](./10-11-swap-table.md) · [目录](./README.md) · [下一节 balloon/FPR](./12-balloon-fpr-and-overcommit.md)

两套视角不要混：

| | guest 看见的 | 宿主看见的 |
|---|---|---|
| 虚拟地址 | GVA，guest 进程的 | 宿主进程（qemu）的 HVA |
| 「物理」地址 | GPA，guest 内核眼中的物理页 | 仍是某个 HVA，直到 EPT 把它钉到 HPA |
| 缺页 | guest 的 `handle_mm_fault` | EPT violation / NPF → `kvm_mmu_page_fault` |
| 大页 | guest THP / hugetlb | EPT 的页大小。两边独立 |

---

## 11.1 memslot：GPA 区间 ↔ 宿主用户地址

qemu 用 `KVM_SET_USER_MEMORY_REGION` 登记一段 GPA。内核落成 `struct kvm_memory_slot`。

`kvm_set_memory_region`：`virt/kvm/kvm_main.c:2001`。

### `struct kvm_memory_slot`（`kvm_host.h:593`）全部成员

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct hlist_node id_node[2]` | 始终 | 按 slot id 哈希。两份是为了 generation 双缓冲：读者看旧的，写者改新的 |
| `struct interval_tree_node hva_node[2]` | 始终 | 按宿主用户虚拟地址（HVA）的区间树节点，同样双份 |
| `struct rb_node gfn_node[2]` | 始终 | 按 GFN 排序的红黑树节点，双份 |
| `gfn_t base_gfn` | 始终 | 这段 GPA 的起始页帧号（GPA >> PAGE_SHIFT） |
| `unsigned long npages` | 始终 | 页数。0 表示删除 slot |
| `unsigned long *dirty_bitmap` | 始终 | 脏页位图。迁移时宿主问「guest 写过哪些 GPA」 |
| `struct kvm_arch_memory_slot arch` | 始终 | 架构私有。x86 见下 |
| `unsigned long userspace_addr` | 始终 | 对应的 HVA。qemu 进程里 `mmap` 出来的那块 |
| `u32 flags` | 始终 | `KVM_MEM_LOG_DIRTY_PAGES`、`KVM_MEM_READONLY` 等 |
| `short id` | 始终 | slot 号 |
| `u16 as_id` | 始终 | 地址空间 id。SMM 是另一套 |
| `struct file *file` | `CONFIG_KVM_GUEST_MEMFD`，嵌在 `gmem` 里 | guest_memfd。私有内存（保护虚拟机）不走普通用户页 |
| `pgoff_t pgoff` | 同上 | 在 guest_memfd 里的偏移 |

写 `gmem.file` 要 `kvm->slots_lock`。用 `kvm_gmem_get_file()` 拿引用时，`slots_lock` 或 `kvm->srcu` 都行（注释 `:608`）。

### `struct kvm_memslots`（`kvm_host.h:753`）

| 成员 | 中文 |
|---|---|
| `u64 generation` | 代际。改 slot 时加一，MMU 用它发现「我 fault-in 时用的 slot 已经过期」 |
| `atomic_long_t last_used_slot` | 上次命中的 slot，热路径少搜树 |
| `struct rb_root_cached hva_tree` | HVA 区间树的根 |
| `struct rb_root gfn_tree` | GFN 树的根 |
| `DECLARE_HASHTABLE(id_hash, 7)` | id → slot。7 位桶，注释说是为了 512 个 slot 的老数组大小和查找速度的折中 |
| `int node_idx` | 节点索引，NUMA |

查找：`gfn_to_memslot`（`kvm_host.h:1226`），vcpu 热路径 `kvm_vcpu_gfn_to_memslot`（`:1228`）。

### x86 `struct kvm_arch_memory_slot`（`asm/kvm_host.h:1038`）

| 成员 | 中文 |
|---|---|
| `struct kvm_rmap_head *rmap[KVM_NR_PAGE_SIZES]` | 反向映射。一个 GFN 被哪些影子 PTE 指着。TDP 直接映射不靠它做主路径，影子页表靠它 |
| `struct kvm_lpage_info *lpage_info[KVM_NR_PAGE_SIZES - 1]` | 大页禁止信息。`disallow_lpage` 非 0 就不能在这一级建大页 |
| `unsigned short *gfn_write_track` | 写跟踪计数。有人在盯这个 GFN 的写（模拟页表写）时不能用大页、也不能直接映射成可写 |

`struct kvm_lpage_info`（`:1034`）只有 `int disallow_lpage`。

`struct kvm_rmap_head`（`:424`）只有 `atomic_long_t val`。

---

## 11.2 两层 MMU：影子页表和 TDP

| 模式 | 什么时候 | 谁翻译 GVA |
|---|---|---|
| 影子页表（shadow） | 硬件没有 EPT/NPT，或嵌套时 L2 的页表被 L0 影子化 | KVM 建的影子页表，硬件只走一张 |
| TDP（two-dimensional paging） | EPT 或 NPT 打开。现代默认 | guest 自己的页表走 GVA→GPA；EPT/NPT 走 GPA→HPA |

`struct kvm_mmu`（`asm/kvm_host.h:495`）是 vcpu 上的一套 MMU。`vcpu->arch.mmu` 指向当前用的那套；`root_mmu` 是 TDP/直接根，`guest_mmu` 是嵌套时 L2 的（`:765` 一带，字段在 `kvm_vcpu_arch` 里）。

### `struct kvm_mmu` 全部成员

| 成员 | 中文 |
|---|---|
| `int (*page_fault)(...)` | 缺页回调。TDP 时是 `kvm_tdp_page_fault` |
| `int (*sync_spte)(...)` | 把影子 PTE 和 guest PTE 同步。TDP 直接映射基本不用 |
| `struct kvm_pagewalk *w` | 当前分页模式的走表方法。见下 |
| `struct kvm_mmu_root_info root` | 当前根。`pgd` 是 guest 侧根 GPA，`hpa` 是根页表的 HPA |
| `hpa_t mirror_root_hpa` | 镜像根的 HPA。保护虚拟机 / TDX 用 |
| `union kvm_mmu_page_role root_role` | 根页的角色。`direct == 1` 表示 TDP 直接映射，缺页地址已经是 GPA |
| `struct kvm_mmu_root_info prev_roots[KVM_MMU_NUM_PREV_ROOTS]` | 以前的根，避免每次切 CR3 都拆掉。`KVM_MMU_NUM_PREV_ROOTS` 是 3（`:465`） |
| `u64 *pae_root` | 32 位 PAE 的根 |
| `u64 *pml4_root` | 4 级根 |
| `u64 *pml5_root` | 5 级根 |
| `struct kvm_page_format fmt` | 保留位、PKRU、权限位图 |

`struct kvm_mmu_root_info`（`:457`）：`gpa_t pgd`，`hpa_t hpa`。无效值是两边都是 `INVALID_PAGE`（`:462`）。

### `struct kvm_pagewalk`（`:481`）全部成员

| 成员 | 中文 |
|---|---|
| `get_guest_pgd` | 取 guest CR3 |
| `get_pdptr` | PAE 的 PDPT |
| `inject_page_fault` | 向 guest 注入缺页 |
| `gva_to_gpa` | 软件走 guest 页表 |
| `union kvm_cpu_role cpu_role` | 当前 CPU 的角色，CR0/CR4/EFER 变了才重配 MMU |
| `struct kvm_page_format fmt` | 和 `kvm_mmu.fmt` 同类的格式描述 |

### `union kvm_mmu_page_role`（`:339`）

整体是一个 `u32 word`，位域：

| 位域 | 中文 |
|---|---|
| `level:4` | 这一页在第几级 |
| `has_4_byte_gpte:1` | guest PTE 是 4 字节（32 位） |
| `quadrant:2` | 32 位影子页把 4K 拆成象限 |
| `direct:1` | 直接映射（TDP）。没有 guest 页表要影子化 |
| `access:4` | 访问权限 |
| `invalid:1` | 角色已失效 |
| `efer_nx:1` | EFER.NXE |
| `cr0_wp:1` | CR0.WP |
| `smap_andnot_wp:1` | SMAP 且 WP=0 的组合 |
| `ad_disabled:1` | 不用 A/D 位 |
| `guest_mode:1` | 嵌套：这是 L2 |
| `passthrough:1` | 穿透 |
| `is_mirror:1` | 镜像页表 |
| `cr4_smep:1` | CR4.SMEP。EPT MBEC 和 AMD GMET 也复用这一位，注释 `:356` 说明了原因 |
| 填充 `:3` | 凑字节 |
| `smm:8` | SMM。放在 word 的高位，方便一条移位取出。给了一整个字节 |

`union kvm_mmu_extended_role`（`:396`）补页角色里没有、但换 CR 时要比较的位：`valid`、`execonly`、`cr4_pse`、`cr4_pke`、`cr4_smap`、`cr4_la57`、`efer_lma`、`has_pferr_fetch`。`valid` 用来区分「全 0」和「真的配过」。

`union kvm_cpu_role`（`:416`）：`base` + `ext`，合成一个 `u64`。

### `struct kvm_page_format`（`:437`）

| 成员 | 中文 |
|---|---|
| `u64 rsvd_bits_mask[2][PT64_ROOT_MAX_LEVEL]` | 每一级 PTE 的保留位。guest 置了这些位就是保留位缺页。`PT64_ROOT_MAX_LEVEL` 是 5 |
| `u64 bad_mt_xwr` | 非法的内存类型 × 权限组合 |
| `u32 pkru_mask` | 保护键。16 个域，按缺页错误码的 [4:1] 索引 |
| `u16 permissions[16]` | 权限检查位图。下标是错误码 [4:1]，bit 是 `ACC_*` 权限。置位表示这种组合算权限错 |

---

## 11.3 `struct kvm_mmu_page`（`mmu_internal.h:44`）

一页影子页表或一页 TDP 页表。注释要求 `link` 到 `spt` 落在一条 64 字节 cache line 里。

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct list_head link` | 始终 | 活动页链表 |
| `struct hlist_node hash_link` | 始终 | 按 (gfn, role) 哈希 |
| `bool tdp_mmu_page` | 始终 | 这是 TDP MMU 的页，不是影子 MMU 的页 |
| `bool unsync` | 始终 | 影子页和 guest 页表不一致，等 sync |
| `u8 mmu_valid_gen` | 与下一个同 union | 代际。zapping 时整代作废 |
| `bool tdp_mmu_scheduled_root_to_zap` | 同 union | 只在 `slots_lock` 下用。这个根已经排进去要拆 |
| `bool nx_huge_page_disallowed` | 始终 | 因为 NX huge page 缓解，这一页不能被等价大页替换。它映射了 guest 的可执行页 |
| `union kvm_mmu_page_role role` | 始终 | 哈希键的一半 |
| `gfn_t gfn` | 始终 | 哈希键的另一半。这一页影子的是哪个 guest 页帧 |
| `u64 *spt` | 始终 | 页表内容本身，512 个 64 位项 |
| `u64 *shadowed_translation` | 始终 | 每个 SPTE 影子化的结果：高位是 GFN，低位是 KVM 格式的访问权限（`ACC_*`），不是 guest 原始格式。注释 `:77` |
| `int root_count` | 与下一个同 union | 作为活动根的次数。非 TDP |
| `refcount_t tdp_mmu_root_count` | 同 union | TDP 根的引用 |
| `bool has_mapped_host_mmio` | 始终 | 映射了宿主 MMIO |
| `unsigned int unsync_children` | 非 TDP 那一臂 | 下面有多少不同步的子页 |
| `atomic_t write_flooding_count` | 同上 | 上次遍历以来被写的次数。写太频繁就拆掉这一级，避免模拟写页表的开销 |
| `void *external_spt` | 与上面同 union | 外部页表（TDX 模块持有，KVM 不直接访问） |
| `struct kvm_rmap_head parent_ptes` | 与 `ptep` 同 union | 哪些父 SPTE 指着这一页 |
| `tdp_ptep_t ptep` | 同 union | TDP 页的父项指针 |
| `unsync_child_bitmap` | 始终，512 bit | 哪个子项不同步 |
| `struct list_head possible_nx_huge_page_link` | 始终 | 拆掉它就能再建 NX 大页的那些页。已经因为别的原因禁止大页的不在这条链上 |
| `int clear_spte_count` | `CONFIG_X86_32` | 无锁读 SPTE 时的顺序计数 |
| `struct rcu_head rcu_head` | `CONFIG_X86_64` | TDP 页异步释放 |

---

## 11.4 `struct tdp_iter`（`tdp_iter.h:83`）

TDP MMU 走树的游标。不加「影子」那一层。

| 成员 | 中文 |
|---|---|
| `gfn_t next_last_level_gfn` | 迭代器要走向的那个最后一级 GFN |
| `gfn_t yielded_gfn` | 上次让出 CPU 时的 `next_last_level_gfn`。只有两者不同才让出，保证有进展 |
| `tdp_ptep_t pt_path[PT64_ROOT_MAX_LEVEL]` | 从根走到当前项经过的页表 |
| `tdp_ptep_t sptep` | 当前 SPTE 的指针 |
| `gfn_t gfn` | 当前 SPTE 映射的最低 GFN（掩码位除外） |
| `gfn_t gfn_bits` | 把 GFN 转成 GPA 时要或回去的位（保护虚拟机的 shared bit 一类） |
| `int root_level` | 根的级别 |
| `int min_level` | 最深走到哪一级。大页可以停在 2MB/1GB |
| `int level` | 当前级别 |
| `int as_id` | 地址空间，SMM 还是普通 |
| `u64 old_spte` | `sptep` 上的值的快照 |
| `bool valid` | 还没走出树 |
| `bool yielded` | 中途放下过 `mmu_lock`。下次 `tdp_iter_next()` 要从根重走，不能顺着旧指针往下 |

---

## 11.5 `struct kvm_page_fault`（`mmu_internal.h:226`）

一次缺页的全部输入输出。`kvm_mmu_do_page_fault` 在栈上填它（`mmu.c:4990`）。

| 成员 | 中文 |
|---|---|
| `const gpa_t addr` | 出错地址。TDP 下是 GPA；影子模式下可能是 GVA，走完 guest 页表后另算 gfn |
| `const u64 error_code` | 架构错误码，外加 KVM 合成位 |
| `const bool prefetch` | 预取，不是真的 VM-exit |
| `const bool exec` / `write` / `present` / `rsvd` / `user` | 从 error_code 拆出来的 |
| `const bool is_tdp` | `mmu->page_fault == kvm_tdp_page_fault` |
| `const bool is_private` | 私有内存（`PFERR_PRIVATE_ACCESS`） |
| `const bool nx_huge_page_workaround_enabled` | 是否在做 NX 大页缓解 |
| `bool huge_page_disallowed` | 这一次不许建 >4KB |
| `u8 max_level` | 允许的最大页（给 `kvm_tdp_mmu_map` / `direct_map`） |
| `u8 req_level` | 结合宿主映射页大小之后想建的级别 |
| `u8 goal_level` | 再扣掉 `huge_page_disallowed` 之后真正要建的级别 |
| `gfn_t gfn` | 页帧。TDX 一类多别名时，选别名的那个 bit 不在 gfn 里，留在 `addr` |
| `struct kvm_memory_slot *slot` | `gfn` 所在 slot。可以是 NULL（MMIO） |
| `unsigned long mmu_seq` | fault-in 时的 memslot generation，用来发现过期 |
| `kvm_pfn_t pfn` | `kvm_mmu_faultin_pfn` 的输出。初始 `KVM_PFN_ERR_FAULT` |
| `struct page *refcounted_page` | 拿了引用的宿主页 |
| `bool map_writable` | 宿主页可写，EPT 才能建成可写 |
| `bool write_fault_to_shadow_pgtable` | guest 在写自己的页表。模拟这条写，不能当成普通数据写 |

返回值（`:312`）：`RET_PF_CONTINUE`（0）、`RETRY`、`EMULATE`、`WRITE_PROTECTED`、`INVALID`、`FIXED`、`SPURIOUS`。新增名字要在 `mmutrace.h` 里 `TRACE_DEFINE_ENUM` 导出。

---

## 11.6 guest 缺页 → EPT violation

guest 里一次写匿名页，其实是两次翻译：

```text
guest 用户态写 GVA
  → guest 页表没有映射
  → guest 内核 handle_mm_fault，分配 guest 「物理页」
  → 这条 GPA 在 EPT 里还没有
  → VM-exit：EPT violation

Intel：
  handle_ept_violation                         # vmx/vmx.c:5972
    读 VMCS 的 GPA 和 exit qualification
    trace_kvm_page_fault
    __vmx_handle_ept_violation                 # vmx/common.h:83
      把 EPT_VIOLATION_ACC_* 收成 PFERR_* error_code
      kvm_mmu_page_fault                       # mmu.c:6573

AMD：
  npf_interception                             # svm/svm.c:1968
    exit_info_2 是 GPA，exit_info_1 是错误码
    kvm_mmu_page_fault                         # :2019

kvm_mmu_page_fault
  保留位错：handle_mmio_page_fault             # MMIO 走模拟
  否则 kvm_mmu_do_page_fault                   # :4986
    填 struct kvm_page_fault
    direct 角色：gfn = gpa_to_gfn，去掉 shared bit
              slot = kvm_vcpu_gfn_to_memslot
    retpoline 且 is_tdp：直接 kvm_tdp_page_fault   # :4976
    否则 mmu->page_fault()

kvm_tdp_page_fault
  tdp_mmu_enabled → kvm_tdp_mmu_page_fault     # :4941
  否则 direct_page_fault                       # 老的 TDP，仍是直接映射但不是 TDP MMU

kvm_tdp_mmu_page_fault
  page_fault_handle_page_track                 # 有人在跟踪写，就写保护
  fast_page_fault                              # 只把已有 SPTE 的 W 位打开，不走慢路径
  mmu_topup_memory_caches                      # 准备分配页表页的 cache
  kvm_mmu_faultin_pfn                          # 关键：把 GPA 变成宿主 PFN
       通过 memslot 找到 HVA
       对 qemu 的用户页做 GUP / 缺页
       这一步会进宿主的 handle_mm_fault
  read_lock(kvm->mmu_lock)
  is_page_fault_stale ? 重试 : kvm_tdp_mmu_map # 用 tdp_iter 把 PFN 装进 EPT
```

`kvm_mmu_faultin_pfn` 是两层交汇点。guest 觉得自己缺的是 GPA；宿主在这里缺的是 qemu 进程的匿名页或 hugetlb 页。宿主页如果还没分配，你会在宿主上看到普通的 minor fault，同时 guest 停在 EPT violation 里。

宿主用 4KB 映射时，guest 的 2MB 大页在 EPT 里通常也只能建成 4KB（`req_level` 受宿主页大小限制，见 `kvm_page_fault` 的注释 `:256`）。要 EPT 大页，宿主侧那一段也得是大页：hugetlb 或 THP，并且对齐。

---

## 11.7 嵌套

L2 的 GPA（从 L0 看是 nGPA）要先翻译成 L1 的 GPA，再翻译成 HPA。TDP 只解决最后一跳时，中间那跳仍可能是影子页表。`kvm_mmu.page_fault` 在嵌套时不一定是 `kvm_tdp_page_fault`，`is_tdp` 的判断就是比较函数指针（`mmu.c:4999`）。

本环境没有 KVM（云端 box，`/dev/kvm` 未作为实验对象打开）。嵌套、EPT 大页、balloon 与 EPT 的交互都标 RECIPE，步骤在 [12](./12-balloon-fpr-and-overcommit.md) 和 [14](./14-perf-and-dev-practice.md)。不要在没有 `/dev/kvm` 的机器上编一套「我跑过」的数字。

---

## 11.8 `mmu_notifier`：宿主改页表时拆掉 EPT

**结论**：KVM 在 `struct kvm` 里嵌了一个 `struct mmu_notifier`（`include/linux/kvm_host.h:851`）。宿主 munmap、迁移、mprotect、回收清 young 时，回调进 `virt/kvm/kvm_main.c` 的 `kvm_mmu_notifier_ops`（`:879`），把对应 HVA 上的 EPT/NPT 拆掉或清 accessed。不注册这个回调，宿主把页换走之后 guest 仍可能从旧 EPT 访问那一页。

`kvm_init_mmu_notifier` 在 `:888`。填进去的回调：

| ops 成员 | KVM 函数 | 行 |
|---|---|---|
| `invalidate_range_start` | `kvm_mmu_notifier_invalidate_range_start` | `:722` |
| `invalidate_range_end` | `kvm_mmu_notifier_invalidate_range_end` | `:798` |
| `clear_flush_young` | `kvm_mmu_notifier_clear_flush_young` | `:829` |
| `clear_young` | `kvm_mmu_notifier_clear_young` | `:838` |
| `test_young` | `kvm_mmu_notifier_test_young` | `:859` |
| `release` | `kvm_mmu_notifier_release` | `:868`，里面 `kvm_flush_shadow_all` |

KVM 没填 `arch_invalidate_secondary_tlbs`、`alloc_notifier`、`free_notifier`。

### `struct mmu_notifier_ops`（`include/linux/mmu_notifier.h:64`）

回调都可以是 NULL。头文件注释写了配对和睡眠限制，下面只记每个指针干什么。

| 成员 | 中文 |
|---|---|
| `void (*release)(struct mmu_notifier *subscription, struct mm_struct *mm)` | mm 销毁（`exit_mmap`）或 `mmu_notifier_unregister` 时调用，在页被释放之前。要拆掉全部二级映射并冻住二级 MMU。可以和 mm 外的其它回调并发 |
| `bool (*clear_flush_young)(..., unsigned long start, unsigned long end)` | 主 MMU 清 young/accessed 之后调用。二级页表也要 test-and-clear，并且刷二级 TLB。返回值表示二级映射是否年轻。start/end 是因为二级粒度可以比主页表更细 |
| `bool (*clear_young)(...)` | 同上，允许不刷二级 TLB。KVM 的 `clear_young` 注释（`kvm_main.c:850` 一带）写明当前不从这里发二级 TLB 刷新 |
| `bool (*test_young)(..., unsigned long address)` | 只查询二级 PTE 的 young，不清、不拆 |
| `int (*invalidate_range_start)(..., const struct mmu_notifier_range *range)` | 区间里的页都还映射着，refcount 至少 1。返回前调用者不能再使用这个区间的 SPTE。`MMU_NOTIFIER_RANGE_BLOCKABLE` 没置位时不能睡眠，需要睡眠就返回 `-EAGAIN`。能失败的 start 不允许再实现 end |
| `void (*invalidate_range_end)(...)` | 区间已经解映射，VM 已经把页释放。start 到 end 之间页表项会被拆掉。驱动若要保住页，必须在 start（或最初映射时）把 refcount 抬住 |
| `void (*arch_invalidate_secondary_tlbs)(..., unsigned long start, unsigned long end)` | 和 CPU 共享页表的非 CPU TLB。在持 PTL 时调用，不能睡眠。实现了这个就不要再实现 start/end |
| `struct mmu_notifier *(*alloc_notifier)(struct mm_struct *mm)` | get/put 接口下分配一个 notifier |
| `void (*free_notifier)(struct mmu_notifier *subscription)` | 引用全放掉、没有回调在跑之后，从 SRCU 回调里调用，不能睡眠 |

### `struct mmu_notifier`（`:228`）

| 成员 | 中文 |
|---|---|
| `struct hlist_node hlist` | 挂到这个 mm 的 notifier 链上。链的修改要同时拿 mmap_lock 和反向映射锁 |
| `const struct mmu_notifier_ops *ops` | 上面那张回调表 |
| `struct mm_struct *mm` | 被监视的地址空间 |
| `struct rcu_head rcu` | 延迟释放 |
| `unsigned int users` | get/put 引用 |

### `enum mmu_notifier_event`（`:51`）

| 值 | 头文件注释里的原因 |
|---|---|
| `MMU_NOTIFY_UNMAP` | `munmap`，或 `mremap` 把区间搬走 |
| `MMU_NOTIFY_CLEAR` | 清页表项：`madvise`、一页换成另一页等 |
| `MMU_NOTIFY_PROTECTION_VMA` | `mprotect`。按 VMA 的 `vm_page_prot` 更新整个区间即可 |
| `MMU_NOTIFY_PROTECTION_PAGE` | 单页读写位变化。end 回调里要自己看 CPU 页表 |
| `MMU_NOTIFY_SOFT_DIRTY` | soft-dirty 记账，页和访问位都没变。end 回调里要把二级映射也标脏 |
| `MMU_NOTIFY_RELEASE` | mm refcount 到 0，区间不再可访问。给 interval notifier 的 invalidate 用 |
| `MMU_NOTIFY_MIGRATE` | `migrate_vma_collect` 期间。`owner` 对得上设备私有 pgmap 时，驱动可以忽略这次失效 |
| `MMU_NOTIFY_EXCLUSIVE` | 页表项转成 device-exclusive。`owner` 是 `make_device_exclusive()` 的调用者，用来过滤 |

`MMU_NOTIFIER_RANGE_BLOCKABLE` 是 `flags` 的 bit 0（`:62`）。

### `struct mmu_notifier_range`

`CONFIG_MMU_NOTIFIER` 下（`:304`）：

| 成员 | 中文 |
|---|---|
| `struct mm_struct *mm` | 哪个地址空间 |
| `unsigned long start` | 区间起（含） |
| `unsigned long end` | 区间止（不含） |
| `unsigned flags` | 含 `MMU_NOTIFIER_RANGE_BLOCKABLE` |
| `enum mmu_notifier_event event` | 上面的原因 |
| `void *owner` | 迁移 / exclusive 时用来过滤的设备所有者 |

没编 `CONFIG_MMU_NOTIFIER` 时（`:559`）这个结构只剩 `start` 和 `end`。`mmu_notifier_range_init` 变成只填这两个字段的宏（`:572`）。

`mm_has_notifiers`（`:313`）看 `mm->notifier_subscriptions`。KVM 注册之后这个指针非空，unmap / 保护位变化才会走到二级 MMU。

---

## 11.9 HMM：`hmm_range_fault`

**结论**：HMM 让设备按 CPU 页表的视图取 PFN，同时用 `mmu_interval_notifier` 盯着这段虚拟地址。区间在 fault 过程中被拆掉，调用者必须丢掉这次拿到的 PFN 重来。它不给设备建影子页表，影子页表仍是 KVM 那条路。

### `struct hmm_range`（`include/linux/hmm.h:111`）

| 成员 | 中文 |
|---|---|
| `struct mmu_interval_notifier *notifier` | 罩住 start/end 的 interval notifier。失效序号从它来 |
| `unsigned long notifier_seq` | `mmu_interval_read_begin()` 的返回值。walk 之后对不上就说明区间失效了 |
| `unsigned long start` | 虚拟地址起，含 |
| `unsigned long end` | 虚拟地址止，不含 |
| `unsigned long *hmm_pfns` | 输出数组，长度要盖住整个区间。每一项是 PFN 加 HMM 标志 |
| `unsigned long default_flags` | 这一段的默认标志（读、写等，见 `Documentation/mm/hmm.rst`） |
| `unsigned long pfn_flags_mask` | 掩掉 pfn 自带的标志，让 `default_flags` 说了算 |
| `void *dev_private_owner` | device private 页的 owner。对得上的页按设备自己的页处理 |

`hmm_range_fault`（`mm/hmm.c:762`）要求调用者已经拿着 mmap lock，返回时锁还在。函数体就是 `return hmm_range_fault_locked(range, NULL);`。注释写明：它和 `get_user_pages` 类似，但可以只读页表、不把缺页做成对页表的修改；新代码应改用 `hmm_range_fault_unlocked_timeout`（`:788`），除非必须把 mmap lock 一直拿在手里。这个 locked 版本伺候不了会放下 mmap lock 的 fault handler。

`hmm_range_fault_unlocked_timeout` 自己拿 mmap 读锁。锁被放下或区间失效时，它刷新 `notifier_seq` 再走一遍。`timeout` 为 0 表示一直重试。非 0 是调用者自己的预算，超时返回 `-EBUSY`，不会把已经在进行的缺页打断。`HMM_RANGE_DEFAULT_TIMEOUT` 是 1000（毫秒，`:136`）。

`hmm_pfn_to_map_order`（`:94`）从 hmm_pfn 里取出映射阶，调用前要先确认 `HMM_PFN_VALID`。

ZONE_DEVICE 页怎么迁移、GPU 的 device private 状态机，本节没有展开。入口就是上面这个 range。

---

## 11.10 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `kvm_set_memory_region` | `virt/kvm/kvm_main.c:2001` | 安装 / 删除 memslot |
| `gfn_to_memslot` | `include/linux/kvm_host.h:1226` | GFN → slot |
| `kvm_vcpu_gfn_to_memslot` | `:1228` | vcpu 热路径 |
| `handle_ept_violation` | `arch/x86/kvm/vmx/vmx.c:5972` | Intel EPT violation |
| `__vmx_handle_ept_violation` | `arch/x86/kvm/vmx/common.h:83` | 错误码转换后进 MMU |
| `npf_interception` | `arch/x86/kvm/svm/svm.c:1968` | AMD NPT |
| `kvm_mmu_page_fault` | `arch/x86/kvm/mmu/mmu.c:6573` | MMU 总入口 |
| `kvm_mmu_do_page_fault` | `:4986` | 填 `kvm_page_fault` 并分发 |
| `kvm_tdp_page_fault` | `:4976` | TDP |
| `kvm_tdp_mmu_page_fault` | `:4941` | TDP MMU |
| `kvm_tdp_mmu_map` | `arch/x86/kvm/mmu/tdp_mmu.c:1217` | 把 PFN 写入 EPT。`kvm_tdp_mmu_page_fault` 在 `:4967` 调用它 |
| `kvm_mmu_faultin_pfn` | `arch/x86/kvm/mmu/mmu.c:4694` | HVA 缺页，拿到 PFN。调用点在 `:4957` |
| `kvm_mmu_notifier_ops` | `virt/kvm/kvm_main.c:879` | KVM 注册的 mmu_notifier 回调表 |
| `kvm_init_mmu_notifier` | `:888` | 把 notifier 挂到 kvm |
| `hmm_range_fault` | `mm/hmm.c:762` | 按 CPU 页表填 `hmm_pfns`，调用者持 mmap lock |
| `hmm_range_fault_unlocked_timeout` | `:788` | 自己拿锁，失效了内部重试 |

---

[上一篇 swap table](./10-11-swap-table.md) · [目录](./README.md) · [下一节 balloon/FPR](./12-balloon-fpr-and-overcommit.md)
