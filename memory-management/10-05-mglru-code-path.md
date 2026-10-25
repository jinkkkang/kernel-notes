# 10.5 MGLRU 代码路径

**结论**：页进来时 `lru_gen_add_folio` 按热度放进某一代。文件描述符上的访问只调 `folio_inc_refs` 加档，不搬链表。老化由 `try_to_inc_max_seq` 走页表，把年轻 PTE 提升到 `max_seq`。驱逐从最老一代里按 PID 选出类型和档，`sort_folio` 把不该走的页挪走，剩下的交给同一个 `shrink_folio_list`。`min_ttl_ms` 保不住时，kswapd 直接进 `out_of_memory`。

> 函数都在 `mm/vmscan.c` 的 `CONFIG_LRU_GEN` 段，从 `:3227` 起。入代和加档在 `include/linux/mm_inline.h` 与 `mm/swap.c`。
> 导航：[上一篇 设计](./10-04-mglru-design.md) · [本目录](./10-00-reclaim.md) · [下一篇 对照](./10-06-lru-vs-mglru.md)

没编 `CONFIG_LRU_GEN` 时，这一节的老化、驱逐函数是空函数，调用点被 `lru_gen_enabled()` 的常量 false 裁掉。见 [01](./10-01-overview.md)。

---

## 入代

`folio_add_lru`（`mm/swap.c:493`）在 MGLRU 开着、folio 可回收、当前在带新近度的缺页里、且不是 `PF_MEMALLOC` 时，先 `folio_set_active`（`:502-504`）。注释指向 `lru_gen_add_folio`。

batch 刷下去时 `lruvec_add_folio`（`mm_inline.h:318`）先试 `lru_gen_add_folio`（`:220`）。成功就 `list_add` 到 `lrugen->folios[gen][type][zone]` 的头，并清掉 `PG_active`，把 gen+1 写进 `folio->flags`。四种选代规则在 [04](./10-04-mglru-design.md)。`reclaiming == true` 时改 `list_add_tail`，给 `folio_rotate_reclaimable` 用。

`lrugen->enabled` 为假，或 folio 不可回收，函数返回 false，页回到 `lruvec->lists[]`。

---

## 访问：加档，不搬代

`folio_mark_accessed`（`mm/swap.c:450`）在 `lru_gen_enabled()` 时只调 `folio_inc_refs`（`:407`）然后返回。

`folio_inc_refs` 的顺序：

```text
unevictable：直接返回
还没 PG_referenced：置上，返回          # 第一档
还没 PG_workingset：置上，返回          # 第二档
否则：LRU_REFS 位加 1，到顶就停         # 中间档。见 MAX_NR_TIERS 的注释
```

这几步都是 `folio->flags` 上的原子操作，不拿 `lru_lock`。

页表通道不走这个函数。老化或 look-around 发现年轻 PTE 时调 `folio_update_gen`（`vmscan.c:3763`）：folio 还在分代链表上，就把 gen 改成目标代，并清掉 refs 位；如果 LRU 位已经被摘掉（`LRU_GEN_MASK` 为 0），只置 `PG_referenced`，留给 `shrink_folio_list`（`:1757` 的注释：`folio_update_gen()` 想提升这一页）。

多次经文件描述符访问、又被 PID 判成值得保护的页，由 `folio_inc_gen`（`:3786`）在 LRU 锁下挪到 `min_seq+1`。`reclaiming` 为真时同时置 `PG_reclaim`，给写回结束用。

---

## 老化

kswapd 在 `balance_pgdat` 里先调 `kswapd_age_node`（`:7182`，调用点 `:7518`）。MGLRU 开着时它只转去 `lru_gen_age_node`（`:4614`）然后返回。经典路径才会在匿名 inactive 太短时 `shrink_active_list`。

```text
kswapd_age_node
  lru_gen_age_node                         # :4614，只允许 kswapd 进来
    set_initial_priority                   # :4539，按可回收页估算起始 priority
    遍历每个 memcg 的 lruvec
      低于 min：跳过
      最老一代的出生时间 + min_ttl 已过：这组还算可回收
    所有组都保不住，且抢到 oom_lock：
      out_of_memory                        # 见 10-08，以及下面的 min_ttl

驱逐侧也会老化，不等 kswapd：
  get_nr_to_scan                           # :5336
    should_run_aging                      # :5264
      冷页已经不够（min_seq + MIN_NR_GENS > max_seq）：该老化
      代数已经比 MIN_NR_GENS+1 多：先不老化
      年轻页超过 1/MIN_NR_GENS，或老页少于 1/(MIN_NR_GENS+2)：该老化
    priority 还是 DEF_PRIORITY：跳过老化，只返回扫描量
    否则 try_to_inc_max_seq                # :4485
```

`try_to_inc_max_seq`：

```text
这一轮的 mm_state.seq 已经追上 max_seq：不再走
硬件不会自动置 accessed，或分配不到 walk：
  iterate_mm_list_nowalk                  # :3641，只把 mm_state.seq 加一
否则：
  set_mm_walk                            # :4317，kswapd 用 pgdat->mm_walk，别人临时分配
  walk->can_swap / force_scan 从参数填入
  iterate_mm_list + walk_mm              # :3573 / :4272
    walk_page_range 扫 PTE
    年轻 PTE：folio_update_gen 提升到 max_seq 那一格
成功：inc_max_seq                        # :4428
```

`inc_max_seq` 在锁里做三件事：窗口已经有 `MAX_NR_GENS` 代时先 `inc_min_seq`（`:4349`）把最老一代整体挪到下一格；按 active/inactive 的 ABI 改 `nr_pages` 统计；`timestamps[next] = jiffies`，然后 `max_seq++`。

注释（`:4452-4456`）写了 cold/hot inversion：`max_seq+1` 可能和 `min_seq[LRU_GEN_ANON]` 重叠（swap 受限制时），重叠的话冷热会反号。所以改统计时，当前 `max_seq` 两侧都要算进去。这是分代链表自己的记账，不是经典双链表的行为。

`should_walk_mmu`（`:3237`）要求 `arch_has_hw_pte_young()` 并且 `LRU_GEN_MM_WALK` 这位开着。硬件不置 accessed 位时，老化退回 `lru_gen_look_around`，一次只清少数 PTE，避免突发的缺页（`:4502-4506`）。

---

## look-around

`lru_gen_look_around`（`:4661`）。注释（`:4654-4659`）：`shrink_folio_list` 走 rmap 时，如果碰到年轻 PTE，就扫相邻 PTE，把热页提升；扫得对 cache line 友好，就把指向这张 PTE 表的 PMD 放进 Bloom filter。驱逐和老化由此接成环。

它要求已经拿着页表锁。锁竞争、或者 VMA 带 `VM_SPECIAL`，直接返回。扫描范围夹在同一个 PMD 里，超过 `MIN_LRU_BATCH` 页就截短。rmap 侧的调用点在 `mm/rmap.c:830`（条件从 `:828` 起）。

---

## 驱逐

全局回收：

```text
shrink_node
  lru_gen_enabled 且 root_reclaim：
    lru_gen_shrink_node                  # :5570
      may_writepage 或 may_unmap 为假：只更新 kswapd_failures，不扫
      set_mm_walk(pgdat)
      memcg 关掉：shrink_one(&pgdat->__lruvec)
      否则：shrink_many                  # :5465
        从老代的随机桶头开始
        shrink_one                       # :5422
          低于 min：MEMCG_LRU_YOUNG
          低于 low 且还没在 tail：MEMCG_LRU_TAIL
          try_to_shrink_lruvec
          shrink_slab
          成功且 memcg 还在线：MEMCG_LRU_YOUNG
```

memcg 自己的回收不走 `shrink_many`。`shrink_lruvec`（`:6297`）在非 root 且 MGLRU 开着时调 `lru_gen_shrink_lruvec`（`:5535`），里面直接 `try_to_shrink_lruvec`。

```text
try_to_shrink_lruvec                     # :5387
  get_swappiness                         # :3290
    may_swap 为假，或没有 swap 又不能降级：返回 0
    有 __GFP_IO 限制时，非 0 的 swappiness 改成 1   # 先扫文件
  循环：
    get_nr_to_scan                       # 可能在这里老化，返回 -1 表示该轮转 memcg
    evict_folios                         # :5175
    扫够了，或 should_abort_scan：停
```

`should_abort_scan`（`:5356`）：memcg 回收不提前停，为了公平。全局回收收够了 `nr_to_reclaim` 就停。kswapd 且 order 为 0 时，所有合格 zone 都高于 HIGH（开了内存分层则是 `WMARK_PROMO`）再加 `MIN_LRU_BATCH`，也可以停。

`evict_folios`：

```text
isolate_folios                           # :5133
  swappiness 0：只看文件
  min_seq[anon] < min_seq[file]：先匿名（匿名更老）
  swappiness 1：同一代时先文件
  swappiness 200：同一代时先匿名
  其余：get_type_to_scan                 # :5105，PID
  scan_folios                            # :5009
    sort_folio 返回 true 的页留在分代链表上
    isolate_folio 成功的放进待回收链表
try_to_inc_min_seq                       # 空代直接把 min_seq 往前推
shrink_folio_list                        # 和经典路径同一个函数
没成功的：脏且 PG_reclaim 的留在原代；其余打上 PG_active 再放回
干净、没被拒绝的再试一轮
move_folios_to_lru
```

`sort_folio`（`:4912`）在 LRU 锁下决定这一页还要不要被隔离：

| 条件 | 动作 |
|---|---|
| 不可回收 | 放行，交给通用路径剔除 |
| 文件类型上的脏匿名页（lazyfree） | 设回 swapbacked，丢到链尾 |
| gen 已经不是最老 | 挪到它自己那一代，不算驱逐 |
| tier 高于保护线，或 refs 满了 | `folio_inc_gen`，计入 `protected` |
| zone 高于 `reclaim_idx` | `folio_inc_gen`，这轮不收 |
| 锁着、写回中，或脏文件页 | `folio_inc_gen(..., reclaiming=true)`，等写回 |

`get_tier_idx`（`:5085`）用 1:2 的 gain 比较第一档和更高档。注释：留一点波动余量，因为其它档的 refault 至少是第一档的两倍，才值得保护。

---

## `min_ttl`

变量是 `lru_gen_min_ttl`（`:4612`），单位 jiffies。sysfs `min_ttl_ms` 用毫秒读写（`:5754` / `:5760`）。

`lruvec_is_reclaimable`（`:4590`）读文件侧最老一代的 `timestamps`，出生时间加上 `min_ttl` 还在现在之后，这组就还受保护。`lru_gen_age_node` 发现每个 memcg 要么太小、要么低于 min、要么最老一代还太年轻，就 `out_of_memory`（`:4639`）。设计文档 Working set protection（`multigen_lru.rst:145-152`）写的是同一件事：保护 `lru_gen_min_ttl` 毫秒内的工作集，保不住就 OOM。默认 0，等于关掉。管理文档里 `N=1000` / `N=3000` 的说法见 [06](./10-06-lru-vs-mglru.md)，标 RECIPE。

---

## refault

`workingset_refault`（`mm/workingset.c:537`）在 MGLRU 开着时转去 `lru_gen_refault`（`:279`）。

```text
lru_gen_test_recent                      # :262
  shadow 里的 min_seq 还等于现在这一类的 min_seq：算近
记 WORKINGSET_REFAULT
不够近：返回
够近：
  按 shadow 里的 refs 算 tier，refaulted[hist][type][tier] += 页数
  记 WORKINGSET_ACTIVATE
  当前在缺页里，或 refs 已经到顶：
    置满 LRU_REFS 并置 PG_workingset
    记 WORKINGSET_RESTORE
```

「近」比的是 shadow 里记下的 `min_seq` 和现在的 `min_seq` 是否还是同一代（`:276`），不是经典路径的 `nonresident_age` 距离。

赶走时 `workingset_eviction`（`:381`）转去 `lru_gen_eviction`（`:231`）：把当时的 `min_seq` 和 refs 打进 shadow，并给 `evicted[]` 加页数。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `lru_gen_add_folio` | `include/linux/mm_inline.h:220` | 入代 |
| `folio_inc_refs` | `mm/swap.c:407` | 文件描述符通道加档 |
| `folio_update_gen` | `mm/vmscan.c:3763` | 页表通道提升代 |
| `folio_inc_gen` | `:3786` | 保护或推迟时挪到下一代 |
| `lru_gen_age_node` | `:4614` | kswapd 的工作集保护和 min_ttl OOM |
| `set_initial_priority` | `:4539` | 按可回收页估算 priority |
| `should_run_aging` | `:5264` | 冷页不够或分布太偏时该老化 |
| `get_nr_to_scan` | `:5336` | 扫描量；必要时老化并返回 -1 |
| `try_to_inc_max_seq` | `:4485` | 走一遍 mm 链表，成功则 `max_seq++` |
| `inc_max_seq` | `:4428` | 窗口满了先推 `min_seq`，再加 `max_seq` |
| `inc_min_seq` | `:4349` | 最老一代整体挪到下一格 |
| `try_to_inc_min_seq` | `:4385` | 空代直接把 `min_seq` 向前推 |
| `iterate_mm_list` | `:3573` | 从 memcg 的 mm 链表取下一个 mm |
| `iterate_mm_list_nowalk` | `:3641` | 硬件不扫页表时只推进 `mm_state.seq` |
| `walk_mm` | `:4272` | 对一个 mm 走页表 |
| `set_mm_walk` | `:4317` | 挂上 `reclaim_state->mm_walk` |
| `clear_mm_walk` | `:4336` | 卸下，非 kswapd 的 walk 要释放 |
| `lru_gen_look_around` | `:4661` | rmap 上扫相邻 PTE，热点 PMD 进 Bloom |
| `should_walk_mmu` | `:3237` | 硬件置 accessed 且 `LRU_GEN_MM_WALK` 开着 |
| `should_clear_pmd_young` | `:3242` | 非叶子 accessed 位 |
| `lru_gen_shrink_node` | `:5570` | 全局回收入口 |
| `shrink_many` | `:5465` | 按 memcg 代扫。无 memcg 时 `:5558` 是 `BUILD_BUG()` |
| `shrink_one` | `:5422` | 一个 memcg：保护、驱逐、shrinker |
| `lru_gen_shrink_lruvec` | `:5535` | memcg 自己的回收。无 memcg 时 `:5563` 是 `BUILD_BUG()` |
| `try_to_shrink_lruvec` | `:5387` | 老化加驱逐的循环 |
| `should_abort_scan` | `:5356` | 全局回收可以提前停 |
| `get_swappiness` | `:3290` | 这次能不能换出、该用多大的 swappiness |
| `evict_folios` | `:5175` | 隔离、`shrink_folio_list`、放回 |
| `isolate_folios` | `:5133` | 先选类型，再 `scan_folios` |
| `get_type_to_scan` | `:5105` | PID 选匿名还是文件，以及保护到哪一档 |
| `get_tier_idx` | `:5085` | 同一类里保护到哪一档 |
| `scan_folios` | `:5009` | 从最老一代挑 folio |
| `sort_folio` | `:4912` | 不该走的页挪代或推迟 |
| `isolate_folio` | `:4975` | 真正摘下来 |
| `lru_gen_rotate_memcg` | `:4782` | 按 HEAD/TAIL/OLD/YOUNG 挪 memcg |
| `lru_gen_refault` | `mm/workingset.c:279` | MGLRU 的 refault |
| `lru_gen_test_recent` | `workingset.c:262` | shadow 里的 `min_seq` 是否还是当前代 |
| `lru_gen_eviction` | `workingset.c:231` | 赶走时打包 shadow 并计 `evicted` |
| `out_of_memory` | `mm/oom_kill.c:1113` | `min_ttl` 保不住时由 kswapd 调用 |

---

[上一篇 设计](./10-04-mglru-design.md) · [本目录](./10-00-reclaim.md) · [下一篇 经典 LRU 与 MGLRU 对照](./10-06-lru-vs-mglru.md)
