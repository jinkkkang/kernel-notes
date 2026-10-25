# 10.4 MGLRU 设计

**结论**：MGLRU 把一个 lruvec 上的可回收 folio 分成多代。最年轻的代编号是 `max_seq`，匿名和文件共用，老化时加。最老的代是 `min_seq[]`，两类分开，回收时加。一代里面再按访问次数分档（tier），档的移动只改 `folio->flags` 上的原子位，不拿 LRU 锁。页表遍历负责发现年轻 PTE，rmap 上的 look-around 负责把热点 PMD 塞进 Bloom filter。PID 用 refault 比例决定保护哪一档、匿名和文件扫哪一类。memcg 代只服务全局回收。

> 结构在 `include/linux/mmzone.h`，`CONFIG_LRU_GEN` 包着。设计文档 `Documentation/mm/multigen_lru.rst` 和下面的注释是同一套说法，行号以源码为准。
> 导航：[上一篇 问题](./10-03-lru-problems.md) · [本目录](./10-00-reclaim.md) · [下一篇 代码路径](./10-05-mglru-code-path.md)

没编 `CONFIG_LRU_GEN` 时，这些结构不存在。`mm_inline.h:295` 的 `lru_gen_enabled()` 恒返回 false，`lru_gen_add_folio`（`:305`）恒返回 false。`lru_gen_age_node`（`vmscan.c:6272`）、`lru_gen_shrink_lruvec`（`:6276`）、`lru_gen_shrink_node`（`:6280`）是空函数。

编了 `CONFIG_LRU_GEN` 但没编 `CONFIG_MEMCG` 时，`shrink_many`（`:5558`）和 `lru_gen_shrink_lruvec`（`:5563`）的函数体是 `BUILD_BUG()`。全局回收在 `mem_cgroup_disabled()` 时改走 `shrink_one(&pgdat->__lruvec, sc)`（`:5596-5597`）。

---

## 代和档

设计文档 Workflow（`multigen_lru.rst:82-107`）和 `mmzone.h:320-370` 说的是同一件事。

`MIN_NR_GENS` 是 2，`MAX_NR_GENS` 是 4（`:348-349`）。窗口长度在这两者之间滑动。`folio->flags` 里的 gen 计数存的是 `gen+1`，范围 `[1, MAX_NR_GENS]`；不在 `lrugen->folios[]` 上时存 0（`:325-326`）。截断宽度是 `order_base_2(MAX_NR_GENS+1)` 位（`:345`，设计文档 `:90`）。这个数由 `kernel/bounds.c:26` 写成 `LRU_GEN_WIDTH`（`CONFIG_LRU_GEN` 时），生成进 `include/generated/bounds.h`。`MAX_NR_GENS + 1` 是 5，`order_base_2(5)` 是 3。`vmscan.c:6258` 有 `BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS)`，保证这几位装得下。

second chance（`:328-336`）：folio 缺页进来之后，老化至少要查两次访问位。第一次清掉缺页时带上的 accessed，第二次确认之后没用过。这至少要两代，所以 `MIN_NR_GENS` 是 2。为了 `/proc/vmstat` 一类 ABI，这两代算 active，更老的代算 inactive。`lru_gen_is_active`（`mm_inline.h:164`）就是 `gen` 等于 `max_seq % MAX_NR_GENS` 或 `(max_seq-1) % MAX_NR_GENS` 的那两格。

`PG_active` 在 folio 挂上 `lrugen->folios[]` 期间一直是清的，老化不用管它。隔离去做迁移这类「不是回收」的事时，如果这一代算 active，再把 `PG_active` 设回去（`:338-341`）。看 `lru_gen_add_folio` / `lru_gen_del_folio`。

`min_seq[anon]` 和 `min_seq[file]` 平常同步。swap 受限制时（例如没有 swap 空间），文件的 `min_seq` 可以往前走，把匿名的 `min_seq` 留在后面（`:411-413`）。`seq_is_valid`（`vmscan.c:3310`）把这个关系写成：文件的代数在 `[MIN_NR_GENS, 匿名的代数]` 里，匿名的代数不超过 `MAX_NR_GENS`。也就是文件的 `min_seq` 不小于匿名的。

档：经文件描述符访问 N 次的 folio 在 tier `order_base_2(N)`（`:352-353`）。`MAX_NR_TIERS` 是 4（`:371`）。第一档（N=0 或 1）靠 `PG_referenced`，除非是经页表缺页进来或预读。其余档（N>1）同时靠 `PG_referenced` 和 `PG_workingset`（`:353-357`）。`kernel/bounds.c:27` 把 `__LRU_REFS_WIDTH` 定义成 `MAX_NR_TIERS - 2`，也就是 2。真正的 `LRU_REFS_WIDTH` 在 `page-flags-layout.h:110` 里是这个值和「页标志剩余位数」的较小者，位数不够会被截短。

跨代要拿 LRU 锁。跨档只是 `folio->flags` 上的原子操作（`:359-361`）。驱逐时比较第一档和其余档的 `refaulted/(evicted+protected)`，决定多次经文件描述符访问的页算不算热、值不值得保护（`:361-364`）。第一档不需要 protected 计数，所以 `protected[]` 的最后一维是 `MAX_NR_TIERS - 1`（`:433`）。

页表访问和文件描述符访问是两条通道。设计文档 Assumptions（`multigen_lru.rst:51-78`）写明页表这条保护更强，三条理由：访问位是近似的，不确定性更高；赶走它要刷 TLB，也更容易碰到脏位，成本更高；保护不足的惩罚更高，因为应用程序通常不像对待阻塞 IO 那样为 major fault 做准备。默认假设页表通道有时间局部性，除非 VMA 带 `VM_SEQ_READ` 或 `VM_RAND_READ`；文件描述符通道默认没有，除非观察到离群的 refault。

`vma_has_recency`（`mm_inline.h:600`）在 `VM_SEQ_READ`、`VM_RAND_READ` 或文件的 `FMODE_NOREUSE` 时返回 false。`lru_gen_in_fault`（`:120`）看的是 `current->in_lru_fault`。缺页入口 `handle_mm_fault`（`mm/memory.c:5323`）在 `:5350` 调用 `lru_gen_enter_fault`（`:5262`），把它设成 `vma_has_recency(vma)`。注释：LRU 算法只处理带新近度的访问。

---

## `struct lru_gen_folio`（`mmzone.h:418`）

```c
struct lru_gen_folio {
	/* 最年轻一代的编号。老化时增加。匿名和文件共用 */
	unsigned long max_seq;
	/*
	 * 各自最老一代。回收时增加。
	 * 平常同步。swap 受限制时，文件可以走在匿名前面。
	 */
	unsigned long min_seq[ANON_AND_FILE];
	/* 每一代的出生时间，单位 jiffies。inc_max_seq 里写入（vmscan.c:4478） */
	unsigned long timestamps[MAX_NR_GENS];
	/* 分代链表。下标是代、匿名/文件、zone。驱逐时才懒排序 */
	struct list_head folios[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
	/*
	 * 每一格的页数。注释（:415-416）写明最终一致，
	 * reset_batch_size() 还没做完时可以暂时是负数。
	 */
	long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
	/* refault 的指数滑动平均。PID 的 I 项，平滑系数 1/2（vmscan.c:3673） */
	unsigned long avg_refaulted[ANON_AND_FILE][MAX_NR_TIERS];
	/* 被驱逐加被保护的滑动平均 */
	unsigned long avg_total[ANON_AND_FILE][MAX_NR_TIERS];
	/*
	 * 受保护页。只能在 LRU 锁下改。
	 * 第一档不需要保护，所以最后一维是 MAX_NR_TIERS - 1。
	 */
	unsigned long protected[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS - 1];
	/* 被驱逐的。可以不拿 LRU 锁 */
	atomic_long_t evicted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
	/* 被重新缺页进来的。同样可以不拿 LRU 锁 */
	atomic_long_t refaulted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
	/*
	 * 这个 lruvec 是否启用 MGLRU。
	 * lru_gen_add_folio 看到它为假就返回 false，页回到经典链。
	 */
	bool enabled;
#ifdef CONFIG_MEMCG
	/* 所属 memcg 代 */
	u8 gen;
	/* 在 memcg 链表的哪一段（head / tail / default） */
	u8 seg;
	/* 挂到节点的全局回收 memcg 链表 */
	struct hlist_nulls_node list;
#endif
};
```

`NR_HIST_GENS`：编了 `CONFIG_LRU_GEN_STATS` 时等于 `MAX_NR_GENS`，否则是 1（`:399-402`）。历史统计关掉时，被赶走的代只留最新一格。`lru_gen_full` 要历史，见 [06](./10-06-lru-vs-mglru.md)。

`lru_gen_add_folio`（`mm_inline.h:220`）按四种情况选代，注释写在 `:234-243`：

| 情况 | 放进哪一代 |
|---|---|
| `PG_active`，刚缺页进来的热页 | `max_seq`，最年轻 |
| 匿名且还没进 swap cache，或者带 `PG_reclaim` 的脏页 / 写回中 | `max_seq - 1`，次年轻 |
| 正在回收，或者窗口还没比 `MIN_NR_GENS` 更长 | `min_seq[type]`，最老 |
| 其余 | 算 inactive 则 `min_seq+1`，否则最老 |

unevictable 或 `enabled == false` 时直接返回 false（`:231`）。

---

## `struct lru_gen_mm_state`（`:463`）

双缓冲 Bloom filter。`NR_BLOOM_FILTERS` 是 2（`:461`）。挂在 `lruvec` 里，和 `CONFIG_LRU_GEN` 一起出现，没有再套别的配置宏。

```c
struct lru_gen_mm_state {
	/* 和 max_seq 对齐。每轮迭代之后同步 */
	unsigned long seq;
	struct list_head *head;		/* 当前这轮从 mm 链表的哪里继续 */
	struct list_head *tail;		/* 上一轮在哪里结束 */
	/* Bloom filter。每轮翻转用哪一张 */
	unsigned long *filters[NR_BLOOM_FILTERS];
	/* 调试用的 mm 计数 */
	unsigned long stats[NR_HIST_GENS][NR_MM_STATS];
};
```

`NR_MM_STATS`（`:450-457`）：

| 枚举 | 含义 |
|---|---|
| `MM_LEAF_TOTAL` | 叶子项总数 |
| `MM_LEAF_OLD` | 老叶子 |
| `MM_LEAF_YOUNG` | 年轻叶子 |
| `MM_NONLEAF_TOTAL` | 非叶子总数 |
| `MM_NONLEAF_FOUND` | 在 Bloom filter 里找到的非叶子 |
| `MM_NONLEAF_ADDED` | 加进 Bloom filter 的非叶子 |

设计文档 Bloom filters 一节（`multigen_lru.rst:195-209`）：成员测试是概率的。假阳性的代价是多扫一段 PTE，而这段里仍可能有热页。假阳性率由 filter 自己的参数封顶。代码里的参数写在 `vmscan.c:3322-3326`：`m = 1<<15`，`k = 2`，插入约 10000 项时假阳性大约 1/5，约 20000 项时大约 1/2。`BLOOM_FILTER_SHIFT` 是 15（`:3344`）。

---

## `struct lru_gen_mm_walk`（`:476`）

一个回收线程正在做的页表遍历。挂在 `struct reclaim_state.mm_walk` 上（`include/linux/swap.h:160`）。kswapd 还有一份嵌在 `pg_data_t` 里（`mmzone.h:1390`）。

```c
struct lru_gen_mm_walk {
	struct lruvec *lruvec;			/* 正在回收的 lruvec */
	/*
	 * 从 lru_gen_folio 读来的 max_seq。注释写明可以是过期的（unstable）。
	 */
	unsigned long max_seq;
	unsigned long next_addr;		/* 这个 mm 里下一次从哪个地址接着扫 */
	/* 攒着的、待提升页数。批量刷进 lrugen->nr_pages */
	int nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
	int mm_stats[NR_MM_STATS];		/* 攒着的 mm 计数 */
	int batched;				/* 攒下的总项数 */
	bool can_swap;				/* 这次遍历能不能动匿名 */
	/*
	 * 为真时少用启发式、多扫。debugfs 的 force_scan 最终传到这里。
	 */
	bool force_scan;
};
```

---

## `struct ctrl_pos`（`mm/vmscan.c:3688`）

PID 的一个采样点。注释（`:3669-3686`）：

- P 项：正在被驱逐的那一代里，某一档的 `refaulted/(evicted+protected)`。
- I 项：以前各代 P 项的指数滑动平均，平滑系数 1/2。
- D 项：不支持。注释留了以后可以用来给长寿的代打折，免得被过期信息钉死。
- 设定点 SP 永远是某一类的第一档。过程变量 PV 是另一类的任意档，或同一类的其它档。
- 误差是 SP 和 PV 的差。修正是 SP>PV 时关掉保护，SP<PV 时打开保护。

```c
struct ctrl_pos {
	unsigned long refaulted;	/* 这一侧计入的 refault */
	unsigned long total;		/* 被驱逐加上被保护 */
	/*
	 * 增益。比较匿名和文件时，一边放 swappiness，
	 * 另一边放 200 - swappiness（get_type_to_scan，:5109）。
	 */
	int gain;
};
```

`positive_ctrl_err`（`:3747`）：PV 的 refault 少于 `MIN_LRU_BATCH`，或者 PV 的 refault/total 比 SP 低（按 gain 缩放），返回 true。`MIN_LRU_BATCH` 是 `BITS_PER_LONG`（`mmzone.h:395`）。`MAX_LRU_BATCH` 是它的 64 倍（`:396`）。

`read_ctrl_pos`（`:3694`）把平均和当前历史格加在一起。有 tier 时，`total` 再加 `protected[hist][type][tier - 1]`。`reset_ctrl_pos`（`:3709`）在一代结束时把当前值除 2 写回平均，或清掉历史格。

---

## `struct lru_gen_memcg`（`mmzone.h:543`）

每个 node 一份 memcg 的 LRU，本身也是「LRU 的 LRU」：每个 node×memcg 还有自己的 folio LRU（`mem_cgroup_lruvec()`）。`MEMCG_NR_GENS` 是 3，`MEMCG_NR_BINS` 是 8（`:540-541`）。注释（`:536-538`）说明为什么是 3 代而不是 2 代：有效的只有老（seq）和年轻（seq+1）。多出来的一格是为了无锁读到过期的 `seq-1` 时，不会绕回年轻代。没编 memcg 时这个结构是空的，`MEMCG_NR_GENS` 定义成 1（`:565-567`）。

```c
struct lru_gen_memcg {
	/* 这个 node 的 memcg 代计数。模 MEMCG_NR_GENS 之后指向老代 */
	unsigned long seq;
	unsigned long nr_memcgs[MEMCG_NR_GENS];	/* 每一代里有多少 memcg */
	/*
	 * 每代再随机分到 8 个桶。桶用 hlist_nulls，
	 * 虚拟地分成 head、tail、default 三段。
	 */
	struct hlist_nulls_head	fifo[MEMCG_NR_GENS][MEMCG_NR_BINS];
	spinlock_t lock;			/* 保护上面的计数和链表 */
};
```

操作和触发事件写在 `:509-530`：

| 操作 | 做什么 |
|---|---|
| `MEMCG_LRU_HEAD` | 挪到当前代（老或年轻）某个随机桶的头，`seg` 改成 head |
| `MEMCG_LRU_TAIL` | 挪到当前代某个随机桶的尾，`seg` 改成 tail |
| `MEMCG_LRU_OLD` | 挪到老代某个随机桶的头，`gen` 改成老，`seg` 复位成 default |
| `MEMCG_LRU_YOUNG` | 挪到年轻代某个随机桶的尾，`gen` 改成年轻，`seg` 复位成 default |

触发：超过 soft limit → HEAD。第一次回收一个低于 low 的 memcg → TAIL。第一次回收已下线或低于可回收尺寸阈值的 memcg → TAIL；第二次 → YOUNG。试图回收低于 min 的 → YOUNG。驱逐路径上老化做完 → YOUNG。memcg 下线 → OLD。新上线的 memcg 加到老代某个随机桶的尾。驱逐从老代某个随机桶的头开始。老代所有桶空了，node 的 memcg 代计数加一。

节点上的这份在 `pg_data_t.memcg_lru`（`mmzone.h:1392`），只在 `CONFIG_LRU_GEN` 下编译。

---

## 运行时能力位

`mmzone.h:388-393`：

```c
enum {
	LRU_GEN_CORE,			/* 主开关，bit 0 */
	LRU_GEN_MM_WALK,		/* 成批清叶子 PTE 的访问位，bit 1 */
	LRU_GEN_NONLEAF_YOUNG,		/* 连非叶子一起清，bit 2 */
	NR_LRU_GEN_CAPS
};
```

`/sys/kernel/mm/lru_gen/enabled` 读出来的就是这三位的掩码。实现和文档对照见 [06](./10-06-lru-vs-mglru.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `lru_gen_enabled` | `include/linux/mm_inline.h:105` / `:112` / `:295` | 主开关。三种编译条件 |
| `lru_gen_in_fault` | `mm_inline.h:120` | 当前缺页是否带新近度 |
| `lru_gen_enter_fault` | `mm/memory.c:5262` | `handle_mm_fault`（`:5323`）在 `:5350` 调用它 |
| `vma_has_recency` | `mm_inline.h:600` | 顺序读、随机读、`FMODE_NOREUSE` 时为假 |
| `lru_gen_from_seq` | `mm_inline.h:125` | `seq % MAX_NR_GENS` |
| `lru_gen_is_active` | `mm_inline.h:164` | 最年轻的两代算 active |
| `folio_lru_gen` | `mm_inline.h:157` | 从 `folio->flags` 读代，不在链表上是 -1 |
| `folio_lru_refs` | `mm_inline.h:143` | 文件描述符通道的访问次数（第一档算 0） |
| `lru_gen_add_folio` | `mm_inline.h:220` | 按热度放进某一代 |
| `lru_gen_del_folio` | `mm_inline.h:271` | 从分代链表摘下 |
| `seq_is_valid` | `mm/vmscan.c:3310` | 文件代数夹在 MIN 和匿名代数之间 |
| `positive_ctrl_err` | `vmscan.c:3747` | PV 比 SP 冷，或样本太少 |
| `read_ctrl_pos` | `:3694` | 读一档的 refault / total / gain |
| `reset_ctrl_pos` | `:3709` | 一代结束时把历史折进平均 |

---

[上一篇 问题](./10-03-lru-problems.md) · [本目录](./10-00-reclaim.md) · [下一篇 MGLRU 代码路径](./10-05-mglru-code-path.md)
