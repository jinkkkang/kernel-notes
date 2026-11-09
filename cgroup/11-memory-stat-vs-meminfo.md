# 11. `memory.stat` 对账 `/proc/meminfo`

**结论**：根目录的 `memory.stat` **可以近似看作全系统内存**，但只对**用户态那部分**成立：`file`、`shmem`、LRU 四项能和 `/proc/meminfo` 逐字节对上；`anon`、`file_mapped` 有几百 KB 到几 MB 的偏差。**内核态那部分对不上**，`slab_unreclaimable`、`vmalloc` 在 memcg 侧只有 meminfo 的零头——因为大量内核分配根本不带 `__GFP_ACCOUNT`，或者发生在根 cgroup / 内核线程上下文里，压根不进 memcg 的账本。

> 源码：`mm/memcontrol.c`、`fs/proc/meminfo.c`、`mm/slab.h`、`kernel/cgroup/rstat.c`
> 文档：`cgroup-v2.rst` 目录 **5-2**（Memory）、**5-2-3**（Memory Ownership）
> 实测脚本：[demos/03_stat_vs_meminfo.sh](./demos/03_stat_vs_meminfo.sh)（只读，sampled on 6.8）

---

## 11.1 两个文件是怎么生成的

**`/proc/meminfo`** 在 `meminfo_proc_show()`（`fs/proc/meminfo.c:34`）里，绝大多数值直接读**全局 node 计数器**：

```c
	cached = global_node_page_state(NR_FILE_PAGES) -
		 total_swapcache_pages() - i.bufferram;    /* Cached 显式扣掉 Buffers 和 swapcache */
	if (cached < 0)
		cached = 0;

	for (lru = LRU_BASE; lru < NR_LRU_LISTS; lru++)
		pages[lru] = global_node_page_state(NR_LRU_BASE + lru);
	...
	show_val_kb(m, "Cached:         ", cached);
	show_val_kb(m, "AnonPages:      ", global_node_page_state(NR_ANON_MAPPED));
	show_val_kb(m, "Mapped:         ", global_node_page_state(NR_FILE_MAPPED));
	show_val_kb(m, "Shmem:          ", i.sharedram);
	show_val_kb(m, "SReclaimable:   ", sreclaimable);
	show_val_kb(m, "SUnreclaim:     ", sunreclaim);
	show_val_kb(m, "VmallocUsed:    ", vmalloc_nr_pages());
```

**`memory.stat`** 在 `memcg_stat_format()`（`mm/memcontrol.c:1597`），先把 rstat 刷到汇总计数，再按 `memory_stats[]`（`:1524`）逐项打印：

```c
static const struct memory_stat memory_stats[] = {
	{ "anon",			NR_ANON_MAPPED			},
	{ "file",			NR_FILE_PAGES			},
	{ "kernel",			MEMCG_KMEM			},
	{ "kernel_stack",		NR_KERNEL_STACK_KB		},
	{ "pagetables",			NR_PAGETABLE			},
	{ "percpu",			MEMCG_PERCPU_B			},
	{ "sock",			MEMCG_SOCK			},
	{ "vmalloc",			MEMCG_VMALLOC			},
	{ "shmem",			NR_SHMEM			},
	...
	{ "inactive_anon",		NR_INACTIVE_ANON		},
	{ "active_anon",		NR_ACTIVE_ANON			},
	{ "inactive_file",		NR_INACTIVE_FILE		},
	{ "active_file",		NR_ACTIVE_FILE			},
	{ "unevictable",		NR_UNEVICTABLE			},
	{ "slab_reclaimable",		NR_SLAB_RECLAIMABLE_B		},
	{ "slab_unreclaimable",		NR_SLAB_UNRECLAIMABLE_B		},
	/* 之后是 workingset_*、pgscan/pgsteal、pgfault 等累计事件 */
};
```

```c
static void memcg_stat_format(struct mem_cgroup *memcg, struct seq_buf *s)
{
	...
	mem_cgroup_flush_stats(memcg);          /* 先把 per-CPU 增量刷到汇总，见 11.4 */
	for (i = 0; i < ARRAY_SIZE(memory_stats); i++) {
		size = memcg_page_state_output(memcg, memory_stats[i].idx);
		seq_buf_printf(s, "%s %llu\n", memory_stats[i].name, size);
```

两点先记住：

- 单位是**字节**，meminfo 是 kB；对账时除以 1024。
- `memory_stats[]` 里的 `NR_*` 和 meminfo 读的是**同一组 node 计数器**，这是"能对上"的根本原因。`MEMCG_*` 系列是 memcg 私有计数器，meminfo 里没有对应项。

---

## 11.2 实测对账

```text
memory.stat            meminfo            memcg_kB meminfo_kB     diff
file                   Cached+Buffers      2215764    2215764        0
shmem                  Shmem                136216     136216        0
anon                   AnonPages           4209024    4208872     -152
active_file            Active(file)         432176     432176        0
inactive_anon          Inactive(anon)      2215144    2215144        0
file_mapped            Mapped              555500     559592    4092
unevictable            Unevictable           28960      28960        0

slab_reclaimable       SReclaimable         569469     639168    69699
slab_unreclaimable     SUnreclaim            42947     216508   173561
vmalloc                VmallocUsed            3752      46004    42252
kernel_stack           KernelStack           14528      16252     1724
percpu                 Percpu                 3010       7424     4414

MemTotal - MemFree            =  7449300 kB
memcg anon + file + kernel    =  7115868 kB
unaccounted                   =   333432 kB
```

**用户态：对得上。** `file`、`shmem`、`active_file`、`inactive_anon`、`unevictable` 差值精确为 0。原因有两层：

1. **每个 LRU 页都有主人。** 匿名页、页缓存页在被加入 LRU 时，都已经 `commit_charge` 到了某个 memcg（进程在根 cgroup 的页，主人是 `root_mem_cgroup`），不存在"无主页"。
2. **`file` 要配 `Cached + Buffers`。** memcg 的 `file` 直接取 `NR_FILE_PAGES`，而 meminfo 的 `Cached` 减掉了 `Buffers` 和 swapcache，所以对应关系是 `file = Cached + Buffers`，不是一个对一个。

**两个小偏差的解释：**

- `anon` 差 152 kB（约 38 页）。`memory.stat` 读之前先 `mem_cgroup_flush_stats()`，flush 有节流（见 11.4），比 meminfo 读全局计数器晚一瞬；这期间有页进出就会差一点。
- `file_mapped` 差 4092 kB。`Mapped` 还会统计**共享匿名映射**（`mmap(MAP_SHARED|MAP_ANONYMOUS)`，即 shmem 被映射的部分），而 memcg 的 `NR_FILE_MAPPED` 只算文件页映射。两者语义原本就不同，差值就是这部分的量。

---

## 11.3 内核态：为什么对不上

memcg 记账内核内存要同时满足下面三条。

### 条件一：分配带了 `__GFP_ACCOUNT` 或所在 slab 带 `SLAB_ACCOUNT`

SLUB 的分配路径，`slab_alloc_node() → slab_post_alloc_hook() → memcg_slab_post_alloc_hook()`。判断在 `memcg_slab_pre_alloc_hook`（`mm/slab.h:479`）：

```c
static inline bool memcg_slab_pre_alloc_hook(struct kmem_cache *s,
					     struct list_lru *lru,
					     struct obj_cgroup **objcgp,
					     size_t objects, gfp_t flags)
{
	...
	/* 既没带 __GFP_ACCOUNT，slab cache 也没标 SLAB_ACCOUNT -> 直接不记账 */
	if (!(flags & __GFP_ACCOUNT) && !(s->flags & SLAB_ACCOUNT))
		return true;

	objcg = get_obj_cgroup_from_current();
	if (!objcg)
		return true;
```

`__GFP_ACCOUNT` 的定义（`include/linux/gfp_types.h:49`）：

```c
#define ___GFP_ACCOUNT		0x400000u
#define __GFP_ACCOUNT	((__force gfp_t)___GFP_ACCOUNT)
...
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)          /* 0xcc0，没有 ACCOUNT 位 */
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)                /* 0x40cc0 */
```

一个普通 `kmalloc(sizeof(struct foo), GFP_KERNEL)` 的 gfp 是 `0xcc0`，**ACCOUNT 位是 0**。所以设备驱动、子系统里大量 `kmalloc` 出来的对象，**永远不会出现在任何 `memory.stat` 里**。只有 dentry/inode 缓存、`kvasprintf`、页表等少数路径显式用了 `GFP_KERNEL_ACCOUNT` 或把 cache 标成 `SLAB_ACCOUNT`。

### 条件二：当前上下文属于某个非根 memcg

`get_obj_cgroup_from_current()`（`mm/memcontrol.c:3050`）→ `memcg_kmem_bypass()`（`:1100`）：

```c
static __always_inline bool memcg_kmem_bypass(void)
{
	/* Allow remote memcg charging from any context. */
	if (unlikely(active_memcg()))
		return false;

	/* Memcg to charge can't be determined. */
	if (!in_task() || !current->mm || (current->flags & PF_KTHREAD))
		return true;
	return false;
}
```

内核线程（`PF_KTHREAD`）和中断上下文**直接 bypass**。再加上 `__get_obj_cgroup_from_memcg()`（`:3041` 附近）向上找时**跳过 root**：

```c
	for (; !mem_cgroup_is_root(memcg); memcg = parent_mem_cgroup(memcg)) {
		objcg = rcu_dereference(memcg->objcg);
		if (objcg && obj_cgroup_tryget(objcg))
			break;
		objcg = NULL;
	}
	return objcg;
```

进程在根 cgroup 时一路走到 `root_mem_cgroup`，循环条件不成立，返回 NULL，**不记账**。所以即使是"本进程带 ACCOUNT 的分配"，只要进程住在根 cgroup，也进不了 memcg 统计。

### 条件三：不同字段的更新点不一样

页表计数不在 charge 路径上，而是页表页创建时直接加到 lruvec（`include/linux/mm.h:2983`）：

```c
static inline bool pagetable_pte_ctor(struct ptdesc *ptdesc)
{
	struct folio *folio = ptdesc_folio(ptdesc);

	if (!ptlock_init(ptdesc))
		return false;
	__folio_set_pgtable(folio);
	lruvec_stat_add_folio(folio, NR_PAGETABLE);   /* 页表页本身一定属于某个 mm，必有 memcg */
	return true;
}
```

页表页跟着 `mm_struct` 走，所以它记账相对完整（实测只差 1.7 M，主要是 init_mm 和内核线程）。

`vmalloc` 则是另一套：`__vmalloc_node_range` 里只有两个动作（`mm/vmalloc.c:3192`）：

```c
	atomic_long_add(area->nr_pages, &nr_vmalloc_pages);     /* 全局计数：meminfo 的 VmallocUsed */
	if (gfp_mask & __GFP_ACCOUNT) {
		int i;

		for (i = 0; i < area->nr_pages; i++)
			mod_memcg_page_state(area->pages[i], MEMCG_VMALLOC, 1);   /* memcg 侧 */
	}
```

`nr_vmalloc_pages` 无条件累加；`MEMCG_VMALLOC` 需要 `__GFP_ACCOUNT`。`vmalloc()` 的 gfp 是 `GFP_KERNEL`，不带 ACCOUNT；`vmalloc_huge()` / `kvmalloc()` 同样只是原样透传调用者的 gfp（`vmalloc.c:3488`、`mm/util.c:659`），只有调用者写 `kvmalloc(size, GFP_KERNEL_ACCOUNT)` 时才会进 memcg。这就是 `vmalloc` 在实测里只有 3752 kB 对 46004 kB 的原因。

### 落到具体字段

| 字段 | 实测差值 | 主要来源 |
|---|---|---|
| `slab_unreclaimable` | 216508 → 42947，差 173 M | 绝大多数 `kmalloc(GFP_KERNEL)` 缓存没标 ACCOUNT |
| `slab_reclaimable` | 639168 → 569469，差 70 M | dentry/inode 记账了，但很多其它可回收缓存没记；另有 root/内核线程分配 |
| `vmalloc` | 46004 → 3752，差 42 M | `vmalloc()` 走 GFP_KERNEL；驱动大量使用 |
| `kernel_stack` | 16252 → 14528，差 1.7 M | 内核线程的栈；`PF_KTHREAD` 被 bypass |
| `percpu` | 7424 → 3010，差 4.4 M | `MEMCG_PERCPU_B` 只记带 ACCOUNT 的 percpu 分配 |

还有一块**两边都不在明细里**的内存：驱动直接 `alloc_pages` 拿走的页、hugetlb 预留（这台机器为 0）。它只能从 `MemTotal - MemFree - 各项之和` 推出来，实测约 333 M。

---

## 11.4 为什么 `anon` 会有抖动：rstat 的延迟刷新

每个 CPU 上的 charge 增量先写本 CPU 的 `vmstats_percpu`（`__mod_memcg_lruvec_state`，`memcontrol.c:820`），`memcg_rstat_updated()`（`:709`）只在增量累积到 `MEMCG_CHARGE_BATCH * num_online_cpus()` 时才把"可刷"标记打上：

```c
static inline void memcg_rstat_updated(struct mem_cgroup *memcg, int val)
{
	...
	__cgroup_rstat_updated(memcg->css.cgroup, cpu);
	...
		if (stats_updates < MEMCG_CHARGE_BATCH)
			continue;
		if (!memcg_vmstats_needs_flush(statc->vmstats))
			atomic64_add(stats_updates, &statc->vmstats->stats_updates);
```

`memory.stat` 读之前会调 `mem_cgroup_flush_stats()`（`:754`），但只有超过阈值才真的刷。`mem_cgroup_css_rstat_flush`（`:5611`）把本层增量加进 `state_local`，同时 `parent->vmstats->state_pending[i] += delta` **向上传播**——这就是"根目录 `memory.stat` = 整棵树之和"的实现方式。传播不是实时的，所以根目录看到的数和 meminfo 的全局计数器之间天然存在几页的窗口期。

同样地，进程退出后页缓存仍记在原 memcg 上（见 [06](./06-memory.md) 的归属规则），根目录照样统计它，所以这类差异在总账上不会出现。

---

## 11.5 实用结论

| 想看什么 | 用哪个 |
|---|---|
| 全系统匿名页、页缓存、活跃/非活跃 LRU、当前内存状态 | 根 `memory.stat`（字段更细，还能看到 `workingset_refault_file`） |
| 全系统内核内存（slab / vmalloc / 页表 / 内核栈）总量 | **必须** `/proc/meminfo`，必要时配合 `slabtop`、`/proc/vmallocinfo` |
| 某个容器 / 服务的"真实内存开销" | `memory.current` + `memory.stat`，但要知道它**不含没记账的内核分配**，真实开销更大 |
| 判断是否在发生回收 / refault | 根或目标的 `workingset_refault_*`、`pgscan_*`、`pgsteal_*` |

最容易踩的坑：**把 `memory.stat` 的 `kernel` 当成 `kernel_stack + pagetables + percpu + vmalloc + slab`**。文档（`cgroup-v2.rst` 的 `kernel` 条目）确实这么描述，但代码里这些是**彼此独立的计数器**，各自在不同路径上 `mod`：

- `kernel` = `MEMCG_KMEM`，在 `obj_cgroup_charge_pages()`（`memcontrol.c:3130`）里 `try_charge_memcg` 成功后由 `memcg_account_kmem()`（`:3092`）累加，记的是**按页 charge 的内核内存**；
- `pagetables`、`slab_reclaimable/unreclaimable`、`kernel_stack`、`percpu` 各自由 `pagetable_pte_ctor()`、SLUB 的 objcg 记账、栈分配等**分别**更新。

所以 `kernel` 与 `kernel_stack`、`slab_*` 之间**没有包含关系**，直接相加会重复计算。这也是本笔记的约定：**"对不上"时先回到代码确认每个字段来自哪个计数器，不要相信文档的归类描述。**

---

*对照：`fs/proc/meminfo.c`（`meminfo_proc_show` `:34`）、`mm/memcontrol.c`（`memory_stats[]` `:1524`、`memcg_stat_format` `:1597`、`memcg_rstat_updated` `:709`、`mem_cgroup_css_rstat_flush` `:5611`、`get_obj_cgroup_from_current` `:3050`、`memcg_kmem_bypass` `:1100`）、`mm/slab.h`（`memcg_slab_pre_alloc_hook` `:479`）、`include/linux/gfp_types.h`（`___GFP_ACCOUNT` `:49`）、`include/linux/mm.h`（`pagetable_pte_ctor` `:2983`）、`mm/vmalloc.c`（`MEMCG_VMALLOC` `:3192`）。*
