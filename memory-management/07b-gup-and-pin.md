# 7b. GUP 与 pin

**结论**：内核要按用户虚拟地址拿到 `struct page` 并在页表之外持有一段时间，走 `get_user_pages*` 或 `pin_user_pages*`。两族都会在页还没在页表里时进入 [07](./07-page-fault.md) 的 `handle_mm_fault`。差别在引用怎么记、怎么还。`get_*` 还 `put_page`。`pin_*` 在内部置上 `FOLL_PIN`，还 `unpin_user_page*`。公开的 `FOLL_*` 枚举里没有 `FOLL_PIN`。

> 源码：Linux v6.6.160。旗标在 `include/linux/mm_types.h`、`mm/internal.h`。实现在 `mm/gup.c`。纪律说明在 `Documentation/core-api/pin_user_pages.rst`。
> 对照：[elixir v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)。
> 导航：[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 页缓存](./08-page-cache-and-writeback.md)

本篇写 API、旗标、寿命和调用方交界。匿名/文件缺页的分派留在 07。`migrate_pages` 的批处理留在 [10-10](./10-10-migration.md)。EPT/SPTE 留在 [11](./11-kvm-memory.md)。bio 怎么提交留在 [block-io/11](../block-io/11-direct-io.md)。

---

## 7b.1 内核怎样把用户页拿在手里

用户自己碰一个还没有 PTE 的地址，从架构异常进 `handle_mm_fault`。那次缺页结束时，页在这个进程的页表里，引用记在页表和 folio 的普通账上。进程不再映射它，页就可以回到回收或 buddy。细节在 07。

另一条是内核主动来拿。Direct IO、KVM 把 HVA 填进 EPT、RDMA 注册内存、io_uring 注册缓冲，调用方手里是用户虚拟地址，要的是 `struct page`，而且在这次缺页返回之后还要拿着。这条入口在 `mm/gup.c`。页不在页表里时，慢路径的 `faultin_page`（`mm/gup.c:829`）仍然调用 `handle_mm_fault`（`:868`）。`FOLL_WRITE` 变成 `FAULT_FLAG_WRITE`，`FOLL_REMOTE` 变成 `FAULT_FLAG_REMOTE`。`FOLL_NOFAULT` 在这里直接 `-EFAULT`（`:836`），不去缺页。

```text
用户虚拟地址
  用户访问、PTE 不在     → handle_mm_fault          页留在页表里（07）
  内核要 struct page     → get_user_pages* / pin_user_pages*
       已映射：follow_page_mask                    gup.c:721
       未映射：faultin_page → handle_mm_fault      仍是 07 的缺页
       返回后：调用方自己持有引用，直到 put 或 unpin
```

`get_user_pages_remote` 的 kerneldoc 标题写着 “pin user pages in memory”（`gup.c:2199`）。同一段注释要求调用方用 `put_page` 释放（`:2219`）。读代码时以函数名前缀为准：`get_*` 配对 `put_page`，`pin_*` 配对 `unpin_user_page*`。

---

## 7b.2 两族 API：拿和还必须成对

| 获取 | 定义 | 释放 |
|---|---|---|
| `get_user_pages` | `mm/gup.c:2295` | `put_page`（`include/linux/mm.h:1551`） |
| `get_user_pages_remote` | `:2254`（`CONFIG_MMU`） | `put_page` |
| `get_user_pages_unlocked` | `:2323` | `put_page` |
| `get_user_pages_fast` | `:3294` | `put_page` |
| `get_user_pages_fast_only` | `:3260` | `put_page`。失败不回落慢路径 |
| `pin_user_pages` | `:3394` | `unpin_user_page` / `unpin_user_pages*` |
| `pin_user_pages_remote` | `:3360` | 同上 |
| `pin_user_pages_unlocked` | `:3414` | 同上 |
| `pin_user_pages_fast` | `:3328` | 同上 |

`get_user_pages` 假定 `current->mm`，内部或上 `FOLL_TOUCH`（`:2300`）。`get_user_pages_remote` 再或上 `FOLL_REMOTE`（`:2262`）。`get_user_pages_unlocked` 或上 `FOLL_TOUCH | FOLL_UNLOCKABLE`（`:2329`），自己拿、放 `mmap_lock`。三条都进 `__get_user_pages_locked`（`gup.c:1391`），不经过下面的 longterm 包装。

`pin_user_pages*` 在参数检查里或上 `FOLL_PIN`，然后进 `__gup_longterm_locked`（`:2116`）。`pin_user_pages` 只或 `FOLL_PIN`（`:3399`），kerneldoc 写明它不置 `FOLL_TOUCH`（`:3385`）。`pin_user_pages_remote` 是 `FOLL_PIN | FOLL_TOUCH | FOLL_REMOTE`（`:3368`）。`pin_user_pages_unlocked` 是 `FOLL_PIN | FOLL_TOUCH | FOLL_UNLOCKABLE`（`:3420`）。

返回值：`nr_pages > 0` 且一页都没拿到时，慢路径返回负的 errno（`__get_user_pages_locked` 的注释，`:1388`）。拿到的页数可以小于请求数，调用方只释放真正返回的那些。参数非法时 `is_valid_gup_args`（`:2151`）失败：`get_user_pages*` 和 `pin_user_pages_fast` 返回 `-EINVAL`；`pin_user_pages`、`_remote`、`_unlocked` 返回 `0`（`:3400`、`:3369`、`:3421`）。这三条返回 0 的路径先走了 `WARN_ON_ONCE`，表示 `gup_flags` 非法。

零页：`try_grab_folio` 看见 `is_zero_folio` 就返回 0，不改引用、不改 pincount（`:155`）。`gup_put_folio` 对零页直接返回（`:103`）。`unpin_user_page` 对零页是空操作。kerneldoc 在 `pin_user_pages_fast`（`:3325`）写了这一点。

`Documentation/core-api/pin_user_pages.rst` 把调用分成五类，这里只留选择，不抄正文：

| 场景 | 用哪一族 | 旗标 |
|---|---|---|
| 只碰 `struct page`，不写页里的数据 | `get_user_pages*` | 要页数组时内部会补 `FOLL_GET` |
| Direct IO，持有时间短，要访问页数据 | `pin_user_pages*` | 内部 `FOLL_PIN` |
| RDMA 一类，持有时间不定 | `pin_user_pages*` | 调用者再或 `FOLL_LONGTERM` |
| 注册了 mmu notifier，失效回调里会停 DMA 并放开页 | `get_user_pages*` | 文档写这两面旗标都不必加 |
| 其余会写页里数据的路径 | `pin_user_pages*` | 内部 `FOLL_PIN` |

同一次调用里 `FOLL_GET` 和 `FOLL_PIN` 同时置位，`is_valid_gup_args` 拒绝（`:2176`）。同一页上可以既有 get 引用又有 pin，那是多次调用叠上去的。

`FOLL_PIN` 且没有 `FOLL_WRITE` 时，`gup_must_unshare`（`mm/internal.h:1063`）要求把还在共享的匿名页拆开，避免钉住的人看见别人 COW 的结果。文件页只在「只读的长期 pin + 私有可写映射」上走这条。`FOLL_WRITE` 的 pin 靠 PTE 可写和匿名页的 exclusive 标记，这条函数直接返回 false（`:1071`）。fork 侧用 `mm->write_protect_seq`（字段在 [06](./06-mm-and-vma.md)）和 `MMF_HAS_PINNED`：`page_needs_cow_for_dma`（`mm.h:1964`）在这个 mm 曾经 pin 过时，复制页表会提前拆 COW。快路径怎么对这个序号，见 7b.6。

---

## 7b.3 公开 `FOLL_*`

调用者能组合的旗标在 `include/linux/mm_types.h`。头上的说明是 `:1350–1394`：`FOLL_GET` 用 `get_user_pages*` 拿、`put_page` 还；`FOLL_PIN` 用 `pin_user_pages*` 拿、`unpin_user_page` 还；`FOLL_PIN` 由 `pin_user_pages*` 内部置位。枚举本身从 `:1396` 开始，成员到 `FOLL_HONOR_NUMA_FAULT` 结束。

```c
/* include/linux/mm_types.h:1396 */
enum {
	FOLL_WRITE = 1 << 0,             /* PTE 要可写 */
	FOLL_GET = 1 << 1,               /* 普通引用：folio_ref 加 1 */
	FOLL_DUMP = 1 << 2,              /* 洞若会填零页则报错，core dump 留洞 */
	FOLL_FORCE = 1 << 3,             /* 越过 VMA 的读写许可。ptrace 用 */
	FOLL_NOWAIT = 1 << 4,            /* 需要磁盘 IO 时启动后不等 */
	FOLL_NOFAULT = 1 << 5,           /* 不缺页 */
	FOLL_HWPOISON = 1 << 6,          /* 认出硬件坏页 */
	FOLL_ANON = 1 << 7,              /* 拒绝文件映射 */
	FOLL_LONGTERM = 1 << 8,          /* 持有时间不定，常在用户态控制下 */
	FOLL_SPLIT_PMD = 1 << 9,         /* 返回前拆开大页 PMD */
	FOLL_PCI_P2PDMA = 1 << 10,       /* 允许 PCI P2PDMA 页 */
	FOLL_INTERRUPTIBLE = 1 << 11,    /* 普通信号可打断这次缺页 */
	FOLL_HONOR_NUMA_FAULT = 1 << 12, /* 主动走 NUMA hint fault */

	/* 内部旗标见 mm/internal.h，从 1 << 16 起 */
};
```

`FOLL_LONGTERM` 的源码注释（`:1417`）把它和 `iov_iter_get_pages()` 的短持有对过。6.6 里对应的符号是 `iov_iter_get_pages2`（`lib/iov_iter.c:1159`），见 7b.7。

`FOLL_HONOR_NUMA_FAULT` 的注释（`:1429`）：`FOLL_WRITE` 已经会撞上 `PROT_NONE` 的 hint（`FOLL_FORCE` 另算）；`get_user_pages_fast_only` 也总是认 hint。KVM 慢路径显式或上这一位，见 7b.7。hint 缺页进 `do_numa_page`，见 [07](./07-page-fault.md)。

`__get_user_pages_locked` 还有一条旧习惯（`:1426`）：调用者传了 `pages` 数组、又没有 `FOLL_PIN`，函数自己补 `FOLL_GET`。所以 `get_user_pages` 的调用方可以不写 `FOLL_GET`。`FOLL_PIN` 的路径不走这个补位。

---

## 7b.4 内部旗标：`FOLL_PIN` 不在公开枚举里

```c
/* mm/internal.h:1022 */
enum {
	FOLL_TOUCH = 1 << 16,         /* 标 accessed */
	FOLL_TRIED = 1 << 17,         /* 上一轮已发起 IO，这是重试 */
	FOLL_REMOTE = 1 << 18,        /* mm 属于别的任务 */
	FOLL_PIN = 1 << 19,           /* 释放走 unpin_user_page* */
	FOLL_FAST_ONLY = 1 << 20,     /* 快路径失败即停 */
	FOLL_UNLOCKABLE = 1 << 21,    /* 允许放下 mmap_lock */
	FOLL_MADV_POPULATE = 1 << 22, /* 按 MADV_POPULATE 做 VMA 检查 */
};

/* :1039 */
#define INTERNAL_GUP_FLAGS (FOLL_TOUCH | FOLL_TRIED | FOLL_REMOTE | FOLL_PIN | \
			    FOLL_FAST_ONLY | FOLL_UNLOCKABLE | \
			    FOLL_MADV_POPULATE)
```

`is_valid_gup_args`（`gup.c:2151`）是导出接口的门。`gup_flags` 里带任何 `INTERNAL_GUP_FLAGS`，`WARN_ON_ONCE` 后返回 false（`:2163`）。调用者把 `FOLL_PIN` 写进自己的参数，过不了这道门。置位发生在包装函数把 `to_set` 或进去之后（`:2166`）。

同一函数还卡住这几条：

| 条件 | 位置 | 结果 |
|---|---|---|
| `FOLL_GET` 与 `FOLL_PIN` 同时在 | `:2176` | 拒绝 |
| 有 `FOLL_LONGTERM`、没有 `FOLL_PIN` | `:2181` | 拒绝。所以 `get_*` 传 `FOLL_LONGTERM` 过不了 |
| `FOLL_GET` 或 `FOLL_PIN` 但 `pages == NULL` | `:2185` | 拒绝 |
| `FOLL_LONGTERM` 与 `FOLL_PCI_P2PDMA` 同时在 | `:2190` | 拒绝 |

`FOLL_LONGTERM` 本身在公开枚举里，调用者可以或到 `pin_user_pages*` 的 `gup_flags` 上。`FOLL_PIN` 仍然只由包装函数补。

引用怎么加，看 `try_grab_folio`（`gup.c:139`，声明 `mm/internal.h:1012`）。`FOLL_GET`：`folio_ref_add(folio, refs)`。`FOLL_PIN` 在大 folio（`folio_test_large`）上：refcount 加 `refs`，`_pincount` 再加 `refs`（`:164`）。order-0：refcount 加 `refs * GUP_PIN_COUNTING_BIAS`（`:166`）。然后 `NR_FOLL_PIN_ACQUIRED` 加上这次的逻辑 pin 数（`:169`）。

```c
/* include/linux/mm.h:1594 */
#define GUP_PIN_COUNTING_BIAS (1U << 10)
```

1024 这个偏置把「普通 +1 引用」和「一次 pin」摊在同一个 `_refcount` 上：低位留给普通引用，每多一次 pin 跳 1024。页上普通引用多到超过 1024 时，目测会把普通引用看成一次 pin。`folio_maybe_dma_pinned`（`mm.h:1936`）把这写成：小 folio 上 false 表示确定没有 DMA pin，true 表示很可能有，也可能是普通引用堆过了偏置。大 folio 改读 `_pincount`，结果是准的。

`_pincount` 在 folio 第二页槽，顺序是 `_entire_mapcount`、`_nr_pages_mapped`、`_pincount`（`mm_types.h:331–333`）。order-0 用不到这槽，改走上面的偏置。refcount 和 mapcount 的其余规则在 [02 §2.10](./02-physical-layout.md#210-folio-的-refcount-与-mapcount)。

还的时候 `gup_put_folio`（`gup.c:100`）带着 `FOLL_PIN`：大 folio 从 `_pincount` 减去 `refs`；小 folio 把 `refs` 乘上 `GUP_PIN_COUNTING_BIAS` 再 `folio_put_refs`。同时记 `NR_FOLL_PIN_RELEASED`（`:105`）。

导出的释放函数：

| 符号 | 声明 | 定义 | 做什么 |
|---|---|---|---|
| `unpin_user_page` | `mm.h:1596` | `gup.c:184` | 一页，`gup_put_folio(..., FOLL_PIN)` |
| `unpin_user_pages_dirty_lock` | `:1597` | `:270` | 先按需标脏再放。`make_dirty == false` 时转去 `unpin_user_pages` |
| `unpin_user_page_range_dirty_lock` | `:1599` | `:336` | 连续页范围，同样可选标脏 |
| `unpin_user_pages` | `:1601` | `:381` | 数组。`npages` 是负的 errno 时 `WARN` 后直接返回（`:392`） |

`unpin_user_pages_dirty_lock` 的注释把 “gup-pinned” 写成 get 或 pin 都算（`:254`）。实现只走 `FOLL_PIN` 的 `gup_put_folio`（`:310`）。从 `get_user_pages*` 拿到的页用它来放，pin 计数和普通引用会对不上。配对按函数族，不按这句注释的措辞。

---

## 7b.5 `FOLL_LONGTERM`：先迁走不能长期钉的页

长期 pin 钉在 CMA、`ZONE_MOVABLE` 上，会把这段连续内存切碎。`folio_is_longterm_pinnable`（`mm.h:1999`，要 `CONFIG_MIGRATION`）拒绝这几类：

| 页 | 结果 |
|---|---|
| `MIGRATE_CMA` 或 `MIGRATE_ISOLATE`（编了 `CONFIG_CMA`） | 不可长期钉 |
| 零页 | 允许。计数仍按 7b.2 的零页特例 |
| coherent device 页 | 不可长期钉，必须能被迁走 |
| `folio_is_zone_movable` | 不可长期钉 |
| 其余 | 允许 |

CMA 区平时怎么借给可移动分配，在 [03-09](./03-09-cma.md)。这里只接「长期 pin 之前要把借用者迁走」。

```text
pin_user_pages*  →  __gup_longterm_locked          gup.c:2116
  没有 FOLL_LONGTERM：直接 __get_user_pages_locked
  有：
    memalloc_pin_save                            sched/mm.h:383
      current 置 PF_MEMALLOC_PIN                 sched.h:1776
      current_gfp_context 去掉 __GFP_MOVABLE     sched/mm.h:249
    循环：
      __get_user_pages_locked
      check_and_migrate_movable_pages            :2090
        都允许：返回 0，页保持 pin
        有不允许的：迁完返回 -EAGAIN，整段重来
    memalloc_pin_restore                         sched/mm.h:391
```

`collect_longterm_unpinnable_pages`（`:1949`）把不可钉的 folio 收进链表。普通页先 `folio_isolate_lru`。`migrate_longterm_unpinnable_pages`（`:2008`）在迁移前 `unpin_user_page`（`:2043`），因为页还带着 pin 时迁不动。然后：

```text
migrate_pages(movable_page_list, alloc_migration_target,
              NULL, &mtc, MIGRATE_SYNC, MR_LONGTERM_PIN, NULL)
    调用从 gup.c:2053 起，MR_LONGTERM_PIN 在 :2055
```

`MR_LONGTERM_PIN` 和 `MIGRATE_SYNC` 的含义、以及 `migrate_pages` 的分批和重试，在 [10-10](./10-10-migration.md)。`migrate_pages` 返回非 0（还有没迁走的 folio）时，这里收成 `-ENOMEM`（`:2056`），已经 unpin 的页在错误路径上再清一遍，调用方应把错误交回去。返回 `-EAGAIN` 表示这轮迁完了，`__gup_longterm_locked` 的循环整段重新 pin（`:2142`）。

进缺页之前，`check_vma_flags`（`:942`）还有两道长期 pin 的拒绝：

- `FOLL_LONGTERM` 且 `vma_is_fsdax`：`-EOPNOTSUPP`（`:955`）。头文件注释（`mm_types.h:1358`）写 FS DAX 的 VMA 会直接失败。`pin_user_pages.rst` 给出的原因是：DAX 页没有单独的页缓存，长期 pin 会把文件系统的块锁死。
- 写文件映射、又是 `FOLL_PIN | FOLL_LONGTERM`、且 VMA 要 dirty tracking：`writable_file_mapping_allowed`（`:924`）不放行，`check_vma_flags` 返回 `-EFAULT`（`:964`）。注释写的失败形态是：GUP 经内核线性映射改了页，文件系统的写保护通知看不到。

`VM_IO | VM_PFNMAP` 在 `:949` 直接 `-EFAULT`。secretmem 同样 `-EFAULT`（`:958`）。这两条对 get 和 pin 都生效。

快路径上，`try_grab_folio_fast` 遇到 `FOLL_LONGTERM` 且 `folio_is_longterm_pinnable` 为假，放下刚才的引用并返回 NULL（`gup.c:2527`），让调用方落到慢路径去做迁移。

---

## 7b.6 快路径：关中断走页表，失败再回落

`get_user_pages_fast` 和 `pin_user_pages_fast` 都进 `internal_get_user_pages_fast`（`gup.c:3189`）。差别只是进门时或上的是 `FOLL_GET`（`:3303`）还是 `FOLL_PIN`（`:3331`）。

```text
internal_get_user_pages_fast                 :3189
  FOLL_PIN：mm_set_has_pinned_flag           :3206
  lockless_pages_from_mm                     :3140
    FOLL_PIN 且 write_protect_seq 为奇数：返回 0     fork 正在写保护
    local_irq_save
    gup_pgd_range                            锁外走页表。本篇不展开每一级
    local_irq_restore
    FOLL_PIN 且序号变了：unpin_user_pages_lockless，返回 0
  已经拿满，或带 FOLL_FAST_ONLY：返回
  否则剩下的页：
    __gup_longterm_locked(..., FOLL_TOUCH | FOLL_UNLOCKABLE)
```

关中断是为了挡住页表页被释放，以及 THP 拆分时发出的 IPI。文件头上的假设写在 `:2338` 起：架构要能原子读 PTE，`access_ok` 足够检查用户地址，并且 TLB 刷新会用 IPI，或者页表页走 `MMU_GATHER_RCU_TABLE_FREE`。哪一级 PTE 怎么读，留在这段注释和 `gup_pgd_range` 里。

`get_user_pages_fast_only` 额外或上 `FOLL_FAST_ONLY`（`:3271`）。快路径拿不到就返回已拿到的数量（可以是 0），不拿 `mmap_lock`。`get_user_page_fast_only`（`mm.h:2607`）是它的单页包装，返回值是「拿到了 1 页」。

快路径的 pin 计数在 `try_grab_folio_fast`（`gup.c:2492`），必须在关中断里调用（`:2498`）。小 folio 先 `try_get_folio` 加 `refs`，再补 `refs * (GUP_PIN_COUNTING_BIAS - 1)`（`:2546`），加总仍是偏置的整数倍。大 folio 在已有的 refcount 之外 `atomic_add` 到 `_pincount`（`:2543`）。

`MMF_HAS_PINNED`（`include/linux/sched/coredump.h:83`，位号 27）由 `mm_set_has_pinned_flag`（`gup.c:408`）置上。注释写：置上之后这个 mm 的一生都留着，后面的 unpin 不清这个位。慢路径在 `__get_user_pages_locked` 看见 `FOLL_PIN` 时置（`:1414`），快路径在进锁外走页表前置（`:3206`）。soft-dirty 清位时用它跳过可能被 DMA pin 的 PTE，见 [13](./13-observability.md) 里的 `pte_is_pinned`。

---

## 7b.7 调用方只写到交界

| 调用方 | 入口 | 拿页 | 放页 |
|---|---|---|---|
| 块层 Direct IO | `bio_iov_iter_get_pages` `block/bio.c:1331` | 用户迭代器走 `iov_iter_extract_pages` | `BIO_PAGE_PINNED` 时 `unpin_user_page` |
| `iov_iter_get_pages2` | `lib/iov_iter.c:1159` | 用户地址走 `get_user_pages_fast`（`:1126`） | `put_page` 这一族 |
| `iov_iter_extract_pages` | `lib/iov_iter.c:1836` | 用户地址走 `pin_user_pages_fast`（`:1785`） | `unpin_user_page*` |
| KVM HVA → PFN | `hva_to_pfn` `virt/kvm/kvm_main.c:2695` | 慢路径 `get_user_pages_unlocked`（`:2573`） | `kvm_release_page_clean` → `put_page`（`:2952`） |
| io_uring 注册缓冲 | `io_pin_pages` `io_uring/io_uring.c:2715` | `pin_user_pages_fast`，`FOLL_WRITE \| FOLL_LONGTERM`（`:2736`） | 用完在 `io_buffer_unmap` 里 `unpin_user_page`（`io_uring/rsrc.c:142`） |
| RDMA `ib_umem_get` | `drivers/infiniband/core/umem.c:164` | `pin_user_pages_fast`，旗标含 `FOLL_LONGTERM`（`:176`、`:235`） | `unpin_user_page_range_dirty_lock`（`:59`） |

Direct IO 的 bio 路径在 [block-io/11](../block-io/11-direct-io.md)。这里只对上 pin。`bio_iov_iter_get_pages` 看见 bvec 迭代器就 `bio_iov_bvec_set`（`bio.c:1338`），不再拿一次页。io_uring 固定缓冲已经是这种 bvec，固定缓冲本身的 pin 发生在注册时，见 [block-io/12](../block-io/12-io-uring.md)。用户 iovec 则进 `__bio_iov_iter_get_pages`（`:1239`），里面调用 `iov_iter_extract_pages`（`:1269`）。`iov_iter_extract_will_pin`（`include/linux/uio.h:418`）对用户迭代器返回 true，bio 置上 `BIO_PAGE_PINNED`（`include/linux/blk_types.h:334`，置位在 `bio.c:1345`）。完成时 `bio_release_page`（`block/blk.h:468`）看见这个旗标就 `unpin_user_page`。

`iov_iter_get_pages2` 走的是另一族。`__iov_iter_get_pages_alloc`（`iov_iter.c:1098`）在 `iov_iter_rw(i) != WRITE` 时或上 `FOLL_WRITE`（条件 `:1115`，旗标 `:1116`），然后 `get_user_pages_fast`。`iov_iter_extract_pages` 的用户分支 `iov_iter_extract_user_pages`（`:1760`）在 `data_source == ITER_DEST` 时或上 `FOLL_WRITE`（条件 `:1772`，旗标 `:1773`），然后 `pin_user_pages_fast`。`ITER_DEST` 就是读进用户缓冲的方向（`uio.h:33`）。两处条件是同一件事的两种写法：内核要把数据写进用户页，GUP 才要求 PTE 可写。

KVM 的页表怎么建在 [11](./11-kvm-memory.md)。交界只有这一段：`hva_to_pfn_fast`（`kvm_main.c:2515`）用 `get_user_page_fast_only`（`:2528`）。慢路径 `hva_to_pfn_slow`（`:2543`）的旗标是 `FOLL_HWPOISON | FOLL_HONOR_NUMA_FAULT`（`:2557`），写故障再或 `FOLL_WRITE`，然后 `get_user_pages_unlocked`。释放是 `put_page`，走的是 get 族。

io_uring 和 RDMA 还会改 `mm->pinned_vm`。`io_account_mem`（`io_uring/rsrc.c:70`）在 `mm_account` 非空时 `atomic64_add`（`:81`），注销时 `io_unaccount_mem` 再减（`:67`）。注册中途失败由 `io_pin_pages`（`io_uring.c:2748`）和 `io_sqe_buffer_register`（`rsrc.c:932`）把已经拿到的页 unpin。`ib_umem_get` 在 pin 之前 `atomic64_add_return`（`umem.c:221`），超过 `RLIMIT_MEMLOCK` 且调用者没有 `CAP_IPC_LOCK` 就减回去并返回 `-ENOMEM`。v6.6.160 的 `mm/gup.c` 不写 `pinned_vm`。Direct IO 的短 pin 不带 `FOLL_LONGTERM`，也不进这个计数。

mlock 的 fault-in 也在 `gup.c`，但是另一条。`populate_vma_page_range`（`:1548`）用 `FOLL_TOUCH`，可写私有映射再或 `FOLL_WRITE`，`pages` 传 NULL（`:1593`）。它不置 `FOLL_PIN`。`VM_LOCKONFAULT` 直接返回页数（`:1567`）。`locked_vm` / `VmLck` 在 [10-09](./10-09-mlock-unevictable.md)。`account_locked_vm`（`mm/util.c:544`）改的是 `locked_vm`。

---

## 7b.8 怎么看钉住了

三本账不要合成一个数。

| 你读到的 | v6.6.160 从哪来 | 数的是什么 |
|---|---|---|
| `/proc/<pid>/status` 的 `VmPin` | `fs/proc/task_mmu.c:63`，`atomic64_read(&mm->pinned_vm)`，再按页换成 kB | 长期持有者自己加的页数。字段在 [06](./06-mm-and-vma.md)，`mm_types.h:859`，注释是 “Refcount permanently increased” |
| `smaps` / `smaps_rollup` 的 `Locked` | `__show_smap`（`task_mmu.c:813`）打到 `:848` | mlock 的 PSS。见 10-09 |
| `/proc/vmstat` 的 `nr_foll_pin_acquired` / `nr_foll_pin_released` | 枚举 `NR_FOLL_PIN_ACQUIRED` / `NR_FOLL_PIN_RELEASED`（`include/linux/mmzone.h:197`），名字在 `mm/vmstat.c:1233` | 开机以来 `FOLL_PIN` 的逻辑次数。大页按子页计。说明在 `pin_user_pages.rst` 的 “Unit testing” |

`smaps` 的字段名 `Pinned` 在 v6.6.160 的 `fs/proc/task_mmu.c` 里查不到。`__show_smap` 的最后一项是 `Locked`。和 `mm->pinned_vm` 对得上的是 `status` 里的 `VmPin`。

长期 pin 失败时常见的返回：FS DAX 是 `-EOPNOTSUPP`；要 dirty tracking 的可写文件映射是 `-EFAULT`；CMA / movable 上的页迁不走是 `-ENOMEM`（7b.5）。迁的过程中这批页会先 unpin 再迁，成功则整段重 pin。这段时间里页可以换位置，调用方在 `pin_user_pages*` 返回之后才拿到最终的 `struct page`。

### RECIPE

只读 proc。不要为了看数字去写一个 pin 用户页的模块。

```bash
grep -E '^(VmLck|VmPin):' /proc/<pid>/status
awk '/^[0-9a-f]+-/ {n++} n==1 {print} n==1 && /^VmFlags/ {exit}' /proc/<pid>/smaps
grep -E 'nr_foll_pin_' /proc/vmstat
```

对照：

1. `VmPin` 对 `mm->pinned_vm`。单位是 kB，计数本身是页。io_uring 固定缓冲或 RDMA umem 还在时，这里应大于 0。一次普通 `O_DIRECT` 读写用的是短 pin，不写这个字段。
2. `smaps` 里找 `Locked`。它跟 mlock。同一份输出里没有 `Pinned:` 这一行，是 v6.6.160 的 `__show_smap` 就这样。
3. `nr_foll_pin_acquired` 和 `nr_foll_pin_released`。没有长期 pin、也没有正在飞的短 pin 时，两者相等。`pin_user_pages.rst` 写：长期 RDMA pin 还在，或者正处于 pin/unpin 的窗口，两者可以不等。

回看已经写过的路径，不必再跑危险样例：

- 块设备 direct 的钉页点：[`bio_iov_iter_get_pages`](../block-io/11-direct-io.md)
- io_uring 固定缓冲注册之后，提交时迭代器已是 bvec：[`12`](../block-io/12-io-uring.md)
- KVM 用的是 get 族：[`hva_to_pfn`](./11-kvm-memory.md)

本机快照（**sampled on 6.12**，`uname` 为 `6.12.94+`）读的是一个短命进程的 `/proc/self`：`VmLck` 0 kB，`VmPin` 0 kB，第一段 `smaps` 有 `Locked: 0 kB`，没有 `Pinned` 这一行；`nr_foll_pin_acquired` 和 `nr_foll_pin_released` 都是 0。6.12 这台机器的 `smaps` 同样没有 `Pinned`。两个 0 只说明当时没有 pin 记账，字段名以 v6.6.160 源码为准。

---

## 7b.9 边界

| 本篇写了 | 留在别处 |
|---|---|
| get 与 pin 的 API、旗标、释放、`GUP_PIN_COUNTING_BIAS` | 缺页主路径（07）、folio 引用的其余持有者（[02 §2.10](./02-physical-layout.md#210-folio-的-refcount-与-mapcount)） |
| `FOLL_LONGTERM` 为什么调用 `migrate_pages(..., MR_LONGTERM_PIN)` | `migrate_pages` 的分批、重试、单页拷贝（10-10） |
| DIO / KVM / io_uring / RDMA 从哪一个 GUP 函数进来 | bio、blk-mq（block-io）；EPT/SPTE（11） |
| `VmPin`、`nr_foll_pin_*`、smaps 里没有 `Pinned` | mlock 的 `VmLck`（10-09） |
| `FOLL_HONOR_NUMA_FAULT` 这一位，以及 KVM 慢路径会或上它 | 扫描把 PTE 改成 hint、再决定迁页的闭环 |
| — | IOMMU、VFIO 的 DMA 映射；HMM 的 `hmm_range`（11 到 range API 为止） |

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| 公开 `FOLL_*` | `include/linux/mm_types.h:1396` | 调用者能组合的旗标。枚举里没有 `FOLL_PIN` |
| get 与 pin 的头注释 | `mm_types.h:1350` | `put_page` 与 `unpin_user_page*` |
| `FOLL_PIN` 等内部旗标 | `mm/internal.h:1022` | `INTERNAL_GUP_FLAGS` 在 `:1039` |
| `is_valid_gup_args` | `mm/gup.c:2151` | 拒绝调用者手写的内部旗标 |
| `get_user_pages` | `mm/gup.c:2295` | 当前 mm，`FOLL_TOUCH` |
| `get_user_pages_remote` | `:2254` | 指定 mm，再加 `FOLL_REMOTE` |
| `get_user_pages_unlocked` | `:2323` | 自己拿 `mmap_lock` |
| `get_user_pages_fast` | `:3294` | 快路径，内部补 `FOLL_GET` |
| `get_user_pages_fast_only` | `:3260` | 再加 `FOLL_FAST_ONLY` |
| `pin_user_pages` | `:3394` | 内部 `FOLL_PIN`，不置 `FOLL_TOUCH` |
| `pin_user_pages_remote` | `:3360` | `FOLL_PIN \| FOLL_TOUCH \| FOLL_REMOTE` |
| `pin_user_pages_unlocked` | `:3414` | `FOLL_PIN \| FOLL_TOUCH \| FOLL_UNLOCKABLE` |
| `pin_user_pages_fast` | `:3328` | 快路径的 pin |
| `__gup_longterm_locked` | `:2116` | `FOLL_LONGTERM` 时迁走不可钉的页 |
| `migrate_pages` 的长期 pin 调用 | `:2053`（`MR_LONGTERM_PIN` 在 `:2055`） | `MIGRATE_SYNC`，`MR_LONGTERM_PIN` |
| `try_grab_folio` | `:139` | 慢路径按 GET 或 PIN 加引用 |
| `try_grab_folio_fast` | `:2492` | 快路径，关中断 |
| `gup_put_folio` | `:100` | PIN 时按偏置或 `_pincount` 退回 |
| `unpin_user_page` | 声明 `mm.h:1596`，定义 `gup.c:184` | pin 的单页释放 |
| `unpin_user_pages` | `mm.h:1601`，`gup.c:381` | pin 的数组释放 |
| `GUP_PIN_COUNTING_BIAS` | `mm.h:1594` | order-0 的 pin 步长，`1U << 10` |
| `folio_maybe_dma_pinned` | `mm.h:1936` | 大 folio 看 `_pincount`，小 folio 看偏置 |
| `folio_is_longterm_pinnable` | `mm.h:1999` | CMA、isolate、movable、device coherent |
| `faultin_page` | `gup.c:829` | 转到 `handle_mm_fault` |
| `bio_iov_iter_get_pages` | `block/bio.c:1331` | DIO 从迭代器钉页 |
| `iov_iter_get_pages2` | `lib/iov_iter.c:1159` | 经典 GUP，`get_user_pages_fast` |
| `iov_iter_extract_pages` | `lib/iov_iter.c:1836` | 用户页走 `pin_user_pages_fast` |
| `pinned_vm` / `VmPin` | `mm_types.h:859`，`fs/proc/task_mmu.c:63` | 长期持有者自己加的页数。`gup.c` 不写它 |
| `NR_FOLL_PIN_ACQUIRED` | `mmzone.h:197`，`vmstat.c:1233` | `/proc/vmstat` 的 `nr_foll_pin_acquired` |

---

[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 页缓存](./08-page-cache-and-writeback.md)
