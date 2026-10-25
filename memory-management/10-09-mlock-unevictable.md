# 10.9 unevictable 与 mlock

**结论**：mlock 住的 folio 不进 inactive/active 的扫描。`PG_unevictable` 表示这个状态，`PG_mlocked` 表示至少有一次 mlock，`folio->mlock_count` 记下还有多少次。`enum lru_list` 里有 `LRU_UNEVICTABLE`，`lruvec->lists[]` 也为它留了表头，但 `lruvec_add_folio` / `lruvec_del_folio` 不把 folio 串进这张表。计数走 `update_lru_size`。MGLRU 的 `lru_gen_add_folio` 对 unevictable 直接返回 false，锁住的页不进分代链表。

> 系统调用在 `mm/mlock.c`。`mlock_vma_folio` 在 `mm/internal.h:673`。
> 导航：[上一篇 OOM](./10-08-oom.md) · [本目录](./10-00-reclaim.md) · [下一篇 迁移](./10-10-migration.md)

---

## 链表和计数各管什么

`mlock.c:50-58` 的注释：

> An mlocked folio is unevictable. As such, it will be ostensibly placed on the LRU "unevictable" list (actually no such list exists), rather than the [in]active lists. PG_unevictable is set to indicate the unevictable state.

和代码的对应关系：

- `enum lru_list` 有 `LRU_UNEVICTABLE`（`include/linux/mmzone.h:270`）。`folio_lru_list`（`mm_inline.h:92`）在 `PG_unevictable` 立着时返回它。
- `lruvec_add_folio`（`mm_inline.h:318`）照样 `update_lru_size`，所以 `/proc/vmstat` 一类的 unevictable 页数会计上。接着（`:327-328`）：`lru != LRU_UNEVICTABLE` 才 `list_add`。unevictable 的 folio 不挂到 `lists[LRU_UNEVICTABLE]` 上。
- `lruvec_del_folio`（`:346`）同样：不是 unevictable 才 `list_del`（`:353-354`），然后把计数减掉。
- `folio_mark_accessed` 的注释（`mm/swap.c:461-463`）写：unevictable 页在 `"LRU_UNEVICTABLE"` 上，但这条链从不旋转、也不维护，所以标记访问没有效果。

所以「表头在数组里」和「folio 不串成链表」是同一套实现。回收扫描用 `for_each_evictable_lru`（`mmzone.h:284`），走到 `LRU_ACTIVE_FILE` 就停。

`mlock_count` 在 folio 上，不在链表节点上：

| 函数 | `mlock_count` |
|---|---|
| `__mlock_folio`（`mlock.c:61`）第一次变成 unevictable | 设成 `!!folio_test_mlocked`，再 `lruvec_add_folio` |
| 已经 unevictable 且仍 `PG_mlocked` | `mlock_count++` |
| `__munlock_folio`（`:122`） | 先减。减到 0 且 `folio_evictable` 为真：清 unevictable，放回普通 LRU |
| `lru_add_fn`（`mm/swap.c:176`）发现不可回收 | 设成 0。注释（`:180-185`）写少算是安全的，让回收去补，不要把页永远留在 unevictable 上 |

少算会把页留在 unevictable 上，直到回收发现它其实可以收。多算也会。`mlock_vma_pages_range` 的注释（`:378-381`）写了并发迁移或回收可能再调 `mlock_vma_folio`，把 `mlock_count` 加重复，页就一直不可回收。这段扫描期间临时加上 `VM_IO`（`VM_SPECIAL` 的一种），`mlock_vma_folio` 看到 `VM_SPECIAL` 就不再加计数。扫完把 `VM_IO` 清掉。

统计看 vmstat 的 `unevictable_pgs_culled` / `unevictable_pgs_rescued` / `unevictable_pgs_munlocked` / `unevictable_pgs_stranded`。对应事件名是 `UNEVICTABLE_PGCULLED`、`UNEVICTABLE_PGRESCUED`、`UNEVICTABLE_PGMUNLOCKED`、`UNEVICTABLE_PGSTRANDED`（`__mlock_folio` / `__munlock_folio` 里的 `__count_vm_events`）。

`lru_gen_add_folio`（`mm_inline.h:231`）对 `folio_test_unevictable` 返回 false。锁住的页留在经典计数这条路上，不进 `lrugen->folios[]`。

---

## 系统调用

```text
mlock(start, len)                         # SYSCALL mm/mlock.c:622
  do_mlock                                # :575
    can_do_mlock 否则 -EPERM              # :40。RLIMIT_MEMLOCK 非 0，或有 CAP_IPC_LOCK
    长度按页对齐
    加上 mm->locked_vm，超过 rlimit 且没有 CAP_IPC_LOCK：
      扣掉区间里已经 mlock 的页（count_mm_mlocked_page_nr，:604）再比一次
    apply_vma_lock_flags                  # :477，mmap 写锁下改 VMA
      mlock_fixup                         # :412
        mlock_vma_pages_range             # :369，走页表，已在的页进 mlock batch
    mmap_write_unlock
    __mm_populate                         # :616，把还没缺页的地址补上

mlock2(start, len, MLOCK_ONFAULT)         # :627
  多设 VM_LOCKONFAULT（:634，定义 mm.h:300）
  同样进 do_mlock
  populate_vma_page_range 看到 VM_LOCKONFAULT（gup.c:1567）
    直接返回页数，不在这里 fault-in。页留到以后真正缺页时再锁

munlock                                   # :640，apply_vma_lock_flags 的 flags 是 0，清掉 VM_LOCKED
```

`mlock_fixup`（`:422`）对 `VM_SPECIAL`、hugetlb、gate、DAX、secretmem 直接跳过，不设 `VM_LOCKED` / `VM_LOCKONFAULT`，也不把页数算进 `locked_vm`。

已经映射的页由 `mlock_pte_range`（`:308`）调用 `mlock_folio`（`:243`），排进 per-CPU 的 `mlock_fbatch`。`mlock_folio_batch`（`:186`）里，folio 指针的低两位是标志，不是地址：`LRU_FOLIO`（`0x1`，`:167`）走 `__mlock_folio`，`NEW_FOLIO`（`0x2`，`:168`）走 `__mlock_new_folio`（`:103`），都没有就 `__munlock_folio`。

`mlock_vma_folio`（`mm/internal.h:673`）是 inline。注释要求调用时拿着 VMA 的 mmap 锁，并且拿着正在安装或拆掉的 pte/pmd 的页表锁。它在 `(VM_LOCKED|VM_SPECIAL) == VM_LOCKED` 时调用 `mlock_folio`。大页只在 `compound` 为真时计，避免 THP 的 pte 映射重复计数。rmap 在 `page_add_*_rmap` 末尾调它（例如 `mm/rmap.c:821`）。

`folio_add_lru_vma`（`mm/swap.c:522`）在 VMA 带 `VM_LOCKED` 且不带 `VM_SPECIAL` 时直接 `mlock_new_folio`（`mlock.c:268`）。缺页回来的新页不必等下一次 mlock 扫描。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `mlock` | `mm/mlock.c:622` | 系统调用 |
| `mlock2` | `:627` | 可以带 `MLOCK_ONFAULT` |
| `munlock` | `:640` | 清 `VM_LOCKED` |
| `do_mlock` | `:575` | 检查 rlimit，改 VMA，再 populate |
| `can_do_mlock` | `:40` | rlimit 或 `CAP_IPC_LOCK` |
| `mlock_fixup` | `:412` | 跳过特殊 VMA，合并或拆分，再锁页 |
| `mlock_vma_pages_range` | `:369` | 走页表。期间临时加 `VM_IO` |
| `mlock_pte_range` | `:308` | 已映射的页调用 `mlock_folio` / `munlock_folio` |
| `mlock_folio` | `:243` | 已在 LRU 上的页排进 mlock batch |
| `mlock_new_folio` | `:268` | 还没上 LRU 的新页 |
| `munlock_folio` | `:291` | 排进同一 batch，低位不带标志 |
| `mlock_folio_batch` | `:186` | 按低两位分发 |
| `__mlock_folio` | `:61` | 设 unevictable，维护 `mlock_count` |
| `__mlock_new_folio` | `:103` | 新页直接 `lruvec_add_folio` |
| `__munlock_folio` | `:122` | 计数到 0 且可回收时放回普通 LRU |
| `mlock_vma_folio` | `mm/internal.h:673` | rmap 安装 pte 时调用 |
| `folio_lru_list` | `include/linux/mm_inline.h:86` | unevictable 返回 `LRU_UNEVICTABLE` |
| `lruvec_add_folio` | `mm_inline.h:318` | unevictable 只改计数，不 `list_add` |
| `lruvec_del_folio` | `mm_inline.h:346` | unevictable 只改计数，不 `list_del` |
| `folio_add_lru_vma` | `mm/swap.c:522` | `VM_LOCKED` 且非 `VM_SPECIAL` 时 `mlock_new_folio` |
| `populate_vma_page_range` | `mm/gup.c:1548` | `VM_LOCKONFAULT` 时不 fault-in |
| `lru_gen_add_folio` | `include/linux/mm_inline.h:220` | unevictable 返回 false |

---

[上一篇 OOM](./10-08-oom.md) · [本目录](./10-00-reclaim.md) · [下一篇 迁移](./10-10-migration.md)
