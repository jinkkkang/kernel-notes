# 10.9 unevictable LRU 与 mlock

**结论**：mlock 住的 folio 不进 inactive/active 的扫描。`folio_set_unevictable` 之后，`folio_lru_list`（`include/linux/mm_inline.h:82`）返回 `LRU_UNEVICTABLE`（`mmzone.h:394`），`lruvec_add_folio` 把它放进 `lruvec->lists[LRU_UNEVICTABLE]`。回收看这个标志，不从这条链上偷页。MGLRU 的 `lru_gen_add_folio` 对 unevictable 直接返回 false，所以锁住的页仍在这条经典链上，不进分代链表。

> 系统调用在 `mm/mlock.c`。
> 导航：[上一篇 OOM](./08-oom.md) · [本目录](./README.md) · [下一篇 迁移](./10-migration.md)

`mlock.c:55-58` 的注释写「unevictable 链表其实不存在」。v7.3 的 `folio_lru_list` 和 `enum lru_list` 里这条链是有的。以这两个定义为准，那句注释和当前链表对不上。

---

## 系统调用

```text
mlock(start, len)                         # SYSCALL mm/mlock.c:667
  do_mlock                                # :619
    can_do_mlock 否则 -EPERM
    长度按页对齐
    RLIMIT_MEMLOCK（页数）。超了且没有 CAP_IPC_LOCK：
      扣掉区间里已经 mlock 的页（count_mm_mlocked_page_nr，:578）再比一次
    apply_vma_lock_flags                  # :519，mmap 写锁下改 VMA
      mlock_fixup                         # :467
        mlock_vma_pages_range             # :423，走页表，已在的页进 mlock batch
    mmap_write_unlock
    __mm_populate                         # :661，把还没缺页的地址补上
      populate_vma_page_range             # mm/gup.c:1813

mlock2(start, len, MLOCK_ONFAULT)         # :674
  多设 VMA_LOCKONFAULT_BIT（:682）
  同样进 do_mlock
  populate_vma_page_range 看到 VM_LOCKONFAULT（mm.h:425，判断在 gup.c:1832）
    直接返回页数，不在这里 fault-in。页留到以后真正缺页时再锁

munlock                                   # :687，flags 清空 VMA_LOCKED_MASK
```

已经映射的页进 `mlock_folio_batch`（`:186`）。folio 指针的低两位是标志，不是地址：`LRU_FOLIO`（`0x1`）走 `__mlock_folio`（`:61`），`NEW_FOLIO`（`0x2`）走 `__mlock_new_folio`（`:103`），都没有就 `__munlock_folio`（`:122`）。

`__mlock_folio` 拿 lruvec 锁。folio 不可驱逐时：从当前链摘下，`folio_set_unevictable`，`mlock_count` 记成「是否已经 PG_mlocked」，再 `lruvec_add_folio` 放回 unevictable 链，计 `UNEVICTABLE_PGCULLED`。已经在这条链上且仍 mlocked，只把 `mlock_count++`。`__munlock_folio` 把 `mlock_count` 减到 0 且 `folio_evictable` 为真时，清掉 unevictable，放回普通 LRU。

`mlock_count` 少算会把页留在 unevictable 上。`mlock_fixup` 的注释（`:432-436`）写了并发迁移或回收可能重复加计数。统计看 vmstat 的 `unevictable_pgs_culled` / `unevictable_pgs_rescued` / `unevictable_pgs_mlocked`。

secretmem 和 `vma_supports_mlock` 为假的 VMA 在 `mlock_fixup`（`:476`）里直接跳过，不设 `VMA_LOCKED_BIT`。

`folio_add_lru_vma`（`mm/folio.c:501`）在 VMA 带 `VM_LOCKED` 且不带 `VM_SPECIAL` 时直接 `mlock_new_folio`，缺页回来的新页不必等下一次 mlock 扫描。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `do_mlock` | `mm/mlock.c:619` | mlock 的公共实现 |
| `mlock` | `:667` | 系统调用 |
| `mlock2` | `:674` | onfault 锁 |
| `munlock` | `:687` | 解锁 |
| `mlock_folio_batch` | `:186` | 批量把 folio 挪到 unevictable |
| `__mlock_folio` | `:61` | 已在 LRU 上的页 |
| `__mlock_new_folio` | `:103` | 新页 |
| `__munlock_folio` | `:122` | 减 `mlock_count`，到 0 放回普通 LRU |
| `folio_lru_list` | `include/linux/mm_inline.h:82` | unevictable 时返回 `LRU_UNEVICTABLE` |

---

[上一篇 OOM](./08-oom.md) · [本目录](./README.md) · [下一篇 迁移](./10-migration.md)
