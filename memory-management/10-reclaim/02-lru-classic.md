# 10.2 经典双链表 LRU

**结论**：经典 LRU 把可回收 folio 放在五条链表上：inactive/active 各分匿名和文件，再加上 unevictable。新页先进 per-CPU 的 `folio_batch`，批量刷进 inactive 的头部。访问打 `PG_referenced`，第二次再升到 active。回收从 inactive 尾部摘，active 太「满」时先把冷页降下来。匿名和文件扫多少，由 swappiness 和 `lru_cost` 一起定。换出去的页在 radix tree 里留一个 shadow，再缺页时用 refault 距离决定要不要直接拉回 active。

MGLRU 打开时，下面「访问时」和「入链时」有一部分函数会提前返回，改走 [05](./05-mglru-code-path.md)。本节写的是 `lru_gen_enabled()` 为假时的行为。两套并排见 [06](./06-lru-vs-mglru.md)。

> `struct lruvec` 在 `include/linux/mmzone.h:766`。入链在 `mm/folio.c`。扫描在 `mm/vmscan.c`。refault 在 `mm/workingset.c`。
> 导航：[上一篇 总入口](./01-overview.md) · [本目录](./README.md) · [下一篇 经典 LRU 的问题](./03-lru-problems.md)

---

## `struct lruvec`（`mmzone.h:766`）

可回收的 folio 挂在这里。没开 memcg 时，node 的 `__lruvec` 就是唯一的一份。开了 memcg，每个 memcg×node 有自己的 lruvec，`mem_cgroup_lruvec()` 查找。`pgdat->__lruvec` 那份在开了 memcg 时不用。注释（`mmzone.h:1587-1591`）写 `NOTE: THIS IS UNUSED IF MEMCG IS ENABLED`，查找走 `mem_cgroup_lruvec()`。

| 成员 | 条件 | 中文 |
|---|---|---|
| `struct list_head lists[NR_LRU_LISTS]` | 始终 | 五条链。下标是 `enum lru_list`（`mmzone.h:389`）：`LRU_INACTIVE_ANON`、`LRU_ACTIVE_ANON`、`LRU_INACTIVE_FILE`、`LRU_ACTIVE_FILE`、`LRU_UNEVICTABLE`。active 的下标比对应 inactive 大 `LRU_ACTIVE`（1），file 比 anon 大 `LRU_FILE`（2）。注释（`:377-380`）要求保持这个算术，扫描代码靠减法选链 |
| `spinlock_t lru_lock` | 始终 | 这把 lruvec 的锁。memcg 每组一把，避免全局一把锁 |
| `struct lru_cost cost[ANON_AND_FILE]` | 始终 | 回收匿名和回收文件的代价。代价高的那边少扫。`prepare_scan_control()` 时更新。成员见下 |
| `spinlock_t cost_lock` | 始终 | 保护 `cost[]` |
| `atomic_long_t nonresident_age` | 始终 | 不在内存中的「年龄」，由 LRU 移动推动。workingset 用它给 shadow 盖时间戳 |
| `unsigned long refaults[ANON_AND_FILE]` | 始终 | 上一轮回收时看到的 refault 计数。和当前 `WORKINGSET_ACTIVATE_*` 比较，变了就说明有新工作集在建立 |
| `unsigned long flags` | 始终 | `enum lruvec_flags`（`:424`）：`LRUVEC_CGROUP_CONGESTED`、`LRUVEC_NODE_CONGESTED`。脏页背后的 BDI 拥塞。kswapd 可以解开 cgroup 设的拥塞，cgroup 回收解不开 kswapd 设的 |
| `struct lru_gen_folio lrugen` | `CONFIG_LRU_GEN` | 分代链表。全成员在 [04](./04-mglru-design.md) |
| `struct lru_gen_mm_state mm_state` | `CONFIG_LRU_GEN` 且 `CONFIG_LRU_GEN_WALKS_MMU` | 并发走 mm 链表的 Bloom filter 状态。全成员在 [04](./04-mglru-design.md) |
| `struct pglist_data *pgdat` | `CONFIG_MEMCG` | 回指 node |
| `struct zswap_lruvec_state zswap_lruvec_state` | 始终 | 这个 lruvec 的 zswap 状态。编了 `CONFIG_ZSWAP` 时只有一个成员 `atomic_long_t nr_disk_swapins`（`include/linux/zswap.h:14`）：从磁盘换入、zswap 池里没有的页数，shrinker 用它惩罚以前缩得太狠。没编 zswap 时这个结构是空的（`:40`） |

`struct lru_cost`（`mmzone.h:760`）：

| 成员 | 中文 |
|---|---|
| `unsigned long count` | 衰减后的代价。`prepare_scan_control` 里加上「IO 次数 × `SWAP_CLUSTER_MAX` + 旋转页数」，总和超过 lruvec 大小的 1/4 时两边同时除 2 |
| `unsigned long last_rotated` | 上次采样时的 `PGROTATE_*`。用来取增量 |
| `unsigned long last_io` | 上次采样时的 refault 恢复计数，匿名侧再加 `NR_VMSCAN_WRITE` |

`ANON_AND_FILE` 是 2（`mmzone.h:422`）。下标 0 是 `WORKINGSET_ANON`，1 是 `WORKINGSET_FILE`。

---

## `struct folio_batch`（`include/linux/folio_batch.h:28`）

注释（`:14`）写明 31 个指针加头部，把整个结构对齐到 2 的幂。`FOLIO_BATCH_SIZE` 是 31（`:15`）。

| 成员 | 中文 |
|---|---|
| `unsigned char nr` | 现在装了多少个 folio |
| `unsigned char i` | 队列式消费时的游标。`folio_batch_next` 用它。LRU 批量移动按 `nr` 扫，不靠 `i` |
| `bool percpu_pvec_drained` | 这个 batch 的 per-CPU 向量是否已经排空。LRU 热路径不读它，留给会混进 exceptional entry 的调用者 |
| `struct folio *folios[FOLIO_BATCH_SIZE]` | 指针数组。有的调用者会塞 exceptional entry，用 `folio_batch_remove_exceptionals()` 清 |

真正挂在每 CPU 上的是 `struct cpu_fbatches`（`mm/folio.c:44`），里面好几份 `folio_batch`：

| 成员 | 条件 | 中文 |
|---|---|---|
| `local_lock_t lock` | 始终 | 保护下面这组。关抢占，中断还开着 |
| `struct folio_batch lru_add` | 始终 | 新页等待入 LRU |
| `struct folio_batch lru_deactivate_file` | 始终 | 文件页准备降到 inactive |
| `struct folio_batch lru_deactivate` | 始终 | 匿名页准备降级 |
| `struct folio_batch lru_lazyfree` | 始终 | `MADV_FREE` 一类可以丢内容的页 |
| `struct folio_batch lru_activate` | `CONFIG_SMP` | 等待从 inactive 升到 active |
| `local_lock_t lock_irq` | 始终 | 保护下面需要关中断的 batch |
| `struct folio_batch lru_move_tail` | 始终 | 把页挪到链表尾 |

每 CPU 一份：`DEFINE_PER_CPU(struct cpu_fbatches, cpu_fbatches)`（`folio.c:62`）。

---

## 分配时：新页进哪条链

缺页或读页把 folio 准备好之后调用 `folio_add_lru`（`mm/folio.c:468`）。函数注释写明：先入队，不立刻决定 active 还是 inactive。这样调用者还能紧接着 `folio_mark_accessed()`，等 batch 刷下去时页已经带着 active 标记。

```text
folio_add_lru(folio)                         # folio.c:468
  VM_BUG：已经 active 又 unevictable，或者已经在 LRU 上
  MGLRU 且 lru_gen_in_fault() 且不是 PF_MEMALLOC：
    workingset → folio_set_active            # 给「活跃代」准备，见 05
    否则若还没 referenced → folio_mark_accessed
  folio_batch_add_and_move(folio, lru_add)   # 放进本 CPU 的 cpu_fbatches.lru_add

batch 满了，或 folio 不适合缓存，或 LRU 缓存被关掉：
  folio_batch_move_lru                       # :152
    引用冻成 1 的死页：丢进 free_fbatch，稍后 free_unref_folios
    其余：folio_lruvec_relock_irqsave
          lru_add(lruvec, folio)             # :112
            不可回收：清 active，设 unevictable，mlock_count = 0
            lruvec_add_folio                 # mm_inline.h:347
              lru_gen_add_folio 成功就返回   # MGLRU 开着且 lruvec 启用时，不进 lists[]
              否则 list_add 到 lists[folio_lru_list(folio)] 的头部
          folio_set_lru
    trace_mm_lru_insertion                   # include/trace/events/pagemap.h:28
```

`folio_lru_list`（`mm_inline.h:82`）：unevictable 返回 `LRU_UNEVICTABLE`；否则文件页从 `LRU_INACTIVE_FILE` 起、匿名页从 `LRU_INACTIVE_ANON` 起，`PG_active` 再加 `LRU_ACTIVE`。

新匿名页、新文件页默认进 **inactive 的头**。头是「刚来的」，尾是「最久没被提到前面的」。回收从尾部摘。

`folio_add_lru_vma`（`folio.c:501`）：VMA 带 `VM_LOCKED` 且不带 `VM_SPECIAL` 时走 `mlock_new_folio`，进 unevictable，见 [09](./09-mlock-unevictable.md)。否则和 `folio_add_lru` 一样。

---

## 访问时：两次机会才升 active

缓冲读、`mark_page_accessed` 一类路径调用 `folio_mark_accessed`（`folio.c:423`）。函数注释写了三条转移：

| 现在 | 之后 |
|---|---|
| inactive，未 referenced | inactive，referenced |
| inactive，referenced | active，未 referenced |
| active，未 referenced | active，referenced |

`PG_dropbehind` 的 folio 直接返回，不记访问。MGLRU 打开时这里不走上面三行，改调 `lru_gen_inc_refs`（`:355`），见 [05](./05-mglru-code-path.md)。

经典路径（`:432-453`）：

```text
还没 PG_referenced：
  folio_set_referenced                     # 第一次，留在 inactive
已经 referenced，但是 unevictable：
  什么也不做。注释（:435-438）写这条链不旋转
已经 referenced，还在 inactive：
  已在 LRU 上：folio_activate              # 排进 cpu_fbatches.lru_activate
  还在 batch 里：__lru_cache_activate_folio # 就地把 batch 里的那项设上 PG_active
  folio_clear_referenced
  workingset_activation                    # 记一笔激活，推动 nonresident_age 的使用者
PG_idle：清掉
```

映射页的「被碰过」不在 `folio_mark_accessed` 里看，在回收时用 `folio_referenced`（`mm/rmap.c:1072`）走 rmap，对每条映射 `test_and_clear` young 位。返回值是还有多少条映射带着 young。rmap 锁抢不到返回 -1。`VM_LOCKED` 的 VMA 会被标出来，调用者把页送去 unevictable。

`shrink_active_list` 的注释（`vmscan.c:2054-2056`）写了这件事的代价：folio 大多没映射时，持着 `lru_lock` 扫完就行；映射着的页要调 `folio_referenced()`，慢，所以先把页从 LRU 摘下来再逐个查，不能为了平衡锁的持有时间去猜。

---

## 回收时：比例、降级、摘除

### `inactive_is_low`（`vmscan.c:2254`）

目标是 active:inactive。注释（`:2227-2252`）给了一张表：10MB 比例 1，1GB 比例 3，10GB 比例 10，1TB 比例 101。算式是 `inactive_ratio = sqrt(10 * gb)`，内存不到 1GB 时比例是 1。`inactive * inactive_ratio < active` 就说明 inactive 太短，该从 active 降一批下来。

注释同时写了两条约束：inactive 匿名要短，VM 不必做太多工作；inactive 文件要短到把内存留给 active 上的工作集，又要长到盖住预读窗口，并且每页在被回收前有机会再被引用一次。refault 出现时 inactive 会长。

### `prepare_scan_control`（`:2280`）与 `get_scan_count`（`:2550`）

MGLRU 开着且不在切换中，`prepare_scan_control` 直接返回（`:2287`），这套代价模型不跑。

代价（`:2300-2341`）：每侧用 `PGROTATE_*` 的增量和 refault/写回的增量。注释写明「不打算做精确比较」：重载差不多，或者旋转次数差得很大时，才用旋转去调 CPU 侧的扫描平衡。公式是 `count += nr_io * SWAP_CLUSTER_MAX + nr_rotated`。

`may_deactivate`（`:2369-2393`）：这一侧的 `WORKINGSET_ACTIVATE_*` 相对上次变了，或者 `inactive_is_low`，就允许从 active 降级。refault 在涨，说明新工作集正在建立，把陈旧的 active 页赶快降下去。

`cache_trim_mode`（`:2395-2405`）：inactive 文件页按 priority 右移之后还有剩，而且文件侧不许降级，就先只扫文件，不去动匿名。注释：有足够的、没在颠簸的 inactive 文件页时，先收它们。

`file_is_tiny`（`:2407-2440`）：文件 LRU 一起始就是 inactive，每次文件缺页都把扫描压力推向文件侧；文件 LRU 越缩，被引用后转回 active 的窗口越小。注释把这叫做 runaway feedback：一条很小的、正在颠簸的文件 LRU 会比匿名页「无限更有吸引力」。检测条件是文件页加空闲页不超过各 zone 的 high 水位之和，匿名侧还不许降级，而且 inactive 匿名按 priority 右移后仍有页。全局回收才做这个检测。

`get_scan_count` 给四条可回收链填 `nr[]`：0/1 匿名 inactive/active，2/3 文件 inactive/active。分支：

| 条件 | `scan_balance` |
|---|---|
| swappiness 是 `SWAPPINESS_ANON_ONLY`（只出现在主动回收） | 只扫匿名。匿名收不走就清零返回 |
| 不许 swap，或这个 memcg/node 收不了匿名 | 只扫文件 |
| cgroup 回收且 swappiness 为 0 | 只扫文件。注释：memcg 用户用这个旋钮彻底关掉某一组的 swap |
| `priority == 0` 且 swappiness 非 0 | 按链表大小对等扫。接近 OOM，不再玩平衡 |
| `file_is_tiny` | 只扫匿名 |
| `cache_trim_mode` | 只扫文件。注释：流式文件访问不要把匿名工作集换出去 |
| 其余 | `SCAN_FRACT`，走 `calculate_pressure_balance`（`:2444`） |

压力公式（`:2450-2478`）：每一侧的压力和回收代价成反比，再乘 swappiness。swappiness 100 时匿名和文件的 IO 代价相等。注释写了下限：在 swappiness 起作用之前，至少大约三分之一的压力会打到每一侧，避免一条链被完全扔下。

然后 `lruvec_size >> priority`，再按 memcg 的 min/low 用 `apply_proportional_protection`（`:2481`）缩小。保护的注释写了为什么不能做成「没到阈值一点不扫、过了就全扫」：那样保护阈值必须设得非常松，设低了等于没有保护。

### 三条扫描函数

```text
shrink_list                                # vmscan.c:2212
  active 且 may_deactivate 允许：
    shrink_active_list                     # :2065
  active 但不允许：skipped_deactivate = 1，返回 0
  inactive：
    shrink_inactive_list                   # :1983
      too_many_isolated 则 reclaim_throttle # :1816 注释：直接回收者睡过去之后堆在 CPU 上，
                                            #   LRU 会被扫得比需要的快，多余 swap、颠簸、OOM
      lru_add_drain                        # 先把 per-CPU batch 刷进链表
      isolate_lru_folios                   # :1679，从链尾摘
      shrink_folio_list                    # :1058
      move_folios_to_lru                   # 没收回去的放回
```

`shrink_active_list`（`:2048` 起的注释）：从 active 摘下来，被引用的送回 active，其余清 `PG_active`、设 `PG_workingset`、送去 inactive。文件可执行页第一次被引用就多留一圈 active（`:2117-2124`）：中等压力下让可执行代码更有机会留下。匿名页这里不算，注释写 JVM 会造出很多带 `VM_EXEC` 的匿名页。

`shrink_folio_list`（`:1058`）对每个 folio：

```text
trylock 失败：keep，放回
hwpoison 大页：keep。普通页 unmap_poisoned_folio 后放下
不可回收：activate（交给 unevictable 逻辑）
may_unmap 为假且仍映射：keep
folio_check_dirty_writeback               # 匿名和 lazyfree 不当脏页去堵回收
写回中且已经 PG_reclaim：记 congested。注释（:1141）分三种写回尾页
folio_check_references                    # :868，除非调用者 ignore_references
  folio_referenced                        # rmap 查 young，见上
  VM_LOCKED：ACTIVATE
  rmap 锁竞争（返回 -1）：KEEP，转一圈
  MGLRU：lru_gen_set_refs                 # :840
  经典：PTE young 则再给一次 inactive 机会；
        页本身也 referenced，或 young 的映射多于 1，或是可执行文件页：ACTIVATE
        否则 KEEP
  没有 young：RECLAIM
ACTIVATE → 送回 active
KEEP → 留在 inactive
RECLAIM：
  能降级到更慢的 node：放进 demote_folios，本轮不丢（demote_folio_list :997）
  匿名且 swapbacked 且还没进 swap cache：
    没有 __GFP_IO，或被 pin：keep
    folio_alloc_swap；大页失败再拆开试
    folio_mark_dirty                      # 注释 :1299：MADV_FREE 的脏位和 SwapBacked 可能不一致
  仍映射：try_to_unmap                    # 大页加 TTU_SYNC
    还有映射：activate
  仍 pin：activate
  文件页且脏：设 PG_reclaim，activate，等 flusher。回收上下文不自己写文件页
  匿名脏页：pageout                       # :621，里面才进 zswap / swap 设备，见 07
  干净且已 unmap：从 address_space 摘掉，进 free batch
```

`pageout` 的结果（`:606`）：`PAGE_KEEP`、`PAGE_ACTIVATE`、`PAGE_SUCCESS`、`PAGE_CLEAN`。成功写出且不再脏，页就可以放掉。

---

## refault 时：shadow 与 workingset

换出或从页缓存丢掉时，空出来的 xarray 槽可以留下一个 shadow，而不是纯空。shadow 里编码了「被赶走时」`lruvec->nonresident_age` 的值。之后 LRU 每移动一批页，`workingset_age_nonresident`（`mm/workingset.c:366`）就把这个年龄往前推。注释写明：内存里的页在变老，不在内存里的记录也得一起变老，后来的 refault 距离才比得了。cgroup 回收是轮转孩子，所以叶子在变老时，祖先的虚拟 inactive 也一起变老（`:368-377`）。

再次缺页分配出替换 folio 时调用 `workingset_refault`（`:548`）。MGLRU 打开时转去 `lru_gen_refault`（`:558`），经典路径不往下走。

经典路径（`:563-588`）：

```text
记 WORKINGSET_REFAULT
workingset_test_recent(shadow, ...)
  距离 = 现在的 nonresident_age - shadow 里的年龄
  距离大于当时的工作集：只记 refault，不激活。页确实冷
  距离还在工作集里：
    folio_set_active                     # 直接进 active，不再从 inactive 头爬
    workingset_age_nonresident
    记 WORKINGSET_ACTIVATE
    shadow 标明当初是 workingset：
      folio_set_workingset
      记 WORKINGSET_RESTORE              # 这就是 lru_cost 里「IO」的来源之一
```

工作集大小（`:521` 一带）按文件/匿名、有没有 swap 槽，把 active 文件、以及需要时的 inactive 文件和匿名加进去。`refault_distance <= workingset_size` 才算「最近」。

`prepare_scan_control` 拿 `WORKINGSET_ACTIVATE_*` 和 `lruvec->refaults[]` 比。refault 激活在涨，就允许把 active 降级，给新工作集腾位置。代价模型拿 `WORKINGSET_RESTORE_*` 当 IO 成本。两边是同一套计数的两个用法。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `folio_add_lru` | `mm/folio.c:468` | 新页排进 per-CPU batch |
| `folio_add_lru_vma` | `folio.c:501` | mlock 的 VMA 走 unevictable，否则同 `folio_add_lru` |
| `lru_add` | `folio.c:112` | batch 刷进 lruvec |
| `lruvec_add_folio` | `include/linux/mm_inline.h:347` | MGLRU 失败时 `list_add` 到对应链的头 |
| `folio_lru_list` | `mm_inline.h:82` | 五条链里选哪条 |
| `folio_mark_accessed` | `mm/folio.c:423` | 经典两次机会，或转去 MGLRU |
| `folio_activate` | `mm/folio.c:295`（无 SMP 时 `:309`） | 已在 LRU 上的页排进 activate batch。`folio_mark_accessed` 在 `:448` 调用 |
| `folio_referenced` | `mm/rmap.c:1072` | rmap 上 test-and-clear young |
| `inactive_is_low` | `mm/vmscan.c:2254` | active 相对 inactive 是否超标 |
| `prepare_scan_control` | `:2280` | 填 cost、may_deactivate、cache_trim_mode、file_is_tiny |
| `get_scan_count` | `:2550` | 四条链各扫多少 |
| `calculate_pressure_balance` | `:2444` | swappiness 和代价合成比例 |
| `shrink_list` | `:2212` | active 降级或 inactive 回收 |
| `shrink_active_list` | `:2065` | active → inactive，引用过的留在 active |
| `shrink_inactive_list` | `:1983` | 隔离再 `shrink_folio_list` |
| `isolate_lru_folios` | `:1679` | 从链上摘 folio |
| `shrink_folio_list` | `:1058` | 释放 / 写出 / 放回 / 降级 |
| `folio_check_references` | `:868` | young 位和 `PG_referenced` 决定 ACTIVATE/KEEP/RECLAIM |
| `pageout` | `:621` | 回收上下文把脏匿名页写出去 |
| `demote_folio_list` | `:997` | 降到更慢的节点。迁移细节在 [10](./10-migration.md) |
| `workingset_age_nonresident` | `mm/workingset.c:366` | 推动 nonresident 年龄 |
| `workingset_refault` | `workingset.c:548` | 按 shadow 距离决定激活 |
| `workingset_activation` | `workingset.c:597` | 激活时的记账 |
| `too_many_isolated` | `vmscan.c:1822` | 直接回收者太多时先睡 |

---

[上一篇 总入口](./01-overview.md) · [本目录](./README.md) · [下一篇 经典 LRU 的问题](./03-lru-problems.md)
