# 8. 页缓存与回写（内存侧）

**结论**：文件页的「在不在内存里」由 `address_space->i_pages` 这棵 xarray 回答，里面挂的是 folio。读路径把 folio 从磁盘拉进来；写路径把 folio 标脏。脏页在回写完成前不能被回收当成空闲页。回收因此经常卡在写回上，而不是卡在 LRU 算法本身。

> 结构：`include/linux/fs.h:471`。查找和缺页：`mm/filemap.c`。脏页限速：`mm/page-writeback.c`。
> 具体文件系统怎么实现 `a_ops->read_folio` / `writepages`，留给以后的 vfs 章。
> 导航：[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 THP](./09-thp-and-hugetlb.md)

---

## 8.1 `struct address_space` 全部成员（`fs.h:471`）

每个 inode 有一个（`inode->i_mapping`），也有不挂 inode 的（swap cache 等）。

| 成员 | 中文 |
|---|---|
| `struct inode *host` | 属主 inode。没有 inode 的 mapping（例如某些伪文件）为 NULL |
| `struct xarray i_pages` | **页缓存本体**。index → folio。以前是 radix tree，现在是 xarray。锁是 xarray 自己的 |
| `struct rw_semaphore invalidate_lock` | 截断、punch hole、失效页缓存时的锁。和 `i_pages` 的锁不是同一把 |
| `gfp_t gfp_mask` | 给这个文件分配 folio 用的 GFP。通常含 `GFP_HIGHUSER_MOVABLE`，文件系统可以去掉 `__GFP_FS` |
| `atomic_t i_mmap_writable` | 多少个 `VM_SHARED` + `VM_MAYWRITE` 的映射。非 0 表示有人能把文件写脏 |
| `struct rb_root_cached i_mmap` | 映射了这个文件的 VMA 区间树。VMA 侧的挂钩是 `vma->shared.rb`（见 [06](./06-mm-and-vma.md)） |
| `unsigned long nrpages` | 页缓存里的页数。由 `i_pages` 的锁保护 |
| `pgoff_t writeback_index` | 循环回写的起点，避免每次都从文件头写 |
| `const struct address_space_operations *a_ops` | 文件系统的方法。内存侧只依赖下面几个 |
| `unsigned long flags` | `AS_*`：错误、回收中、unevictable 等 |
| `errseq_t wb_err` | 最近一次回写错误。`fsync` 用它判断要不要向用户报告 |
| `spinlock_t i_private_lock` | 属主私用，内核通用代码不解释它 |
| `struct rw_semaphore i_mmap_rwsem` | 保护 `i_mmap` 和 `i_mmap_writable` |

结构按 `sizeof(long)` 对齐（`:485`）。原因：folio 的 `mapping` 指针最低位被拿来做 `FOLIO_MAPPING_ANON`。地址不对齐，匿名标记会和真实指针撞车。

xarray 的标记（`:493`）：

| 标记 | 含义 |
|---|---|
| `PAGECACHE_TAG_DIRTY`（`XA_MARK_0`） | 脏 |
| `PAGECACHE_TAG_WRITEBACK`（`XA_MARK_1`） | 正在回写 |
| `PAGECACHE_TAG_TOWRITE`（`XA_MARK_2`） | 这一轮回写要写的 |

`mapping_tagged()`（`:500`）问这棵树上有没有某个标记。回写和回收都用它，不自己扫全部 folio。

### `address_space_operations` 里和内存有关的成员（`fs.h:401`）

整张表是 VFS 的。本章只解释内存路径会调用的：

| 方法 | 中文 |
|---|---|
| `read_folio` | 把一个不在内存里的 folio 从磁盘读上来 |
| `readahead` | 批量预读 |
| `writepages` | 把脏 folio 写回去 |
| `dirty_folio` | 把 folio 标脏。返回值表示这次是不是由干净变成脏 |
| `write_begin` / `write_end` | buffered write 锁定或分配 folio 的两端 |
| `invalidate_folio` | 截断时作废 folio 的一部分 |
| `release_folio` | 回收要丢页缓存前问文件系统：私有数据（buffer_head）能不能放 |
| `free_folio` | folio 离开页缓存时的通知 |
| `migrate_folio` | 把内容迁到另一个 folio。`MIGRATE_ASYNC` 不许睡觉 |
| `launder_folio` | 回收前把脏页洗干净 |
| `is_partially_uptodate` | 子页读：这一段是不是已经在 folio 里 |
| `is_dirty_writeback` | 文件系统私有状态下，这页算不算脏、算不算正在回写。回收用它，避免只看 xarray 标记看漏 |
| `error_remove_folio` | 硬件中毒时把 folio 从页缓存摘掉 |
| `swap_activate` / `swap_deactivate` | swap 文件，不是普通页缓存 |
| `direct_IO` | 绕过页缓存。和本章的 folio 路径平行 |
| `bmap` | FIBMAP 的旧接口。注释写了别用 |

---

## 8.2 folio 怎么进页缓存

```text
generic_file_read_iter                 # filemap.c:2975
  页缓存命中：从 i_pages 拿 folio，拷给用户
  未命中：分配 folio → 插进 i_pages → a_ops->read_folio / readahead

filemap_add_folio                      # filemap.c:955
  把 folio 放进 xarray 的 index
  失败说明别的任务已经插入了同一个 index，调用者要丢掉自己的 folio

filemap_fault                          # :3555
  缺页侧的同一套查找。命中是 minor fault，读盘是 major

buffered write：
  a_ops->write_begin 拿到 folio（没有就分配并插入）
  拷贝用户数据
  dirty_folio → xarray 打 PAGECACHE_TAG_DIRTY
  balance_dirty_pages_ratelimited      # page-writeback.c:2100
```

folio 的 `mapping` 指向这个 `address_space`，`index` 是文件内页偏移。这就是 [02](./02-physical-layout.md) 里那两个字段在文件页上的用法。

一个文件页可以被多个进程的 PTE 同时映射（`i_mmap` 上多个 VMA）。`folio_mapcount` 大于 1。回收时要先把所有 PTE 拆掉，再丢页缓存。

---

## 8.3 脏页、回写、回收

脏页限速不在回收里做，在写的时候做。`balance_dirty_pages_ratelimited` 看全局和这个 backing device 的脏页是否超过 `dirty_ratio` / `dirty_bytes` 以及 background 阈值。超过 background，叫醒 flusher；超过 dirty，写进程自己睡觉等回写。

本机快照（6.12.94+，[13](./13-observability.md)）：

| 项 | 值 | 读法 |
|---|---|---|
| `Dirty` | 764 kB | 当时很干净 |
| `Writeback` | 0 | 没有正在写的页 |
| `nr_dirty` | 191 页 | 和 Dirty 同一量级（191×4kB ≈ 764kB） |
| `nr_dirty_threshold` | 166635 页 | 约 650MB，写进程开始被限速的位置 |
| `nr_dirty_background_threshold` | 83216 页 | 约 325MB，后台回写的位置 |
| `nr_dirtied` / `nr_written` | 2389870 / 2112397 | 启动以来的累计，不是当前值 |

阈值按可用内存的比例算。机器空闲时阈值高；`MemAvailable` 下降时阈值跟着降。所以「脏页只有几百 MB 却已经在限速」不一定是 bug。

回收（`shrink_folio_list`，`vmscan.c:1058`）看见脏文件页：

1. 先尝试 `pageout` 把它写走。这次回收就会变慢，并且要求 GFP 允许 IO（`__GFP_IO`）。
2. 写不走就放回 LRU，等 flusher。
3. `PG_writeback` 的页不能立刻释放。直接回收可能在这上面睡觉，kswapd 也会。

`/proc/vmstat` 的 `nr_vmscan_write` 是回收路径自己写出的页。本机是 0：脏页很少，回收主要丢的是干净文件页（`pgsteal_file` 1629411，`pgsteal_anon` 0）。没有 swap 时匿名页本来也换不走，见 [10](./10-reclaim.md)。

memcg 有自己的回写域（`cgwb`）。接口和 charge 在 [cgroup/06-memory.md](../cgroup/06-memory.md)。本章不重复那套结构体。

---

## 8.4 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `filemap_add_folio` | `mm/filemap.c:955` | folio 插入 `i_pages` |
| `filemap_fault` | `mm/filemap.c:3555` | 文件缺页 |
| `generic_file_read_iter` | `mm/filemap.c:2975` | buffered read |
| `generic_file_write_iter` | `mm/filemap.c:4508` | buffered write 的通用迭代器 |
| `mapping_tagged` | `fs.h:500` | xarray 上有没有脏/回写标记 |
| `balance_dirty_pages_ratelimited` | `mm/page-writeback.c:2100` | 写路径脏页限速 |
| `balance_dirty_pages_ratelimited_flags` | `:2030` | 带额外标志的同一逻辑 |
| `shrink_folio_list` | `mm/vmscan.c:1058` | 回收时处理脏页和回写 |

---

[上一节 缺页](./07-page-fault.md) · [目录](./README.md) · [下一节 THP](./09-thp-and-hugetlb.md)
