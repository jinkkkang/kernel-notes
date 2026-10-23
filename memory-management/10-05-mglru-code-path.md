# 10.5 MGLRU 代码路径

**结论**：新页在 `lruvec_add_folio` 里被 `lru_gen_add_folio` 放进某一代的链表头，代编号由 `PG_active` / `PG_workingset` / `PG_referenced` 决定。访问不搬链表：文件描述符走 `lru_gen_inc_refs`，缺页期间 `lru_gen_enter_fault` 把这次 fault 标成「带新近度」。老化只给 kswapd 做，`lru_gen_age_node` 决定要不要加 `max_seq`，真正扫页表的是 `try_to_inc_max_seq`。驱逐从最老的代拿页，`sort_folio` 把已经变年轻或该保护的页挪走，剩下的才进 `shrink_folio_list`。`min_ttl` 让最老一代还太年轻时直接 OOM，而不是把工作集赶走。

> 入代在 `include/linux/mm_inline.h`。访问计数在 `mm/folio.c`。老化、驱逐在 `mm/vmscan.c`。
> 导航：[上一篇 设计](./10-04-mglru-design.md) · [本目录](./10-00-reclaim.md) · [下一篇 对照](./10-06-lru-vs-mglru.md)

查不到的符号：`folio_inc_refs`。v7.3 没有这个函数。加引用次数的是 `lru_gen_inc_refs`（`mm/folio.c:355`）。升一代的是 `folio_inc_gen`（`mm/vmscan.c:3299`）。`mmzone.h:507` 和 `mm_inline.h:232` 的注释写的就是 `folio_inc_gen()`。

---

## 新页入哪代

`folio_add_lru`（`mm/folio.c:468`）在刷进 lruvec 之前，如果 MGLRU 开着、当前在 `lru_gen_in_fault()`、任务不是 `PF_MEMALLOC`、folio 又可回收（`:481-487`）：

- `PG_workingset` 已经立着：`folio_set_active`。注释：refault 回来的工作集页，让它们进年轻代。
- 否则若还没有 `PG_referenced`：`folio_mark_accessed`。MGLRU 下这个函数转去 `lru_gen_inc_refs`，先立 `PG_referenced`。注释：预读进来的文件页由此进「次老」那一带。

真正挂链在 `lruvec_add_folio`（`mm_inline.h:347`）→ `lru_gen_add_folio`（`:255`）。unevictable 或 `lrugen->enabled` 为假，返回 false，调用者退回经典 `lists[]`。

`lru_gen_folio_seq`（`:221`）决定序号，注释里的表（`:230-240`）按标志把页放进窗口的不同位置：

| 条件 | 放哪 |
|---|---|
| `PG_active` | `MIN_NR_GENS - folio_test_workingset`。workingset 更靠近 `max_seq`，只是 referenced 的略老一点。注释：这是页表通道被 `folio_update_gen` 提升过、或者隔离期间设了 active 的页 |
| 正在回收（`reclaiming`） | `MAX_NR_GENS`，也就是尽量靠近 `min_seq` 的老端，放链表尾 |
| 匿名且不在 swap cache，或者脏/写回中且带 `PG_reclaim` | `MIN_NR_GENS`。先别当成最老的可赶走页 |
| 其余 | `MAX_NR_GENS - (workingset \|\| referenced)`。什么标志都没有就落在最老一代；有 referenced 或 workingset 就年轻一代 |

返回值再和 `min_seq[type]` 取最大，不会把页放到已经回收掉的代里。

`lru_gen_add_folio` 把 gen 写成 `(gen+1) << LRU_GEN_PGOFF`，清掉 `PG_active`（在分代链表上 active 位必须是 0，见 `mmzone.h:461-464`），`lru_gen_update_size` 改 `nr_pages`，同时为了 `/proc/vmstat` 维护 active/inactive 的大小。非回收路径 `list_add` 到链表头，回收路径 `list_add_tail`（`:277-280`）。

---

## 访问

三条，不要混。

**文件描述符 / `folio_mark_accessed`。** MGLRU 开着时 `folio_mark_accessed`（`folio.c:427`）只调 `lru_gen_inc_refs`（`:355`）就返回。这个函数不拿 LRU 锁：

```text
unevictable：返回
还没有 PG_referenced：
  立上 PG_referenced，清掉 LRU_REFS_MASK 里的旧值
已经 referenced，且 LRU_REFS_MASK 还没满：
  flags += BIT(LRU_REFS_PGOFF)          # 档加一
已经满了：
  立 PG_workingset，返回              # 最后一档。真正挪代是驱逐时懒做的
```

`mmzone.h:501-509` 写了懒晋升：所有引用位和 `PG_workingset` 都立上之后，驱逐路径里 `folio_inc_gen` 把它放进次老一代，并清掉 `LRU_REFS_FLAGS`，计数可以重新开始。`LRU_REFS_MASK` 只在 `PG_referenced` 立着时有意义。

**缺页。** `lru_gen_enter_fault`（`mm/memory.c:6779`）把 `current->in_lru_fault` 设成 `vma_has_recency(vma)`。`lru_gen_exit_fault`（`:6785`）清掉。这个标志不改 folio 的代，它决定 `folio_add_lru` 要不要按上面的规则预热标志。顺序读、随机读的 VMA 没有新近度，缺页路径不加分。

**页表遍历和 rmap。** 老化扫到年轻 PTE，走 `folio_update_gen`（`vmscan.c:3269`）。第一次只立 `PG_referenced` 并返回 -1，不改代（`:3280-3283`）。第二次把 gen 写成目标代，立 `PG_workingset`，清 `LRU_REFS_FLAGS`。可执行文件页第一次就当 workingset，注释（`:3276-3278`）说是为了避免典型 IO 把它们收回去造成颠簸。rmap 驱逐路径上对应的是 `lru_gen_set_refs`（`:840`），注释写明：映射页在驱逐里要先从 LRU 摘下、设 `PG_active` 再加回去；老化那边的页表遍历用的是 `folio_update_gen`，两条不要当成一个函数。

---

## 老化

老化只给 kswapd 做。`balance_pgdat`（`:7157`）在 `:7265` 调 `kswapd_age_node`（`:6906`）。MGLRU 打开或正在切换时，它转入 `lru_gen_age_node`（`:4209`），函数开头 `VM_WARN_ON_ONCE(!current_is_kswapd())`。

```text
kswapd_age_node                         # vmscan.c:6906
  lru_gen_enabled 或正在切换：
    lru_gen_age_node                    # :4209
      set_initial_priority              # :4128。按可回收页和 nr_to_reclaim 估一个起点，
                                        #   夹在 DEF_PRIORITY/2 和 DEF_PRIORITY 之间。
                                        #   注释：估算只用 LRU 页，封顶是为了别把 shrinker 带飞
      mem_cgroup_iter 扫这个 node 上的每个 memcg
        lruvec_is_reclaimable           # :4185
          低于 min：false
          不够大（lruvec_is_sizable）：false
          最老一代的出生时间 + min_ttl 已经过了：true
      min_ttl 非 0 且一个都不可回收，并且 trylock(oom_lock) 成功：
        out_of_memory                   # 保护最近 min_ttl 这段工作集，宁可 OOM
  没在切换：直接 return，不走下面的传统 inactive 老化
  切换中：接着做经典的匿名 inactive 老化（inactive_is_low）
```

`min_ttl` 来自 `lru_gen_min_ttl`（`:4207`，jiffies）。sysfs 的 `min_ttl_ms` 在 [06](./10-06-lru-vs-mglru.md)。注释（`:4229-4232`）写了两种「全都不可回收」：每一代都比 `min_ttl` 年轻，或者所有 memcg 太小 / 低于 min。两条都进同一个 `out_of_memory`。`trylock` 失败就跳过，不在 kswapd 里排队等 OOM 锁。

`lru_gen_age_node` 自己不加 `max_seq`。加序号是驱逐驱动的：窗口里的代数接近下限时，驱逐路径调用 `try_to_inc_max_seq`（`:4070`）。

```text
try_to_inc_max_seq(lruvec, seq, swappiness, force_scan)   # :4070
  没有 mm_state（没编 LRU_GEN_WALKS_MMU）：
    直接 inc_max_seq
  硬件不会自动置 accessed 位：
    iterate_mm_list_nowalk               # 退回 look-around。注释（:4089-4092）：
                                         #   把清访问位的工作摊开通常更慢，但避免突发缺页
  否则：
    set_mm_walk
    iterate_mm_list → walk_mm            # 走 lruvec 所属 memcg 的 mm_list，walk_page_range
    成功后再 inc_max_seq                 # :4005
```

`inc_max_seq`（`:4005`）在某一类已经堆满 `MAX_NR_GENS` 时，先 `inc_min_seq` 把最老的空代或该收的代推掉。然后更新 active/inactive 的 vmstat 大小：`max_seq-1` 和 `max_seq+1` 两格对一下（`:4035-4056`）。注释写明两边都要盖住，因为换出受限时 `max_seq+1` 可能和 `min_seq[LRU_GEN_ANON]` 重叠，重叠就是 cold/hot inversion。这是 MGLRU 自己的记账角落，不是经典 LRU 的行为。最后 `timestamps[next] = jiffies`，`smp_store_release` 把 `max_seq` 加一。

`lru_gen_look_around`（`:4256`）是回收走 rmap 时的反馈。注释和设计文档一致：已经拿着 PTL。清掉当前 PTE 的 young 之后，若锁没人抢、VMA 不是特殊映射，就在同一个 PMD 覆盖的相邻 PTE 里找热页，把它们提升到更年轻的代。扫得够整齐就把这个 PMD 记进 Bloom filter，下一轮页表扫描可以对准它。声明在 `mmzone.h:692`。没编 MGLRU 时 `mmzone.h:714` 的内联版本直接返回 false。2023 年底的修复 `c28ac3c7eb94` 让它跳过 `VM_PFNMAP` 一类特殊 VMA；v7.3 的函数体里对 VMA 的过滤以 `:4256` 之后的代码为准，本篇不把旧补丁的条件再抄一遍。

---

## 驱逐

全局回收进 `lru_gen_shrink_node`（`:5229`）。`VM_WARN_ON_ONCE(!root_reclaim(sc))`。注释（`:5236-5240`）：未映射的干净页已经优先过了，memcg 很多时再专门去找它们，延迟会很高，所以 `may_writepage` 或 `may_unmap` 为假就什么也不做，直接返回。

```text
lru_gen_shrink_node                         # :5229
  lru_add_drain                             # 先把 per-CPU batch 刷进去
  set_mm_walk(pgdat, sc->proactive)
  set_initial_priority
  memcg 关闭：shrink_one(&pgdat->__lruvec)
  否则：shrink_many                         # memcg 代，随机桶起步
    → 每个选中的 lruvec：try_to_shrink_lruvec 一类，最终 evict_folios

memcg 自己的回收（不是 root）：
  lru_gen_shrink_lruvec                     # :5208
    同样 lru_add_drain、set_mm_walk
    try_to_shrink_lruvec
    做完把这个 memcg 转到 YOUNG
```

`evict_folios`（`:4875`）：

```text
拿 lruvec 锁
try_to_inc_min_seq                          # 删除页之后留下的空老代先扔掉
isolate_folios                              # :4841
  get_type_to_scan                          # :4822。swappiness 极低只扫文件，极高只扫匿名
                                            #   否则 read_ctrl_pos + positive_ctrl_err
  get_tier_idx                              # 这一类里该扫的档
  scan_folios                               # :4727。从 min_seq 那代的链表上拿
    sort_folio 返回 true：这个页还不该走，挪去别的代
    否则 isolate_folio                      # :4703。folio_try_get、清 PG_lru、lru_gen_del_folio
  这一类一个都没隔离且 scanned==0：才换另一类
  注释（:4863-4866）：扫过但隔离数为 0，不要急着换类，换得太勤会打乱 positive_ctrl_err 的偏向
放锁
shrink_folio_list                           # 和经典路径同一个函数，见 02
  folio_check_references 在 MGLRU 下走 lru_gen_set_refs
没收回去的页 move 回代链表
```

`sort_folio`（`:4654`）在持锁时决定一个还在最老代链表上的 folio 去哪：

| 情况 | 动作 |
|---|---|
| 不可回收 | 返回 false。注释（`:4668`）：让它走通用路径，由那边剔除。不要在这里直接塞 unevictable。2026 年的修复说明（提交 `f7e698e326b2`）写过旧的捷径会把 `mlock_count` 污染成 `LIST_POISON2`；v7.3 的代码是「返回 false」这条 |
| gen 已经不是 `min_seq` 这一代 | `list_move` 到它现在的代。老化已经提升过它 |
| `tier > tier_idx`，或者引用位全部立满 | `folio_inc_gen`（`:3299`）升一代，记入 `protected[]`。引用位全满的那次懒晋升不计入 protected |
| `zone > sc->reclaim_idx` | 这次分配用不上这个 zone。`folio_inc_gen` 后 `list_move_tail`，先放到一边 |
| 其余 | 返回 false，调用者隔离它去 `shrink_folio_list` |

`folio_inc_gen` 清 `LRU_REFS_FLAGS`，所以文件描述符通道的计数可以重新从 `PG_referenced` 开始。这就是 `mmzone.h:507` 那句。

---

## refault

`workingset_refault`（`mm/workingset.c:548`）在 MGLRU 打开时调用 `lru_gen_refault`（`:350` 的空桩在未编配置里；编了的实现在同文件前部）后返回，不走经典的 `folio_set_active` 距离判断。

`mmzone.h:518-520`：`PG_workingset` 一旦立上，就留到这个 folio 被回收，或者被 `lru_gen_clear_refs()` 清掉。refault 时如果 `lru_gen_test_recent()` 为真，可以再次立上。shadow 里除了代序号，还带着 `LRU_REFS_WIDTH` 位的引用计数（`workingset.c:259`、`:317`）。距离用 `max_seq` 的差，小于 `MAX_NR_GENS` 算最近（`:289`）。

经典 shadow 和 `nonresident_age` 仍在，两套 LRU 共用 `workingset_*` 计数。本机有这些计数，不能用来判断 MGLRU 开没开。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `lru_gen_add_folio` | `include/linux/mm_inline.h:255`（未编时 `:330` 返回 false） | 新页或放回的页进入某一代 |
| `lru_gen_folio_seq` | `mm_inline.h:221` | 按标志计算该进哪一代 |
| `lru_gen_del_folio` | `mm_inline.h:285` | 离开分代链表 |
| `lru_gen_inc_refs` | `mm/folio.c:355`（未编时 `:399` 是空函数） | 文件描述符通道加档。没有 `folio_inc_refs` 这个符号 |
| `lru_gen_enter_fault` | `mm/memory.c:6779` | 缺页期间的新近度标记 |
| `lru_gen_exit_fault` | `mm/memory.c:6785` | 清标记 |
| `folio_update_gen` | `mm/vmscan.c:3269` | 页表遍历发现年轻 PTE 后改代 |
| `folio_inc_gen` | `vmscan.c:3299` | 驱逐时保护，升一代并清引用位 |
| `lru_gen_set_refs` | `vmscan.c:840` | rmap 驱逐路径上的第二次访问 |
| `kswapd_age_node` | `:6906` | `balance_pgdat` 的老化入口 |
| `lru_gen_age_node` | `:4209`（未编时 `:5952` 为 `BUILD_BUG()`） | kswapd 看 min_ttl，必要时 OOM |
| `try_to_inc_max_seq` | `:4070` | 页表遍历之后加 `max_seq` |
| `inc_max_seq` | `:4005` | 持锁加最年轻代，更新 vmstat |
| `lru_gen_look_around` | `:4256`（声明 `mmzone.h:692`） | rmap 上顺手提升相邻热页，喂 Bloom filter |
| `lru_gen_shrink_node` | `:5229`（未编时 `:5962` 为 `BUILD_BUG()`） | MGLRU 全局回收 |
| `lru_gen_shrink_lruvec` | `:5208`（未编时 `:5957` 为 `BUILD_BUG()`） | 单个 memcg 的 MGLRU 回收 |
| `evict_folios` | `:4875` | 一个 lruvec 上隔离、排序、交给 `shrink_folio_list` |
| `isolate_folios` | `:4841` | 选类、选档、扫最老代 |
| `sort_folio` | `:4654` | 该保护的升代，该隔离的留给调用者 |
| `get_type_to_scan` | `:4822` | PID 决定扫匿名还是文件 |
| `scan_folios` | `:4727` | 扫一代里的一个档 |
| `isolate_folio` | `:4703` | 从分代链表摘下一个 folio |
| `lru_gen_refault` | `mm/workingset.c:292`（未编时 `:350` 是空函数） | refault 时恢复档位。调用点 `:559` |

---

[上一篇 设计](./10-04-mglru-design.md) · [本目录](./10-00-reclaim.md) · [下一篇 两套对照](./10-06-lru-vs-mglru.md)
