# 4. SLUB

**结论**：v6.6.160 的小对象分配器默认是 SLUB。每个 CPU 有一份 `struct kmem_cache_cpu`，里面的 `freelist` 指向下一个空闲对象。快路径把这个指针和事务号 `tid` 一起做一次 cmpxchg，拨到下一个对象，不用锁。拨不动、链表空了、或者节点不对，才进 `___slab_alloc`：先用本 CPU 上缓存的 partial slab，再去节点的 partial 链表，最后向伙伴系统要一块新 slab。`kmalloc` 只按大小选一个通用 cache，然后走同一条路。

`CONFIG_SLUB_TINY` 打开时没有这份 per-CPU 结构。每次分配直接在 partial 链表里拿，没有就向伙伴系统要新 slab。

> `struct kmem_cache` / `struct kmem_cache_cpu`：`include/linux/slub_def.h`。
> slab 页描述符 `struct slab`、节点 `struct kmem_cache_node`：`mm/slab.h`。
> 分配实现：`mm/slub.c`。`kmalloc` 的大小分档在 `include/linux/slab.h` 和 `mm/slab_common.c`。
> 导航：[上一篇 CMA](./03-09-cma.md) · [目录](./README.md) · [下一节 vmalloc](./05-vmalloc-and-kernel-va.md)

配置菜单里还可以选 `SLAB (DEPRECATED)`（`CONFIG_SLAB_DEPRECATED`，`mm/Kconfig:249`）。选它时 `struct slab` 是另一套成员。本章只写选了 `CONFIG_SLUB` 时编译出来的结构和路径。

---

## 4.1 对象、slab、cache

SLUB 管的是「大小固定的小块」。调用者要 64 字节，就从一个专门装 64 字节对象的 cache 里拿一块，而不是每次都向伙伴系统要一整页。

| 词 | 是什么 |
|---|---|
| object | `kmalloc` 或 `kmem_cache_alloc` 返回的那一块，大小在建 cache 时定死 |
| slab | 从伙伴系统来的一页或几页，切成同尺寸对象。描述符是 `struct slab`，和 `struct page` 叠在同一块内存上 |
| `kmem_cache` | 一种尺寸（加上对齐、标志、构造函数）的管理结构 |
| freelist | 空闲对象串成的链表。CPU 上有一条可以无锁拨动的，slab 上还有一条要加锁的 |
| cpu slab | 这个 CPU 当前专用的那块 slab，记在 `kmem_cache_cpu.slab` |
| partial | 只用了一部分对象的 slab。节点链表在 `kmem_cache_node.partial`；开了 `CONFIG_SLUB_CPU_PARTIAL` 时，CPU 上还能再留几块 |

`mm/slab.h:109` 的 `SLAB_MATCH` 用静态断言把 `struct slab` 和 `struct page` 的对应字段钉在同一偏移：`flags` 对 `__page_flags`，`compound_head` 对 `slab_cache`（注释要求 bit 0 是 0），`_refcount` 对 `__page_refcount`。开了 `CONFIG_MEMCG` 时 `memcg_data` 也对齐。`struct slab` 不得比 `struct page` 大（`:118`）。

普通 cache 用满后不挂在链表上。有对象释放回来，它再出现在 partial 上。空了就还回伙伴系统。文件头注释写在 `mm/slub.c:138`。`CONFIG_SLUB_DEBUG` 给节点留了 `full` 链，但 `add_full` 只在 cache 带了 `SLAB_STORE_USER` 时才把 slab 挂上去（`slub.c:1381`）。

---

## 4.2 `struct kmem_cache`（`include/linux/slub_def.h:98`）

`slab_flags_t` 是 `unsigned int`（`include/linux/types.h:156`）。`MAX_NUMNODES` 是编译期的节点上限。

```c
struct kmem_cache {
#ifndef CONFIG_SLUB_TINY
	/* 每个 CPU 一份。快路径只碰自己这份。 */
	struct kmem_cache_cpu __percpu *cpu_slab;
#endif
	/* --- 尺寸和空闲指针 --- */
	/* 取 partial、调试、回收时看的标志，取值是下面的 SLAB_* */
	slab_flags_t flags;
	unsigned long min_partial;	/* 节点上至少留多少 partial slab */
	unsigned int size;		/* 含元数据的步长，在 slab 里往前跳用它 */
	unsigned int object_size;	/* 调用者请求的大小，不含元数据 */
	struct reciprocal_value reciprocal_size; /* size 的倒数，用乘法算对象下标 */
	unsigned int offset;		/* 空闲对象把 next 指针塞在对象体内的偏移 */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	/*
	 * 每个 CPU 打算留多少空闲对象。真正卡住的是下面的 slab 个数：
	 * 按「slab 半满」把对象数换算成 cpu_partial_slabs（slub.c:507）。
	 */
	unsigned int cpu_partial;
	unsigned int cpu_partial_slabs;	/* 每个 CPU 最多留几块 partial slab */
#endif
	/* --- 向伙伴系统要页 --- */
	struct kmem_cache_order_objects oo;	/* 首选 order 和这块 slab 能放的对象数 */
	struct kmem_cache_order_objects min;	/* 首选 order 失败时允许降到的 order */
	gfp_t allocflags;		/* 向伙伴系统要 slab 页时额外带的 GFP */
	/* --- 身份 --- */
	int refcount;			/* 还有几处引用。减到 0 才能 destroy */
	void (*ctor)(void *);		/* 构造函数。只在新 slab 上摆对象时调用 */
	unsigned int inuse;		/* 元数据起点。对象可用区间是 [0, inuse) */
	unsigned int align;		/* 对齐 */
	unsigned int red_left_pad;	/* 左侧 redzone 填充。不开调试时是 0 */
	const char *name;		/* /proc/slabinfo 和 sysfs 上的名字 */
	struct list_head list;		/* 挂到全局 slab_caches */
#ifdef CONFIG_SYSFS
	struct kobject kobj;		/* /sys/kernel/slab/<name> */
#endif
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	unsigned long random;		/* 给 freelist 指针异或的随机数 */
#endif
#ifdef CONFIG_NUMA
	/*
	 * 多大比例去别的节点找 partial，用来把碎片凑到一起。
	 * 0 表示只在本节点拿。读 sysfs 时 100 对应这里的 1000（slub.c:2388）。
	 */
	unsigned int remote_node_defrag_ratio;
#endif
#ifdef CONFIG_SLAB_FREELIST_RANDOM
	unsigned int *random_seq;	/* 新 slab 上对象入链的随机顺序 */
#endif
#ifdef CONFIG_KASAN_GENERIC
	struct kasan_cache kasan_info;	/* 通用 KASAN 的元数据偏移 */
#endif
#ifdef CONFIG_HARDENED_USERCOPY
	unsigned int useroffset;	/* 允许和用户空间互拷的起点 */
	unsigned int usersize;		/* 允许互拷的长度 */
#endif
	struct kmem_cache_node *node[MAX_NUMNODES]; /* 每个 NUMA 节点一份 */
};
```

`kmem_cache_order_objects`（`slub_def.h:91`）只有一个 `unsigned int x`。高位是 order，低位是对象个数，`oo_order` / `oo_objects`（`slub.c:496`、`:501`）负责拆开。

`struct kasan_cache`（`include/linux/kasan.h:294`，只在 `CONFIG_KASAN_GENERIC` 下有定义）是两个 `int`：`alloc_meta_offset` 和 `free_meta_offset`。

和这些字段绑在一起的配置（`mm/Kconfig`）：

| 选项 | 条件 | 作用 |
|---|---|---|
| `SLUB_TINY` | `SLUB && EXPERT`，默认关 | 去掉 per-CPU 快路径，换小内存。Kconfig 写明不建议用于 16MB 以上的机器 |
| `SLUB_CPU_PARTIAL` | `SLUB && SMP && !SLUB_TINY`，默认开 | 每个 CPU 再留一小串已冻结的 partial slab |
| `SLUB_STATS` | `SLUB && SYSFS && !SLUB_TINY`，默认关 | 在 `kmem_cache_cpu.stat[]` 里计数 |
| `SLAB_FREELIST_RANDOM` | `SLUB && !SLUB_TINY` | 新 slab 的空闲链顺序打乱 |
| `SLAB_FREELIST_HARDENED` | `SLUB && !SLUB_TINY` | freelist 指针异或随机数 |
| `RANDOM_KMALLOC_CACHES` | `SLUB && !SLUB_TINY`，默认关 | 普通 kmalloc 多备几份 cache，按调用地址挑选 |

`SLAB_SUPPORTS_SYSFS`（`slub_def.h:158`）在 `CONFIG_SYSFS` 打开且没有 `SLUB_TINY` 时成立。这时 `slab_kset` 挂在 `kernel_kobj` 下，名字是 `"slab"`（`slub.c:6289`），也就是 `/sys/kernel/slab/<name>/`。

本章会碰到的 `SLAB_*`（`include/linux/slab.h:30` 起）：

| 标志 | 值 | 作用 |
|---|---|---|
| `SLAB_CONSISTENCY_CHECKS` | `0x00000100` | 分配和释放时做检查 |
| `SLAB_RED_ZONE` | `0x00000400` | 对象两侧放红区，抓越界 |
| `SLAB_POISON` | `0x00000800` | 用固定字节填充，抓使用未初始化内存 |
| `SLAB_KMALLOC` | `0x00001000` | 这是 kmalloc 的通用 cache |
| `SLAB_HWCACHE_ALIGN` | `0x00002000` | 对象按 cache line 对齐 |
| `SLAB_STORE_USER` | `0x00010000` | 记下最后一次分配和释放的调用者 |
| `SLAB_TYPESAFE_BY_RCU` | `0x00080000` | 延迟释放 slab 页，不延迟释放对象 |
| `SLAB_RECLAIM_ACCOUNT` | `0x00020000`；`SLUB_TINY` 下是 0（`slab.h:161`） | 对象可回收，计入 `SReclaimable` |

调试类标志会把分配赶出无锁快路径。`SLAB_DEBUG_FLAGS`（`mm/slab.h:331`）在 `CONFIG_SLUB_DEBUG` 下是 `SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | SLAB_TRACE | SLAB_CONSISTENCY_CHECKS`。这个选项依赖 `SLUB && SYSFS && !SLUB_TINY`，默认开（`mm/Kconfig.debug:56`）。没开时宏是 0。`slub.c:164` 的注释写明：带这些标志的 cache 走 `list_lock` 保护的路径，并关掉 lockless freelist。

---

## 4.3 配套结构

### `struct kmem_cache_cpu`（`slub_def.h:50`）

整个结构包在 `#ifndef CONFIG_SLUB_TINY` 里。`SLUB_TINY` 下没有它，`kmem_cache` 里也没有 `cpu_slab`。

`freelist` 和 `tid` 放在一个 union 里，是为了能和 `this_cpu_cmpxchg_double()` 要求的对齐一起交换。`freelist_aba_t` 定义在 `mm/slab.h:33`。

```c
/* mm/slab.h:33。64 位上 freelist_full_t 是 u128，32 位上是 u64。 */
typedef union {
	struct {
		void *freelist;		/* 链表头 */
		unsigned long counter;	/* 和指针一起交换的计数，这里用来装 tid */
	};
	freelist_full_t full;		/* 上面两栏合成的一个字，一次 cmpxchg */
} freelist_aba_t;

struct kmem_cache_cpu {
	union {
		struct {
			void **freelist;	/* 下一个能拿走的对象 */
			unsigned long tid;	/* 本 CPU 上单调增加的事务号 */
		};
		freelist_aba_t freelist_tid;	/* 上面两个字段的整体 */
	};
	struct slab *slab;		/* 当前这条 freelist 属于哪块 slab */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	struct slab *partial;		/* 本 CPU 缓存的、已冻结的 partial slab */
#endif
	local_lock_t lock;		/* 保护上面这些字段。无锁快路径不拿它 */
#ifdef CONFIG_SLUB_STATS
	unsigned stat[NR_SLUB_STAT_ITEMS]; /* 本 CPU 的分配/释放计数 */
#endif
};
```

`stat[]` 的下标是 `enum stat_item`（`slub_def.h:15`）。快路径会计 `ALLOC_FASTPATH`，慢路径会计 `ALLOC_SLOWPATH`、`ALLOC_REFILL`、`ALLOC_FROM_PARTIAL`、`ALLOC_SLAB`、`CPU_PARTIAL_ALLOC`、`ORDER_FALLBACK` 等。没开 `CONFIG_SLUB_STATS` 时这些 `stat()` 是空操作。

`tid` 的步长是 `TID_STEP`（`slub.c:2453`）。开了抢占时它是不小于 `CONFIG_NR_CPUS` 的 2 的幂，这样不同 CPU 的事务号不会撞车；没开抢占时步长是 1（`:2459`）。

### `struct slab`（`mm/slab.h:42`，`CONFIG_SLUB` 这一支）

同一块内存，分配器把它看成 `struct slab`，别的代码把它看成 `struct page` / folio。互相转换用 `folio_slab`、`slab_folio`、`page_slab`、`slab_page`（`slab.h:132` 起）。

```c
struct slab {
	unsigned long __page_flags;	/* 与 page->flags 同一偏移 */

	/* --- 属于哪个 cache，以及空闲链 --- */
	struct kmem_cache *slab_cache;	/* 与 page->compound_head 同一偏移 */
	union {
		struct {
			union {
				struct list_head slab_list;	/* 挂在节点的 partial 或 full 上 */
#ifdef CONFIG_SLUB_CPU_PARTIAL
				struct {
					struct slab *next;	/* 串在 CPU partial 链上 */
					int slabs;		/* 这条链上还剩几块 */
				};
#endif
			};
			/* 双字边界，好让 freelist 和 counters 一次 cmpxchg */
			union {
				struct {
					void *freelist;		/* slab 自己的空闲链头 */
					union {
						unsigned long counters;	/* 下面的位域合成一个字 */
						struct {
							unsigned inuse:16;	/* 已分出的对象数 */
							unsigned objects:15;	/* 这块 slab 的对象总数 */
							/*
							 * 1 表示这块 slab 正被某个 CPU 独占，
							 * 节点链表先不要动它。
							 * 开了调试时，这个位也可以表示 slab 已损坏。
							 */
							unsigned frozen:1;
						};
					};
				};
#ifdef system_has_freelist_aba
				freelist_aba_t freelist_counter; /* freelist 和 counters 的整体 */
#endif
			};
		};
		struct rcu_head rcu_head;	/* 带 SLAB_TYPESAFE_BY_RCU 时延迟释放 */
	};
	unsigned int __unused;		/* 占位，好让后面的引用计数对齐到 page */

	atomic_t __page_refcount;	/* 与 page->_refcount 同一偏移 */
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;	/* 与 page 的 memcg 字段同一偏移 */
#endif
};
```

`slab->freelist` 和 `kmem_cache_cpu.freelist` 是两条链。CPU 那条给无锁快路径用。对象被别的 CPU 释放时，先挂回 slab 自己的那条；慢路径再把 slab 那条整段接过来（`slub.c:3110` 的注释）。`frozen == 1` 的 slab 不参与节点链表的摘挂，它专供某个 CPU 分配。

页是不是从 pfmemalloc 水位以下拿的，记在 folio 的 active 标志上，用 `slab_test_pfmemalloc`（`slab.h:181`）读。

### `struct kmem_cache_node`（`mm/slab.h:784`，`CONFIG_SLUB` 这一支）

```c
struct kmem_cache_node {
	spinlock_t list_lock;		/* 保护下面的链表 */
	unsigned long nr_partial;	/* partial 链表上的 slab 数 */
	struct list_head partial;	/* 部分使用的 slab */
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;		/* 本节点 slab 数 */
	atomic_long_t total_objects;	/* 本节点对象总数 */
	struct list_head full;		/* 用满的 slab。只有 SLAB_STORE_USER 的 cache 才挂进来 */
#endif
};
```

`get_node(s, node)` 就是 `s->node[node]`（`slab.h:814`）。

---

## 4.4 `kmalloc` / `kmem_cache_alloc` 路径

`kmem_cache_alloc` 是 `mm/slub.c:3540` 的函数。`include/linux/slab.h:500` 只有声明。`kmalloc` 是 `slab.h:591` 的 `static __always_inline`。

大小能在编译期看出来、并且不超过 `KMALLOC_MAX_CACHE_SIZE` 时，`kmalloc` 自己算下标，调用 `kmalloc_trace`。否则交给 `__kmalloc`。超过通用 cache 上限的，直接走伙伴系统。

SLUB 下 `KMALLOC_SHIFT_HIGH` 是 `PAGE_SHIFT + 1`（`slab.h:318`），所以 `KMALLOC_MAX_CACHE_SIZE`（`:328`）是两页。再大的请求用 `kmalloc_large`，上限是 `KMALLOC_MAX_SIZE`，也就是 `PAGE_SIZE << MAX_ORDER`。`MAX_ORDER` 默认 10（`include/linux/mmzone.h:29`），架构可以用 `CONFIG_ARCH_FORCE_MAX_ORDER` 改掉。

```text
kmalloc(size, flags)                         /* slab.h:591，inline */
  编译期已知 size，且 size > KMALLOC_MAX_CACHE_SIZE
    → kmalloc_large                        /* slab_common.c:1163 */
         → __kmalloc_large_node            /* :1138：alloc_pages_node，带 __GFP_COMP */
  编译期已知 size，能放进通用 cache
    → kmalloc_index / kmalloc_type         /* slab.h:486、:401，选出 kmalloc_caches[][] */
         → kmalloc_trace                   /* slab_common.c:1096 */
              → __kmem_cache_alloc_node    /* slub.c:3553 */
  运行期才知道 size
    → __kmalloc                            /* slab_common.c:1018 */
         → __do_kmalloc_node               /* :989 */
              size > KMALLOC_MAX_CACHE_SIZE
                → __kmalloc_large_node
              否则 kmalloc_slab            /* slab_common.c:709，按 size 和 gfp 选 cache */
                → __kmem_cache_alloc_node

kmem_cache_alloc(s, gfpflags)              /* slub.c:3540 */
  → __kmem_cache_alloc_lru(s, NULL, ...)   /* :3530，打 trace_kmem_cache_alloc */
       → slab_alloc                        /* :3523，node = NUMA_NO_NODE */
            → slab_alloc_node              /* :3493 */
```

`slab_alloc_node`（`slub.c:3493`）是真正的快路径入口，用 `__fastpath_inline` 标成尽量内联（没开 `SLUB_TINY` 时就是 `__always_inline`，`slub.c:192`）。函数体：

```c
s = slab_pre_alloc_hook(s, lru, &objcg, 1, gfpflags); /* slab.h:707。fault injection 可以让它返回 NULL */
object = kfence_alloc(s, orig_size, gfpflags);       /* kfence.h:118。抽中就直接返回这块采样对象 */
object = __slab_alloc_node(...);                     /* 下面两条实现，看 SLUB_TINY */
maybe_wipe_obj_freeptr(s, object);                   /* :3474。若释放时擦过对象，把体内的 freelist 指针清掉 */
slab_post_alloc_hook(...);                           /* slab.h:725。KASAN、kmemleak、memcg、按需清零 */
```

`kmalloc_type`（`slab.h:401`）按 GFP 选一组 cache，优先级是 `__GFP_DMA`、`__GFP_RECLAIMABLE`、`__GFP_ACCOUNT`。都没有时用 `KMALLOC_NORMAL`。开了 `CONFIG_RANDOM_KMALLOC_CACHES` 时，普通分配在 `KMALLOC_NORMAL` 再加 15 份拷贝里按调用地址哈希（`RANDOM_KMALLOC_CACHES_NR` 是 15，`slab.h:351`）。`SLUB_TINY` 把 `KMALLOC_RECLAIM` 折叠成 `KMALLOC_NORMAL`（`:375`）。

### 没开 `SLUB_TINY`：无锁 freelist

`__slab_alloc_node`（`slub.c:3368`）在开了抢占的情况下也能跑。它先读本 CPU 的 `kmem_cache_cpu` 和 `tid`，中间有一个 `barrier()`，再读 `freelist` 和 `slab`。`tid` 必须和这份 CPU 数据来自同一次观察：中途被抢占到别的 CPU，后面的 cmpxchg 会对不上，然后重试。

`USE_LOCKLESS_FAST_PATH()`（`slub.c:176`）在非 `PREEMPT_RT` 上是真。同时满足「有对象、有 slab、节点匹配」时：

1. 用 `get_freepointer_safe` 读出下一个对象。
2. `__update_cpu_freelist_fast` 原子地检查「freelist 和 tid 都没变」，然后写成「下一个对象 + 新 tid」。
3. 成功就记 `ALLOC_FASTPATH` 并返回。失败就 `goto redo`。

`PREEMPT_RT` 上这个宏是假（`:188`）。注释（`:110`）说明原因：RT 的 `local_lock` 既不关中断也不关抢占，无锁快路径会和正在走慢路径的操作撞车。于是每次都进 `__slab_alloc`，由 `local_lock` 配上 `migrate_disable` 把操作留在当前 CPU。freelist 仍然是那条链表，只是不再用 cmpxchg 去拨。

慢路径 `__slab_alloc`（`:3347`）在可能发生抢占时重新取一次 per-CPU 指针，然后调用 `___slab_alloc`（`:3129`）。`___slab_alloc` 的注释把顺序写在 `:3110`：

1. 本 CPU 的 `c->freelist` 还有对象：拿 `local_lock`，弹出一个，返回。
2. CPU freelist 空了，但 `c->slab` 自己的 freelist 还有（别的 CPU 释放回来的）：`get_freelist` 把整条接过来，弹出一个。这是 `ALLOC_REFILL`。
3. 这块 cpu slab 也空了：先看 `c->partial`（`CONFIG_SLUB_CPU_PARTIAL`）。有的话取下一块冻结的 slab，记 `CPU_PARTIAL_ALLOC`，回到第 1 步。
4. 再没有：`get_partial`（`:2430`）。
   - `get_partial_node`（`:2306`）在指定节点的 `kmem_cache_node.partial` 上拿。第一块交给本次分配，后面几块可以经 `put_cpu_partial` 放进 CPU partial。
   - 调用方没指定节点时，本节点没有，再 `get_any_partial`（`:2366`）。它看 `remote_node_defrag_ratio`：0 就直接放弃；非 0 时按 `get_cycles() % 1024` 决定这次要不要去别的节点，并且只考虑 `nr_partial > min_partial` 的节点。
   - 调用方指定了节点时，`get_partial` 不会再去别的节点。
5. partial 也没有：`new_slab`（`:2096`）→ `allocate_slab`（`:2029`）→ `alloc_slab_page`（`:1895`）。节点未知时调用 `alloc_pages`，指定了节点就调用 `__alloc_pages_node`。两者都进 `__alloc_pages`（`mm/page_alloc.c:4465`）。首选 `s->oo` 里的 order；失败再试 `s->min`，并记 `ORDER_FALLBACK`。新 slab 的对象在这里串成 freelist。`setup_object`（`slub.c:1880`）在这一步调用 `ctor`。
6. 调试 cache 不把整条 freelist 装上 CPU，而是 `alloc_single_from_partial` / `alloc_single_from_new_slab` 一次拿一个对象。

节点对不上，或者 pfmemalloc 水位对不上，会先 `deactivate_slab` 把当前 cpu slab 交回去，再走上面的第 3 步。

所以 slab 增长时，伙伴系统少掉的是 `oo` 或 `min` 里的那个 order，常见是 order 0。

### 开了 `SLUB_TINY`

`__slab_alloc_node` 换成 `slub.c:3443` 的版本：没有 `kmem_cache_cpu`，没有 cmpxchg。`get_partial` 拿到就返回；否则 `new_slab`，再 `alloc_single_from_new_slab`。`___slab_alloc` 在这个配置下不存在。

### 释放

`kmem_cache_free`（`slub.c:3869`）先 `cache_from_obj` 核对对象属于这个 cache，再 `slab_free`（`:3844`）→ `do_slab_free`（没开 `SLUB_TINY` 时在 `:3778`）。快路径把对象推回当前 CPU 的 freelist，同样用 tid 做 cmpxchg。对象不属于当前 cpu slab 时走 `__slab_free`（`:3644`），可能把 slab 放回 partial，或在它变空时还回伙伴系统。

`kfree`（`slab_common.c:1037`）先看这块内存是不是 slab folio。是，就按 `slab->slab_cache` 调用 `__kmem_cache_free`。不是，说明它来自 `kmalloc_large`，走 `free_large_kmalloc` 把页还回伙伴系统。`ZERO_SIZE_PTR`（地址 16，`slab.h:175`）和 NULL 都让 `kfree` 直接返回。

`trace_kmalloc` 打在 `__do_kmalloc_node`（`slab_common.c:996`、`:1008`）、`kmalloc_trace`（`:1101`）和 `kmalloc_large`（`:1166`）。`trace_kmem_cache_alloc` 打在 `__kmem_cache_alloc_lru`（`slub.c:3535`）和 `kmem_cache_alloc_node`（`:3565`）。

---

## 4.5 `/proc/slabinfo`

`/proc/slabinfo` 只在 `CONFIG_SLAB` 或 `CONFIG_SLUB_DEBUG` 下创建（`slab_common.c:1225`）。SLUB 的权限是 `0400`（`:1229`），所以非 root 读会得到权限错误。SLAB 那一档才是 `0600`。

表头在 `print_slabinfo_header`（`:1243`）：

```text
name  <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>
      : tunables <limit> <batchcount> <sharedfactor>
      : slabdata <active_slabs> <num_slabs> <sharedavail>
```

`cache_show`（`:1269`）先把 `struct slabinfo` 清零，再调用 `get_slabinfo`。SLUB 的 `get_slabinfo`（`slub.c:6540`，同样要求 `CONFIG_SLUB_DEBUG`）只填这些字段：

| 打印列 | SLUB 实际写入 |
|---|---|
| `active_objs` | 对象总数减去 partial 上的空闲对象 |
| `num_objs` | 对象总数 |
| `objsize` | 打印的是 `s->size`（含元数据的步长），不是 `object_size` |
| `objperslab` | `oo_objects(s->oo)` |
| `pagesperslab` | `1 << oo_order(s->oo)` |
| `active_slabs` 和 `num_slabs` | 都写成节点 slab 数，两个数相同 |
| `limit`、`batchcount`、`shared`、`sharedavail` | `get_slabinfo` 不赋值，清零后仍是 0 |

非 root 看总量用 `/proc/meminfo` 的 `Slab`、`SReclaimable`、`SUnreclaim`（`fs/proc/meminfo.c:109`）。它们来自 `NR_SLAB_RECLAIMABLE_B` / `NR_SLAB_UNRECLAIMABLE_B` 换算成的 kB，是页的字节数，不是对象个数。

---

## 4.6 自定义 cache（RECIPE）

示例源码：[demos/kmod_slab_cache.c](./demos/kmod_slab_cache.c)。

`kmem_cache_create`（`mm/slab_common.c:392`）是函数，五个参数：

```c
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
			unsigned int align, slab_flags_t flags,
			void (*ctor)(void *));
```

它转去 `kmem_cache_create_usercopy`（`:278`），usercopy 的偏移和长度传 0。真正分配 `struct kmem_cache` 并调用 `__kmem_cache_create`（`slub.c:5153`）的是 `create_cache`（`slab_common.c:209`）。

`SLAB_HWCACHE_ALIGN` 让对象按 cache line 对齐，避免两个 CPU 改相邻的小对象时互相踩同一条 cache line。

`ctor` 不能睡眠。`kmem_cache_create` 的注释写明：它在 cache 分配新页时运行。实现上是 `allocate_slab` 给每个新对象调用 `setup_object`（`slub.c:1880`）。从 freelist 再次拿出来的对象不会再进 `ctor`。`new_slab` 里如果同时带了 `ctor` 和 `__GFP_ZERO` 会 `WARN_ON_ONCE`（`:2101`）。

`kmem_cache_destroy`（`slab_common.c:480`）把 `refcount` 减 1。减到 0 才 `shutdown_cache`。还有对象时 `__kmem_cache_shutdown` 失败，函数打出 `Slab cache still has objects` 并留下 cache。所以退出模块前要把 `kmem_cache_alloc` 得到的对象都 `kmem_cache_free` 掉。带 `SLAB_TYPESAFE_BY_RCU` 的 cache 会把最后的释放推迟到 RCU 回调之后。

```text
# RECIPE：本环境不编译、不 insmod。需要一份已配置好的 v6.6 内核构建树。
obj-m += kmod_slab_cache.o
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod kmod_slab_cache.ko
sudo awk '$1=="demo_obj"' /proc/slabinfo
sudo rmmod kmod_slab_cache
```

---

## 4.7 page_frag：一页里切小块

**结论**：网络栈要大量几十到几百字节的缓冲。按对象走 SLUB 太碎，按页走伙伴系统又浪费。`struct page_frag_cache` 拿住一页（页比较小时可以是几页），从这一页的高地址往低地址切。引用计数预先加了一大截，每次切只改 cache 上的偏移和 bias，不原子改 `page->_refcount`。

这不是 SLUB。SLUB 的对象大小在建 cache 时定死；page frag 每次的长度由调用者传入。说明在 `Documentation/mm/page_frags.rst`，实现在 `mm/page_alloc.c:4604` 附近。

`struct page_frag`（`include/linux/mm_types_task.h:39`）是已经切出来、交给某个使用者的一块。套接字的 `sk_frag`、任务的 `task_frag` 用的是它：

```c
struct page_frag {
	struct page *page;		/* 这块所在的页 */
#if (BITS_PER_LONG > 32) || (PAGE_SIZE >= 65536)
	__u32 offset;			/* 页内偏移 */
	__u32 size;			/* 这块的长度 */
#else
	__u16 offset;			/* 页内偏移 */
	__u16 size;			/* 这块的长度 */
#endif
};
```

x86-64 走上面的 `__u32` 分支。

`struct page_frag_cache`（`include/linux/mm_types.h:532`）是分配游标：

```c
struct page_frag_cache {
	void *va;			/* 当前这页的内核虚拟地址。空表示还没有页 */
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	__u16 offset;			/* 下一次从这一页的这个位置往下切 */
	__u16 size;			/* 当前这页能用的字节数 */
#else
	__u32 offset;			/* 页很大时没有单独的 size，长度就是 PAGE_SIZE */
#endif
	/*
	 * 已经加到 page->_refcount 上、但还没按「切出一块」扣掉的预支。
	 * 分配热路径只减它，不碰原子引用计数。
	 */
	unsigned int pagecnt_bias;
	bool pfmemalloc;		/* 这页来自低水位保留内存 */
};
```

`PAGE_FRAG_CACHE_MAX_SIZE`（`mm_types.h:511`）把 32768 向上对齐到页。`PAGE_FRAG_CACHE_MAX_ORDER`（`:512`）是这个尺寸的 order。x86 常见的 4KB 页上，32768 已经按页对齐，所以最大值是 32768 字节，order 是 3，`offset` 和 `size` 都是 `__u16`。

```c
/* include/linux/gfp.h:312，align_mask 传 ~0u，也就是不做额外对齐 */
page_frag_alloc(nc, fragsz, gfp_mask)
  → page_frag_alloc_align                 /* mm/page_alloc.c:4655 */

void *page_frag_alloc_align(struct page_frag_cache *nc,
			    unsigned int fragsz, gfp_t gfp_mask,
			    unsigned int align_mask)
{
	/* va 空：向伙伴系统补页 */
	if (unlikely(!nc->va)) {
		page = __page_frag_cache_refill(nc, gfp_mask); /* :4615，static */
		/* 一次加上 PAGE_FRAG_CACHE_MAX_SIZE 个引用 */
		page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		nc->offset = size;          /* 从页尾开始往低地址切 */
	}
	/* offset 减去 fragsz，再按 align_mask 对齐。放不下就扣掉预支引用或重新补页 */
}
```

`__page_frag_cache_refill` 在 `PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE` 时先用 `PAGE_FRAG_CACHE_MAX_ORDER` 要一块复合页（带 `__GFP_COMP | __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC`）。失败再要 order 0。拿到的地址写进 `nc->va`。

bias 的意思：分配成功只做 `pagecnt_bias--`，并改 `offset`。bias 用完，或者这一页剩余空间放不下时，才 `page_ref_sub_and_test(page, nc->pagecnt_bias)` 把预支一次扣实。函数注释（`:4673`）写明这里不能用 `atomic_set` 改引用计数，否则会弄坏 `get_page_unless_zero` 的使用者。扣到 0 并且页是 pfmemalloc 时，把页还掉再补一页。调用者要的长度比整页还大，函数返回 NULL，并且不把当前页释放掉，避免雪上加霜。

用完一块调用 `page_frag_free`（`page_alloc.c:4731`）：对 compound head 做 `put_page`，减到 0 才按 compound order 还回伙伴系统。整页交给别人之后，不要再从同一个 cache 往下切。缓存本身不用了，用 `page_frag_cache_drain`（`:4636`）按剩余 bias 一次扣掉。

`Documentation/mm/page_frags.rst` 写了网络栈的两个 per-CPU 缓存：`netdev_alloc_cache`（`net/core/skbuff.c:276`，类型就是 `struct page_frag_cache`）和 `napi_alloc_cache`（`:277`，里面嵌着一份 `page_frag_cache`）。文档要求调用 cache 的一侧要么限制在一个 CPU 上，要么在关中断的前提下使用。netdev 那组函数自己关中断，napi 那组只在 softirq 里用。

---

## 4.8 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `kmalloc` | `include/linux/slab.h:591` | inline。编译期常量走 `kmalloc_trace` 或 `kmalloc_large`，否则 `__kmalloc` |
| `kmalloc_node` | `slab.h:607` | 带节点的 `kmalloc` |
| `kmalloc_type` | `slab.h:401` | 按 GFP 选 `KMALLOC_NORMAL` / DMA / RECLAIM / CGROUP |
| `kmalloc_index` | 宏 `slab.h:486`，实现 `__kmalloc_index` `:444` | 把字节数变成 `kmalloc_caches` 的下标 |
| `__kmalloc` | `mm/slab_common.c:1018` | 运行期大小的入口，节点用 `NUMA_NO_NODE` |
| `__kmalloc_node` | `slab_common.c:1012` | 带节点 |
| `__do_kmalloc_node` | `slab_common.c:989` | 太大走 large，否则 `kmalloc_slab` + `__kmem_cache_alloc_node` |
| `kmalloc_slab` | `slab_common.c:709` | 按大小和 GFP 选出 `struct kmem_cache *` |
| `kmalloc_trace` | `slab_common.c:1096` | 编译期大小的 cache 分配，并打 `trace_kmalloc` |
| `kmalloc_node_trace` | `slab_common.c:1108` | 带节点的 `kmalloc_trace` |
| `kmalloc_large` | `slab_common.c:1163` | 超过 `KMALLOC_MAX_CACHE_SIZE` |
| `kmalloc_large_node` | `slab_common.c:1173` | 带节点的 large |
| `__kmalloc_large_node` | `slab_common.c:1138` | `alloc_pages_node`，标志里加 `__GFP_COMP` |
| `kmem_cache_alloc` | 声明 `slab.h:500`，定义 `mm/slub.c:3540` | 从指定 cache 拿一个对象 |
| `kmem_cache_alloc_lru` | 声明 `slab.h:501`，定义 `slub.c:3546` | 带 `list_lru`，给 dentry / inode 这类缓存 |
| `kmem_cache_alloc_node` | 声明 `slab.h:522`，定义 `slub.c:3561` | 指定节点，并打 trace |
| `__kmem_cache_alloc_lru` | `slub.c:3530` | `kmem_cache_alloc` 的内联包装 |
| `__kmem_cache_alloc_node` | `slub.c:3553` | kmalloc 路径调用的节点分配，本身不打 trace |
| `slab_alloc` | `slub.c:3523` | 固定 `NUMA_NO_NODE` |
| `slab_alloc_node` | `slub.c:3493` | 钩子、KFENCE，然后 `__slab_alloc_node` |
| `__slab_alloc_node` | `slub.c:3368`；`SLUB_TINY` 时 `:3443` | 无锁拨 freelist，或 tiny 配置下直接 partial / 新 slab |
| `__slab_alloc` | `slub.c:3347` | 只在非 `SLUB_TINY` 存在。补上抢占后的 CPU 指针 |
| `___slab_alloc` | `slub.c:3129` | 只在非 `SLUB_TINY` 存在。cpu slab、CPU partial、节点 partial、新 slab |
| `__update_cpu_freelist_fast` | `slub.c:3065` | 非 RT 快路径：同时比较并交换 freelist 和 tid |
| `get_freepointer_safe` | `slub.c:429` | 从对象里读出下一个空闲指针 |
| `get_freelist` | `slub.c:3084` | 把 slab 自己的 freelist 整段接过来 |
| `deactivate_slab` | `slub.c:2526` | cpu slab 的节点或水位不对时，把它交回节点链表 |
| `put_cpu_partial` | `slub.c:2730`；没开 CPU partial 时 `:2298` 是空函数 | 把多余的 partial slab 留在本 CPU |
| `alloc_single_from_partial` | `slub.c:2186` | 调试 cache 从 partial 上一次拿一个对象 |
| `alloc_single_from_new_slab` | `slub.c:2216` | 调试 cache 从刚分配的 slab 上一次拿一个对象 |
| `add_full` | `slub.c:1378` | 带 `SLAB_STORE_USER` 时把用满的 slab 挂到 `full` |
| `maybe_wipe_obj_freeptr` | `slub.c:3474` | 清掉对象里残留的 freelist 指针 |
| `slab_pre_alloc_hook` | `mm/slab.h:707` | `might_alloc`、fault injection、memcg |
| `slab_post_alloc_hook` | `mm/slab.h:725` | KASAN、kmemleak、kmsan、清零、memcg |
| `kfence_alloc` | `include/linux/kfence.h:118` | 低概率采样。没开 KFENCE 时是返回 NULL 的 inline（`:231`） |
| `get_partial` | `slub.c:2430` | 本节点 partial；没指定节点时可以再看别的节点 |
| `get_partial_node` | `slub.c:2306` | 从某一个 `kmem_cache_node` 拿 partial |
| `get_any_partial` | `slub.c:2366` | 按 `remote_node_defrag_ratio` 看别的节点。只在 `CONFIG_NUMA` 下有搜索循环 |
| `new_slab` | `slub.c:2096` | 检查 GFP，调用 `allocate_slab` |
| `allocate_slab` | `slub.c:2029` | 向伙伴系统要页，串好 freelist，调用 `ctor` |
| `alloc_slab_page` | `slub.c:1895` | `alloc_pages` 或 `__alloc_pages_node`，再标成 slab |
| `__alloc_pages` | 声明 `include/linux/gfp.h:177`，定义 `mm/page_alloc.c:4465` | 伙伴系统入口 |
| `alloc_pages` | `gfp.h:264`（`CONFIG_NUMA` 的声明）或 `:269`（否则的 inline） | `alloc_slab_page` 在节点未知时用它 |
| `__alloc_pages_node` | `gfp.h:232` | 指定节点要页 |
| `setup_object` | `slub.c:1880` | 调试初始化和 `ctor` |
| `kmem_cache_free` | 声明 `slab.h:503`，定义 `slub.c:3869` | 对象还回 freelist |
| `cache_from_obj` | `mm/slab.h:657` | free 时核对对象属于这个 cache |
| `free_large_kmalloc` | `mm/slab_common.c:971` | 释放 `kmalloc_large` 拿到的页 |
| `slab_free` | `slub.c:3844` | free 的钩子包装 |
| `do_slab_free` | `slub.c:3778`；`SLUB_TINY` 时 `:3834` | 推回 cpu freelist，或走慢路径 |
| `__slab_free` | `slub.c:3644` | 不是当前 cpu slab 时的释放 |
| `kfree` | `slab_common.c:1037` | slab 对象走 cache，大块走 `free_large_kmalloc` |
| `kmem_cache_create` | 声明 `slab.h:189`，定义 `slab_common.c:392` | 五个参数创建 cache |
| `kmem_cache_create_usercopy` | 声明 `slab.h:192`，定义 `slab_common.c:278` | 多两个 usercopy 范围参数 |
| `create_cache` | `slab_common.c:209` | 分配并初始化 `struct kmem_cache` |
| `__kmem_cache_create` | `slub.c:5153` | SLUB 侧把 order、per-CPU、sysfs 设好 |
| `kmem_cache_destroy` | 声明 `slab.h:197`，定义 `slab_common.c:480` | `refcount` 到 0 才销毁。还有对象就告警 |
| `get_slabinfo` | 声明 `mm/slab.h:390`，SLUB 定义 `slub.c:6540` | 填 `/proc/slabinfo`。要求 `CONFIG_SLUB_DEBUG` |
| `cache_show` | `slab_common.c:1269` | 把一行 slabinfo 打出来 |
| `page_frag_alloc` | `include/linux/gfp.h:312` | 从 page frag cache 切一块，对齐掩码 `~0u` |
| `page_frag_alloc_align` | 声明 `gfp.h:308`，定义 `mm/page_alloc.c:4655` | 带对齐的切割 |
| `__page_frag_cache_refill` | `page_alloc.c:4615` | static。cache 空了向伙伴系统要页 |
| `page_frag_free` | 声明 `gfp.h:318`，定义 `page_alloc.c:4731` | 放下一块。引用到 0 才还页 |
| `page_frag_cache_drain` | 声明 `gfp.h:306`，定义 `page_alloc.c:4636` | 按剩余 bias 把整份 cache 放掉 |
| `__page_frag_cache_drain` | 声明 `gfp.h:307`，定义 `page_alloc.c:4646` | 从一页上一次减掉 `count` 个引用 |

---

[上一篇 CMA](./03-09-cma.md) · [目录](./README.md) · [下一节 vmalloc](./05-vmalloc-and-kernel-va.md)
