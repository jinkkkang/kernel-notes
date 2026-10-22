# 4. SLUB

**结论**：v7.3 的小对象分配器是 SLUB，快路径是 **per-CPU sheaf**（一把对象指针），不是老笔记里的 per-CPU freelist 指针。sheaf 空了才进 `___slab_alloc`：先 partial slab，再向 buddy 要新 slab。`kmalloc` 只是按大小选一个通用 cache。

> `struct kmem_cache` 在 `mm/slab.h:240`。分配在 `mm/slub.c`。
> 函数上方注释（`slub.c:4960` 附近）仍写 “lockless freelist”。函数体先调用 `alloc_from_pcs()`。以函数体为准。
> 导航：[上一节 buddy](./03-buddy.md) · [目录](./README.md) · [下一节 vmalloc](./05-vmalloc-and-kernel-va.md)

---

## 4.1 对象、slab、cache

| 词 | 是什么 |
|---|---|
| object | 你 `kmalloc` 拿到的那一块，大小固定 |
| slab | 从 buddy 来的一页或几页，切成同尺寸对象。`struct slab` 复用 `struct page` 的位（`slab.h:116`） |
| `kmem_cache` | 一种尺寸（加构造函数、对齐、标志）的管理结构 |
| sheaf | per-CPU 的一小捆 `void *`，分配时直接弹一个出来 |
| barn | 每节点的 sheaf 仓库：满捆和空捆分链表，CPU 之间通过它倒手 |
| partial | 用了一部分对象的 slab，挂在 `kmem_cache_node.partial` |

`struct slab`（`slab.h:116`）和 `page` 的对应由 `SLAB_MATCH` 静态断言钉住（`:136`）。`slab_cache` 必须落在 `compound_info` 的位置，保证 bit 0 是 0。

---

## 4.2 `struct kmem_cache` 全部成员（`mm/slab.h:240`）

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct slub_percpu_sheaves __percpu *cpu_sheaves` | 始终 | 每 CPU 的 sheaf 三指针。注释（`:296`）：每个 cache 的 `cpu_sheaves` 非 NULL，但启动期、boot cache、开了调试、或 `CONFIG_SLUB_TINY` 可能永远指着 `bootstrap_sheaf`。`cache_has_sheaves()`（`:302`）用 `sheaf_capacity` 判断是不是真 sheaf |
| `slab_flags_t flags` | 始终 | `SLAB_*`：调试、redzone、cache 对齐、reclaim 等 |
| `unsigned long min_partial` | 始终 | 每个节点至少留多少 partial slab，避免抖着向 buddy 要页 |
| `unsigned int size` | 始终 | **含**元数据的对象步长。真正往前跳用它 |
| `unsigned int object_size` | 始终 | **不含**元数据的对象大小，也就是调用者请求的大小 |
| `struct reciprocal_value reciprocal_size` | 始终 | `size` 的倒数，用乘法代替除法算对象下标（`__obj_to_index`，`:335`） |
| `unsigned int offset` | 始终 | 空闲指针在对象里的偏移。空闲对象把 next 指针塞在对象体内 |
| `unsigned int sheaf_capacity` | 始终 | 一把 sheaf 能装多少对象。0 表示不用 sheaf 快路径 |
| `struct kmem_cache_order_objects oo` | 始终 | 首选 slab order 和该 order 能放的对象数，压在一个 `unsigned int` 里（`kmem_cache_order_objects`，`:228`，唯一成员 `x`） |
| `struct kmem_cache_order_objects min` | 始终 | 允许的最小 order/对象数。分配 slab 失败时可以降 |
| `gfp_t allocflags` | 始终 | 向 buddy 要 slab 页时用的 GFP |
| `int refcount` | 始终 | cache 被多少处引用。减到 0 才能 destroy。合并 cache 时会大于 1 |
| `void (*ctor)(void *object)` | 始终 | 对象构造函数。只在对象第一次从新 slab 取出时调用，不是每次 alloc |
| `unsigned int inuse` | 始终 | 元数据开始的偏移。对象可用区间是 `[0, inuse)` |
| `unsigned int align` | 始终 | 对齐 |
| `unsigned int red_left_pad` | 始终 | 左侧 redzone 填充。调试用，正常为 0 |
| `const char *name` | 始终 | `/proc/slabinfo` 和 sysfs 里的名字。只用于显示 |
| `struct list_head list` | 始终 | 挂到全局 cache 链表 |
| `struct kobject kobj` | `CONFIG_SYSFS` | `/sys/kernel/slab/<name>` |
| `unsigned long random` | `CONFIG_SLAB_FREELIST_HARDENED` | freelist 指针异或的随机数，防止伪造空闲链 |
| `unsigned int remote_node_defrag_ratio` | `CONFIG_NUMA` | 多大比例从别的节点的 partial 拿，用来碎片整理。注释写 “Defragmentation by allocating from a remote node” |
| `unsigned int *random_seq` | `CONFIG_SLAB_FREELIST_RANDOM` | 空闲对象弹出顺序的随机表 |
| `struct kasan_cache kasan_info` | `CONFIG_KASAN_GENERIC` | KASAN 辅助 |
| `unsigned int useroffset` | `CONFIG_HARDENED_USERCOPY` | 允许 `copy_from_user` 写到对象里的起点 |
| `unsigned int usersize` | `CONFIG_HARDENED_USERCOPY` | 允许拷贝的长度 |
| `struct kmem_cache_stats __percpu *cpu_stats` | `CONFIG_SLUB_STATS` | `slabinfo -A` / alloc 慢路径计数 |
| `struct kmem_cache_per_node_ptrs per_node[MAX_NUMNODES]` | 始终 | 每节点两个指针：`barn` 和 `node`（`kmem_cache_node`） |

`kmem_cache_per_node_ptrs`（`:232`）：`struct node_barn *barn`，`struct kmem_cache_node *node`。

---

## 4.3 配套结构（sheaf 路径上会碰到的，成员写全）

`struct slub_percpu_sheaves`（`slub.c:447`）：

| 成员 | 中文 |
|---|---|
| `local_trylock_t lock` | 本 CPU sheaf 的锁。分配热路径用 trylock，失败就当慢路径 |
| `struct slab_sheaf *main` | 当前这把。未加锁时不为 NULL |
| `struct slab_sheaf *spare` | 备用：要么空要么满，可以是 NULL。main 空了就和它换 |
| `struct slab_sheaf *rcu_free` | 攒 `kfree_rcu` 的一把 |

`struct slab_sheaf`（`:429`）：

| 成员 | 中文 |
|---|---|
| `struct rcu_head rcu_head` | 与下面几项同 union。延迟释放 |
| `struct list_head barn_list` | 挂在 barn 的满链或空链上 |
| `struct llist_node llnode` | 不确定上下文里推迟 `call_rcu` |
| `unsigned int capacity` | 仅 prefilled sheaf 使用 |
| `bool pfmemalloc` | 这把里的对象来自 PFMEMALLOC 水位以下，归还时要记住 |
| `struct kmem_cache *cache` | 属于哪个 cache |
| `unsigned int size` | 当前装了多少对象 |
| `int node` | 只给 rcu sheaf 用的节点号 |
| `void *objects[]` | 柔性数组，对象指针 |

`struct node_barn`（`:421`）：`spinlock_t lock`，`sheaves_full`，`sheaves_empty`，`nr_full`，`nr_empty`。满链和空链各自最多留 `MAX_FULL_SHEAVES` / `MAX_EMPTY_SHEAVES`（都是 10，`:418`）。多出来的会拆回 slab。

`struct kmem_cache_node`（`:457`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `spinlock_t list_lock` | 始终 | 保护 partial / full |
| `unsigned long nr_partial` | 始终 | partial slab 个数 |
| `struct list_head partial` | 始终 | 部分使用的 slab |
| `atomic_long_t nr_slabs` | `CONFIG_SLUB_DEBUG` | slab 数 |
| `atomic_long_t total_objects` | `CONFIG_SLUB_DEBUG` | 对象总数 |
| `struct list_head full` | `CONFIG_SLUB_DEBUG` | 完全用满的 slab。非调试构建不保留这条链，满了就从 partial 摘掉，靠对象里的 freelist 指针自己找回来 |

`struct slab`（`slab.h:116`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `memdesc_flags_t flags` | 始终 | 与 `page->flags` 重叠 |
| `struct kmem_cache *slab_cache` | 始终 | 属于哪个 cache。与 `page` 的 `compound_info` 重叠 |
| `struct list_head slab_list` | 始终 | 挂 partial 或 full |
| `struct freelist_counters` | 匿名嵌在 union 里 | 见下。注释要求落在双字边界上 |
| `struct rcu_head rcu_head` | 与上面同 union | RCU 释放 slab |
| `unsigned int __page_type` | 始终 | 与 page 的类型字重叠 |
| `atomic_t __page_refcount` | 始终 | 与 `page->_refcount` 重叠 |
| `unsigned long obj_exts` | `CONFIG_SLAB_OBJ_EXT` | 每对象扩展（memcg objcg、alloc tag） |

`struct freelist_counters`（`slab.h:82`）：

| 成员 | 条件 | 中文 |
|---|---|---|
| `void *freelist` | 始终 | 下一个空闲对象 |
| `unsigned long counters` | 与下面的位域同 union | 把计数当成一个字做 cmpxchg |
| `unsigned inuse:16` | 位域 | 已分配对象数 |
| `unsigned objects:15` | 位域 | slab 里对象总数 |
| `unsigned frozen:1` | 位域 | slab 正被某 CPU 独占。调试时这个 bit 也可表示 slab 已损坏 |
| `unsigned obj_exts_in_object:1` | `CONFIG_64BIT` | 对象扩展塞在对象体内 |
| `unsigned obj_exts_needs_objcg:1` | `CONFIG_64BIT` | 需要 objcg |
| `freelist_full_t freelist_counters` | `system_has_freelist_aba` | 128 位（64 位机）或 64 位的整体，一次 cmpxchg，避免 freelist 指针 ABA |

---

## 4.4 `kmalloc` / `kmem_cache_alloc` 路径

```text
kmalloc(size, flags)                         # 宏 slab.h:1056
  → kmalloc_noprof                           # :997
       → _kmalloc_noprof                     # :982
            → __kmalloc_noprof               # slub.c:5431
                 → __do_kmalloc_node         # :5394
                      size > KMALLOC_MAX_CACHE_SIZE
                        → __kmalloc_large_node_noprof   # 直接 buddy，:5383
                      否则 kmalloc_slab() 选 cache
                        → slab_alloc_node    # :4969

kmem_cache_alloc(s, flags)                   # 宏 slab.h:856
  → kmem_cache_alloc_noprof                  # slub.c:5001
       → slab_alloc_node
```

`slab_alloc_node`（`:4969`）：

```c
s = slab_pre_alloc_hook(s, gfpflags);          /* 可能被 fault injection 拒绝 */
object = kfence_alloc(...);                    /* KFENCE 抽查，命中就直接返回 */
node = apply_strict_numa_policy(node);
object = alloc_from_pcs(s, gfpflags, ac->alloc_flags, node);  /* per-CPU sheaf */
if (!object)
    object = ___slab_alloc(s, gfpflags, node, ac);            /* 慢路径 */
maybe_wipe_obj_freeptr(s, object);            /* 硬化：清掉对象里残留的 freelist 指针 */
slab_post_alloc_hook(...);                    /* KASAN、memcg、init-on-alloc */
```

`___slab_alloc`（`:4533`）注释写得很直：sheaf 失败，或 bootstrap / 调试 / `SLUB_TINY` 根本没有 sheaf。然后：

1. 指定了 node 又没带 `__GFP_THISNODE`：先只在目标节点的 partial 里找。
2. 没有就用 `GFP_NOWAIT | __GFP_THISNODE` 试着在目标节点新开一块 slab。
3. 还没有，就用原来的 gfp，允许别的节点的 partial，最后才向 buddy 要页。

新 slab 来自 `alloc_frozen_pages_noprof` / `__alloc_frozen_pages_noprof`（`slub.c:5353` 附近）。所以 slab 涨，buddy 的 order 0 或 `oo` 里那个 order 会掉。`/proc/vmstat` 的 `slabs_scanned` 是回收扫过的 slab 缓存页，不是对象个数。

`trace_kmem_cache_alloc` / `trace_kmalloc` 在 noprof 包装里打（`:5012`、`:5415`）。

---

## 4.5 `/proc/slabinfo`

列的含义在 `Documentation/admin-guide/mm/slab.rst` 的表头里（源码 `get_slabinfo`，`mm/slab.h:519`）。典型一行：

```text
name  <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>
      : tunables <limit> <batchcount> <sharedfactor>
      : slabdata <active_slabs> <num_slabs> <sharedavail>
```

本机 6.12.94+ 读 `/proc/slabinfo` 是 `Permission denied`（[13](./13-observability.md)）。非 root 看总量用 `/proc/meminfo` 的 `Slab` / `SReclaimable` / `SUnreclaim`，那是页数不是对象数。

v7.3 还有 `/sys/kernel/slab/<name>/`（`CONFIG_SYSFS` 且非 `SLUB_TINY` 时 `SLAB_SUPPORTS_SYSFS`，`slab.h:309`）。sheaf 相关计数在开了 `CONFIG_SLUB_STATS` 时才有，不要假设每个发行版内核都编了。

---

## 4.6 自定义 cache（RECIPE）

示例源码：[demos/kmod_slab_cache.c](./demos/kmod_slab_cache.c)。

`kmem_cache_create` 在 v7.3 是 `_Generic` 宏（`slab.h:479`）：

- 新：`kmem_cache_create(name, object_size, args, flags)`，`args` 是 `struct kmem_cache_args *`
- 旧：`kmem_cache_create(name, object_size, align, flags, ctor)`

示例用的是五参数形式。`SLAB_HWCACHE_ALIGN` 让对象按 cache line 对齐，避免 false sharing。

```text
# RECIPE：本环境不编译、不 insmod。需要对应 v7.3 的内核头与 Module.symvers。
obj-m += kmod_slab_cache.o
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod kmod_slab_cache.ko
# 看名字是否出现。slabinfo 常常要 root：
sudo awk '$1=="demo_obj"' /proc/slabinfo
sudo rmmod kmod_slab_cache
```

为什么标 RECIPE：当前 box 没有在跑我们对照的 v7.3 内核（`uname` 是 6.12.94+），也没有为笔记去装模块。源码只保证和 v7.3 的 `kmem_cache_create` 五参数形式一致。

析构前必须把对象都 `kmem_cache_free` 掉，否则 `kmem_cache_destroy` 会在还有对象时告警并拒绝。`ctor` 不要做会睡觉的事，也不要假设每次 alloc 都调用——从 freelist 复用的对象不会再 ctor。

---

## 4.7 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `kmalloc` | 宏 `slab.h:1056` | 通用大小 |
| `kmalloc_noprof` | 宏 `slab.h:997` | 无 alloc-tag 包装 |
| `__kmalloc_noprof` | `slub.c:5431` | 实现 |
| `__do_kmalloc_node` | `slub.c:5394` | 选 cache 或 large |
| `__kmalloc_large_node_noprof` | `slub.c:5383` | 超过 `KMALLOC_MAX_CACHE_SIZE` 走 buddy |
| `kmem_cache_alloc` | 宏 `slab.h:856` | 指定 cache |
| `kmem_cache_alloc_noprof` | `slub.c:5001` | 实现 |
| `kmem_cache_alloc_node_noprof` | `slub.c:5059` | 指定节点 |
| `kmem_cache_alloc_lru_noprof` | `slub.c:5018` | 带 list_lru，给 dentry/inode |
| `slab_alloc_node` | `slub.c:4969` | 快路径内联 |
| `alloc_from_pcs` | `slub.c:4827` | per-CPU sheaf |
| `___slab_alloc` | `slub.c:4533` | partial / 新 slab |
| `kmem_cache_prefill_sheaf` | `slub.c:5111` | 调用者自己拿一把 sheaf 批量分 |
| `kmem_cache_create` | 宏 `slab.h:479` | 创建 cache |
| `__kmem_cache_create_args` | `slab.h:389` | 新 API 的实现入口 |
| `do_kmem_cache_create` | `slab.h:454` | 真正初始化 `kmem_cache` |
| `kmem_cache_destroy` | `slab.h:485` | 销毁 |
| `cache_has_sheaves` | `slab.h:302` | 是不是真 sheaf |
| `get_slabinfo` | `slab.h:519` | 填 `/proc/slabinfo` |
| `kmem_cache_charge` | `slub.c:5037` | 对象事后 charge 到 memcg |

---

[上一节 buddy](./03-buddy.md) · [目录](./README.md) · [下一节 vmalloc](./05-vmalloc-and-kernel-va.md)
