# 10.4 MGLRU 设计

**结论**：MGLRU 把一个 lruvec 上的可回收 folio 分成多代。最年轻的代编号是 `max_seq`，匿名和文件共用，老化时加。最老的代是 `min_seq[]`，两类分开，回收时加。一代里面再按访问次数分档（tier），档的移动只改 `folio->flags` 上的原子位，不拿 LRU 锁。页表遍历负责发现年轻 PTE，rmap 上的 look-around 负责把热点 PMD 塞进 Bloom filter。PID 用 refault 比例决定保护哪一档、匿名和文件扫哪一类。memcg 代只服务全局回收。

> 结构在 `include/linux/mmzone.h`，`CONFIG_LRU_GEN` 包着。设计文档 `Documentation/mm/multigen_lru.rst` 和下面的注释是同一套说法，行号以源码为准。
> 导航：[上一篇 问题](./10-03-lru-problems.md) · [本目录](./10-00-reclaim.md) · [下一篇 代码路径](./10-05-mglru-code-path.md)

没编 `CONFIG_LRU_GEN` 时，这些结构不存在。`mm_inline.h:315` 的 `lru_gen_enabled()` 恒返回 false，`lru_gen_age_node`（`vmscan.c:5952`）和 `lru_gen_shrink_node`（`:5962`）的函数体是 `BUILD_BUG()`。调用点包在 `lru_gen_enabled() || lru_gen_switching()` 里，常量条件把调用裁掉。

---

## 代和档

设计文档 Workflow（`multigen_lru.rst:82-107`）和 `mmzone.h:444-494` 说的是同一件事。

`MIN_NR_GENS` 是 2，`MAX_NR_GENS` 是 4（`:471-472`）。窗口长度在这两者之间滑动。`folio->flags` 里的 gen 计数存的是 `gen+1`，范围 `[1, MAX_NR_GENS]`；不在 `lrugen->folios[]` 上时存 0（`:449-450`）。截断宽度是 `order_base_2(MAX_NR_GENS+1)` 位（`:468`，设计文档 `:90`）。这个数由 `kernel/bounds.c:27` 写成 `LRU_GEN_WIDTH`（`CONFIG_LRU_GEN` 时），生成进 `include/generated/bounds.h`，源码树里没有生成结果。`MAX_NR_GENS + 1` 是 5，`order_base_2(5)` 是不小于 5 的最小 2 的幂的指数，即 3。`vmscan.c:5936` 有 `BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS)`，保证这几位装得下。

second chance（`:452-455`）：folio 缺页进来之后，老化至少要查两次访问位。第一次清掉缺页时带上的 accessed，第二次确认之后没用过。这至少要两代，所以 `MIN_NR_GENS` 是 2。为了 `/proc/vmstat` 一类 ABI，这两代算 active，更老的代算 inactive（`:456-459`）。`lru_gen_is_active`（`mm_inline.h:165`）就是 `gen` 等于 `max_seq` 或 `max_seq-1` 的那两格。

`min_seq[anon]` 和 `min_seq[file]` 理想情况下同步。为了极端 swappiness，允许错开，最多 `MAX_NR_GENS - MIN_NR_GENS - 1` 代（`:555-558`）。4 - 2 - 1 = 1，也就是最多差一代。注释说的是「at most」，算出来是 1。

档：经文件描述符访问 N 次的 folio 在 tier `order_base_2(N)`（`:475-476`）。`MAX_NR_TIERS` 是 4（`:494`）。第一档（N=0 或 1）靠 `PG_referenced`，除非是经页表缺页进来或预读。最后一档靠 `PG_workingset`。中间档用 `LRU_REFS_WIDTH` 位。`kernel/bounds.c:28` 把 `__LRU_REFS_WIDTH` 定义成 `MAX_NR_TIERS - 2`，也就是 2。真正的 `LRU_REFS_WIDTH` 在 `page-flags-layout.h:112` 里是这个值和「页标志剩余位数」的较小者，位数不够会被截短。

跨代要拿 LRU 锁。跨档只是 `folio->flags` 上的原子操作（`:482-483`）。驱逐时比较第一档和其余档的 `refaulted/(evicted+protected)`，决定多次经文件描述符访问的页算不算热、值不值得保护（`:484-487`）。

页表访问和文件描述符访问是两条通道。设计文档 Assumptions（`multigen_lru.rst:51-78`）写明页表这条保护更强，三条理由：访问位是近似的，不确定性更高；赶走它要刷 TLB，也更容易碰到脏位，成本更高；保护不足的惩罚更高，因为应用程序通常不像对待阻塞 IO 那样为 major fault 做准备。默认假设页表通道有时间局部性，除非 VMA 带 `VM_SEQ_READ` 或 `VM_RAND_READ`；文件描述符通道默认没有，除非观察到离群的 refault。

`lru_gen_in_fault`（`mm_inline.h:122`）看的是 `current->in_lru_fault`。缺页入口 `lru_gen_enter_fault`（`mm/memory.c:6779`）把它设成 `vma_has_recency(vma)`。注释：LRU 算法只处理带新近度的访问。

---

## `struct lru_gen_folio`（`mmzone.h:563`）

| 成员 | 中文 |
|---|---|
| `unsigned long max_seq` | 最年轻一代的编号。老化时增加。匿名和文件共用 |
| `unsigned long min_seq[ANON_AND_FILE]` | 各自最老一代。回收时增加。允许和另一类差若干代，用来适应极端 swappiness。差多少见上 |
| `unsigned long timestamps[MAX_NR_GENS]` | 每一代的出生时间，单位 jiffies。`inc_max_seq` 里 `WRITE_ONCE(timestamps[next], jiffies)`（`vmscan.c:4061`） |
| `struct list_head folios[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES]` | 分代链表。下标是代、匿名/文件、zone。驱逐时才懒排序 |
| `long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES]` | 每一格的页数。注释（`:560-561`）写明最终一致，`reset_batch_size()` 还没做完时可以暂时是负数 |
| `unsigned long avg_refaulted[ANON_AND_FILE][MAX_NR_TIERS]` | refault 的指数滑动平均。PID 的 I 项，平滑系数 1/2（`vmscan.c:3177`） |
| `unsigned long avg_total[ANON_AND_FILE][MAX_NR_TIERS]` | 被驱逐加被保护的滑动平均 |
| `unsigned long protected[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS]` | 受保护页。只能在 LRU 锁下改 |
| `atomic_long_t evicted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS]` | 被驱逐的。可以不拿 LRU 锁 |
| `atomic_long_t refaulted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS]` | 被重新缺页进来的 |
| `bool enabled` | 这个 lruvec 是否启用 MGLRU。`lru_gen_add_folio` 看到它为假就返回 false，页回到经典链 |
| `u8 gen` | 所属 memcg 代 |
| `u8 seg` | 在 memcg 链表的哪一段（head / tail / default） |
| `struct hlist_nulls_node list` | 挂到节点的全局回收 memcg 链表 |

`NR_HIST_GENS`：编了 `CONFIG_LRU_GEN_STATS` 时等于 `MAX_NR_GENS`，否则是 1（`:545-549`）。历史统计关掉时，被赶走的代只留最新一格。`lru_gen_full` 要历史，见 [06](./10-06-lru-vs-mglru.md)。

---

## `struct lru_gen_mm_state`（`:604`）

双缓冲 Bloom filter。`NR_BLOOM_FILTERS` 是 2（`:602`）。

| 成员 | 中文 |
|---|---|
| `unsigned long seq` | 和 `max_seq` 对齐。每轮迭代之后同步 |
| `struct list_head *head` | 当前这轮从 mm 链表的哪里继续 |
| `struct list_head *tail` | 上一轮在哪里结束 |
| `unsigned long *filters[NR_BLOOM_FILTERS]` | Bloom filter。每轮翻转用哪一张 |
| `unsigned long stats[NR_HIST_GENS][NR_MM_STATS]` | 调试用的 mm 计数 |

`NR_MM_STATS`（`:593-599`）：`MM_LEAF_TOTAL` 叶子项总数，`MM_LEAF_YOUNG` 年轻叶子，`MM_NONLEAF_FOUND` 在 Bloom filter 里找到的非叶子，`MM_NONLEAF_ADDED` 加进 Bloom filter 的非叶子。

设计文档 Bloom filters 一节（`multigen_lru.rst:195-209`）：成员测试是概率的。假阳性的代价是多扫一段 PTE，而这段里仍可能有热页。假阳性率由 filter 自己的参数封顶。

---

## `struct lru_gen_mm_walk`（`:617`）

一个回收线程正在做的页表遍历。挂在 `struct reclaim_state.mm_walk` 上（`include/linux/swap.h:151`）。

| 成员 | 中文 |
|---|---|
| `struct lruvec *lruvec` | 正在回收的 lruvec |
| `unsigned long seq` | 从 `lru_gen_folio` 读来的 `max_seq`。注释写明可以是过期的 |
| `unsigned long next_addr` | 这个 mm 里下一次从哪个地址接着扫 |
| `int nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES]` | 攒着的、待提升页数。批量刷进 `lrugen->nr_pages` |
| `int mm_stats[NR_MM_STATS]` | 攒着的 mm 计数 |
| `int batched` | 攒下的总项数 |
| `int swappiness` | 这次遍历用的 swappiness |
| `bool force_scan` | 为真时少用启发式、多扫。debugfs 的 `force_scan` 最终传到这里 |

---

## `struct ctrl_pos`（`mm/vmscan.c:3192`）

PID 的一个采样点。注释（`:3174-3190`）：

- P 项：正在被驱逐的那一代里，某一档的 `refaulted/(evicted+protected)`。
- I 项：以前各代 P 项的指数滑动平均，平滑系数 1/2。
- D 项：不支持。注释留了以后可以用来给长寿的代打折，免得被过期信息钉死。
- 设定点 SP 永远是某一类的第一档。过程变量 PV 是另一类的任意档，或同一类的其它档。
- 误差是 SP 和 PV 的差。修正是 SP>PV 时关掉保护，SP<PV 时打开保护。

| 成员 | 中文 |
|---|---|
| `unsigned long refaulted` | 这一侧计入的 refault |
| `unsigned long total` | 被驱逐加上被保护 |
| `int gain` | 增益。比较匿名和文件时，一边放 swappiness，另一边放 `MAX_SWAPPINESS - swappiness`（`get_type_to_scan`，`:4835`） |

`positive_ctrl_err`（`:3253`）：PV 的 refault 少于 `MIN_LRU_BATCH`，或者 PV 的 refault/total 比 SP 低（按 gain 缩放），返回 true。`MIN_LRU_BATCH` 是 `BITS_PER_LONG`（`mmzone.h:541`）。

`read_ctrl_pos`（`:3198`）把平均和当前历史格加在一起。`reset_ctrl_pos`（`:3217`）在一代结束时把当前值除 2 写回平均，或清掉历史格。

---

## `struct lru_gen_memcg`（`mmzone.h:679`）

每个 node 一份 memcg 的 LRU，本身也是「LRU 的 LRU」：每个 node×memcg 还有自己的 folio LRU（`mem_cgroup_lruvec()`）。`MEMCG_NR_GENS` 是 3，`MEMCG_NR_BINS` 是 8（`:676-677`）。注释（`:669-674`）说明为什么是 3 代而不是 2 代：有效的只有老（seq）和年轻（seq+1）。多出来的一格是为了无锁读到过期的 `seq-1` 时，不会绕回年轻代。

| 成员 | 中文 |
|---|---|
| `unsigned long seq` | 这个 node 的 memcg 代计数。模 `MEMCG_NR_GENS` 之后指向老代 |
| `unsigned long nr_memcgs[MEMCG_NR_GENS]` | 每一代里有多少 memcg |
| `struct hlist_nulls_head fifo[MEMCG_NR_GENS][MEMCG_NR_BINS]` | 每代再随机分到 8 个桶。桶用 `hlist_nulls`，虚拟地分成 head、tail、default 三段 |
| `spinlock_t lock` | 保护上面的计数和链表 |

操作和触发事件写在 `:645-666`，这里按注释复述，不另起一套名字：

| 操作 | 做什么 |
|---|---|
| `MEMCG_LRU_HEAD` | 挪到当前代（老或年轻）某个随机桶的头，`seg` 改成 head |
| `MEMCG_LRU_TAIL` | 挪到当前代某个随机桶的尾，`seg` 改成 tail |
| `MEMCG_LRU_OLD` | 挪到老代某个随机桶的头，`gen` 改成老，`seg` 复位成 default |
| `MEMCG_LRU_YOUNG` | 挪到年轻代某个随机桶的尾，`gen` 改成年轻，`seg` 复位成 default |

触发：超过 soft limit → HEAD。第一次回收一个低于 low 的 memcg → TAIL。第一次回收已下线或低于可回收尺寸阈值的 memcg → TAIL；第二次 → YOUNG。试图回收低于 min 的 → YOUNG。驱逐路径上老化做完 → YOUNG。memcg 下线 → OLD。新上线的 memcg 加到老代某个随机桶的尾。驱逐从老代某个随机桶的头开始。老代所有桶空了，node 的 memcg 代计数加一。

设计文档（`multigen_lru.rst:241-251`）补了全局回收上的两点：分片让每个线程随机起步，提高并行；直接回收可以随时停，公平放在一段时间上看（各 memcg 的 `max_seq` 轮转增加）。最好情况 O(1)，最坏仍 O(n)。

---

## 老化、驱逐、工作集，各一句

老化生产年轻代。`max_seq - min_seq + 1` 接近 `MIN_NR_GENS` 时增加 `max_seq`。找到经页表访问过的热页，把它的 gen 更新成 `(max_seq % MAX_NR_GENS) + 1`，冷页因为编号前移而自然变老（设计文档 `:113-126`）。页表遍历走 `lruvec_memcg()->mm_list`，对每个 `mm_struct` 调 `walk_page_range()`。多个遍历者分到不同的 mm，可以并行（`:166-170`）。已经迁移走的 mm，留在旧 memcg 里的页会被忽略；别的 node 上的页也会被忽略（`:172-175`）。上下文切换之间会记 mm 的使用，睡着的进程可以跳过（`:177-179`）。

驱逐消费老代。`min_seq` 那一格的链表空了，就把 `min_seq` 加一（`:130-132`）。先比 `min_seq[]`，选更老的那一类；一样老，就选第一档 refault 百分比更低的那一类。第一档是单次使用、未映射、干净的页，文档称之为最好的下注（`:134-136`）。

工作集保护：每代出生时盖时间戳。`lru_gen_min_ttl` 非 0 时，最老一代的年龄还在这个毫秒数之内，这个 lruvec 不被驱逐。保不住就 OOM（`:145-152`）。文档列的好处：和具体应用、内存大小无关，所以好配；直接接到 OOM killer，所以比一个软阈值可靠（`:154-158`）。

代码怎么把这些串起来，在 [05](./10-05-mglru-code-path.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `lru_gen_enabled` | `include/linux/mm_inline.h:107` 或 `:114`；未编时 `:315` | 主开关。未编时恒 false |
| `lru_gen_is_active` | `mm_inline.h:165` | 最年轻的两代算 active |
| `lru_tier_from_refs` | `mm_inline.h:137` | 引用次数到档。`workingset` 直接最后一档 |
| `folio_lru_refs` | `mm_inline.h:145` | 从 `PG_referenced` 和 `LRU_REFS_MASK` 读次数 |
| `folio_lru_gen` | `mm_inline.h:158` | 从 `LRU_GEN_MASK` 读代，没有则 -1 |
| `read_ctrl_pos` | `mm/vmscan.c:3198` | 读 PID 的一个点 |
| `positive_ctrl_err` | `:3253` | PV 是不是比 SP 更值得扫 |
| `reset_ctrl_pos` | `:3217` | 滑动平均除 2，或清历史 |
| `lru_gen_enter_fault` | `mm/memory.c:6779` | 缺页期间标记 `in_lru_fault`。未编时 `:6790` 是空函数 |

---

[上一篇 问题](./10-03-lru-problems.md) · [本目录](./10-00-reclaim.md) · [下一篇 MGLRU 代码路径](./10-05-mglru-code-path.md)
