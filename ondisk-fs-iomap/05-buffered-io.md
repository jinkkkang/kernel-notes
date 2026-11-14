# 5. iomap 缓冲读写

**结论**：`iomap_file_buffered_write` 沿着 `iomap_iter` 把用户字节拷进已锁的 folio，用 `iomap_folio_state` 的位图记下哪些文件系统块是 uptodate、哪些是 dirty，再 `filemap_dirty_folio`。读和预读走同一个迭代，把 folio 收成 bio。XFS 的缓冲写调用这个函数；ext4 的缓冲写停在 [03](./03-buffer-head.md) 的 `generic_perform_write`。

> 脏页限速 `balance_dirty_pages_ratelimited_flags` 的策略在 [MM §08](../memory-management/08-page-cache-and-writeback.md)。这里只写它在拷贝循环里的位置。

---

## 5.1 子块状态：`iomap_folio_state`

块比 folio 小时，整页 uptodate 不够用。iomap 在 folio 的 `private` 里放：

```c
/* fs/iomap/buffered-io.c:31
 * 每个块两位：低半是 uptodate，高半是 dirty。
 */
struct iomap_folio_state {
	atomic_t		read_bytes_pending;
	atomic_t		write_bytes_pending;
	spinlock_t		state_lock;
	unsigned long		state[];
};
```

`ifs_block_is_uptodate`（`:54`）测 `state` 的低半。`ifs_block_is_dirty`（`:91`）测 `block + blocks_per_folio`。范围更新是 `ifs_set_range_uptodate`（`:60`）和 `ifs_set_range_dirty`（`:127`）。

`iomap_set_range_uptodate`（`:74`）：有 `ifs` 就在锁里改位图；位图满了，或者根本没有 `ifs`（整 folio 一个块的快路径），就 `folio_mark_uptodate`。`ifs_alloc` 在 `:154`。

这和 bh 的 `BH_Uptodate` / `BH_Dirty` 是同一类信息，挂的地方不同。bh 是每块一个对象；这里是一块一位。

---

## 5.2 缓冲写

```text
xfs_file_write_iter                 fs/xfs/xfs_file.c:822
  非 DAX、非 IOCB_DIRECT：
    xfs_file_buffered_write         :758
      iomap_file_buffered_write     :779
        ops = xfs_buffered_write_iomap_ops
```

`iomap_file_buffered_write`（`fs/iomap/buffered-io.c:993`）造一个 `IOMAP_WRITE` 的 iter。`IOCB_NOWAIT` 时再加 `IOMAP_NOWAIT`（`:1004`）。然后：

```text
while iomap_iter(&iter, ops) > 0:
  iter.processed = iomap_write_iter(&iter, i)    :1007
返回写过的字节，并推进 iocb->ki_pos
```

`iomap_write_iter`（`:906`）在当前映射长度里循环：

```text
balance_dirty_pages_ratelimited_flags
fault_in_iov_iter_readable          先碰用户页，避免和「写到同一页」死锁（:934 的注释）
iomap_write_begin                   :764
  __iomap_get_folio                 锁 folio
  folio_ops->iomap_valid            映射过期则置 IOMAP_F_STALE 并停
  INLINE：读 inline 数据
  IOMAP_F_BUFFER_HEAD：__block_write_begin_int
  否则 __iomap_write_begin          按子块补零或读旧数据
copy_folio_from_iter_atomic
iomap_write_end                     :872
```

`__iomap_write_end`（`:829`）的规则：整段都写到的块可以标 uptodate。短写且 folio 还不是 uptodate 时，这次算 0 字节，让调用者重做，避免随后的 `read_folio` 盖掉写了一半的块。成功则 `iomap_set_range_uptodate`、`iomap_set_range_dirty`、`filemap_dirty_folio`。

文件大小在拷进页缓存之后用 `i_size_write` 更新（`:893`），并置 `IOMAP_F_SIZE_CHANGED`。注释写明：把新大小写进磁盘是文件系统的事，最好在 IO 完成之后，避免露出旧数据。

`IOMAP_INLINE` 走 `iomap_write_end_inline`（`:853`）：从 folio 拷回 `iomap->inline_data`，再 `mark_inode_dirty`。

XFS 的 `iomap_end` 是 `xfs_buffered_write_iomap_end`（`xfs_iomap.c:1202`）。短写时它调用 `iomap_file_buffered_write_punch_delalloc`，把没写到的延迟分配打掉。

ext4 缓冲写不进这个函数。`ext4_buffered_write_iter` 在 `IOCB_NOWAIT` 时直接返回 `-EOPNOTSUPP`（`file.c:291`），然后 `generic_perform_write`。

---

## 5.3 缓冲读和预读

```text
xfs_vm_read_folio                   fs/xfs/xfs_aops.c:519
  iomap_read_folio(folio, &xfs_read_iomap_ops)

xfs_vm_readahead                    :527
  iomap_readahead(rac, &xfs_read_iomap_ops)
```

`iomap_read_folio`（`buffered-io.c:443`）对 folio 覆盖的文件区间做 `iomap_iter`，循环体是 `iomap_readpage_iter`。映射需要读盘时把 folio 加进 bio；循环结束若还有 bio 就 `submit_bio`（`:464`），否则直接解锁。函数始终返回 0，出错只 `folio_set_error`。注释（`:472`）说这和 `mpage_readahead`、`block_read_full_folio` 一样，错误标志留在 folio 上。

`iomap_readahead`（`:520`）同样迭代，用 `readahead_folio` 取出调用者已经放进预读窗口的 folio（`iomap_readahead_iter`，`:480`）。洞和已 uptodate 的范围不必发 IO。

ext4 的 `read_folio` / `readahead` 指向 `ext4_read_folio` / `ext4_readahead`（`inode.c:3610`），里面是 `ext4_mpage_readpages`，不调用 `iomap_read_folio`。

部分 uptodate 的查询是 `iomap_is_partially_uptodate`，声明在 `iomap.h:286`。XFS 的 `a_ops.is_partially_uptodate` 填的就是它（`xfs_aops.c:552`）。ext4 填的是 `block_is_partially_uptodate`（`inode.c:3621`）。

---

## 5.4 标脏

XFS 的 `a_ops.dirty_folio` 是 `iomap_dirty_folio`（`xfs_aops.c:548`，声明 `iomap.h:290`）。它和位图里的 dirty 位一起，让回写知道 folio 里哪些块要写。回写消费这些位的路径在 [06](./06-writeback.md)。

ext4 的 `dirty_folio` 是 `ext4_dirty_folio`（`inode.c:3595`），里面 `WARN_ON` 要求 folio 上已经有 bh，然后 `block_dirty_folio`。journal 数据模式换成 `ext4_journalled_dirty_folio`（`:3586`）。

---

## 5.5 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/iomap/buffered-io.c` | `struct iomap_folio_state` | 31 | 子块 uptodate / dirty 位图 |
| `fs/iomap/buffered-io.c` | `iomap_set_range_uptodate` | 74 | 标一段 uptodate，满了则标 folio |
| `fs/iomap/buffered-io.c` | `iomap_read_folio` | 443 | 缓冲读 |
| `fs/iomap/buffered-io.c` | `iomap_readahead` | 520 | 预读 |
| `fs/iomap/buffered-io.c` | `iomap_write_begin` | 764 | 锁 folio，处理 stale / inline / bh |
| `fs/iomap/buffered-io.c` | `iomap_write_end` | 872 | 标脏、可能更新 `i_size` |
| `fs/iomap/buffered-io.c` | `iomap_file_buffered_write` | 993 | 缓冲写入口 |
| `fs/xfs/xfs_file.c` | `xfs_file_buffered_write` | 758 | XFS 的调用点 |
| `fs/xfs/xfs_aops.c` | `xfs_vm_read_folio` | 519 | XFS 缓冲读 |
| `fs/ext4/file.c` | `ext4_buffered_write_iter` | 285 | ext4 缓冲写，不进 iomap |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← iomap 模型](./04-iomap-model.md) · [目录](./README.md) · [回写 →](./06-writeback.md)
