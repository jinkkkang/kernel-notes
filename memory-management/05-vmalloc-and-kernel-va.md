# 5. vmalloc 与内核虚拟地址

**结论**：`kmalloc` 给的是物理连续、而且落在直接映射里的内存，所以 `virt_to_page()` 成立。`vmalloc` 给的是内核虚拟地址连续、物理页可以散落在 buddy 各处的内存，只能 `vmalloc_to_page()`。要 DMA、要大页连续、要 `page_address()`，就不要用 vmalloc。

> `struct vm_struct` / `struct vmap_area`：`include/linux/vmalloc.h`。实现：`mm/vmalloc.c`。
> 布局表：`Documentation/arch/x86/x86_64/mm.rst`，摘要在 [02](./02-physical-layout.md)。
> 导航：[上一节 SLUB](./04-slab.md) · [目录](./README.md) · [下一节 mm/VMA](./06-mm-and-vma.md)

---

## 5.1 怎么选

| | `kmalloc` / `alloc_pages` | `vmalloc` |
|---|---|---|
| 虚拟地址 | 直接映射（x86-64 的 `page_offset_base` 一带） | vmalloc 区（文档里 4 级页表是 `vmalloc_base` 起 32TB） |
| 物理 | 连续 | 按页向 buddy 要，默认不连续 |
| 上限 | `MAX_PAGE_ORDER`（默认 10）以内才物理连续；`kmalloc` 大块也会落到 buddy，但仍然受 order 限制 | 受 vmalloc 区大小和碎片限制，不受单个 buddy order 限制 |
| 开销 | 低。大块失败是外碎片 | 要改内核页表，TLB shootdown，慢 |
| `virt_addr_valid` | 真 | 假 |
| 适合 | slab 对象、DMA 缓冲、和硬件共享的页 | 模块、大数组、每个 CPU 一份但不必物理连续的表、ioremap 的表亲 |

`kvmalloc()` 先试 kmalloc，失败再 vmalloc。实现是 `__kvmalloc_node_noprof`（`mm/slub.c:7000`），宏在 `include/linux/slab.h:1358`。有的调用点随后会 `virt_to_page()`，vmalloc 回退会炸，用之前看调用者。

`/proc/meminfo` 的 `VmallocTotal` 是这块虚拟窗口的大小，不是已经用掉的。本机快照 `VmallocTotal` 约 32TB（`34359738367 kB`），`VmallocUsed` 只有约 16MB。`VmallocChunk` 在较新的内核里常为 0，不代表没有洞，这个字段已经不可靠，不要用它判断碎片。

---

## 5.2 `struct vm_struct`（`vmalloc.h:52`）

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct vm_struct *next` | 与 `llnode` 同 union | 早期启动登记 vm 区时的单链表 |
| `struct llist_node llnode` | 同 union | 出错路径上异步释放 |
| `void *addr` | 始终 | 这块映射的内核虚拟地址 |
| `unsigned long size` | 始终 | 字节大小，含 guard 页时比请求大 |
| `unsigned long flags` | 始终 | `VM_ALLOC`、`VM_MAP`、`VM_IOREMAP` 等。位定义在 `vmalloc.h` 靠前的宏，用之前对一下 |
| `struct page **pages` | 始终 | 物理页数组。ioremap 可以是 NULL |
| `unsigned int page_order` | `CONFIG_HAVE_ARCH_HUGE_VMALLOC` | 尝试用多大的页来建页表。不是 buddy order 的保证 |
| `unsigned long nr_pages` | 始终 | `pages[]` 的长度 |
| `phys_addr_t phys_addr` | 始终 | ioremap 时的物理地址。普通 vmalloc 为 0 |
| `const void *caller` | 始终 | 分配调用点，`/proc/vmallocinfo` 用来打印是谁申请的 |
| `unsigned long requested_size` | 始终 | 调用者要的大小，不含 guard |

---

## 5.3 `struct vmap_area`（`vmalloc.h:71`）

vmalloc 区的虚拟地址分配器。空闲和占用是两棵树。

| 成员 | 中文 |
|---|---|
| `unsigned long va_start` | 这段虚拟地址起点 |
| `unsigned long va_end` | 终点 |
| `struct rb_node rb_node` | 按地址排序的红黑树节点 |
| `struct list_head list` | 按地址排序的链表 |
| `unsigned long subtree_max_size` | 在 **空闲树**（`free_vmap_area_root`）里：子树最大空洞，分配时用来剪枝 |
| `struct vm_struct *vm` | 在 **占用树**（`vmap_area_root`）里：对应的 `vm_struct`。和上一字段同 union |
| `unsigned long flags` | `vm_map_ram` 等类型标记 |

一个 `vmap_area` 要么在空闲树，要么在占用树，所以 `subtree_max_size` 和 `vm` 可以叠。

---

## 5.4 分配路径

```text
vmalloc(size)                              # 宏 → vmalloc_noprof，mm/vmalloc.c:4198
  → __vmalloc_node_range_noprof            # :4025
       → __get_vm_area_node                # :3219，在 vmalloc 区找一段 va
            → alloc_vmap_area              # 从空闲 vmap_area 切
       → 向 buddy 要 nr_pages 个页（gfp 由调用者给，通常 GFP_KERNEL）
       → vmap_pages_range                   # :743，把页填进内核页表
            → 可能 vmap_try_huge_pmd/pud   # 开了 huge vmalloc 且物理也齐的时候
       → 刷新 TLB

vmap(pages, count, flags, prot)            # :3559
  已有 struct page *[]，只建内核映射，不再分配物理页
```

`__get_vm_area_node` 会在末尾留 guard page（除非标志不要），用来抓住越界。所以 `vm_struct.size` 经常比 `requested_size` 大一页。

释放走 `vfree`：摘掉页表、把页还 buddy、把 `vmap_area` 还回空闲树。`vfree` 可以在中断里调用，真正的拆页表会延后到工作队列。这一点和 `kfree` 不同，不要在 NMI 里 vfree。

---

## 5.5 和模块、ioremap

模块的可执行映射在 x86-64 文档布局里是 `ffffffffa0000000` 起的 module mapping space，不是 vmalloc 区那 32TB。`module_alloc()` 有自己的窗口，因为调用指令的相对偏移有范围。`/proc/vmallocinfo` 里能看到模块，不代表它们和 `vmalloc()` 共用同一段基址。

`ioremap` 复用 vmalloc 的虚拟地址分配器，`vm_struct.phys_addr` 指向设备物理地址，`pages` 通常为空。不要把 ioremap 的指针交给 `vfree`，用 `iounmap`。

---

## 5.6 highmem 与 `kmap_local_page`

**结论**：highmem 是 32 位机上「物理页在内核直接映射窗口之外」的那一段。要读写它的内容，得临时借一个内核虚拟地址。x86-64 的物理内存都在直接映射里，`PageHighMem` 为假，`kmap_local_page` 就是 `page_address`。

`include/linux/highmem-internal.h` 按 `CONFIG_HIGHMEM` 给了两套。

开了 HIGHMEM 时（`:71`）：

```c
static inline void *kmap_local_page(const struct page *page)
{
	return __kmap_local_page_prot(page, kmap_prot);
}
```

`__kmap_local_page_prot` 在每个 CPU 的临时窗口里建一条映射。窗口是栈式的：后 map 的必须先 `kunmap_local`。不能拿着这个指针睡觉，不能把指针交给别的 CPU，不能在中断里假设进程上下文的 kmap 还在。`kmap_local_folio`（`:84`）在页内再加上 `offset % PAGE_SIZE`。

老的 `kmap` / `kmap_atomic` 还在。`kmap_atomic`（`:116`）在 local 映射之外再 `preempt_disable`（PREEMPT_RT 上是 `migrate_disable`）并 `pagefault_disable`。新代码用 `kmap_local_page`，不要再写 `kmap_atomic`。

没开 HIGHMEM 时（`:163` 的 `#else`，x86-64 走这里）：

```c
static inline void *kmap_local_page(const struct page *page)
{
	return page_address(page);   /* :186，直接映射，没有临时窗口 */
}
```

`kmap`（`:170`）同样返回 `page_address`。`kmap_local_page_try_from_panic` 在 HIGHMEM 配置下对高端页返回 NULL（`:76`，panic 路径不能再去建映射）；非 HIGHMEM 配置下仍是 `page_address`（`:191`）。

所以在 x86-64 上看到 `kmap_local_page`，把它读成「拿到这页的内核虚拟地址，并且写成和 32 位同一套调用形式」。不要在 x86-64 的代码里为 highmem 预留 PKMAP 窗口。

---

## 5.7 vmalloc 内核栈

**结论**：`CONFIG_VMAP_STACK` 把内核栈放进 vmalloc 区，栈底（或栈顶，看架构增长方向）外侧是 vmalloc 自带的 guard page。溢出立刻缺页，而不是悄悄改写相邻栈。x86 在 `HAVE_ARCH_VMAP_STACK` 之内，文档 `Documentation/mm/vmalloced-kernel-stacks.rst` 列了 arm64、powerpc、riscv、s390、um、x86。

`dup_task_struct`（`kernel/fork.c:915`）调用 `alloc_thread_stack_node`（`:930`）。`CONFIG_VMAP_STACK` 下的实现（`:333`）：

```text
alloc_thread_stack_node
  → alloc_thread_stack_node_from_cache   # :338，CPU 下线时缓存过的 vm 栈
  → 没有缓存：__vmalloc_node(THREAD_SIZE, THREAD_ALIGN, GFP_VMAP_STACK, ...)  # :359
       GFP_VMAP_STACK = GFP_KERNEL | __GFP_ZERO | __GFP_SKIP_KASAN   # :208
  → find_vm_area，记到 tsk->stack_vm_area
```

guard page 来自 vmalloc 划虚拟区间时留的那一页（[5.4](#54-分配路径)），不是栈内容里的魔数。`set_task_stack_end_magic`（`fork.c:907`）另在栈底写 `STACK_END_MAGIC`，用来在溢出发生后确认「这块栈被踩过」。魔数抓不到所有溢出；guard page 才能在写越界的那一条指令上故障。

`/proc/meminfo` 的 `KernelStack` 是 `NR_KERNEL_STACK_KB`（`fs/proc/meminfo.c:111`），按栈占用的页记账，不含 guard page 本身（guard 没有 `struct page` 的用户数据）。

没开 `CONFIG_VMAP_STACK` 时，`alloc_thread_stack_node` 是同文件 `:410` 或 `:444` 的另一套实现，直接从页分配器拿连续页，没有 guard page。溢出的表现回到「踩邻栈」。

---

## 5.8 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `vmalloc_noprof` | `mm/vmalloc.c:4198` | `vmalloc()` 的实现 |
| `__vmalloc_node_range_noprof` | `:4025` | 指定节点、对齐、起止虚拟范围、gfp、prot |
| `vmalloc_node_noprof` | `:4279` | 指定节点 |
| `vmalloc_user_noprof` | `:4257` | 给用户 mmap 用的零页映射，页会被标成可回收 |
| `vmalloc_32_noprof` | `:4485` | 物理地址落在 32 位以内，给 DMA 掩码窄的设备 |
| `__get_vm_area_node` | `:3219` | 划虚拟区间 |
| `vmap` | `:3559` | 把现成的 page 数组映射进内核 |
| `vmap_pfn` | `:3633` | 按 PFN 映射 |
| `vmap_pages_range` | `:743` | 填页表 |
| `vmalloc_to_page` | `mm.h:1885` | vmalloc 地址 → `struct page` |
| `vfree` | `mm/vmalloc.c:3484` | 释放。可以在中断上下文调用，拆页表会延后 |
| `__kvmalloc_node_noprof` | `mm/slub.c:7000` | 先 kmalloc 再 vmalloc |
| `kmap_local_page` | `highmem-internal.h:71`（HIGHMEM）/ `:186`（否则） | 临时内核映射，或直接 `page_address` |
| `kmap_local_folio` | `:84` / `:196` | folio 内偏移 |
| `alloc_thread_stack_node` | `kernel/fork.c:333`（VMAP_STACK） | 内核栈，vmalloc + guard |
| `__vmalloc_node` | 由 `fork.c:359` 调用 | vmap 栈的物理+虚拟分配 |

---

[上一节 SLUB](./04-slab.md) · [目录](./README.md) · [下一节 mm/VMA](./06-mm-and-vma.md)
