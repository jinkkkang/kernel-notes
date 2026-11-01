# 10. 回写与 fsync

**结论**：脏页先留在页缓存里，flusher 调用文件系统的 `writepages`，由 `mpage` 或 iomap 把连续的脏 folio 收成 bio 再 `submit_bio`；`fsync` 在 VFS 里等到这些回写结束，块设备文件还会再发一次 `REQ_PREFLUSH` 把易失写缓存推下去。

> 脏页阈值和 flusher 何时醒来在 [MM §08](../memory-management/08-page-cache-and-writeback.md) §8.3。VFS 的 `fsync` 停在 `f_op->fsync`，见 [vfs/05](../vfs/05-read-write.md) §5.6。本章从 `writepages` 组 bio 写起。

C 新手区分两件事。回写是「把已经标脏的页变成 bio」。fsync 是「调用者要等到指定范围里的脏数据写完，并且设备承认写缓存里的数据已经不能再丢」。第二步不一定有：设备没有写回缓存（`QUEUE_FLAG_WC` 没置）时，空的 flush bio 在 `submit_bio_noacct` 里直接以 `BLK_STS_OK` 结束。

---

## 10.1 从 flusher 到 writepages

内存章已经写了这条唤醒链，这里只留交接点：

```text
balance_dirty_pages / 周期定时器
  wakeup_flusher_threads          fs/fs-writeback.c:2405
    wb_writeback                  :2115
      对每个 inode 调 mapping->a_ops->writepages
      参数是栈上的 struct writeback_control
```

`struct writeback_control` 在 `include/linux/writeback.h:42`。和块层有关的是 `sync_mode`：`WB_SYNC_NONE` 不等每个 mapping，`WB_SYNC_ALL` 会等。`fsync` 走的是后面这种。`nr_to_write` 限制这一轮最多写多少页，用完 flusher 可以换下一个 inode。

文件系统填哪一个 `writepages`，决定 bio 怎么组。6.6 里常见两条：

| 路径 | 谁用 | 组 bio 的函数 |
|---|---|---|
| `fs/mpage.c` | 仍用 `get_block` 的文件系统 | `mpage_writepages` 里的 `__mpage_writepage`（`:469`） |
| `fs/iomap/buffered-io.c` | iomap（xfs、以及走 iomap 的 ext4 缓冲写） | `iomap_writepages`（`:2036`） |

两条都在最后调用 `submit_bio`。页缓存的 xarray、`PAGECACHE_TAG_DIRTY`、`folio_start_writeback` 仍是内存章的内容。块层看到的是一个已经标了 `PG_writeback` 的 folio，外加 `bi_end_io` 里要 `folio_end_writeback`。

---

## 10.2 mpage：一页页拼进同一个 bio

`__mpage_writepage` 对 folio 里的每个块调用文件系统的 `get_block`。块在磁盘上连续、并且 bio 还没超过段数和扇区上限时，就 `bio_add_folio` 进当前 bio。不连续或 bio 满了，就把当前 bio 提交，再开一个。

```text
mpage_bio_submit_write            fs/mpage.c:86
  bio->bi_end_io = mpage_write_end_io
  guard_bio_eod                   越过设备末尾的尾巴砍掉
  submit_bio(bio)

mpage_write_end_io                :62
  err = blk_status_to_errno(bio->bi_status)
  bio_for_each_folio_all：
    出错：folio_set_error，mapping_set_error
    folio_end_writeback           清 PG_writeback，叫醒等这页的人
  bio_put
```

读路径对称：`mpage_bio_submit_read`（`:78`）把 `bi_end_io` 设成 `mpage_read_end_io`（`:46`），同样 `submit_bio`。缓冲读未命中的 bio 和回写的 bio 在块层里没有区别，差别只在完成回调把页标成 uptodate 还是结束 writeback。

---

## 10.3 iomap：ioend 包着 bio

iomap 先按映射区间把脏页收成 `struct iomap_ioend`，bio 挂在 `ioend->io_bio` 上。一个 ioend 对应一段连续的磁盘映射。映射变了就结束当前 ioend、提交，再分配下一个。

```text
iomap_writepages                  fs/iomap/buffered-io.c:2036
  iomap_writepage_map             :1815     按 folio 填 bio
  返回前 iomap_submit_ioend       :1672     把最后一个 ioend 提交

iomap_submit_ioend
  io_bio->bi_private = ioend
  io_bio->bi_end_io = iomap_writepage_end_bio
  文件系统可以提供 prepare_ioend  在提交前改 bio（例如加元数据）
  prepare 失败：
    bi_status = errno_to_blk_status(error)
    bio_endio                     不再交给驱动
  否则 submit_bio(ioend->io_bio)  :1693
```

`iomap_writepage_end_bio`（`:1657`）把 `bio->bi_status` 转成 errno，再 `iomap_finish_ioend`。那里面会 `folio_end_writeback`，和 mpage 的回调是同一类事情。

`iomap_alloc_ioend`（`:1697`）分配 ioend 和它的 bio，操作码是写。带 `REQ_META` 与否由文件系统的 iomap 标志决定，块层只看见 `bi_opf` 里的位。

---

## 10.4 PREFLUSH 和 FUA

写回缓存（writeback cache）意味着设备可以先应答「写完了」，数据还在盘自己的易失缓存里。掉电会丢。两个标志对付这件事（`include/linux/blk_types.h:447`）：

| 标志 | 含义 |
|---|---|
| `REQ_PREFLUSH` | 在处理这个请求的数据之前，先把设备写缓存刷下去 |
| `REQ_FUA` | 这次写的数据必须落稳，不能只停在易失缓存 |

`op_is_flush`（`blk_types.h:489`）为真表示带了其中任意一个。

设备没有 `QUEUE_FLAG_WC` 时，`submit_bio_noacct`（`blk-core.c:765`）把这两个标志清掉。清完如果 bio 里没有数据扇区，状态改成 `BLK_STS_OK` 直接 `bio_endio`，不进驱动。有数据的写则变成普通写。

设备有写缓存时，块层不把「先刷再写」交给驱动一次做完，除非驱动声明支持。`blk_insert_flush`（`block/blk-flush.c:404`）在 `blk_mq_submit_bio` 里、请求已经生成之后被调用（`blk-mq.c:3078`）：

```text
请求同时带数据和 PREFLUSH/FUA：
  按队列是否支持 FUA（QUEUE_FLAG_FUA）、是否有 WC，拆成最多三段
  1. 空的 PREFLUSH request，操作码被改成 REQ_OP_FLUSH
  2. 原来的数据写，可能去掉 FUA
  3. 若不能用 FUA 表达「写完要落稳」，再来一次 post-flush
空的 flush（只有 PREFLUSH、没有数据）：
  变成一条 REQ_OP_FLUSH
flush 请求的 end_io 是 flush_end_io   blk-flush.c:216
  这一段完成才放行下一段
```

文件头注释（`blk-flush.c:12` 起）写了三种设备：

```text
没有写回缓存：PREFLUSH 和 FUA 都被吃掉
有写回缓存且支持 FUA：PREFLUSH 变成独立的 flush，数据写保留 FUA
有写回缓存但不支持 FUA：数据写前后各一次 flush
```

`REQ_OP_FLUSH` 不能由文件系统直接放进 bio。`submit_bio_noacct` 见到这个操作码会走 `not_supported`（`blk-core.c:782`）。它只出现在 flush 状态机合成的 `struct request` 上。

显式刷缓存的辅助函数：

```text
blkdev_issue_flush                blk-flush.c:477
  bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH)
  submit_bio_wait                 等这条空写完成
```

没有页，`bi_io_vec` 是空的。操作码仍是 `REQ_OP_WRITE`，靠 `REQ_PREFLUSH` 触发上面的状态机。

---

## 10.5 fsync 从 VFS 接到设备

VFS 入口（[vfs/05](../vfs/05-read-write.md) §5.6）：

```text
fsync                             fs/sync.c:218
  do_fsync                        :206
    vfs_fsync_range               :180
      f_op->fsync(file, start, end, datasync)
```

块设备文件自己的 `fsync` 是 `blkdev_fsync`（`block/fops.c:523`），挂在 `def_blk_fops`（`:851`）：

```text
file_write_and_wait_range(filp, start, end)     mm/filemap.c:772
  mapping 需要回写：
    __filemap_fdatawrite_range(..., WB_SYNC_ALL)
    __filemap_fdatawait_range     等 PG_writeback 清掉
  file_check_and_advance_wb_err  把这期间的回写错误带出来
然后 blkdev_issue_flush(bdev)
  返回 -EOPNOTSUPP 时改成 0     设备不支持 flush 不算 fsync 失败
```

注释（`:533`）说这里不再拿 `i_mutex` 去串行 flush，否则并发的 `O_SYNC` 写会互相挡。

普通文件的 `f_op->fsync` 由文件系统实现。共同的前半段仍是 `file_write_and_wait_range` 或 `filemap_fdatawrite_range`（`mm/filemap.c:438`）：把范围内脏页推进 `writepages`，等到 `folio_end_writeback`。之后文件系统还要等自己的日志。日志写进设备时同样是 bio，日志提交点上常常带 `REQ_PREFLUSH` 或在提交后调用 `blkdev_issue_flush`。ext4 的 journal 和 xfs 的 log 不在本章展开，它们最终都回到 `submit_bio`。

`O_SYNC` / `IOCB_SYNC` 的写在 `generic_write_sync` 里对刚写过的范围做一次类似 fsync 的等待。那是 VFS 写路径的尾巴（[vfs/05](../vfs/05-read-write.md) §5.4），块层看到的仍是「带 PREFLUSH/FUA 的写」或「写完再来一条 flush」。

---

## 10.6 完成时页才算干净

回写 bio 提交之后，页还带着 `PG_writeback`。在 `bi_end_io` 跑完之前：

```text
同一段上的读会看到旧数据或等 writeback，取决于具体读取路径
fsync 的 filemap_fdatawait 睡在这页的等待队列上
回收把正在 writeback 的 folio 放回去，等下次扫描
  内存章 §8.3 的 folio_check_dirty_writeback
```

出错时 `bi_status` 不是 `BLK_STS_OK`。`blk_status_to_errno` 得到负的 errno，`mapping_set_error` 记在 `address_space` 上。下一次 `file_write_and_wait_range` 通过 `file_check_and_advance_wb_err` 把这个错误交给 fsync 的调用者。状态码表在 [15](./15-errors-timeout-retry.md)。

Direct IO 不走这一节的标脏和 writeback。它的 bio 在 [11](./11-direct-io.md)。

---

## 10.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/fs-writeback.c` | `wb_writeback` | 2115 | flusher 对每个 mapping 调 `writepages` |
| `fs/fs-writeback.c` | `wakeup_flusher_threads` | 2405 | 按原因叫醒 flusher |
| `fs/mpage.c` | `__mpage_writepage` | 469 | `get_block` 路径上拼写 bio |
| `fs/mpage.c` | `mpage_bio_submit_write` | 86 | `bi_end_io = mpage_write_end_io`，然后 `submit_bio` |
| `fs/mpage.c` | `mpage_write_end_io` | 62 | 错误记到 mapping，`folio_end_writeback` |
| `fs/iomap/buffered-io.c` | `iomap_writepages` | 2036 | iomap 回写入口 |
| `fs/iomap/buffered-io.c` | `iomap_submit_ioend` | 1672 | `submit_bio(ioend->io_bio)` |
| `fs/iomap/buffered-io.c` | `iomap_writepage_end_bio` | 1657 | 完成时把状态交给 ioend |
| `block/blk-flush.c` | `blk_insert_flush` | 404 | 把 PREFLUSH/FUA 拆成 flush 序列 |
| `block/blk-flush.c` | `flush_end_io` | 216 | 一段 flush 完成，放行下一段 |
| `block/blk-flush.c` | `blkdev_issue_flush` | 477 | 一条空的 `REQ_PREFLUSH` 写，并等待 |
| `block/blk-core.c` | `submit_bio_noacct` 的 WC 分支 | 765 | 没有写缓存就吃掉 flush 标志 |
| `mm/filemap.c` | `file_write_and_wait_range` | 772 | fsync 等待范围内回写 |
| `mm/filemap.c` | `filemap_fdatawrite_range` | 438 | `WB_SYNC_ALL` 写出一个范围 |
| `block/fops.c` | `blkdev_fsync` | 523 | 块设备文件：先等回写，再 `blkdev_issue_flush` |

---

**导航**：[← ioprio](./09-ioprio-and-cgroup-io.md) · [目录](./README.md) · [Direct IO →](./11-direct-io.md)
