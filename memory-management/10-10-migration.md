# 10.10 `migrate_pages`

**结论**：迁移是「隔离出一批 folio，分配目标 folio，把内容搬过去，失败的放回原链」。`migrate_pages`（`mm/migrate.c:1903`）是这条公共路径。返回没迁走的 folio 数（普通页、大页、hugetlb 都算），或者负数错误。调用者在返回非 0 时自己 `putback_movable_pages`。压缩、NUMA 策略、热插拔、回收降级、长期 pin、软下线都进这里。

> 文档：`Documentation/mm/page_migration.rst`。用户态接口是 `mbind` 的 `MF_MOVE` / `MF_MOVE_ALL`、`migrate_pages` 系统调用、以及 `move_pages`。
> 导航：[上一篇 mlock](./10-09-mlock-unevictable.md) · [本目录](./10-00-reclaim.md) · [swap 槽](./10-07-swap-zswap.md)

模式在 `include/linux/migrate_mode.h:15`：

| 模式 | 注释里的含义 |
|---|---|
| `MIGRATE_ASYNC` | 不睡眠 |
| `MIGRATE_SYNC_LIGHT` | 大部分操作可以等，但不走 `->writepage` |
| `MIGRATE_SYNC` | 迁移过程中允许睡眠 |
| `MIGRATE_SYNC_NO_COPY` | 允许睡眠，但 CPU 不在 `migratepage` 回调里拷贝。拷贝在回调外面做，多半用 DMA。使用者是 `migrate_vma()` 和 HMM |

```c
int migrate_pages(struct list_head *from, new_folio_t get_new_folio,
		free_folio_t put_new_folio, unsigned long private,
		enum migrate_mode mode, int reason, unsigned int *ret_succeeded);
```

`reason` 是 `enum migrate_reason`（`migrate_mode.h:22`）：`MR_COMPACTION`、`MR_MEMORY_FAILURE`、`MR_MEMORY_HOTPLUG`、`MR_SYSCALL`（注释写也用于 cpuset）、`MR_MEMPOLICY_MBIND`、`MR_NUMA_MISPLACED`、`MR_CONTIG_RANGE`、`MR_LONGTERM_PIN`、`MR_DEMOTION`。

```text
migrate_pages(from, get_new_folio, ...)   # migrate.c:1903
  migrate_hugetlbs                        # 大页先单独迁，失败的留在 ret_folios
  again：
    按页数切一批，上限 NR_MAX_BATCHED_MIGRATION
      CONFIG_TRANSPARENT_HUGEPAGE：HPAGE_PMD_NR（:1490）
      否则 512（:1492）
    MIGRATE_ASYNC → migrate_pages_batch，重试 NR_MAX_MIGRATE_PAGES_RETRY（10，:1494）
    其它模式 → migrate_pages_sync
    被拆开的大页再以 MIGRATE_ASYNC 试一次
    from 还有剩余：回到 again
  永久失败的 folio 接回 from
  计 PGMIGRATE_SUCCESS / PGMIGRATE_FAIL / THP_MIGRATION_*
  *ret_succeeded = 成功的页数（调用者传了非空指针时）
```

函数注释（`:1893-1901`）写：试完 `NR_MAX_MIGRATE_PAGES_RETRY` 次，或者链表空了、没有还能重试的 folio，就返回。返回非 0 时由调用者 `putback_movable_pages`。返回值把没迁走的普通页、大页、hugetlb 都算进去；大页拆开之后，不管拆出来的页迁走了多少，这一次大页失败仍算 1。链表最后空了则返回 0（`:1981-1982`）。

单页拷贝的入口是 `migrate_folio`（`:688`），多一个参数的是 `migrate_folio_extra`（`:657`）。目标页由调用者给的 `get_new_folio` 分配。通用的分配函数是 `alloc_migration_target`（`:1999`）：按 `migration_target_control` 里的 gfp、nid、nodemask 分配，nid 是 `NUMA_NO_NODE` 时用源 folio 的节点。

谁在调：

| 调用点 | 模式 | reason |
|---|---|---|
| `compact_zone` 的迁移，`mm/compaction.c:2515` | `cc->mode` | `MR_COMPACTION` |
| `__alloc_contig_migrate_range` `mm/page_alloc.c:6156` | `cc->mode` | `MR_CONTIG_RANGE` |
| `do_mbind` 里的迁移 `mm/mempolicy.c:1344` | `MIGRATE_SYNC` | `MR_MEMPOLICY_MBIND` |
| `migrate_to_node` `mempolicy.c:1087` | `MIGRATE_SYNC` | `MR_SYSCALL` |
| `migrate_misplaced_page` `mm/migrate.c:2595` | `MIGRATE_ASYNC` | `MR_NUMA_MISPLACED` |
| `demote_folio_list` `mm/vmscan.c:1684` | `MIGRATE_ASYNC` | `MR_DEMOTION` |
| `do_migrate_range` `mm/memory_hotplug.c:1794` | `MIGRATE_SYNC` | `MR_MEMORY_HOTPLUG` |
| 长期 pin `mm/gup.c:2053` | `MIGRATE_SYNC` | `MR_LONGTERM_PIN` |
| 软下线 `mm/memory-failure.c:2742` | `MIGRATE_SYNC` | `MR_MEMORY_FAILURE` |

热插拔那条在 [12.9](./12-balloon-fpr-and-overcommit.md)。NUMA 策略本身在 [03-08-numa-policy.md](./03-08-numa-policy.md)，这里只标它最后也进 `migrate_pages`。compaction 怎么把页隔离出来再调到这里，在 [03-07-high-order-and-compaction.md](./03-07-high-order-and-compaction.md)。回收降级是 `shrink_folio_list` 把 folio 放进 `demote_folios` 之后的 `demote_folio_list`（`vmscan.c:1656`），分配函数是 `alloc_demote_folio`（`:1621`），见 [02](./10-02-lru-classic.md)。

`migrate_misplaced_page`（`:2565`）是 NUMA balancing 迁一页的入口。文件页被多个进程以可执行方式映射，或者文件页是脏的，就直接放弃（`:2579-2588`）。注释写脏文件页在 `MIGRATE_ASYNC` 下不是每个文件系统都能搬。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `migrate_pages` | `mm/migrate.c:1903` | 迁移一批 folio |
| `migrate_pages_batch` | `:1615` | `MIGRATE_ASYNC` 的一批 |
| `migrate_pages_sync` | `:1826` | 允许睡眠的模式 |
| `migrate_folio` | `:688` | 迁一个 folio |
| `migrate_folio_extra` | `:657` | 同上，多一个模式参数 |
| `alloc_migration_target` | `:1999` | 按 `migration_target_control` 分配目标 |
| `migrate_misplaced_page` | `:2565` | NUMA balancing 迁一页。`migrate_pages` 在 `:2595` |
| `demote_folio_list` | `mm/vmscan.c:1656` | 回收降级。`migrate_pages` 在 `:1684` |
| `alloc_demote_folio` | `vmscan.c:1621` | 降级的目标分配，优先目标节点 |
| `compact_zone` 的迁移调用 | `mm/compaction.c:2515` | `MR_COMPACTION` |
| `migrate_to_node` 的迁移调用 | `mm/mempolicy.c:1087` | `MR_SYSCALL` |
| `do_mbind` 的迁移调用 | `mempolicy.c:1344` | `MR_MEMPOLICY_MBIND` |
| `do_migrate_range` | `mm/memory_hotplug.c:1704` | 热插拔。`migrate_pages` 在 `:1794` |

---

[上一篇 mlock](./10-09-mlock-unevictable.md) · [本目录](./10-00-reclaim.md) · [swap 槽](./10-07-swap-zswap.md)
