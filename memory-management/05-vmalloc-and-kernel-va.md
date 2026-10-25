# 5. vmalloc 与内核虚拟地址

**结论**：`kmalloc` 拿来的页来自伙伴系统，虚拟地址落在直接映射里，所以 `virt_to_page()` 可以用。`vmalloc` 给的是一段连续的内核虚拟地址，物理页按页向伙伴系统要，可以不连续。这种指针用 `vmalloc_to_page()`。要 DMA、要物理连续、要 `page_address()`，就用 `kmalloc` 或 `alloc_pages`。

> `struct vm_struct` / `struct vmap_area`：`include/linux/vmalloc.h`。实现：`mm/vmalloc.c`。
> x86-64 地址布局：`Documentation/arch/x86/x86_64/mm.rst`。摘要也在 [02](./02-physical-layout.md)。
> 导航：[上一节 SLUB](./04-slab.md) · [目录](./README.md) · [下一节 mm/VMA](./06-mm-and-vma.md)

---

## 5.1 怎么选

`Documentation/arch/x86/x86_64/mm.rst` 把 4 级页表和 5 级页表的内核窗口写成两张表。下面只抄和本章有关的行。表里的 End addr 是这一段的最后一个字节。

4 级页表（文件第 45–67 行）：

| 起点 | 终点 | 大小 | 文档里的名字 |
|---|---|---|---|
| `ffff888000000000` | `ffffc87fffffffff` | 64 TB | direct mapping of all physical memory (`page_offset_base`) |
| `ffffc90000000000` | `ffffe8ffffffffff` | 32 TB | vmalloc/ioremap space (`vmalloc_base`) |
| `ffffffffa0000000` | `fffffffffeffffff` | 1520 MB | module mapping space |

5 级页表（文件第 104–126 行）：

| 起点 | 终点 | 大小 | 文档里的名字 |
|---|---|---|---|
| `ff11000000000000` | `ff90ffffffffffff` | 32 PB | direct mapping of all physical memory (`page_offset_base`) |
| `ffa0000000000000` | `ffd1ffffffffffff` | 12.5 PB | vmalloc/ioremap space (`vmalloc_base`) |
| `ffffffffa0000000` | `fffffffffeffffff` | 1520 MB | module mapping space |

模块窗口在两张表里是同一段。文档第 148–150 行写明：开了 `CONFIG_RANDOMIZE_MEMORY` 时，直接映射、vmalloc/ioremap、vmemmap 的基址会在启动时挪开，先后顺序不变。模块那段不在这句里面。

源码里的常量对得上这张表。`__VMALLOC_BASE_L4` 是 `0xffffc90000000000`，`VMALLOC_SIZE_TB_L4` 是 32；5 级是 `__VMALLOC_BASE_L5` 和 `VMALLOC_SIZE_TB_L5`（12800，也就是文档里的 12.5 PB）。见 `arch/x86/include/asm/pgtable_64_types.h:127`。`VMEMORY_END`（`pgtable_64_types.h:156`）是 `VMALLOC_START + (VMALLOC_SIZE_TB << 40) - 1`。非 KMSAN 构建里 `VMALLOC_END` 就等于它（`:159`）。所以 4 级页表的窗口字节数是 `(32 << 40) - 1`，`/proc/meminfo` 把它右移 10 位印成 kB，就是 34359738367。这是宏算出来的，不是某台机器的采样。

| | `kmalloc` / `alloc_pages` | `vmalloc` |
|---|---|---|
| 虚拟地址 | 直接映射。x86-64 文档里的名字是 `page_offset_base` | 文档里的 vmalloc/ioremap space，起点是 `vmalloc_base` |
| 物理 | 一次要到的那几页是连续的 | 默认按页向伙伴系统要，页与页之间可以不连续 |
| 一次能要多大 | 通用 cache 到两页（`KMALLOC_SHIFT_HIGH = PAGE_SHIFT + 1`）。再大走 `kmalloc_large`，仍受 `MAX_ORDER` 限制，默认 10 | 受 vmalloc 窗口和空洞限制，不受单个伙伴 order 限制 |
| 开销 | 小。大块失败通常是外碎片 | 要改内核页表，然后刷新 TLB |
| 换页 | `virt_to_page()` | `vmalloc_to_page()` |
| 适合 | slab 对象、要物理连续的缓冲 | 模块、大数组、不必物理连续的每 CPU 表 |

`is_vmalloc_addr`（`mm/vmalloc.c:79`）判断 `VMALLOC_START <= addr < VMALLOC_END`。x86-64 的 `__virt_addr_valid`（`arch/x86/mm/physaddr.c:47`）按直接映射把地址换算成物理页再问 `pfn_valid`，函数里没有单独排除 vmalloc 窗口。要判断指针是不是 `vmalloc` 出来的，用 `is_vmalloc_addr`。模块文本不在这个窗口里，`is_vmalloc_or_module_addr`（`vmalloc.c:677`）会再认 `MODULES_VADDR .. MODULES_END`。

`kvmalloc()`（`include/linux/slab.h:736`）先 `kmalloc_node`，失败再 `vmalloc`。实现是 `kvmalloc_node`（`mm/util.c:612`）。大于一页时它给 kmalloc 加上 `__GFP_NOWARN`，并且在调用者没写 `__GFP_RETRY_MAYFAIL` 时加上 `__GFP_NORETRY`，同时清掉 `__GFP_NOFAIL`。kmalloc 成功，或者请求不大于一页，就直接返回。一页及以下失败了也不再 vmalloc。不能睡眠的 GFP 在回退前返回 NULL。回退调用 `__vmalloc_node_range`，范围是 `VMALLOC_START .. VMALLOC_END`，并带上 `VM_ALLOW_HUGE_VMAP`。函数注释写明：调用者不能假设指针长什么样。后面还要 `virt_to_page()` 的代码不要用 `kvmalloc()`。

`/proc/meminfo` 的三个字段（`fs/proc/meminfo.c:130`）：

- `VmallocTotal` 是 `(VMALLOC_END - VMALLOC_START) >> 10`，虚拟窗口有多大，不是已经用掉多少。
- `VmallocUsed` 是 `vmalloc_nr_pages()`（`mm/vmalloc.c:839`）换算成的 kB，统计的是 vmalloc 映射上的物理页。
- `VmallocChunk` 在源码里写成常量 0（`meminfo.c:133`）。不能拿它判断窗口里还有没有空洞。

---

## 5.2 `struct vm_struct`（`include/linux/vmalloc.h:49`）

一块已经划出去的内核虚拟区间。`size` 常常比调用者要的大一页，因为末尾留了 guard page。调用者要的长度没有单独的字段；`get_vm_area_size`（`vmalloc.h:180`）在没有 `VM_NO_GUARD` 时返回 `size - PAGE_SIZE`。

```c
struct vm_struct {
	struct vm_struct *next;	/* 早期启动时把 vm 区串成单链表 */
	void *addr;			/* 这块映射的内核虚拟地址 */
	unsigned long size;		/* 字节数。留了 guard 时比请求大一页 */
	unsigned long flags;		/* VM_ALLOC、VM_IOREMAP 等，见下表 */
	struct page **pages;		/* 物理页数组。ioremap 不填它 */
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	/*
	 * 建页表时尝试的页阶。x86-64 选了 HAVE_ARCH_HUGE_VMALLOC。
	 * 这是 vmalloc 层的记录，不是伙伴系统答应一定给连续大页。
	 */
	unsigned int page_order;
#endif
	unsigned int nr_pages;		/* pages[] 里有多少页 */
	phys_addr_t phys_addr;		/* ioremap 记下的设备物理地址。普通 vmalloc 是 0 */
	const void *caller;		/* 谁分配的。/proc/vmallocinfo 用来打印调用点 */
};
```

`flags` 的位（`vmalloc.h:20`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `VM_IOREMAP` | `0x1` | `ioremap` 及其同类 |
| `VM_ALLOC` | `0x2` | `vmalloc` 分配并映射 |
| `VM_MAP` | `0x4` | `vmap` 映射调用者已有的页 |
| `VM_USERMAP` | `0x8` | 可以 `remap_vmalloc_range` 给用户态 |
| `VM_DMA_COHERENT` | `0x10` | `dma_alloc_coherent` |
| `VM_UNINITIALIZED` | `0x20` | `vm_struct` 还没填完 |
| `VM_NO_GUARD` | `0x40` | 不留 guard page。头文件标了危险 |
| `VM_KASAN` | `0x80` | 已经分配 KASAN 影子 |
| `VM_FLUSH_RESET_PERMS` | `0x100` | 解除映射时恢复直接映射权限并刷新 TLB。不能在原子上下文里释放 |
| `VM_MAP_PUT_PAGES` | `0x200` | `vfree` 时放下页并释放页数组 |
| `VM_ALLOW_HUGE_VMAP` | `0x400` | 允许尝试大页。架构要有 `HAVE_ARCH_HUGE_VMALLOC` |
| `VM_DEFER_KMEMLEAK` | `0x800` 或 0 | 通用 KASAN 或软件标签 KASAN、且没开 `KASAN_VMALLOC` 时推迟 kmemleak。否则宏是 0 |

`vfree` 延后释放用的链表节点不是这个结构里的字段。`vfree_atomic` 把分配到的地址本身当成 `llist_node` 挂上去（`mm/vmalloc.c:2842`）。

---

## 5.3 `struct vmap_area`（`vmalloc.h:63`）

vmalloc 窗口里的虚拟地址分配器。空闲区间在 `free_vmap_area_root`，占用区间在 `vmap_area_root`。一个 `vmap_area` 只在其中一棵树上，所以空闲树用的「子树最大空洞」和占用树用的 `vm_struct` 指针叠在同一个 union 里。

```c
struct vmap_area {
	unsigned long va_start;		/* 这段虚拟地址的起点 */
	unsigned long va_end;		/* 终点，不含这一字节 */

	struct rb_node rb_node;		/* 按地址排序的红黑树节点 */
	struct list_head list;		/* 按地址排序的链表 */

	/*
	 * 要么在空闲树，要么在占用树，所以下面两项叠在一起。
	 */
	union {
		unsigned long subtree_max_size;	/* 空闲树：这棵子树里最大的空洞，分配时用来剪枝 */
		struct vm_struct *vm;		/* 占用树：对应的 vm_struct */
	};
	unsigned long flags;		/* vm_map_ram 等类型标记 */
};
```

---

## 5.4 分配路径

`vmalloc` 是 `mm/vmalloc.c:3466` 的函数，不是宏。它固定用 `GFP_KERNEL`、对齐 1、节点 `NUMA_NO_NODE`，并且不带 `VM_ALLOW_HUGE_VMAP`。普通 `vmalloc()` 不会去组大页。要大页用 `vmalloc_huge`（`:3485`）或 `kvmalloc`。

```text
vmalloc(size)                                /* mm/vmalloc.c:3466 */
  → __vmalloc_node                           /* :3432，范围默认是整个 vmalloc 窗口 */
       → __vmalloc_node_range                /* :3285 */
            → __get_vm_area_node             /* :2626，在 [start, end) 里划一段 va */
                 → alloc_vmap_area           /* :1621，从空闲 vmap_area 上切 */
            → __vmalloc_area_node            /* :3151 */
                 → vm_area_alloc_pages       /* :3050，alloc_pages / alloc_pages_node */
                 → vmap_pages_range          /* :667，把页填进内核页表 */
            → clear_vm_uninitialized_flag   /* :2615 */

vmap(pages, count, flags, prot)              /* :2952 */
  已有 struct page *[]，只建内核映射，不再向伙伴系统要页
```

`__vmalloc_node_range` 的参数是：字节数、对齐、虚拟地址起止、GFP、页保护、`vm_flags`、节点、调用者地址。注释（`:3272`）写明伙伴系统那一侧支持 `GFP_KERNEL`、`GFP_NOFS`、`GFP_NOIO`。不能用 zone 修饰符。必须能直接回收（也就是不支持 `GFP_NOWAIT`）。`__GFP_NOFAIL` 可以，`__GFP_NORETRY` 和 `__GFP_RETRY_MAYFAIL` 不行。

划虚拟地址时，没带 `VM_NO_GUARD` 就把长度再加一页（`__get_vm_area_node`，`:2648`）。这一页不映射。`vmap` 里的注释（`:2965`）说：你的高地址那一页 guard，就是旁边那段映射的低地址 guard。所以 `vm_struct.size` 比 `get_vm_area_size` 大一页。`vmap()` 如果看到调用者带了 `VM_NO_GUARD`，会把它清掉（`:2968`），因为缺了高地址 guard 会波及别人。

`__vmalloc_area_node` 先分配 `pages[]` 数组：数组比一页大就递归 `__vmalloc_node`，否则 `kmalloc_node`。然后 `vm_area_alloc_pages` 按页要物理页。高阶分配会 `split_page`，调用者看到的仍是 order 0 的页。页数不够就 `vfree` 掉已经拿到的部分并返回 NULL。页表填好之后 `kasan_unpoison_vmalloc`，再清掉 `VM_UNINITIALIZED`。

大页只在两件事同时成立时尝试（`:3307`）：`vmap_allow_huge` 为真（`CONFIG_HAVE_ARCH_HUGE_VMALLOC` 下默认真，启动参数 `nohugevmalloc` 把它关掉），并且 `vm_flags` 含 `VM_ALLOW_HUGE_VMAP`。x86-64 选了 `HAVE_ARCH_HUGE_VMALLOC`（`arch/x86/Kconfig:176`）。条件成立时，够一个 PMD 就试 `PMD_SHIFT`，否则问架构最小能用的 shift。第一次失败会退回 `PAGE_SHIFT` 再来一次。

`vfree`（`:2863`）可以在中断里调用：发现 `in_interrupt()` 就转去 `vfree_atomic`（`:2829`），把地址挂到 per-CPU 链表并排工作队列，真正的拆映射以后再做。两条路径都 `BUG_ON(in_nmi())`，所以不要在 NMI 里 `vfree`。进程上下文里它 `remove_vm_area`，逐页 `__free_page`，再 `kvfree(vm->pages)` 和 `kfree(vm)`。这和 `kfree` 不同：`kfree` 没有这条「中断里先排队」的路径，但 `vfree` 明确拒绝 NMI。

---

## 5.5 和模块、ioremap

x86-64 的模块可执行映射不在那 32TB（或 5 级页表的 12.5PB）vmalloc 窗口里。文档把它放在 `ffffffffa0000000` 起、长 1520MB 的 module mapping space。`module_alloc`（`arch/x86/kernel/module.c:68`）调用 `__vmalloc_node_range`，起止是 `MODULES_VADDR + get_module_load_offset()`（`arch/x86/include/asm/pgtable_64_types.h:197`）和 `MODULES_END`（`:200`，打开 `CONFIG_DEBUG_KMAP_LOCAL_FORCE_MAP` 时是 `:202`），页保护是 `PAGE_KERNEL`，标志是 `VM_FLUSH_RESET_PERMS | VM_DEFER_KMEMLEAK`。窗口比 `MODULES_LEN` 还大就直接返回 NULL。调用指令的相对位移有范围，所以模块要挤在内核文本附近，不能随便落到 vmalloc 窗口。`/proc/vmallocinfo` 里能看到模块，只说明它们也走了 `vm_struct`，不说明基址和 `vmalloc()` 相同。

x86 的 `ioremap` 最终进 `__ioremap_caller`（`arch/x86/mm/ioremap.c:184`）。它用 `get_vm_area_caller(size, VM_IOREMAP, caller)` 在 vmalloc/ioremap 窗口里要一段虚拟地址（`get_vm_area_caller` 的范围是 `VMALLOC_START .. VMALLOC_END`，`mm/vmalloc.c:2701`），把设备物理地址写进 `area->phys_addr`，再用 `ioremap_page_range` 建页表。`pages` 保持 NULL，因为这些页不是伙伴系统分配出来的。释放用 `iounmap`（`ioremap.c:456`）：`remove_vm_area` 之后 `kfree` 掉 `vm_struct`，不走 `vfree`。`vfree` 会去释放 `pages[]`，对 ioremap 的指针是错的。

---

## 5.6 highmem 与 `kmap_local_page`

**结论**：highmem 是 32 位机上「物理页在内核直接映射窗口之外」的那一段。读写它的内容要临时借一个内核虚拟地址。x86 的 `CONFIG_HIGHMEM` 依赖 `X86_32`（`arch/x86/Kconfig:1472`），x86-64 不选它。于是 x86-64 上 `kmap_local_page` 就是 `page_address`。

契约写在 `include/linux/highmem.h:63`。`highmem.h:14` 包含 `highmem-internal.h`，函数体在那个文件里，按 `CONFIG_HIGHMEM` 分成两套。

调用规则（`highmem.h:68` 起的注释）：

- 任何上下文都能调，包括中断。
- 映射是栈式的。后 map 的必须先 `kunmap_local`。先解开外层是错的。
- 指针只在调用者自己的上下文里有效，不能交给别的任务或别的 CPU。
- 不能拿着这个指针睡觉。注释要求映射期间别做会睡眠的事；highmem 那一侧为了地址稳定还会暂时禁止迁移，但注释写明调用者不能依赖这个副作用。
- `CONFIG_HIGHMEM=n`，或者页在低端内存里：返回直接映射地址。只有真正的 highmem 页才会建临时映射。

开了 `CONFIG_HIGHMEM` 时（`highmem-internal.h:71`）：

```c
static inline void *kmap_local_page(struct page *page)
{
	return __kmap_local_page_prot(page, kmap_prot);
}
```

`kmap_local_folio`（`:76`）先算出 `offset / PAGE_SIZE` 那一页，映射后再加上 `offset % PAGE_SIZE`。

`kmap`（`:40`）对非 highmem 页直接 `page_address`，对 highmem 页调用 `kmap_high`，然后刷新 TLB。`kmap_atomic`（`:108`）在 `kmap_local` 之外再 `preempt_disable`（`PREEMPT_RT` 上是 `migrate_disable`）并 `pagefault_disable`。`highmem.h:135` 的注释把它标成 deprecated，并写明新代码用 `kmap_local_page`。解开用 `kunmap_local` / `kunmap_atomic` 宏（`highmem-internal.h:284`、`:268`），参数是映射得到的地址，不是 `struct page *`。

没开 `CONFIG_HIGHMEM` 时（`:160` 的 `#else`，x86-64 走这里）：

```c
static inline void *kmap_local_page(struct page *page)
{
	return page_address(page);   /* :183。直接映射，没有临时窗口 */
}

static inline void *kmap_local_folio(struct folio *folio, size_t offset)
{
	return page_address(&folio->page) + offset;   /* :188 */
}
```

`kmap`（`:167`）同样返回 `page_address`，只多一个 `might_sleep()`。`kmap_atomic`（`:210`）仍然关抢占（RT 上是禁止迁移）并 `pagefault_disable`，然后返回 `page_address`。所以在 x86-64 上看到 `kmap_local_page`，把它读成「拿到这页的内核虚拟地址，并且写成和 32 位同一套调用形式」。

---

## 5.7 vmalloc 内核栈

**结论**：`CONFIG_VMAP_STACK` 把内核线程栈放进 vmalloc 区。栈两端外侧是没有映射的 guard page，写越界会立刻缺页，而不是悄悄改掉旁边的栈。x86-64 选了 `HAVE_ARCH_VMAP_STACK`（`arch/x86/Kconfig:196`）。`VMAP_STACK` 默认开（`arch/Kconfig:1246`），还要求没有 KASAN，或者用的是硬件标签 KASAN，或者开了 `KASAN_VMALLOC`。

`Documentation/mm/vmalloced-kernel-stacks.rst` 写明：arm64、powerpc、riscv、s390、um、x86 支持这个功能。源码里这些架构的 Kconfig 都 `select HAVE_ARCH_VMAP_STACK`。x86 限定 `X86_64`，riscv 限定 `MMU && 64BIT`。`arch/arm/Kconfig:145` 在 `MMU && ARM_HAS_GROUP_RELOCS` 时也会 select，文档的名单没有写 arm。

文档还写了这几条，和 `kernel/fork.c` 对得上：

- 栈从页分配器拿物理页，再映射成连续的内核虚拟地址，页保护是 `PAGE_KERNEL`。
- 分配出来的栈会按 CPU 缓存，给后面的新线程复用。memcg 记账在把栈交给任务时手工做，所以 `__vmalloc_node_range` 不带 `__GFP_ACCOUNT`。
- `vm_struct` 要留在任务上。`free_thread_stack` 可能在中断里跑，中断里不能再 `find_vm_area`。
- `task_struct.stack_vm_area`（`include/linux/sched.h:1488`，包在 `CONFIG_VMAP_STACK` 里）非空，表示这是 vmalloc 栈。
- 不管中断栈。中断栈另有入口栈。
- 和 KASAN 一起用时，架构要能给虚拟映射配真正的影子内存，并且打开 `KASAN_VMALLOC`。Kconfig 用依赖把这件事卡住了。
- 开了 `VMAP_STACK` 就不能对栈上的数据做 DMA。文档原文是这句。

`dup_task_struct`（`kernel/fork.c:1111`）在 `:1126` 调用 `alloc_thread_stack_node`。`CONFIG_VMAP_STACK` 下的实现从 `:274` 开始。每个 CPU 留 `NR_CACHED_STACKS`（2）个 `struct vm_struct *`。

```text
alloc_thread_stack_node                         /* fork.c:274 */
  → 先在本 CPU 的 cached_stacks[] 里找
       找到：把栈内容清零，memcg_charge_kernel_stack，记到 tsk->stack_vm_area
  → 没有缓存：
       __vmalloc_node_range(THREAD_SIZE, THREAD_ALIGN,
                            VMALLOC_START, VMALLOC_END,
                            THREADINFO_GFP & ~__GFP_ACCOUNT,
                            PAGE_KERNEL, 0, node, ...)     /* :311 */
       THREADINFO_GFP = GFP_KERNEL_ACCOUNT | __GFP_ZERO    /* include/linux/thread_info.h:80 */
       去掉 __GFP_ACCOUNT 之后就是 GFP_KERNEL | __GFP_ZERO
  → find_vm_area，再 memcg_charge_kernel_stack
  → tsk->stack_vm_area = vm，tsk->stack = stack
```

x86-64 的 `THREAD_SIZE_ORDER` 是 `2 + KASAN_STACK_ORDER`（`arch/x86/include/asm/page_64_types.h:15`）。没开 KASAN 时栈是 4 页。`memcg_charge_kernel_stack` 用 `BUG_ON` 要求 `vm->nr_pages == THREAD_SIZE / PAGE_SIZE`（`fork.c:259`），也就是只给映射了的栈页记账，guard 那一页不在 `pages[]` 里。

guard page 来自 [5.4](#54-分配路径) 里 `size += PAGE_SIZE` 的那一页，在这段虚拟地址的高地址端，没有对应的 `struct page` 用户数据。低地址端那一页 guard，是相邻较低那次分配的高地址 guard。`vmap` 的注释就是这个意思。`drivers/misc/lkdtm/bugs.c` 有两个测试：`lkdtm_STACK_GUARD_PAGE_LEADING`（`:468`）读 `task_stack_page(current) - 1`，`lkdtm_STACK_GUARD_PAGE_TRAILING`（`:482`）读 `stack + THREAD_SIZE`。

`set_task_stack_end_magic`（`fork.c:1103`）在 `dup_task_struct` 的 `:1152` 调用，往 `end_of_stack` 写 `STACK_END_MAGIC`（`0x57AC6E9D`，`include/uapi/linux/magic.h:74`）。这是栈内容里的一个字，用来事后判断这块栈被踩过。它抓不到每一次越界。guard page 才能在写到栈外面的那条指令上缺页。

x86 上这次缺页的处理在 `page_fault_oops`（`arch/x86/mm/fault.c:633`）。`CONFIG_VMAP_STACK` 下的判断在 `:656`：地址落在 vmalloc 区，并且 `get_stack_guard_info`（`arch/x86/include/asm/stacktrace.h:42`）确认它是「已知栈下面的那一页」，就切到 double-fault IST 栈上调用 `handle_stack_overflow`（`arch/x86/kernel/traps.c:402`）。注释写明：有可能在走到这里之前已经变成 double fault，那时 `exc_double_fault`（`traps.c:452`）也会在 CR2 指向 guard 时调用同一个函数（`:562`）。这和文档说的「在 double-fault 栈上处理栈溢出缺页」是同一件事。

`/proc/meminfo` 的 `KernelStack` 直接打印 `NR_KERNEL_STACK_KB`（`fs/proc/meminfo.c:112`）。`account_kernel_stack`（`fork.c:554`）对 vmalloc 栈按 `THREAD_SIZE / PAGE_SIZE` 页、每页 `PAGE_SIZE / 1024` kB 累加。guard page 不在这个循环里。

没开 `CONFIG_VMAP_STACK`、但 `THREAD_SIZE >= PAGE_SIZE` 时，`alloc_thread_stack_node` 是 `fork.c:358`：`alloc_pages_node(node, THREADINFO_GFP, THREAD_SIZE_ORDER)`，这次保留 `__GFP_ACCOUNT`，没有 vmalloc guard。`THREAD_SIZE` 比一页还小、并且没开 `VMAP_STACK` 时，走 `:393` 的 `kmem_cache`，cache 名字是 `"thread_stack"`。x86-64 的栈至少 4 页，又默认开 `VMAP_STACK`，正常走的是 `:274`。`CONFIG_ARCH_THREAD_STACK_ALLOCATOR` 会换成架构自己的函数（`:419`）；6.6 里 select 它的是 ia64，x86 不走这条。

---

## 5.8 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `vmalloc` | 声明 `include/linux/vmalloc.h:140`，定义 `mm/vmalloc.c:3466` | `GFP_KERNEL` 的虚拟连续分配。不带 `VM_ALLOW_HUGE_VMAP` |
| `__vmalloc_node` | 声明 `vmalloc.h:152`，定义 `vmalloc.c:3432` | 指定对齐、GFP、节点。测试模块才会导出 |
| `__vmalloc` | 声明 `vmalloc.h:147`，定义 `vmalloc.c:3447` | 指定 GFP，节点是 `NUMA_NO_NODE` |
| `__vmalloc_node_range` | 声明 `vmalloc.h:148`，定义 `vmalloc.c:3285` | 指定节点、对齐、虚拟范围、GFP、页保护、`vm_flags` |
| `__vmalloc_area_node` | `vmalloc.c:3151` | 要物理页并填页表 |
| `vm_area_alloc_pages` | `vmalloc.c:3050` | 按页调用 `alloc_pages` / `alloc_pages_node` |
| `__get_vm_area_node` | `vmalloc.c:2626` | 划虚拟区间。默认末尾加一页 guard |
| `clear_vm_uninitialized_flag` | `vmalloc.c:2615` | 页表填完后清掉 `VM_UNINITIALIZED` |
| `find_vm_area` | `vmalloc.c:2719` | 按地址找 `vm_struct`。内核栈分配后用它记住 `stack_vm_area` |
| `ioremap_page_range` | `vmalloc.c:338` | 按物理地址填 ioremap 的页表 |
| `alloc_vmap_area` | `vmalloc.c:1621` | 从空闲 `vmap_area` 上切一段 |
| `get_vm_area_caller` | `vmalloc.c:2701` | 在 `VMALLOC_START .. VMALLOC_END` 里划区间 |
| `get_vm_area_size` | `vmalloc.h:180` | 去掉 guard 之后的字节数 |
| `vmap_pages_range` | `vmalloc.c:667` | 把 `struct page *[]` 填进内核页表并刷新缓存 |
| `vmap` | 声明 `vmalloc.h:164`，定义 `vmalloc.c:2952` | 映射现成的页数组 |
| `vmap_pfn` | 声明 `vmalloc.h:166`，定义 `vmalloc.c:3026` | 按 PFN 映射。函数体包在 `CONFIG_VMAP_PFN` 里 |
| `vmalloc_to_page` | 声明 `include/linux/mm.h:1149`，定义 `mm/vmalloc.c:698` | vmalloc 或模块地址走到 `struct page` |
| `is_vmalloc_addr` | 声明 `mm.h:1159`，定义 `vmalloc.c:79` | 地址是否落在 `[VMALLOC_START, VMALLOC_END)` |
| `is_vmalloc_or_module_addr` | `vmalloc.c:677` | 再认模块窗口 |
| `vfree` | 声明 `vmalloc.h:161`，定义 `vmalloc.c:2863` | 释放。中断里转 `vfree_atomic`。禁止 NMI |
| `vfree_atomic` | `vmalloc.c:2829` | 把地址挂到 per-CPU 链表，工作队列里再 `vfree` |
| `remove_vm_area` | `vmalloc.c:2740` | 摘掉占用中的 `vmap_area` 并解除页表 |
| `vmalloc_huge` | `vmalloc.c:3485` | 和 `vmalloc` 一样，但带 `VM_ALLOW_HUGE_VMAP` |
| `vzalloc` | `vmalloc.c:3506` | `vmalloc` 再加 `__GFP_ZERO` |
| `vmalloc_user` | `vmalloc.c:3522` | 按 `SHMLBA` 对齐的清零映射，标志 `VM_USERMAP` |
| `vmalloc_node` | `vmalloc.c:3544` | 指定节点 |
| `vmalloc_32` | `vmalloc.c:3590` | GFP 用 `GFP_VMALLOC32`，让物理地址落在 32 位 DMA 窗口 |
| `vmalloc_32_user` | `vmalloc.c:3606` | `vmalloc_32` 再加清零和 `VM_USERMAP` |
| `kvmalloc` | `include/linux/slab.h:736` | inline，转 `kvmalloc_node` |
| `kvmalloc_node` | `mm/util.c:612` | 先 `kmalloc_node`，失败再 `__vmalloc_node_range` |
| `module_alloc` | `arch/x86/kernel/module.c:68` | x86 模块窗口里的 `__vmalloc_node_range` |
| `get_module_load_offset` | `module.c:45`（`CONFIG_RANDOMIZE_BASE`）或 `:62`（否则返回 0） | 在模块窗口里再加一个页对齐的随机偏移 |
| `__ioremap_caller` | `arch/x86/mm/ioremap.c:184` | 划 `VM_IOREMAP` 区间，记下 `phys_addr` |
| `ioremap` | `arch/x86/mm/ioremap.c:343` | 设备内存映射的入口之一 |
| `iounmap` | `arch/x86/mm/ioremap.c:456` | 解除 ioremap。不要对这个指针 `vfree` |
| `kmap_local_page` | 声明 `include/linux/highmem.h:96`；HIGHMEM 实现 `highmem-internal.h:71`，否则 `:183` | 临时内核映射，或直接 `page_address` |
| `__kmap_local_page_prot` | 声明 `highmem-internal.h:10`，定义 `mm/highmem.c:565` | HIGHMEM 下按页保护建立临时映射 |
| `kmap_high` | `mm/highmem.c:297` | 持久的 highmem 映射。`kmap` 对高端页走这里 |
| `kmap_local_folio` | `highmem-internal.h:76` / `:188` | folio 内偏移 |
| `kmap` | `highmem-internal.h:40` / `:167` | 可睡眠的映射。非 highmem 时是 `page_address` |
| `kmap_atomic` | `highmem-internal.h:108` / `:210` | 关缺页再映射。头注释要求改用 `kmap_local_page` |
| `kunmap_local` | 宏 `highmem-internal.h:284` | 按映射地址解开，顺序与 map 相反 |
| `alloc_thread_stack_node` | `kernel/fork.c:274`（`VMAP_STACK`）；`:358`、`:393`、`:419` 是另外三套 | 内核栈 |
| `memcg_charge_kernel_stack` | `fork.c:253` | 把已经映射的栈页逐页 charge 到 memcg |
| `exc_double_fault` | `arch/x86/kernel/traps.c:452` | double fault 入口。CR2 落在 guard 上时调用 `handle_stack_overflow` |
| `set_task_stack_end_magic` | `fork.c:1103` | 在栈底写 `STACK_END_MAGIC` |
| `dup_task_struct` | `fork.c:1111` | 复制任务并分配栈 |
| `account_kernel_stack` | `fork.c:554` | 把栈页计入 `NR_KERNEL_STACK_KB`，不含 guard |
| `get_stack_guard_info` | `arch/x86/include/asm/stacktrace.h:42` | 缺页地址是不是某段栈下面的那一页 |
| `handle_stack_overflow` | `arch/x86/kernel/traps.c:402` | 打印 guard 被打到并 `die` |
| `page_fault_oops` | `arch/x86/mm/fault.c:633` | x86 缺页 oops。vmalloc 栈溢出时切到 double-fault 栈 |

---

[上一节 SLUB](./04-slab.md) · [目录](./README.md) · [下一节 mm/VMA](./06-mm-and-vma.md)
