# 7. Direct IO

**结论**：`iomap_dio_rw` 不把用户字节先拷进页缓存。它按 `iomap_iter` 取映射，要求位置和迭代器按设备逻辑块对齐，否则 `-EINVAL`；然后钉用户页、`submit_bio`。同步 kiocb 在函数里睡到完成；异步 kiocb 在还有 bio 在飞时返回 `-EIOCBQUEUED`，完成回调再调 `ki_complete`。ext4 和 XFS 的 `O_DIRECT` 都进这里。

> VFS 在 `IOCB_DIRECT` 上的分叉、`noop_direct_IO` 的含义，见 [vfs/05](../vfs/05-read-write.md) §5.5。块设备文件的 direct 路径和 io_uring 的 CQE 见 [block-io/11](../block-io/11-direct-io.md)、[block-io/12](../block-io/12-io-uring.md)。本章从文件系统调用 `iomap_dio_rw` 写起。

---

## 7.1 谁调用

```text
ext4 读：
  ext4_file_read_iter
    ext4_dio_read_iter              fs/ext4/file.c:69
      ext4_should_use_dio           :55     不对齐时改走缓冲读
      iomap_dio_rw(..., &ext4_iomap_ops, NULL, ...)   :94

ext4 写：
  ext4_file_write_iter              :691
    IOCB_DIRECT：ext4_dio_write_iter :505
      iomap_dio_rw                  :584
        纯覆盖且共享锁：ext4_iomap_overwrite_ops
        否则 ext4_iomap_ops
        dops = ext4_dio_write_ops   end_io = ext4_dio_write_end_io

XFS 读：xfs_file.c:265
  iomap_dio_rw(..., &xfs_read_iomap_ops, NULL, ...)

XFS 写：xfs_file_dio_write           :700
  对齐：xfs_file_dio_write_aligned   :576 → iomap_dio_rw :601
  不对齐：xfs_file_dio_write_unaligned :627
```

ext4 的 `ext4_should_use_dio`（`file.c:55`）在对齐要求大于 1 且位置或迭代器没对齐时返回 false。读路径于是清掉 `IOCB_DIRECT`，改走 `generic_file_read_iter`（`:90`）。写路径改走 `ext4_buffered_write_iter`（`:546`）。注释（`:39`）说明：加密、verity 这类特性历史上回退到缓冲 IO；普通文件的不对齐 DIO 历史上返回 `EINVAL`，`iomap_dio_rw` 也用这个约定，所以那种情况仍要尝试 DIO，由 iomap 返回 `-EINVAL`。

XFS 注释（`xfs_file.c:842`）写明：直接写只有在 reflink CoW 时才允许回退成缓冲写（返回 `-ENOTBLK`）。其它直接写不回退。

---

## 7.2 对齐

`iomap_dio_bio_iter`（`fs/iomap/direct-io.c:286`）在组 bio 之前：

```c
/* fs/iomap/direct-io.c:302 */
if ((pos | length) & (bdev_logical_block_size(iomap->bdev) - 1) ||
    !bdev_iter_is_aligned(iomap->bdev, dio->submit.iter))
	return -EINVAL;
```

位置、长度、用户迭代器都要落在设备逻辑块上。文件系统块更大时，XFS 会在进 iomap 之前把未对齐的写单独拿锁（`xfs_file_dio_write_unaligned` 的注释，`xfs_file.c:609`）：子块补零必须和同一块上的其它 direct 写串行。

`IOMAP_DIO_OVERWRITE_ONLY`（`iomap.h:395`）让实现不去分配、不去把部分块补零，而是 `-EAGAIN` 交回调用者。ext4 在 `ext4_dio_write_checks` 里决定 `dio_flags`，纯覆盖才换 `ext4_iomap_overwrite_ops`（`file.c:582`）。`ext4_iomap_overwrite_begin`（`inode.c:3445`）把 `IOMAP_WRITE` 清掉，假装是读，省掉开事务；返回后 `WARN_ON` 要求类型是 `IOMAP_MAPPED`。

---

## 7.3 `__iomap_dio_rw`：提交

`iomap_dio_rw`（`direct-io.c:757`）调用 `__iomap_dio_rw`（`:555`），再 `iomap_dio_complete`（`:83`）。文件头注释（`:535`）：`O_[D]SYNC` 写总会在 `iomap_dio_rw` 里做完同步，不管这次 IO 本身是不是异步提交。这样纯数据写可以用 `REQ_FUA`，不必等 `generic_write_sync` 再发一次 flush。

```text
__iomap_dio_rw
  trace_iomap_dio_rw_begin          :573
  wait_for_completion =
    is_sync_kiocb(iocb) 或 IOMAP_DIO_FORCE_WAIT    :567
  读：kiocb_write_and_wait            重叠的脏页先写回 :607
  写：可能 IOMAP_DIO_NEED_SYNC / IOMAP_DIO_WRITE_THROUGH
      kiocb_invalidate_pages          失败且不是 -EAGAIN：改成 -ENOTBLK :658
  while iomap_iter > 0:
    iomap_dio_iter → iomap_dio_bio_iter
  异步且 dio->ref 减完后仍非 0：
    返回 ERR_PTR(-EIOCBQUEUED)        :733
  同步：TASK_UNINTERRUPTIBLE 睡到完成回调清掉 waiter
```

`IOMAP_DIO_PARTIAL`（`iomap.h:402`）允许关了缺页时先返回已经准备好的那一段，调用者处理 fault 后再带着 `done_before` 续传。注释在 `__iomap_dio_rw` 上方 `:543`。

`iomap_dio_bio_iter` 见到 `IOMAP_UNWRITTEN` 就给 dio 置 `IOMAP_DIO_UNWRITTEN`（`:306`），见到 `IOMAP_F_SHARED` 置 `IOMAP_DIO_COW`（`:311`）。新分配或 unwritten 需要把没写到的部分补零（`need_zeroout`）。

操作码在 `iomap_dio_bio_opflags`（`:269`）：写是 `REQ_OP_WRITE`，再按条件加 `REQ_FUA`。提交函数 `iomap_dio_submit_bio` 在 `:64`。`IOCB_HIPRI` 且不是同步 kiocb 时置轮询（`:72`）。同步的 direct 读即使带 HIPRI 也不靠这条路径轮询。轮询队列在 [block-io/08](../block-io/08-multiqueue-affinity.md)，本章不写。

文件系统可以提供 `iomap_dio_ops.submit_io`（`iomap.h:370`）来替换提交，并改 `bi_end_io`。注释要求自定义完成函数最后调用 `iomap_dio_bio_end_io`（`:379`）。ext4 和 XFS 的写 ops 只填了 `end_io`，提交仍走 `submit_bio`。

---

## 7.4 完成：同步睡、异步排队

```c
/* include/linux/iomap.h:367 */
struct iomap_dio_ops {
	int (*end_io)(struct kiocb *iocb, ssize_t size, int error,
		      unsigned flags);
	void (*submit_io)(const struct iomap_iter *iter, struct bio *bio,
		          loff_t file_offset);
	struct bio_set *bio_set;
};
```

`end_io` 的 flags 用 `IOMAP_DIO_UNWRITTEN` 和 `IOMAP_DIO_COW`（`iomap.h:364`）。

`iomap_dio_bio_end_io`（`direct-io.c:226`）在最后一个 bio 完成时：同步等待的 dio 叫醒 `iomap_dio_rw` 里的睡眠；异步则 `ki_complete`，或者因为还要做 unwritten 转换而 `INIT_WORK(..., iomap_dio_complete_work)`（`:221`）。工作队列由 `sb_init_dio_done_wq` 准备（`__iomap_dio_rw` 在异步且队列还不存在时调用，`:664`）。

ext4 的 `ext4_dio_write_end_io`（`file.c:381`）：`flags & IOMAP_DIO_UNWRITTEN` 时调用 `ext4_convert_unwritten_extents`（`:388`），扩展了 `i_disksize` 时再 `ext4_handle_inode_extension`。

XFS 的 `xfs_dio_write_end_io`（`xfs_file.c:482`）：`IOMAP_DIO_COW` 走 `xfs_reflink_end_cow`；`IOMAP_DIO_UNWRITTEN` 走 `xfs_iomap_write_unwritten`（`:529`）。注释强调先转换 unwritten 再更新内存里的 `i_size`，避免并发的 direct 读看见还没转换的 extent。

普通 `read` / `write` 系统调用的 kiocb 是同步的（`ki_complete == NULL`），所以它们不会把 `-EIOCBQUEUED` 带回用户态。io_uring 把 `ki_complete` 设成非空，于是可以在提交后立刻回到环上，完成时写 CQE。环的结构在 [block-io/12](../block-io/12-io-uring.md)。`IOCB_DIO_CALLER_COMP` 时 dio 置 `IOMAP_DIO_CALLER_COMP`（`direct-io.c:620`），完成可以交回调用者，少一次 workqueue。这个标志的定义在 [vfs/05](../vfs/05-read-write.md) 的 `kiocb` 字段表里。

---

## 7.5 和缓冲 IO 的 bio 差在哪

两条路径在 `submit_bio` 之后共用请求和驱动。差别在 bio 指向的页和完成回调：

| | 缓冲回写 | Direct IO |
|---|---|---|
| 页从哪来 | 页缓存 folio | `iov_iter` 里钉住的用户页 |
| 谁调用 `submit_bio` | `iomap_submit_ioend` 或 ext4 自己的回写 | `iomap_dio_submit_bio` |
| 完成时 | `folio_end_writeback`，可能转换 unwritten | 累加 `iomap_dio` 的字节，`end_io`，再 `ki_complete` |
| 对齐 | 页缓存按页 | 逻辑块，否则 `-EINVAL` |

---

## 7.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/iomap/direct-io.c` | `iomap_dio_bio_iter` | 286 | 对齐检查并组 bio |
| `fs/iomap/direct-io.c` | `iomap_dio_submit_bio` | 64 | 提交；HIPRI 时标轮询 |
| `fs/iomap/direct-io.c` | `__iomap_dio_rw` | 555 | 迭代映射、等待或返回 `-EIOCBQUEUED` |
| `fs/iomap/direct-io.c` | `iomap_dio_rw` | 757 | 对外包装，然后 `iomap_dio_complete` |
| `fs/iomap/direct-io.c` | `iomap_dio_complete` | 83 | 字节数、`end_io`、推进偏移 |
| `fs/iomap/direct-io.c` | `iomap_dio_bio_end_io` | 226 | bio 完成 |
| `include/linux/iomap.h` | `struct iomap_dio_ops` | 367 | 文件系统的 `end_io` |
| `fs/ext4/file.c` | `ext4_dio_write_iter` | 505 | ext4 直接写 |
| `fs/ext4/file.c` | `ext4_dio_write_end_io` | 381 | 转换 unwritten、扩展大小 |
| `fs/xfs/xfs_file.c` | `xfs_file_dio_write_aligned` | 576 | XFS 对齐直接写 |
| `fs/xfs/xfs_file.c` | `xfs_dio_write_end_io` | 482 | XFS 的完成转换 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 回写](./06-writeback.md) · [目录](./README.md) · [delalloc →](./08-delalloc-unwritten.md)
