# 10.3 经典 LRU 有什么问题

**结论**：v7.3 的设计文档、管理文档、合入系列的提交说明、以及 `vmscan.c` 自己的注释，写了几件经典回收做起来贵、或者选页选不好的事。下面只列这些出处里实际写了的，每条指回 [02](./10-02-lru-classic.md) 里的函数。出处没写的，这里不写。

> 设计文档：`Documentation/mm/multigen_lru.rst`。
> 管理文档：`Documentation/admin-guide/mm/multigen_lru.rst`。
> 合入系列说明：提交 `e1fd09e3d1dd`（Yu Zhao，2022-09-18，`mm: x86, arm64: add arch_has_hw_pte_young()`）。这是 "Multi-Gen LRU Framework" v14 的第一片，提交说明里带了整组补丁的 cover。groundwork 本体是 `ec1c86b25f4b`（同日，`mm: multi-gen LRU: groundwork`）。
> 导航：[上一篇 经典 LRU](./10-02-lru-classic.md) · [本目录](./10-00-reclaim.md) · [下一篇 MGLRU 设计](./10-04-mglru-design.md)

---

## 1. 回收太贵，而且经常选错该赶走谁

合入系列说明（`e1fd09e3d1dd`）的 TLDR 原文：

> The current page reclaim is too expensive in terms of CPU usage and it often makes poor choices about what to evict.

这是这组补丁对「当时的页回收」的总评，不是对某一行代码的注释。MGLRU 的设计目标写在设计文档的 Objectives（`Documentation/mm/multigen_lru.rst:15-20`）：访问新近度要表示得好、尽量吃空间局部性、明显的选择走快路径、启发式要能自己纠正。

对应代码：贵在 CPU 上的那一截，主要是映射页的 rmap，见下一条。选错页的具体形状，文档拆在「只有 active/inactive 两类」和「匿名/文件怎么分压力」里，见第 2、第 4 条。

---

## 2. active/inactive 只有两类，MGLRU 把类别做成两倍

`include/linux/mmzone.h:466-469`：

> MAX_NR_GENS is set to 4 so that the multi-gen LRU can support twice the number of categories of the active/inactive LRU when keeping track of accesses through page tables.

文件描述符那条通道同样写了两倍（`:489-492`）：`MAX_NR_TIERS` 设成 4，跟踪经文件描述符的访问时，类别数是 active/inactive 的两倍。

`:456-459` 说明这两类和 MGLRU 的对应关系：second chance 至少要两代，所以 `MIN_NR_GENS` 是 2；为了和 active/inactive 的 ABI（例如 `/proc/vmstat`）兼容，这两代算 active，其余代若存在就算 inactive。管理文档（`Documentation/admin-guide/mm/multigen_lru.rst:151-153`）把 `max_gen_nr` 和 `max_gen_nr-1` 称为还没完全老化、等价于 active list，因此不能被主动回收命令直接赶走。

groundwork 提交说明（`ec1c86b25f4b`）把用词分开：hot/cold 留给 multi-gen LRU，active/inactive 留给原来的双链表。

对应代码：

- 五条链的定义 `enum lru_list`（`mmzone.h:389`）。可回收的只有 inactive/active × anon/file 这四条，冷热档位是「在不在 active」加上 `PG_referenced` 这一位。
- 升 active 的规则在 `folio_mark_accessed`（`mm/folio.c:414-418`）和 `folio_check_references`（`mm/vmscan.c:902-928`）：映射页要看两次 young，第二次才 ACTIVATE。两次机会只有一个中间态。
- `inactive_is_low`（`vmscan.c:2227-2252`）用一个随内存增大的比例去维持 active:inactive，没有更细的时间桶。

文档没有使用「过粗」这两个字。上面是「类别数要做成两倍」这句注释和 `MAX_NR_GENS` / `MAX_NR_TIERS` 的取值。

---

## 3. 逐页走 rmap 查访问位，又吃不到旁边的年轻 PTE

设计文档两处：

`Documentation/mm/multigen_lru.rst:29-34`：

> Exploiting spatial locality improves efficiency when gathering the accessed bit. A rmap walk targets a single page and does not try to profit from discovering a young PTE. A page table walk can sweep all the young PTEs in an address space, but the address space can be too sparse to make a profit. The key is to optimize both methods and use them in combination.

`Documentation/mm/multigen_lru.rst:182-187`：

> Searching the rmap for PTEs mapping each page on an LRU list (to test and clear the accessed bit) can be expensive because pages from different VMAs (PA space) are not cache friendly to the rmap (VA space). For workloads mostly using mapped pages, searching the rmap can incur the highest CPU cost in the reclaim path.

合入说明里第 07 片的一行摘要是 "Exploits spatial locality to improve efficiency when using the rmap."（`e1fd09e3d1dd` 的补丁列表）。实现是后来的 `lru_gen_look_around`，见 [05](./10-05-mglru-code-path.md)。

`vmscan.c` 自己也这么写。`shrink_active_list` 注释（`:2054-2056`）：folio 大多没映射时处理很快，可以持着 `lru_lock` 做完；映射着的话处理慢，慢在 `folio_referenced()`，所以先把 folio 从 LRU 摘下来再逐个查。

对应代码：

- `folio_referenced`（`mm/rmap.c:1072`）对一个 folio 做 `rmap_walk`，回调 `folio_referenced_one`（`:922`）。一次走一个 folio 的映射，不扫相邻 PTE。
- `folio_check_references`（`vmscan.c:868`）和 `shrink_active_list`（`:2115`）都是在已经从链表上摘下的 folio 上调用它。
- 设计文档说页表遍历能一次扫一片 young PTE，但地址空间太稀时不划算，所以两条都留着。MGLRU 的做法是 rmap 碰到年轻 PTE 时再看相邻项（look-around），扫得整齐就把 PMD 放进 Bloom filter，老化那边的页表遍历可以跳过或对准。见 [04](./10-04-mglru-design.md)。

---

## 4. 匿名和文件的压力是一套启发式，自己承认不是精确比较，并且会掉进 cache trap

设计文档对 MGLRU 的替代写的是 PID（`Documentation/mm/multigen_lru.rst:103-107` 和 `:211-216`）：反馈环按 refault 百分比、在 swappiness 的比例上决定从哪一档、哪一类赶走。`:216` 的原话是平衡匿名和文件的 refault 百分比，使之与 swappiness 成比例。PID 用代而不是墙上时钟当时间轴，因为 CPU 在不同内存压力下扫页的速率不一样（`:218-220`）。

经典路径的注释没有说「请换成 PID」。它写了这套启发式的边界，两段都在 `prepare_scan_control`：

`vmscan.c:2333-2339`：代价模型反映的是「付出 IO」和「在旋转上花 CPU」的相对成本。注释原文：这不打算做精确比较，只是说重载大致相当、或者两边的旋转次数差得压倒性的大时，按 CPU 工作去调扫描平衡。

`vmscan.c:2407-2414`：cache trap。文件页一开始就在 inactive 上，每次文件缺页都把扫描平衡推向文件 LRU；文件 LRU 一缩，被引用后转回 active 的窗口也变小。于是形成 runaway feedback：一条很小的、正在颠簸的文件 LRU 会变得比匿名页更有吸引力。检测和强制改扫匿名在 `file_is_tiny`（`:2437`）和 `get_scan_count`（`:2609`）。

`get_scan_count`（`:2596-2622`）在接近 OOM（`priority == 0`）时放弃这套平衡，改成按大小对等扫。流式文件访问则靠 `cache_trim_mode` 先不换匿名页。这些是启发式上的特例，不是一个按 refault 百分比闭环的控制器。

对应代码：`calculate_pressure_balance`（`:2444`）、`lruvec->cost[]` 的更新（`:2316-2360`）、`get_scan_count` 的 `SCAN_FRACT` / `SCAN_FILE` / `SCAN_ANON`。MGLRU 打开且不在切换时，`prepare_scan_control` 开头就返回（`:2287`），这套不跑。MGLRU 用 `get_type_to_scan` / `positive_ctrl_err`，见 [04](./10-04-mglru-design.md) 和 [05](./10-05-mglru-code-path.md)。

合入说明里的 "poor choices about what to evict" 没有点名 swappiness。把这条算进「选错」的依据是：设计文档用 PID 去平衡两类的 refault，经典代码自己写下了 cache trap 和「不是精确比较」。

---

## 5. 直接回收者一多，inactive 会被扫得比需要的快

`vmscan.c:1816-1820`，`too_many_isolated` 上面的注释：

> A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and then get rescheduled. When there are massive number of tasks doing page allocation, such sleeping direct reclaimers may keep piling up on each CPU, the LRU list will go small and be scanned faster than necessary, leading to unnecessary swapping, thrashing and OOM.

对应代码：`shrink_inactive_list`（`:1997`）在 `too_many_isolated`（`:1822`）为真时 `reclaim_throttle(..., VMSCAN_THROTTLE_ISOLATED)`。kswapd 直接返回 false，不受这条限制（`:1828`）。

这是经典隔离路径上的注释。MGLRU 的 `isolate_folios` 不走 `too_many_isolated`。不要把合入说明 "What's new" 第 2 条（`e1fd09e3d1dd` 里 long-tailed direct reclaim latency、old direct reclaim backoff）算到经典 LRU 头上：那段写的是 v14 相对这组补丁自己的旧退避，多换出了大约 `(total_mem>>DEF_PRIORITY)-nr_to_reclaim`。那是 MGLRU 补丁迭代，不是 `shrink_inactive_list` 的注释。

管理文档把颠簸单独做成 `min_ttl_ms`（`Documentation/admin-guide/mm/multigen_lru.rst:65-82`）：个人机器对颠簸更敏感，因为 UI 会卡。写出 N 毫秒，就保护这么长的工作集，保不住就 OOM。`N=1000` 按文档的说法通常能去掉人能察觉的、由颠簸引起的卡顿；`N=3000` 更不明显，代价是可能过早 OOM。默认 0，关掉。这是 MGLRU 提供的阀，不是经典 LRU 里的一个参数。实现在 [05](./10-05-mglru-code-path.md) 的 `lru_gen_age_node`。

---

## 6. 全局回收按 memcg 遍历，最好情况是线性的

设计文档 Memcg LRU 一节（`Documentation/mm/multigen_lru.rst:224-251`）：memcg LRU 只用于全局回收，目标是提高全局回收的扩展性。遍历 memcg 时，它把最好情况从 O(n) 做成 O(1)，最坏情况仍是 O(n)。另外两条：分片，每个线程从老一代里随机一个 memcg 开始，提高并行；最终公平，直接回收可以随时停，延迟下去，公平放在一段时间上看。

`mmzone.h:669-671` 重复了同一句：Memcg LRU 只用于全局回收；memcg 自己的回收仍走 `mem_cgroup_iter()`。

对应代码：经典全局回收在 `shrink_node_memcgs`（`vmscan.c:6160`）里 `mem_cgroup_iter` 走子树。MGLRU 全局回收在 `lru_gen_shrink_node`（`:5229`）里，memcg 没关时走 `shrink_many`，用的是节点上的 memcg 代，结构在 [04](./10-04-mglru-design.md)。

---

## 没写进上面的

下面这些经常被口头归到「经典 LRU 的问题」里，但在指定的四处出处中没有对应句子，本篇不把它们列成问题：

- 合入说明里 v14 相对旧 MGLRU 退避的长尾延迟（见第 5 条末尾）。
- `inc_max_seq` 注释里的 cold/hot inversion（`vmscan.c:4035-4039`）。那是 MGLRU 在 `max_seq+1` 和 `min_seq[anon]` 重叠时的记账问题，写在 [05](./10-05-mglru-code-path.md)，不是经典双链表的行为。
- 任何「某类业务应该关 MGLRU」的经验。开关的实践建议只复述管理文档，标 RECIPE，放在 [06](./10-06-lru-vs-mglru.md)。

---

[上一篇 经典 LRU](./10-02-lru-classic.md) · [本目录](./10-00-reclaim.md) · [下一篇 MGLRU 设计](./10-04-mglru-design.md)
