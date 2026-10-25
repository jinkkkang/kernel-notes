# 6. memory

**结论**：memory 控制器把 folio **记到某个 `mem_cgroup` 的 `page_counter` 上**。超过 **high** 时给分配任务加压、回收；撞上 **max** 仍不够就走 **OOM**。本章讲接口和 charge 怎么串起来；页回收算法、LRU、`kswapd` 留给 [memory-management/](../memory-management/README.md)。

> 源码：`mm/memcontrol.c`、`include/linux/memcontrol.h`  
> 文档：`cgroup-v2.rst` 目录 **5-2**（Memory）、**5-2-3**（Memory Ownership）

---

## 6.1 用户可见文件（v2）

`memory_files[]` 在 `mm/memcontrol.c:6843`。名字在 cgroup 里显示为 `memory.current` 这种带前缀的形式。`memory.current` 到 `memory.events.local`，以及 `memory.oom.group`，都带 `CFTYPE_NOT_ON_ROOT`，root 上看不到。`memory.stat`、`memory.numa_stat`（`CONFIG_NUMA`）、`memory.reclaim` 在 root 上也有。

| 文件 | 读写 | 含义 |
|---|---|---|
| `memory.current` | 只读 | 本节点加后代当前用量，字节 |
| `memory.peak` | 只读 | 历史峰值，字节（计数器的 watermark） |
| `memory.min` | 读写 | 保护：尽量别从这组回收 |
| `memory.low` | 读写 | 弱一档的保护 |
| `memory.high` | 读写 | 软顶：超过后回收、给分配者加压，不立刻当硬失败 |
| `memory.max` | 读写 | 硬顶。冲不上去就回收；仍不够则 OOM。读出来是 `max` 表示不限 |
| `memory.events` | 只读 | 见下表。含本节点和后代 |
| `memory.events.local` | 只读 | 同样的键，只计本节点 |
| `memory.stat` | 只读 | anon / file / kernel 等细分 |
| `memory.numa_stat` | 只读 | `CONFIG_NUMA` |
| `memory.oom.group` | 读写 | 对应 `mem_cgroup->oom_group`。可委托（`CFTYPE_NS_DELEGATABLE`） |
| `memory.reclaim` | 只写 | 让用户主动回收若干字节。可委托 |

`memory.events` 的键由 `__memory_events_show`（`:6690`）打印，`memory_events_show`（`:6702`）把 `memory_events[]` 传进去。枚举 `memcg_memory_event` 在 `memcontrol.h:43`：

| 文件里的名字 | 枚举 |
|---|---|
| `low` | `MEMCG_LOW` |
| `high` | `MEMCG_HIGH` |
| `max` | `MEMCG_MAX` |
| `oom` | `MEMCG_OOM` |
| `oom_kill` | `MEMCG_OOM_KILL` |
| `oom_group_kill` | `MEMCG_OOM_GROUP_KILL` |

`CONFIG_SWAP` 时 `mem_cgroup_swap_init`（`:8023`）再挂上：

| 文件 | 含义 |
|---|---|
| `memory.swap.current` | swap 用量 |
| `memory.swap.high` | swap 软顶 |
| `memory.swap.max` | swap 硬顶 |
| `memory.swap.peak` | swap 峰值 |
| `memory.swap.events` | `high` / `max` / `fail`（`:8002`） |

`CONFIG_MEMCG_KMEM && CONFIG_ZSWAP` 时还有 `memory.zswap.current`、`memory.zswap.max`（`:8007`）。

`memory.pressure` 不是这张表里的文件，它是 core 的 PSI 文件，见 [08](./08-psi-rstat.md)。

本机 `/sys/fs/cgroup/agent`（sampled on 6.12，见 [09](./09-observability.md)）：`memory.max=max`，`memory.current` 为 4718989312，`memory.events` 全 0。

v1 的 `memory.limit_in_bytes`、`memory.memsw.*` 在 `mem_cgroup_legacy_files` / `memsw_files`，v2 不使用那些名字。

---

## 6.2 `struct page_counter`

`include/linux/page_counter.h:10`。`memory`、`swap`、`memsw`、`kmem`、`tcpmem` 都是这个类型。用量单位是**页**，接口文件再乘 `PAGE_SIZE` 变成字节。

```c
struct page_counter {
	/*
	 * usage 单独占一条缓存行。memcg->memory.usage 是热字段，
	 * 不和别的成员挤在一起。
	 */
	atomic_long_t usage;
	CACHELINE_PADDING(_pad1_);

	unsigned long emin;			/* 算出来的有效 memory.min */
	atomic_long_t min_usage;		/* 本节点落在 min 保护下的用量 */
	atomic_long_t children_min_usage;	/* 孩子们的 min 用量之和 */

	unsigned long elow;			/* 有效 memory.low */
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	unsigned long watermark;		/* 峰值，对应 *.peak */
	unsigned long failcnt;			/* 撞顶次数；v1 failcnt 用它 */

	CACHELINE_PADDING(_pad2_);

	unsigned long min;			/* 用户写的 memory.min，单位页 */
	unsigned long low;
	unsigned long high;
	unsigned long max;			/* 默认 PAGE_COUNTER_MAX，即「不限」 */
	struct page_counter *parent;		/* 向上累加的父计数器 */
} ____cacheline_internodealigned_in_smp;
```

常用操作：`page_counter_try_charge`（可失败）、`page_counter_charge`（强制加上）、`page_counter_uncharge`、`page_counter_read`。都在同一头文件。

---

## 6.3 `struct mem_cgroup`

`include/linux/memcontrol.h:209`。整段在文件前面的 `CONFIG_MEMCG` 里。第一成员是 `css`，所以 `container_of` 可以从 css 找回 `mem_cgroup`。

```c
struct mem_cgroup {
	/* --- css 与计数器 --- */
	struct cgroup_subsys_state css;		/* 嵌入的 css，必须是第一成员 */

	/* 私有 id。对象比 cgroup 目录活得久时，仍用这个 id 认出它 */
	struct mem_cgroup_id id;

	struct page_counter memory;		/* v1 和 v2 都用的主计数器 */

	union {
		struct page_counter swap;	/* v2：单独的 swap 计数 */
		struct page_counter memsw;	/* v1：memory+swap 合在一起 */
	};

	struct page_counter kmem;		/* v1 内核内存 */
	struct page_counter tcpmem;		/* v1 TCP 内存 */

	/* --- 超限、OOM、旧接口 --- */
	struct work_struct high_work;		/* 不在进程上下文里发现超 high 时排队 */

#if defined(CONFIG_MEMCG_KMEM) && defined(CONFIG_ZSWAP)
	unsigned long zswap_max;		/* memory.zswap.max */
#endif

	unsigned long soft_limit;		/* v1 soft limit */

	struct vmpressure vmpressure;		/* v1 vmpressure 通知 */

	/* OOM 杀掉一个属于本组的任务时，要不要把整组都杀掉 */
	bool oom_group;

	bool oom_lock;				/* memcg_oom_lock 保护 */
	int under_oom;

	int swappiness;
	int oom_kill_disable;			/* v1：关掉 OOM killer */

	struct cgroup_file events_file;		/* memory.events */
	struct cgroup_file events_local_file;	/* memory.events.local */
	struct cgroup_file swap_events_file;	/* memory.swap.events */

	struct mutex thresholds_lock;
	struct mem_cgroup_thresholds thresholds;	/* v1 用量阈值 */
	struct mem_cgroup_thresholds memsw_thresholds;

	struct list_head oom_notify;		/* v1 oom eventfd */

	/*
	 * v1：任务迁入本组时，要不要把已有的 charge 跟着搬。
	 * v2 不提供这个开关；页留在当初分配它的 memcg 上。
	 */
	unsigned long move_charge_at_immigrate;
	spinlock_t move_lock;
	unsigned long move_lock_flags;

	CACHELINE_PADDING(_pad1_);

	/* --- 统计与事件 --- */
	struct memcg_vmstats *vmstats;		/* memory.stat 的汇总 */

	atomic_long_t memory_events[MEMCG_NR_MEMORY_EVENTS];
	atomic_long_t memory_events_local[MEMCG_NR_MEMORY_EVENTS];

	/* socket 内存回收压力的提示；v1 把 socket 分开记账时不要用它 */
	unsigned long socket_pressure;

	bool tcpmem_active;			/* v1 tcp 记账是否开着 */
	int tcpmem_pressure;

#ifdef CONFIG_MEMCG_KMEM
	int kmemcg_id;
	struct obj_cgroup __rcu *objcg;		/* 内核对象记账 */
	struct list_head objcg_list;		/* 从父节点继承来的 objcg，objcg_lock */
#endif

	CACHELINE_PADDING(_pad2_);

	atomic_t moving_account;		/* >0：有页正在迁往别的 cgroup（v1） */
	struct task_struct *move_lock_task;

	struct memcg_vmstats_percpu __percpu *vmstats_percpu;

#ifdef CONFIG_CGROUP_WRITEBACK
	struct list_head cgwb_list;
	struct wb_domain cgwb_domain;
	struct memcg_cgwb_frn cgwb_frn[MEMCG_CGWB_FRN_CNT];
#endif

	struct list_head event_list;		/* v1 eventfd 名单 */
	spinlock_t event_list_lock;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct deferred_split deferred_split_queue;
#endif

#ifdef CONFIG_LRU_GEN
	struct lru_gen_mm_list mm_list;		/* 本 memcg 的 mm 名单 */
#endif

	/* 每个 NUMA 节点一份，柔性数组 */
	struct mem_cgroup_per_node *nodeinfo[];
};
```

`struct mem_cgroup_id`（同文件 `:65`）只有两个字段：`int id` 和 `refcount_t ref`。

子系统对象（`memcontrol.c:6914`）：

```c
struct cgroup_subsys memory_cgrp_subsys = {
	.css_alloc = mem_cgroup_css_alloc,
	.css_online = mem_cgroup_css_online,
	.css_offline = mem_cgroup_css_offline,
	.css_released = mem_cgroup_css_released,
	.css_free = mem_cgroup_css_free,
	.css_reset = mem_cgroup_css_reset,
	.css_rstat_flush = mem_cgroup_css_rstat_flush,
	.can_attach = mem_cgroup_can_attach,
	.attach = mem_cgroup_attach,
	.cancel_attach = mem_cgroup_cancel_attach,
	.post_attach = mem_cgroup_move_task,
	.dfl_cftypes = memory_files,
	.legacy_cftypes = mem_cgroup_legacy_files,
	.early_init = 0,
};
```

`can_attach` / `attach` / `post_attach` 服务的是 v1「迁移时搬走 charge」。v2 的归属规则见下一节，不靠这条路径把已经分配的页改记到新组。

---

## 6.4 Charge 直觉

普通 folio（匿名页、页缓存）的入口是内联函数 `mem_cgroup_charge`（`memcontrol.h:681`）。控制器关掉时它直接返回 0，否则调用 `__mem_cgroup_charge`。

```text
mem_cgroup_charge(folio, mm, gfp)          # memcontrol.h:681
  → __mem_cgroup_charge                    # memcontrol.c:7123
       → get_mem_cgroup_from_mm(mm)        # :1061
            # mm 为空：先 active_memcg()，再 current->mm，都没有就用 root
       → charge_memcg(folio, memcg, gfp)   # :7102
            → try_charge                   # :2859；root memcg 直接成功
                 → try_charge_memcg        # :2669
                      → consume_stock      # 先吃 per-CPU 的批量库存
                      → page_counter_try_charge(memory)，
                        v1 合记 memsw 时先试 memsw
                      → 失败：try_to_free_mem_cgroup_pages
                        重试，仍不行则 mem_cgroup_oom (:1957)
            → commit_charge               # folio 的 memcg_data 指向这个 memcg
            → mem_cgroup_charge_statistics
释放
  → mem_cgroup_uncharge                    # memcontrol.h:701
       → __mem_cgroup_uncharge
            → page_counter_uncharge
```

| 符号 | 位置 | 作用 |
|---|---|---|
| `mem_cgroup_charge` | `include/linux/memcontrol.h:681` | 内联包装；swapin 不要走它 |
| `__mem_cgroup_charge` | `mm/memcontrol.c:7123` | 从 mm 找 memcg 并 charge |
| `charge_memcg` | `:7102` | `try_charge` + `commit_charge` |
| `try_charge` | `:2859` | root 直接返回 0 |
| `try_charge_memcg` | `:2669` | 库存、计数器、回收、OOM |
| `get_mem_cgroup_from_mm` | `:1061` | 按 mm 找 memcg，并拿一个 css 引用 |
| `mem_cgroup_swapin_charge_folio` | `:7147` | swapin：先按 swap 项上的 memcg id 找，找不到再用 mm |
| `mem_cgroup_swapin_uncharge_swap` | `:7179` | v1 memsw 把 swap 槽上的重复记账去掉；v2 这里是空操作 |
| `mem_cgroup_uncharge` | `memcontrol.h:701` | 释放 folio 时退费 |
| `mem_cgroup_oom` | `memcontrol.c:1957` | 硬顶之后的 OOM |
| `mem_cgroup_handle_over_high` | `:2590` | 回到用户态之前处理 high |

巨页不走 `mem_cgroup_charge` 这条包装。hugetlb 控制器自己的入口是 `hugetlb_cgroup_charge_cgroup`（`mm/hugetlb_cgroup.c:298`）。

**归属**（`cgroup-v2.rst` 5-2-3）：一块内存记在**把它创建出来的那个 cgroup** 上，直到这块内存被释放。把进程迁到另一个 cgroup，**不会**把以前分配的用量搬过去。所以迁移之后 `memory.current` 看起来「没跟着人走」是预期行为。

一块内存后来被别的 cgroup 里的进程使用时，最终记在谁头上文档写明是不确定的；时间久了，它更可能留在还付得起回收压力的那一组。

---

## 6.5 high vs max vs OOM

`try_charge_memcg`（`:2669`）把三件事分开做，不要合成一句「超限就杀」：

| 水位 | 代码里发生的事 |
|---|---|
| 低于 high / max | `page_counter_try_charge` 成功，多出来的批量库存用 `refill_stock` 放回 per-CPU |
| 越过 **high** | charge 已经成功之后，在 `:2818` 起比较 `usage` 和 `high`。在任务上下文里给 `current->memcg_nr_pages_over_high` 累加，并 `set_notify_resume`，回到用户态前由 `mem_cgroup_handle_over_high`（`:2590`）做回收或睡眠。不在任务上下文（比如中断）时，改成 `schedule_work(&memcg->high_work)`。事件 `MEMCG_HIGH` |
| 撞上 **max** | `page_counter_try_charge` 失败。先 `MEMCG_MAX` 事件，再 `try_to_free_mem_cgroup_pages`。`PF_MEMALLOC`（回收路径自己在分配）走 `force`，暂时允许超过上限，避免回收递归把自己 OOM 掉。普通分配重试耗尽后调用 `mem_cgroup_oom`（`:2771`）。正在死的任务（`task_is_dying()`）不再无限重试（`:2763`） |
| `oom.group == true` | OOM 杀一个属于本组的任务时，把同组任务一起杀掉。对应文件 `memory.oom.group` |

`__GFP_NOFAIL` 或 `__GFP_HIGH` 在 `:2784` 也会被强制 charge，同时补一次 `MEMCG_MAX` 事件。所以「撞上 max」不等于「这次分配一定失败」。

---

## 6.6 留给 MM 章的

- `shrinker`、lruvec、全局回收和 memcg 回收谁先谁后  
- writeback domain（`mem_cgroup` 里 `CONFIG_CGROUP_WRITEBACK` 那一组字段）  
- zswap 怎么和 `zswap_max` 配合  
- `cpuset.mems` 怎样进入分配慢路径  

本章的目标：看到 sysfs 上的数字时，知道背后是 `page_counter` 加上 `try_charge_memcg`，而不是一个看不懂的计数器。

---

*对照：`mm/memcontrol.c`、`include/linux/memcontrol.h`、`include/linux/page_counter.h`。*
