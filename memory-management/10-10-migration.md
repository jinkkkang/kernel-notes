# 10.10 `migrate_pages`

**结论**：迁移是「隔离出一批 folio，分配目标 folio，把内容搬过去，失败的放回原链」。`migrate_pages`（`mm/migrate.c:2100`）是这条公共路径。返回没迁走的 folio 数（普通页、大页、hugetlb 都算），或者负数错误。调用者在返回非 0 时自己 `putback_movable_pages`。压缩、NUMA 策略、热插拔、回收降级都进这里。

> 导航：[上一篇 mlock](./10-09-mlock-unevictable.md) · [本目录](./10-00-reclaim.md) · [下一篇 swap table](./10-11-swap-table.md)

模式在 `include/linux/migrate_mode.h:11`：

| 模式 | 注释里的含义 |
|---|---|
| `MIGRATE_ASYNC` | 不睡眠 |
| `MIGRATE_SYNC_LIGHT` | 大部分操作可以等，但不走 `->writepage` |
| `MIGRATE_SYNC` | 迁移过程中允许睡眠 |

```text
migrate_pages(from, get_new_folio, ...)   # migrate.c:2100
  migrate_hugetlbs                        # 大页先单独迁，失败的留在 ret_folios
  again：
    按页数切一批，上限 NR_MAX_BATCHED_MIGRATION
      CONFIG_TRANSPARENT_HUGEPAGE：HPAGE_PMD_NR（:1611）
      否则 512（:1613）
    MIGRATE_ASYNC → migrate_pages_batch，重试 NR_MAX_MIGRATE_PAGES_RETRY（10，:1615）
    其它模式 → migrate_pages_sync
    被拆开的大页再以 MIGRATE_ASYNC 试一次
    from 还有剩余：回到 again
  永久失败的 folio 接回 from
  计 PGMIGRATE_SUCCESS / PGMIGRATE_FAIL / THP_MIGRATION_*
```

谁在调（理由在 `enum migrate_reason`，`migrate_mode.h:17`）：

| 调用点 | 模式 | reason |
|---|---|---|
| `compact_zone` `mm/compaction.c:2698` | `cc->mode` | `MR_COMPACTION` |
| `__alloc_contig_migrate_range` `mm/page_alloc.c:7174` | `cc->mode` | `MR_CONTIG_RANGE` |
| `do_mbind` `mm/mempolicy.c:1617` | `MIGRATE_SYNC` | `MR_MEMPOLICY_MBIND` |
| `migrate_to_node` `mempolicy.c:1322` | `MIGRATE_SYNC` | `MR_SYSCALL` |
| `migrate_misplaced_folio` `mm/migrate.c:2765` | `MIGRATE_ASYNC` | `MR_NUMA_MISPLACED` |
| `demote_folio_list` `mm/vmscan.c:1033` | `MIGRATE_ASYNC` | `MR_DEMOTION` |
| `do_migrate_range` `mm/memory_hotplug.c:1920` | `MIGRATE_SYNC` | `MR_MEMORY_HOTPLUG` |

热插拔那条在 [12.9](./12-balloon-fpr-and-overcommit.md)。NUMA 策略本身在 [03-08-numa-policy.md](./03-08-numa-policy.md)，这里只标它最后也进 `migrate_pages`。compaction 怎么把页隔离出来再调到这里，在 [03-07-high-order-and-compaction.md](./03-07-high-order-and-compaction.md)。回收降级是 `shrink_folio_list` 把 folio 放进 `demote_folios` 之后的 `demote_folio_list`（`vmscan.c:997`），见 [02](./10-02-lru-classic.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `migrate_pages` | `mm/migrate.c:2100` | 迁移一批 folio |
| `migrate_misplaced_folio` | `:2755` | NUMA balancing 迁一页。调用点 `:2765` 传入 `MIGRATE_ASYNC` |
| `demote_folio_list` | `mm/vmscan.c:997` | 回收降级到更慢的节点。`migrate_pages` 调用在 `:1033` |
| `compact_zone` | `mm/compaction.c:2562` | 压缩。迁页调用在 `:2698` |
| `do_mbind` | `mm/mempolicy.c:1489` | 策略迁移。`migrate_pages` 在 `:1617` |

---

[上一篇 mlock](./10-09-mlock-unevictable.md) · [本目录](./10-00-reclaim.md) · [下一篇 swap table](./10-11-swap-table.md)
