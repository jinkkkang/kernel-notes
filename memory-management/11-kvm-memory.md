# 11. KVM 内存

**结论**：guest 眼里的物理地址（GPA）不是宿主物理地址（HPA）。x86 上硬件打开 EPT（Intel）或 NPT（AMD）时，CPU 走两张表：guest 自己的页表把 GVA 译成 GPA，EPT/NPT 把 GPA 译成 HPA。第二张表由 KVM 填。EPT violation / NPF 是第二张表没有可用映射，处理在宿主，入口是 `kvm_mmu_page_fault`。

> 源码树：Linux **v6.6.160**（`Makefile` 的 `VERSION/PATCHLEVEL/SUBLEVEL`）。
> 通用 memslot：`include/linux/kvm_host.h`。x86 MMU：`arch/x86/include/asm/kvm_host.h`、`arch/x86/kvm/mmu/`。
> 本环境没有虚拟化设备，嵌套和 EPT 大页没有跑 QEMU，操作步骤标 **RECIPE**。
> 导航：[上一篇 迁移](./10-10-migration.md) · [目录](./README.md) · [下一节 balloon/FPR](./12-balloon-fpr-and-overcommit.md)

名词先钉死：

| 名字 | 是什么 |
|---|---|
| GVA | guest 虚拟地址 |
| GPA | guest 物理地址。guest 内核以为这是物理页 |
| GFN | `GPA >> PAGE_SHIFT` |
| HVA | 宿主用户虚拟地址。qemu 进程里 `mmap` 出来的那块 |
| HPA / PFN | 宿主物理页。EPT 叶子里最终写下的就是它 |
| SPTE | KVM 建的影子页表项或 TDP 页表项，64 位 |

两套视角不要混：

| | guest 看见的 | 宿主看见的 |
|---|---|---|
| 虚拟地址 | GVA | qemu 进程的 HVA |
| 「物理」地址 | GPA | 仍是 HVA，直到 EPT/NPT 叶子钉到 HPA |
| 缺页 | guest 自己的缺页处理 | EPT violation / NPF → `kvm_mmu_page_fault` |
| 大页 | guest 的 THP / hugetlb | EPT 的页大小。两边独立，见 11.6 |

`struct kvm` 在 `include/linux/kvm_host.h:706`。和内存有关的是 `slots_lock`、两套 memslot、以及（`CONFIG_MMU_NOTIFIER` 且架构要 notifier 时）嵌进去的 `struct mmu_notifier`。`struct kvm_vcpu` 在 `kvm_host.h:323`，x86 的页表状态在 `vcpu->arch`（`struct kvm_vcpu_arch`，`asm/kvm_host.h`）。下面只展开内存相关的嵌套结构，不把 `kvm` / `kvm_vcpu` 的无关成员逐个列完。

---

## 11.1 memslot：GPA 区间 ↔ 宿主用户地址

qemu 用 ioctl `KVM_SET_USER_MEMORY_REGION`（`include/uapi/linux/kvm.h:1473`，参数 `struct kvm_userspace_memory_region`，`:91`）登记一段 GPA。x86 的 ioctl 进 `kvm_vm_ioctl_set_memory_region`（`virt/kvm/kvm_main.c:2065`），再进 `kvm_set_memory_region`（`:2053`，拿 `kvm->slots_lock`）和 `__kvm_set_memory_region`（`:1949`）。

`memory_size == 0` 是删除（`:1994`）。非 0 时 `base_gfn = guest_phys_addr >> PAGE_SHIFT`，`npages = memory_size >> PAGE_SHIFT`。已有 slot 不能改 `userspace_addr`、页数、或 `KVM_MEM_READONLY`，否则 `-EINVAL`（`:2017`）。只改 GPA 起点是 `KVM_MR_MOVE`，只改其它 flag（脏页日志）是 `KVM_MR_FLAGS_ONLY`。

`slot` 的低 16 位是 id，高 16 位是地址空间号 `as_id`（`:1964`）。x86 在 `CONFIG_KVM_SMM` 下 `KVM_ADDRESS_SPACE_NUM` 是 2（`asm/kvm_host.h:2149`），SMM 用另一套 memslot（`kvm_arch_vcpu_memslots_id`，`:2150`）。没开 SMM 时这个宏是 1（`kvm_host.h:84`）。用户态能用的 slot 数是 `KVM_USER_MEM_SLOTS = SHRT_MAX - KVM_INTERNAL_MEM_SLOTS`，x86 的内部 slot 是 3（`asm/kvm_host.h:57`）。

### `struct kvm_userspace_memory_region`（`include/uapi/linux/kvm.h:91`）

```c
struct kvm_userspace_memory_region {
	__u32 slot;			/* 低 16 位 id，高 16 位地址空间 */
	__u32 flags;			/* 见下表。bit 16 起留给内核 */
	__u64 guest_phys_addr;		/* GPA 起点，要按页对齐 */
	__u64 memory_size;		/* 字节。0 表示删掉这个 slot */
	__u64 userspace_addr;		/* 对应的 HVA */
};
```

用户可见的 flag（`:104`，注释 `:100` 写明只有 bit 0 到 bit 15 给用户态）：

| 宏 | 值 | 作用 |
|---|---|---|
| `KVM_MEM_LOG_DIRTY_PAGES` | `1UL << 0` | 记脏页，迁移时问 guest 写过哪些 GPA |
| `KVM_MEM_READONLY` | `1UL << 1` | 这段 GPA 对 guest 只读 |

内核自己用的 `KVM_MEMSLOT_INVALID` 是 `1UL << 16`（`kvm_host.h:57`），slot 正在删或挪时置上。`__kvm_faultin_pfn` 看见它就 `RET_PF_RETRY`（`mmu.c:4270`）。

### `struct kvm_memory_slot`（`kvm_host.h:581`）

头文件 `:565` 说明：每个 slot 同时挂在两套 memslot 集合里（一套正在用，一套正在改），所以每个链节点都是 `[2]`。改的时候 VM 继续跑旧的那套。

```c
struct kvm_memory_slot {
	struct hlist_node id_node[2];		/* 按 slot id 哈希，双份 */
	struct interval_tree_node hva_node[2];	/* 按 HVA 区间树，双份 */
	struct rb_node gfn_node[2];		/* 按 GFN 红黑树，双份 */
	gfn_t base_gfn;				/* 这段 GPA 的起始页帧号 */
	unsigned long npages;			/* 页数。0 表示这个 slot 是空的 */
	unsigned long *dirty_bitmap;		/* 脏页位图。没开日志就是 NULL */
	struct kvm_arch_memory_slot arch;	/* 架构私有。x86 见下 */
	unsigned long userspace_addr;		/* HVA */
	u32 flags;				/* 用户 flag 加上 KVM_MEMSLOT_INVALID */
	short id;				/* slot 号 */
	u16 as_id;				/* 地址空间。SMM 是另一套 */
};
```

`struct kvm` 里每个地址空间有两份实体和一枚当前指针（`kvm_host.h:726`）：

```c
struct kvm_memslots __memslots[KVM_ADDRESS_SPACE_NUM][2];
struct kvm_memslots __rcu *memslots[KVM_ADDRESS_SPACE_NUM];
```

### `struct kvm_memslots`（`kvm_host.h:689`）

```c
struct kvm_memslots {
	u64 generation;				/* 代际。改 slot 就变，用来发现过期 */
	atomic_long_t last_used_slot;		/* 上次命中的 slot，热路径少搜树 */
	struct rb_root_cached hva_tree;	/* HVA 区间树的根 */
	struct rb_root gfn_tree;		/* GFN 树的根 */
	/*
	 * id → slot。7 位桶，注释 :697：对齐以前 512 个 slot 的数组大小。
	 */
	DECLARE_HASHTABLE(id_hash, 7);
	int node_idx;				/* 用 id_node/hva_node/gfn_node 的哪一份 */
};
```

查找：`gfn_to_memslot`（`kvm_main.c:2345`）走 `__gfn_to_memslot`（`kvm_host.h:1727`）。vcpu 热路径是 `kvm_vcpu_gfn_to_memslot`（`kvm_main.c:2351`），先试 `vcpu->last_used_slot`，代际对不上就丢掉缓存。

### x86 `struct kvm_arch_memory_slot`（`asm/kvm_host.h:1024`）

```c
struct kvm_lpage_info {
	int disallow_lpage;			/* 非 0 就不能在这一级建大页 */
};

struct kvm_arch_memory_slot {
	/*
	 * 反向映射。一个 GFN 被哪些影子页表项指着。
	 * KVM_NR_PAGE_SIZES = PG_LEVEL_1G - PG_LEVEL_4K + 1 = 3（4K / 2M / 1G）。
	 */
	struct kvm_rmap_head *rmap[KVM_NR_PAGE_SIZES];
	/* 下标 0 是 2M，下标 1 是 1G。4K 没有这一级 */
	struct kvm_lpage_info *lpage_info[KVM_NR_PAGE_SIZES - 1];
	/*
	 * 有人在盯这个 GFN 的写（模拟页表写）时不能用大页，
	 * 也不能直接映射成可写。
	 */
	unsigned short *gfn_write_track;
};
```

`struct kvm_rmap_head`（`asm/kvm_host.h:394`）只有一个 `unsigned long val`。编码在 `mmu.c:918`：bit 0 是 0 时，`val` 就是唯一那条 SPTE 的指针；bit 0 是 1 时，`(val & ~1)` 指向 `struct pte_list_desc`（`mmu.c:146`），里面最多 `PTE_LIST_EXT`（14）个 SPTE 指针。TDP MMU 的主路径不靠这张 rmap，影子页表靠它。

---

## 11.2 两层 MMU：影子页表和 TDP

硬件能力和 KVM 用哪套代码是两件事。

| | `tdp_enabled` | `tdp_mmu_enabled` |
|---|---|---|
| 意思 | CPU 在走 EPT/NPT。guest 页表和 EPT 各走各的 | 用 `tdp_mmu.c` 这套直接页表，而不是 `direct_map` |
| 谁设置 | `kvm_configure_mmu`（`mmu.c:5891`），厂商模块加载时传入 | 只在 `CONFIG_X86_64` 里存在（`:110`）。模块参数 `tdp_mmu`，`0444`，初值 `true` |
| 关掉时 | `init_kvm_softmmu`：影子页表。`kvm_configure_mmu` 把 `tdp_mmu_enabled` 清掉 | `kvm_tdp_page_fault` 落到 `direct_page_fault` |

`kvm_configure_mmu` 在 64 位上做 `tdp_mmu_enabled = tdp_mmu_allowed && tdp_enabled`（`:5899`）。`tdp_mmu_allowed` 是模块初始化时对参数的快照（`kvm_mmu_x86_module_init`，`:6905`）。所以：

- 64 位、硬件有 EPT/NPT、参数没关：TDP MMU。`mmu_lock` 缺页路径用读锁。
- 64 位、硬件有 EPT/NPT、参数关掉：仍是直接映射（GPA 已经是缺页地址），但走 `direct_page_fault` / `direct_map`，写锁。
- 没有硬件二维页表，或 32 位：影子页表。`paging_tmpl.h` 按 guest PTE 宽度编三次（`mmu.c:4694` 起：EPT、64 位、32 位）。

`struct kvm_vcpu_arch` 里挂着几套 `struct kvm_mmu`（`asm/kvm_host.h:762`）。`struct kvm_vcpu` 本身在 `kvm_host.h:323`，下面这些是它的 `arch` 成员，不是把整个 vcpu 列完。

```c
	/* 当前用来处理缺页的那一套。kvm_mmu_create 里先指到 root_mmu */
	struct kvm_mmu *mmu;			/* :762 */

	struct kvm_mmu root_mmu;		/* :765，L1 非嵌套 */
	struct kvm_mmu guest_mmu;		/* :768，跑 L2 时 L1 的 MMU */

	/*
	 * L2 的走表上下文（嵌套 NPT/EPT 用）。
	 * 注释 :770：只用来走页表，不在宿主上处理 L2 自己的缺页。
	 */
	struct kvm_mmu nested_mmu;		/* :778 */

	/* 当前 gva_to_gpa 用的那一套。嵌套时指到 nested_mmu */
	struct kvm_mmu *walk_mmu;		/* :784 */

	/* 分配页表页的缓存。mmu_topup_memory_caches（mmu.c:685）来补 */
	struct kvm_mmu_memory_cache mmu_pte_list_desc_cache;
	struct kvm_mmu_memory_cache mmu_shadow_page_cache;
	struct kvm_mmu_memory_cache mmu_shadowed_info_cache;
	struct kvm_mmu_memory_cache mmu_page_header_cache;
```

`kvm_init_mmu`（`mmu.c:5417`）选哪一套：

```text
kvm_init_mmu
  mmu_is_nested(vcpu)          # x86.h:183，walk_mmu == &nested_mmu
    init_kvm_nested_mmu        # :5378，只填 nested_mmu 的走表函数
  否则 tdp_enabled
    init_kvm_tdp_mmu           # :5210，root_mmu.page_fault = kvm_tdp_page_fault
  否则
    init_kvm_softmmu           # :5366，影子页表
```

`init_kvm_tdp_mmu` 把 `root_role.direct` 设成 1（`kvm_calc_tdp_mmu_root_page_role`，`:5204`）。direct 的意思是：缺页地址已经是 GPA，不用先走 guest 页表。

影子页表的 `page_fault`（`shadow_mmu_init_context`，`:5250`）：

| guest 分页 | 函数 | 模板 |
|---|---|---|
| `CR0.PG = 0` | `nonpaging_page_fault` | 直接映射，最大 2M（`mmu.c:4419`） |
| PAE（长模式也算） | `paging64_page_fault` | `paging_tmpl.h`，`PTTYPE == 64` |
| 否则 | `paging32_page_fault` | `PTTYPE == 32` |

`FNAME(page_fault)` 的函数体在 `paging_tmpl.h:777`。`mmu.c:4694` 把这个头文件包含三次，宏 `FNAME` 展开成上面三个名字，外加嵌套用的 `ept_page_fault`。

### `struct kvm_mmu`（`asm/kvm_host.h:437`）全部成员

这是 vcpu 上的一套 MMU，不是一页页表。页表页是下一节的 `kvm_mmu_page`。

```c
struct kvm_mmu {
	/* --- 回调：这一套 MMU 怎么走、怎么缺页 --- */
	unsigned long (*get_guest_pgd)(struct kvm_vcpu *vcpu); /* 取 guest CR3 或 EPTP */
	u64 (*get_pdptr)(struct kvm_vcpu *vcpu, int index); /* PAE 的 PDPT */
	int (*page_fault)(struct kvm_vcpu *vcpu,
			  struct kvm_page_fault *fault); /* TDP 时是 kvm_tdp_page_fault */
	/* 向 guest 注入缺页 */
	void (*inject_page_fault)(struct kvm_vcpu *vcpu,
				  struct x86_exception *fault);
	/* 软件走 guest 页表。TDP 下仍用来做 GVA→GPA，给模拟指令用 */
	gpa_t (*gva_to_gpa)(struct kvm_vcpu *vcpu, struct kvm_mmu *mmu,
			    gpa_t gva_or_gpa, u64 access,
			    struct x86_exception *exception);
	/* 把影子 PTE 和 guest PTE 对齐。TDP 直接映射里是 NULL */
	int (*sync_spte)(struct kvm_vcpu *vcpu,
			 struct kvm_mmu_page *sp, int i);

	/* --- 当前根 --- */
	struct kvm_mmu_root_info root;		/* pgd 是 guest 侧根，hpa 是根页的 HPA */
	union kvm_cpu_role cpu_role;		/* CR0/CR4/EFER。没变就不必重配 */
	union kvm_mmu_page_role root_role;	/* direct==1 表示缺页地址已是 GPA */

	/*
	 * 保护键。16 个域，按下标是缺页错误码的 [4:1]，
	 * 每个域 2 bit，和 PKRU 的 AD/WD 相与。注释 :452。
	 */
	u32 pkru_mask;

	/*
	 * 以前的根，避免每次切 CR3 都拆掉。
	 * KVM_MMU_NUM_PREV_ROOTS 是 3（:421）。
	 */
	struct kvm_mmu_root_info prev_roots[KVM_MMU_NUM_PREV_ROOTS];

	/*
	 * 权限检查。下标是错误码 [4:1]，一字节里的 bit 是 ACC_*。
	 * 置位表示这种组合算权限错。注释 :462。
	 */
	u8 permissions[16];

	u64 *pae_root;				/* 32 位 PAE 的根 */
	u64 *pml4_root;				/* 4 级根 */
	u64 *pml5_root;				/* 5 级根 */

	/* 影子 PTE 上不允许出现的位，含硬件保留位和 KVM 不用的位 */
	struct rsvd_bits_validate shadow_zero_check;
	struct rsvd_bits_validate guest_rsvd_check; /* guest PTE 的保留位 */

	u64 pdptrs[4];				/* PAE */
};
```

`struct kvm_mmu_root_info`（`:413`）是 `gpa_t pgd` 加 `hpa_t hpa`。无效值两边都是 `INVALID_PAGE`（`KVM_MMU_ROOT_INFO_INVALID`，`:418`）。`INVALID_PAGE` 是 `~(hpa_t)0`（`:134`）。

`struct rsvd_bits_validate`（`:408`）：

```c
struct rsvd_bits_validate {
	/* [0] 是普通页，[1] 是大页。第二维是级别，PT64_ROOT_MAX_LEVEL 是 5 */
	u64 rsvd_bits_mask[2][PT64_ROOT_MAX_LEVEL];
	u64 bad_mt_xwr;				/* 非法的内存类型 × 权限组合 */
};
```

### `union kvm_mmu_page_role`（`:325`）

整体是一个 `u32 word`。注释 `:305` 解释这些位为什么能把同一个 GFN 的上级影子页种类压到大约 2^13 以内。

```c
union kvm_mmu_page_role {
	u32 word;
	struct {
		unsigned level:4;		/* 这一页在第几级 */
		unsigned has_4_byte_gpte:1;	/* guest PTE 是 4 字节 */
		unsigned quadrant:2;		/* 32 位影子页把 4K 拆成象限 */
		unsigned direct:1;		/* 直接映射。没有 guest 页表要影子化 */
		unsigned access:3;		/* 访问权限，ACC_* */
		unsigned invalid:1;		/* 角色已失效 */
		unsigned efer_nx:1;		/* EFER.NX */
		unsigned cr0_wp:1;		/* CR0.WP */
		unsigned smep_andnot_wp:1;	/* SMEP 且 WP=0 */
		unsigned smap_andnot_wp:1;	/* SMAP 且 WP=0 */
		unsigned ad_disabled:1;		/* 不用 A/D 位 */
		unsigned guest_mode:1;		/* 嵌套：这是 L2 */
		unsigned passthrough:1;		/* 5 级根透传 4 级 guest */
		unsigned :5;			/* 填充 */

		/*
		 * SMM。放在 word 的高位，kvm_memslots_for_spte_role 用一条移位取出。
		 * 给了一整个字节。注释 :343。
		 */
		unsigned smm:8;
	};
};
```

`union kvm_mmu_extended_role`（`:372`）补「页角色里没有、但换 CR 时要比较」的位。`valid` 用来区分「全 0」和「真的配过」（注释 `:357`）。

```c
union kvm_mmu_extended_role {
	u32 word;
	struct {
		unsigned int valid:1;
		unsigned int execonly:1;	/* EPT execute-only */
		unsigned int cr4_pse:1;
		unsigned int cr4_pke:1;
		unsigned int cr4_smap:1;
		unsigned int cr4_smep:1;
		unsigned int cr4_la57:1;
		unsigned int efer_lma:1;
	};
};
```

`union kvm_cpu_role`（`:386`）把上面两个拼成 `u64 as_u64`：`base` 是页角色，`ext` 是扩展角色。

---

## 11.3 `struct kvm_mmu_page`（`mmu_internal.h:51`）

一页影子页表，或一页 TDP 页表。注释要求 `link` 到 `spt` 落在一条 64 字节 cache line 里。

```c
struct kvm_mmu_page {
	/* --- 链和身份。link 到 spt 要在同一条 cache line --- */
	struct list_head link;			/* 活动页链表 */
	struct hlist_node hash_link;		/* 按 (gfn, role) 哈希 */
	bool tdp_mmu_page;			/* 1：TDP MMU 的页。0：影子 MMU */
	bool unsync;				/* 影子页和 guest 页表不一致 */
	union {
		u8 mmu_valid_gen;		/* 代际。整代 zap 时作废 */
		/* 只在 slots_lock 下用。这个根已经排进去要拆 */
		bool tdp_mmu_scheduled_root_to_zap;
	};
	/*
	 * NX 大页缓解：这一页不能被等价大页换掉，因为它映了 guest 的可执行页。
	 */
	bool nx_huge_page_disallowed;

	/* 哈希键 */
	union kvm_mmu_page_role role;
	gfn_t gfn;				/* 这一页影子的是哪个 guest 页帧 */

	u64 *spt;				/* 页表本身，512 个 64 位项 */
	/*
	 * 每个 SPTE 影子化的结果。高位是 GFN，低位是 KVM 的 ACC_*，
	 * 不是 guest 原始格式。注释 :84。
	 */
	u64 *shadowed_translation;

	/* --- 根的引用，和父项 --- */
	union {
		int root_count;			/* 作为活动根的次数。非 TDP */
		refcount_t tdp_mmu_root_count;	/* TDP 根的引用 */
	};
	unsigned int unsync_children;		/* 下面有多少不同步的子页 */
	union {
		struct kvm_rmap_head parent_ptes; /* 哪些父 SPTE 指着这一页 */
		tdp_ptep_t ptep;		/* TDP：父项指针。类型是 u64 __rcu * */
	};
	DECLARE_BITMAP(unsync_child_bitmap, 512); /* 哪个子项不同步 */

	/*
	 * 拆掉它就能再建 NX 大页的那些页。已经因为别的原因禁止大页的
	 * 不在这条链上。注释 :110。
	 */
	struct list_head possible_nx_huge_page_link;
#ifdef CONFIG_X86_32
	/* 无锁读 SPTE 时的顺序计数。见 __get_spte_lockless */
	int clear_spte_count;
#endif
	/* 上次遍历以来被写的次数。写太密就拆掉这一级 */
	atomic_t write_flooding_count;
#ifdef CONFIG_X86_64
	/* TDP 页异步释放 */
	struct rcu_head rcu_head;
#endif
};
```

`tdp_ptep_t` 在 `mmu_internal.h:49`：`typedef u64 __rcu *tdp_ptep_t`。TDP 的页表项用 RCU 保护，所以 `kvm_tdp_mmu_page_fault` 可以拿 `mmu_lock` 的读锁（`tdp_iter.h:11` 的注释）。

---

## 11.4 `struct tdp_iter`（`tdp_iter.h:78`）

TDP MMU 走树的游标。前序遍历，不加影子那一层。`for_each_tdp_pte`（`:128`）从根走到 `[start, end)`。

```c
struct tdp_iter {
	/*
	 * 迭代器要走向的那个最后一级 GFN。
	 */
	gfn_t next_last_level_gfn;
	/*
	 * 上次让出 CPU 时的 next_last_level_gfn。
	 * 只有两者不同才让出，保证有进展。
	 */
	gfn_t yielded_gfn;
	/* 从根走到当前项经过的页表。PT64_ROOT_MAX_LEVEL 是 5 */
	tdp_ptep_t pt_path[PT64_ROOT_MAX_LEVEL];
	tdp_ptep_t sptep;			/* 当前 SPTE 的指针 */
	gfn_t gfn;				/* 当前 SPTE 映射的最低 GFN */
	int root_level;				/* 根的级别 */
	int min_level;				/* 最深走到哪一级。大页可以停在 2M/1G */
	int level;				/* 当前级别 */
	int as_id;				/* 地址空间。SMM 还是普通 */
	u64 old_spte;				/* sptep 上的值的快照 */
	bool valid;				/* 还没走出树 */
	/*
	 * 中途放下过 mmu_lock。下次 tdp_iter_next() 要从根重走。
	 */
	bool yielded;
};
```

`kvm_tdp_mmu_map`（`tdp_mmu.c:1041`）用它把缺页得到的 PFN 装进 EPT/NPT。级别等于 `fault->goal_level` 时写下叶子。

---

## 11.5 `struct kvm_page_fault`（`mmu_internal.h:189`）

一次缺页的输入和输出。`kvm_mmu_do_page_fault`（`:280`）在栈上填它。没写进初始化列表的成员是 0：`pfn`、`hva`、`map_writable`、`gfn`、`slot` 都从 0 开始，后面的函数再填。

```c
struct kvm_page_fault {
	/* --- kvm_mmu_do_page_fault 的参数 --- */
	const gpa_t addr;			/* 出错地址。TDP 下是 GPA */
	const u32 error_code;			/* 低 32 位错误码 */
	const bool prefetch;			/* 预取，不是真的 VM-exit */

	/* 从 error_code 拆出来的 */
	const bool exec;
	const bool write;
	const bool present;
	const bool rsvd;
	const bool user;

	/* 从 mmu 和全局状态来的 */
	const bool is_tdp;			/* page_fault == kvm_tdp_page_fault */
	const bool nx_huge_page_workaround_enabled;

	/* 因为 NX 大页，这一次不许建 >4KB */
	bool huge_page_disallowed;

	/*
	 * 这次缺页允许的最大级别。交给 FNAME(fetch)、direct_map、kvm_tdp_mmu_map。
	 * 初值 KVM_MAX_HUGEPAGE_LEVEL，也就是 PG_LEVEL_1G。
	 */
	u8 max_level;
	/*
	 * max_level 再和宿主映射页大小取最小。初值 PG_LEVEL_4K。
	 */
	u8 req_level;
	/* 再扣掉 huge_page_disallowed 之后真正要建的级别。初值 PG_LEVEL_4K */
	u8 goal_level;

	/* addr 右移，或者 addr 是 GVA 时走完 guest 页表得到的 GFN */
	gfn_t gfn;
	struct kvm_memory_slot *slot;		/* 可以是 NULL（MMIO、没有 slot） */

	/* --- kvm_faultin_pfn 的输出 --- */
	unsigned long mmu_seq;			/* 开始 fault-in 时的失效序号 */
	kvm_pfn_t pfn;				/* 宿主 PFN */
	hva_t hva;				/* 对应的 HVA */
	bool map_writable;			/* 宿主页可写，EPT 才能建成可写 */

	/*
	 * guest 在写自己的页表。要模拟这条写，不能当成普通数据写。
	 */
	bool write_fault_to_shadow_pgtable;
};
```

`enum pg_level`（`arch/x86/include/asm/pgtable_types.h:550`）：`PG_LEVEL_NONE = 0`，`PG_LEVEL_4K`，`PG_LEVEL_2M`，`PG_LEVEL_1G`。所以 4K 的值是 1，不是 0。

返回值（`mmu_internal.h:271`）。新名字要在 `mmutrace.h` 里 `TRACE_DEFINE_ENUM`。全部 `>= 0`，好和 `-errno` 分开。

| 名字 | 值 | 意思 |
|---|---|---|
| `RET_PF_CONTINUE` | 0 | 到目前为止没问题，继续 |
| `RET_PF_RETRY` | 1 | 让 CPU 再缺一次页 |
| `RET_PF_EMULATE` | 2 | MMIO 或要模拟这条指令 |
| `RET_PF_INVALID` | 3 | SPTE 无效，走真正的缺页路径 |
| `RET_PF_FIXED` | 4 | 已经修好 |
| `RET_PF_SPURIOUS` | 5 | 别的 vCPU 已经修好 |

---

## 11.6 guest 缺页 → EPT violation

guest 里一次写匿名页，其实是两次翻译。guest 页表没有映射时，guest 内核自己处理，宿主看不到。guest 页表有了、EPT 没有时，才 VM-exit。

```text
guest 用户态写 GVA
  guest 页表没有映射 → guest 内核分配「物理页」，填 guest PTE
  这条 GPA 在 EPT 里还没有
  VM-exit

Intel：
  handle_ept_violation                         # vmx/vmx.c:5745
    读 VMCS 的 GPA 和 exit qualification
    trace_kvm_page_fault                       # 定义在 arch/x86/kvm/trace.h:402
    把 EPT_VIOLATION_ACC_* 收成 PFERR_* error_code   # :5767
    非法 GPA（allow_smaller_maxphyaddr）→ 直接模拟指令
    kvm_mmu_page_fault(vcpu, gpa, error_code, NULL, 0)  # :5796

AMD，NPT 打开：
  npf_interception                             # svm/svm.c:2069
    exit_info_2 是 GPA，exit_info_1 是错误码
    kvm_mmu_page_fault                         # :2077

没开 NPT 的 AMD #PF 不走这条，走 pf_interception → kvm_handle_page_fault（svm.c:2056）。

kvm_mmu_page_fault                             # mmu.c:5709
  错误码撞上 PFERR_IMPLICIT_ACCESS：清掉这一位再继续
  根无效：RET_PF_RETRY
  保留位错：handle_mmio_page_fault             # MMIO 走模拟
  否则 kvm_mmu_do_page_fault                   # mmu_internal.h:280
    填 struct kvm_page_fault
    传入的 error_code 只留低 32 位
    direct 角色：gfn = addr >> PAGE_SHIFT
                 slot = kvm_vcpu_gfn_to_memslot
    is_tdp = (mmu->page_fault == kvm_tdp_page_fault)   # :292
    CONFIG_RETPOLINE 且 is_tdp：直接 kvm_tdp_page_fault
    否则 mmu->page_fault()

kvm_tdp_page_fault                             # mmu.c:4495
  非一致 DMA 且要看 guest MTRR：把 max_level 收到内存类型一致的那一级
  CONFIG_X86_64 且 tdp_mmu_enabled
    → kvm_tdp_mmu_page_fault                   # :4457
  否则
    → direct_page_fault                        # :4370，直接映射，但是写锁 + direct_map

kvm_tdp_mmu_page_fault
  page_fault_handle_page_track                 # 有人在跟踪写：RET_PF_EMULATE
  fast_page_fault                              # :3430，不拿 mmu_lock
    返回不是 RET_PF_INVALID 就结束
    能修两种：access tracking 把 SPTE 收走之后恢复（KVM 关了 A/D）；
    或者 present 的写保护，且 SPTE 是 MMU-writable，把 Writable 打开。
    大页并且开了脏页日志则留给慢路径。注释 :3476：锁外修复只支持 direct MMU
  mmu_topup_memory_caches                      # 准备分配页表页的 cache
  kvm_faultin_pfn                              # :4321。GPA → 宿主 PFN
  read_lock(kvm->mmu_lock)                     # TDP MMU 用读锁
  is_page_fault_stale ? 重试
    : kvm_tdp_mmu_map                          # tdp_mmu.c:1041
         kvm_mmu_hugepage_adjust               # 决定 goal_level
         tdp_iter 往下走，把 PFN 写入叶子

direct_page_fault（:4370）在拿锁之前同样先走 page track 和 fast_page_fault。拿的是 write_lock，锁里还有 make_mmu_pages_available，然后 is_page_fault_stale，最后 direct_map（:3223）。
```

`kvm_faultin_pfn` 是两层交汇点。它先记 `mmu_invalidate_seq`（`:4326`），再叫 `__kvm_faultin_pfn`（`:4260`）。

```text
__kvm_faultin_pfn
  slot 带 KVM_MEMSLOT_INVALID → RET_PF_RETRY
  不是可见 slot：
    L2 看不到私有 slot，pfn = KVM_PFN_NOSLOT
    APIC access 页被关掉 → RET_PF_EMULATE
  __gfn_to_pfn_memslot(..., async=&async, write, &map_writable, &hva)
       # kvm_main.c:2746
       HVA = gfn 在 slot 里的 userspace_addr
       hva_to_pfn                               # :2695
         hva_to_pfn_fast                        # 可写的快路径，get_user_page_fast_only
         否则 hva_to_pfn_slow                   # :2543
           get_user_pages_unlocked              # 宿主页不在内存里，就在这里缺页
  async 为真且允许异步 PF：kvm_arch_setup_async_pf，返回 RET_PF_RETRY
  否则再叫一次 __gfn_to_pfn_memslot，这次允许睡眠等 IO
```

guest 觉得自己缺的是 GPA。宿主在 `get_user_pages_unlocked` 里缺的是 qemu 进程的匿名页或 hugetlb 页。宿主页如果还没分配，宿主上是普通的 minor fault，guest 停在这次 VM-exit 里。

大页：`kvm_mmu_hugepage_adjust`（`mmu.c:3167`）先看是不是可执行且开了 NX 缓解。脏页日志开着就直接返回，保持 4K（`:3180`）。否则 `req_level = kvm_mmu_max_mapping_level`（`:3146`）：slot 的 `disallow_lpage` 和 `host_pfn_mapping_level`（`:3082`，顺着宿主 `pgd/p4d/pud/pmd` 看这一 HVA 是 4K、2M 还是 1G）取最小。`goal_level` 默认等于 `req_level`。所以宿主用 4K 映射时，guest 的 2M 大页在 EPT 里也建成 4K。要 EPT 大页，宿主这一段也得是大页（hugetlb 或 THP）并且对齐。本环境没有 `/dev/kvm`，这条没有实测，标 RECIPE。

---

## 11.7 嵌套

**结论**：L1 给 L2 开了 EPT/NPT 时，KVM 把 `vcpu->arch.mmu` 换成 `guest_mmu`，用影子 EPT/NPT 去影子化 L1 的第二张表。这时 `page_fault` 是 `ept_page_fault` 或影子 NPT，`is_tdp` 为假。`nested_mmu` 只负责走 L2 的页表。

Intel，`nested_cpu_has_ept(vmcs12)` 时调用 `nested_ept_init_mmu_context`（`vmx/nested.c:430`，调用点 `:2620`）：

```text
vcpu->arch.mmu = &vcpu->arch.guest_mmu
kvm_init_shadow_ept_mmu                     # mmu.c:5337
  guest_mmu.page_fault = ept_page_fault     # paging_tmpl.h 的 EPT 那一次编译
  guest_mmu 的 direct = 0，guest_mode = 1
vcpu->arch.walk_mmu = &vcpu->arch.nested_mmu
```

`ept_page_fault` 开头 `WARN_ON_ONCE(fault->is_tdp)`（`paging_tmpl.h:783`）。它走 L1 的 EPT，得到 L1 GPA，再 `kvm_faultin_pfn` 把这个 L1 GPA 变成宿主 PFN，最后装进 L0 的影子 EPT。L0 的 VMCS EPTP 指的是这张影子表，不是 L1 的 EPT。

AMD 对应 `nested_svm_init_mmu_context`（`svm/nested.c:84`）：`mmu` 指到 `guest_mmu`，`kvm_init_shadow_npt_mmu`（`mmu.c:5286`），`walk_mmu` 指到 `nested_mmu`。

`init_kvm_nested_mmu` 的注释（`mmu.c:5397`）把两个指针分开：`arch.mmu->gva_to_gpa` 用 L1 的嵌套页表把 L2 GPA 译成 L1 GPA；`arch.nested_mmu.gva_to_gpa` 先走 L2 页表再走 L1 的嵌套页表。`nested_mmu` 不处理缺页（`asm/kvm_host.h:770`）。

离开 L2 时 `nested_ept_uninit_mmu_context`（`vmx/nested.c:443`）把 `mmu` 和 `walk_mmu` 都指回 `root_mmu`。

`mmu_is_nested`（`x86.h:183`）看的是 `walk_mmu == &nested_mmu`，不是看 `page_fault` 函数指针。`is_tdp` 才比较函数指针。

本环境没有 `/dev/kvm`，没有跑嵌套虚拟机。上面是读 `vmx/nested.c`、`svm/nested.c`、`mmu.c` 得到的调用关系，不是实测。要在有 KVM 的机器上验证，标 RECIPE：开嵌套，看 `trace_kvm_page_fault` 的错误码，并确认 L2 运行时 `mmu->page_fault` 不是 `kvm_tdp_page_fault`。

---

## 11.8 `mmu_notifier`：宿主改页表时拆掉 EPT

**结论**：KVM 在 `struct kvm` 里嵌了一个 `struct mmu_notifier`（`include/linux/kvm_host.h:789`，要 `CONFIG_MMU_NOTIFIER` 并且架构定义了 `KVM_ARCH_WANT_MMU_NOTIFIER`；x86 在 `asm/kvm_host.h:2156`）。宿主 munmap、迁移、mprotect、回收清 young、KSM 换页时，回调进 `virt/kvm/kvm_main.c` 的 `kvm_mmu_notifier_ops`（`:921`），把对应 HVA 上的 EPT/NPT 拆掉、改掉或清 accessed。不注册的话，宿主把页换走之后 guest 仍可能从旧 EPT 访问那一页。

`kvm_init_mmu_notifier`（`:931`）把 `ops` 填上，再 `mmu_notifier_register(&kvm->mmu_notifier, current->mm)`。没编 notifier 时这个函数直接返回 0（`:939`）。

| ops 成员 | KVM 函数 | 行 | 做什么 |
|---|---|---|---|
| `invalidate_range_start` | `kvm_mmu_notifier_invalidate_range_start` | `:770` | `kvm_unmap_gfn_range`，拆 SPTE |
| `invalidate_range_end` | `kvm_mmu_notifier_invalidate_range_end` | `:834` | `mmu_invalidate_seq++`，叫醒等 slot 的人 |
| `clear_flush_young` | `kvm_mmu_notifier_clear_flush_young` | `:866` | `kvm_age_gfn`，并按返回值刷 TLB |
| `clear_young` | `kvm_mmu_notifier_clear_young` | `:877` | 同样 age，不刷二级 TLB。注释 `:884` |
| `test_young` | `kvm_mmu_notifier_test_young` | `:900` | 只查询 |
| `change_pte` | `kvm_mmu_notifier_change_pte` | `:716` | KSM 把 PTE 指到新共享页时，`kvm_set_spte_gfn` |
| `release` | `kvm_mmu_notifier_release` | `:910` | `kvm_flush_shadow_all` |

KVM 没填 `arch_invalidate_secondary_tlbs`、`alloc_notifier`、`free_notifier`。

`clear_young` 的注释写明：即使不刷 TLB，Haswell 之前的 Intel EPT 没有 Access 位，age 只能拆表。当前实现不从 `clear_young` 发二级 TLB 刷新。

### `struct mmu_notifier_ops`（`include/linux/mmu_notifier.h:64`）

回调都可以是 NULL。`clear_flush_young`、`clear_young`、`test_young` 的返回类型是 `int`。

```c
struct mmu_notifier_ops {
	/*
	 * mm 销毁（exit_mmap）或 mmu_notifier_unregister 时调用，在页被释放之前。
	 * 要拆掉全部二级映射并冻住二级 MMU。可以和 mm 外的其它回调并发。
	 */
	void (*release)(struct mmu_notifier *subscription,
			struct mm_struct *mm);

	/*
	 * 主 MMU 清 young/accessed 之后调用。二级页表也要 test-and-clear，
	 * 并且刷二级 TLB。返回值表示二级映射是否年轻。
	 */
	int (*clear_flush_young)(struct mmu_notifier *subscription,
				 struct mm_struct *mm,
				 unsigned long start,
				 unsigned long end);

	/* 同 clear_flush_young，允许不刷二级 TLB */
	int (*clear_young)(struct mmu_notifier *subscription,
			   struct mm_struct *mm,
			   unsigned long start,
			   unsigned long end);

	/* 只查询二级 PTE 的 young，不清、不拆 */
	int (*test_young)(struct mmu_notifier *subscription,
			  struct mm_struct *mm,
			  unsigned long address);

	/*
	 * PTE 换成另一页。头文件注释 :127 的例子是 KSM 把 PTE 指到新的共享页。
	 */
	void (*change_pte)(struct mmu_notifier *subscription,
			   struct mm_struct *mm,
			   unsigned long address,
			   pte_t pte);

	/*
	 * 区间里的页都还映射着，refcount 至少 1。
	 * blockable 为假时不能睡眠，需要睡眠就返回 -EAGAIN。
	 * 能失败的 start 不允许再实现 end。注释 :177。
	 */
	int (*invalidate_range_start)(struct mmu_notifier *subscription,
				      const struct mmu_notifier_range *range);
	/*
	 * 区间已经解映射，VM 已经把页释放。
	 * start 到 end 之间页表项会被拆掉。
	 */
	void (*invalidate_range_end)(struct mmu_notifier *subscription,
				     const struct mmu_notifier_range *range);

	/*
	 * 和 CPU 共享页表的非 CPU TLB。持 PTL 时调用，不能睡眠。
	 * 实现了这个就不要再实现 start/end。
	 */
	void (*arch_invalidate_secondary_tlbs)(
					struct mmu_notifier *subscription,
					struct mm_struct *mm,
					unsigned long start,
					unsigned long end);

	struct mmu_notifier *(*alloc_notifier)(struct mm_struct *mm);
	/* 引用放完、没有回调在跑之后，从 SRCU 回调里调用，不能睡眠 */
	void (*free_notifier)(struct mmu_notifier *subscription);
};
```

### `struct mmu_notifier`（`:237`）

```c
struct mmu_notifier {
	struct hlist_node hlist;		/* 挂到这个 mm 的 notifier 链 */
	const struct mmu_notifier_ops *ops;
	struct mm_struct *mm;			/* 被监视的地址空间 */
	struct rcu_head rcu;
	unsigned int users;			/* get/put 引用 */
};
```

链的修改要同时拿 `mmap_lock` 和反向映射锁（注释 `:226`）。

### `enum mmu_notifier_event`（`:51`）

| 值 | 数值 | 头文件注释里的原因 |
|---|---|---|
| `MMU_NOTIFY_UNMAP` | 0 | `munmap`，或 `mremap` 把区间搬走 |
| `MMU_NOTIFY_CLEAR` | 1 | 清页表项：`madvise`、一页换成另一页 |
| `MMU_NOTIFY_PROTECTION_VMA` | 2 | `mprotect`。按 VMA 的 `vm_page_prot` 更新整个区间即可 |
| `MMU_NOTIFY_PROTECTION_PAGE` | 3 | 单页读写位变化。end 回调里要自己看 CPU 页表 |
| `MMU_NOTIFY_SOFT_DIRTY` | 4 | soft-dirty 记账。页和访问位都没变 |
| `MMU_NOTIFY_RELEASE` | 5 | mm 引用到 0。给 interval notifier 的 invalidate 用 |
| `MMU_NOTIFY_MIGRATE` | 6 | `migrate_vma_collect`。`owner` 对得上设备私有 pgmap 时，驱动可以忽略 |
| `MMU_NOTIFY_EXCLUSIVE` | 7 | 设备不再独占这一页。`owner` 来自 `make_device_exclusive_range()` |

`MMU_NOTIFIER_RANGE_BLOCKABLE` 是 `flags` 的 bit 0（`:62`）。

### `struct mmu_notifier_range`

`CONFIG_MMU_NOTIFIER` 下（`:271`）；没编时在 `:599`。

```c
#ifdef CONFIG_MMU_NOTIFIER
struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;			/* 含 */
	unsigned long end;			/* 不含 */
	unsigned flags;				/* 含 MMU_NOTIFIER_RANGE_BLOCKABLE */
	enum mmu_notifier_event event;
	void *owner;				/* 迁移 / exclusive 时用来过滤 */
};
#else
struct mmu_notifier_range {
	unsigned long start;
	unsigned long end;
};
#endif
```

没编 `CONFIG_MMU_NOTIFIER` 时 `mmu_notifier_range_init` 变成只填 `start` 和 `end` 的宏（`:612`）。

`mm_has_notifiers`（`:280`）看 `mm->notifier_subscriptions`。KVM 注册之后这个指针非空，unmap / 保护位变化才会走到二级 MMU。

---

## 11.9 HMM：`hmm_range_fault`

**结论**：HMM 让设备按 CPU 页表的视图取 PFN，同时用 `mmu_interval_notifier` 盯着这段虚拟地址。区间在 fault 过程中被拆掉，调用者必须丢掉这次拿到的 PFN 重来。它不给设备建影子页表。影子页表仍是 KVM 那条路。

### `struct hmm_range`（`include/linux/hmm.h:91`）

```c
struct hmm_range {
	/*
	 * 罩住 start/end 的 interval notifier。失效序号从它来。
	 */
	struct mmu_interval_notifier *notifier;
	unsigned long notifier_seq;		/* mmu_interval_read_begin() 的返回值 */
	unsigned long start;			/* 含 */
	unsigned long end;			/* 不含 */
	unsigned long *hmm_pfns;		/* 输出。长度要盖住整个区间 */
	unsigned long default_flags;		/* 这段的默认标志，见 hmm.rst */
	unsigned long pfn_flags_mask;		/* 掩掉 pfn 自带的标志 */
	/*
	 * device private 页的 owner。对得上的页按设备自己的页处理。
	 */
	void *dev_private_owner;
};
```

`hmm_range_fault`（`mm/hmm.c:587`）要求调用者已经拿着 mmap lock：函数里 `mmap_assert_locked(mm)`（`:596`）。`mm` 来自 `range->notifier->mm`。序号已经变了就返回 `-EBUSY`（`:600`）。然后 `walk_page_range`。walk 自己返回 `-EBUSY`（迁移项在 `:276`，`return -EBUSY` 在 `:280`）就从上次停住的地址再走。注释 `:584`：它和 `get_user_pages` 类似，但可以只读页表、不把缺页做成对页表的修改。

返回值在函数注释里：`0` 成功，`-EINVAL`、`-ENOMEM`、`-EPERM`、`-EBUSY`、`-EFAULT`。

`HMM_RANGE_DEFAULT_TIMEOUT` 是 1000（毫秒，`hmm.h:114`）。`hmm_range_fault` 自己不读这个数。等 notifier 的驱动（例如 `lib/test_hmm.c`）拿它当超时。

`hmm_pfn_to_map_order`（`hmm.h:74`）从 hmm_pfn 里取出映射阶。调用前要先确认 `HMM_PFN_VALID`。标志在 `enum hmm_pfn_flags`（`:34`）：`HMM_PFN_VALID` / `WRITE` / `ERROR` 占 `unsigned long` 的最高几位。

### 设备私有页，只到这里用到的字段

`hmm_vma_handle_pte`（`mm/hmm.c:223`）看见 device private 的 swap 项，并且 `pfn_swap_entry_to_page(entry)->pgmap->owner == range->dev_private_owner`（`:250`）时，不制造缺页，直接把 PFN 和 `HMM_PFN_VALID`（可写再或上 `HMM_PFN_WRITE`）写进 `hmm_pfns`。对不上就按普通缺页处理。

`struct page` 在 `include/linux/mm_types.h:74`。`flags` 是 `unsigned long`（`:75`）。ZONE_DEVICE 是第一个 union 里的一臂（`:144`），本节只列 HMM 碰到的成员。同一 union 里还有页缓存、复合页尾等臂，不在这里展开。

```c
struct page {
	unsigned long flags;			/* 原子标志位 */
	union {
		/* 其它臂略 */
		struct {			/* ZONE_DEVICE */
			struct dev_pagemap *pgmap;
			void *zone_device_data;
			/*
			 * 注释 :148：私有页被迁走时，后面三个字叠着源页的
			 * mapping、index、private。这里没有单独的成员名。
			 */
		};
	};
	/* 后面还有 _mapcount 等，与 HMM range 无关 */
};
```

`is_device_private_page`（`include/linux/memremap.h:159`）要求 `page->pgmap->type == MEMORY_DEVICE_PRIVATE`。`MEMORY_DEVICE_PRIVATE` 的值是 1（`:70`）。注释 `:34`：CPU 不能直接读写这种页，但内核仍有 `struct page`。

`struct dev_pagemap`（`memremap.h:126`）HMM 比较的是 `owner`。整结构如下，迁移回调在 `dev_pagemap_ops`（`:77`），本节不展开。

```c
struct dev_pagemap {
	struct vmem_altmap altmap;
	struct percpu_ref ref;
	struct completion done;
	enum memory_type type;			/* MEMORY_DEVICE_PRIVATE 等 */
	unsigned int flags;			/* PGMAP_ALTMAP_VALID 等 */
	unsigned long vmemmap_shift;
	const struct dev_pagemap_ops *ops;
	void *owner;				/* 和 hmm_range.dev_private_owner 比 */
	int nr_range;
	union {
		struct range range;
		DECLARE_FLEX_ARRAY(struct range, ranges);
	};
};
```

ZONE_DEVICE 页怎么迁移、GPU 的状态机，本节没有展开。入口就是上面这个 range。

---

## 11.10 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `kvm_set_memory_region` | `virt/kvm/kvm_main.c:2053` | 拿 `slots_lock`，安装或删除 memslot |
| `__kvm_set_memory_region` | `:1949` | 检查参数，区分创建、删除、移动、改 flag |
| `gfn_to_memslot` | `:2345` | GFN → slot |
| `kvm_vcpu_gfn_to_memslot` | `:2351` | vcpu 热路径，带 last_used 缓存 |
| `__gfn_to_pfn_memslot` | `:2746` | slot + GFN → HVA → PFN |
| `hva_to_pfn` | `:2695` | 快路径、GUP，或 VM_PFNMAP |
| `handle_ept_violation` | `arch/x86/kvm/vmx/vmx.c:5745` | Intel EPT violation |
| `npf_interception` | `arch/x86/kvm/svm/svm.c:2069` | AMD NPT |
| `kvm_handle_page_fault` | `arch/x86/kvm/mmu/mmu.c:4423` | 影子模式的 #PF（含 SVM 未开 NPT） |
| `kvm_mmu_page_fault` | `:5709` | MMU 总入口 |
| `kvm_mmu_do_page_fault` | `arch/x86/kvm/mmu/mmu_internal.h:280` | 填 `kvm_page_fault` 并分发 |
| `kvm_tdp_page_fault` | `mmu.c:4495` | TDP。再分 TDP MMU 或 `direct_page_fault` |
| `kvm_tdp_mmu_page_fault` | `:4457` | `CONFIG_X86_64`。读锁 + `kvm_tdp_mmu_map` |
| `direct_page_fault` | `:4370` | 直接映射的写锁路径 |
| `nonpaging_page_fault` | `:4415` | `CR0.PG=0`，最大 2M |
| `paging64_page_fault` / `paging32_page_fault` / `ept_page_fault` | `paging_tmpl.h:777` | 三次包含生成。先走 guest/EPT 页表 |
| `kvm_faultin_pfn` | `mmu.c:4321` | 记序号，调用 `__kvm_faultin_pfn` |
| `__kvm_faultin_pfn` | `:4260` | 异步 PF 或 `__gfn_to_pfn_memslot` |
| `fast_page_fault` | `:3430` | 不拿 mmu_lock。恢复 access-track SPTE，或给 MMU-writable 的 SPTE 开写位 |
| `kvm_mmu_hugepage_adjust` | `:3167` | `req_level` / `goal_level` |
| `host_pfn_mapping_level` | `:3082` | 宿主 HVA 是 4K、2M 还是 1G |
| `kvm_tdp_mmu_map` | `arch/x86/kvm/mmu/tdp_mmu.c:1041` | 用 `tdp_iter` 把 PFN 写入 EPT/NPT |
| `kvm_init_mmu` | `mmu.c:5417` | 嵌套 / TDP / 影子 三选一 |
| `init_kvm_tdp_mmu` | `:5210` | `page_fault = kvm_tdp_page_fault`，`direct = 1` |
| `kvm_configure_mmu` | `:5891` | `tdp_enabled`，64 位上再决定 `tdp_mmu_enabled` |
| `nested_ept_init_mmu_context` | `vmx/nested.c:430` | L2 EPT：`guest_mmu` + `ept_page_fault` |
| `nested_svm_init_mmu_context` | `svm/nested.c:84` | L2 NPT：影子 NPT |
| `kvm_mmu_notifier_ops` | `virt/kvm/kvm_main.c:921` | KVM 注册的回调表 |
| `kvm_init_mmu_notifier` | `:931` | 挂到当前 mm |
| `hmm_range_fault` | `mm/hmm.c:587` | 按 CPU 页表填 `hmm_pfns`。调用者持 mmap lock |

---

[上一篇 迁移](./10-10-migration.md) · [目录](./README.md) · [下一节 balloon/FPR](./12-balloon-fpr-and-overcommit.md)
