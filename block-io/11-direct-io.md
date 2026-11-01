# 11. Direct IO

**结论**：`O_DIRECT` 不把用户字节先放进页缓存再等回写。`iov_iter` 里的用户页被钉住，填进 bio 的 `bio_vec`，`submit_bio` 之后由完成回调把结果交回 `kiocb`。

> VFS 在 `IOCB_DIRECT` 处分叉，见 [vfs/05](../vfs/05-read-write.md) §5.5。`kiocb` 和 `iov_iter` 的字段也在那一节。本章从 `a_ops->direct_IO` 或块设备自己的 `blkdev_direct_IO` 往下写。

C 新手记住：缓冲 IO 的 bio 指向页缓存里的 folio，完成时 `folio_end_writeback` 或把页标成最新。Direct IO 的 bio 指向这次系统调用钉住的用户页（或内核替用户准备的页），完成时把字节数和错误写回 `kiocb`，然后放开这些页。两条路在 `submit_bio` 之后共用同一套 request、调度器和驱动。

---

## 11.1 VFS 交到哪

`generic_file_read_iter`（`mm/filemap.c:2828`）看见 `IOCB_DIRECT`：

```text
kiocb_write_and_wait               这一段上的脏页先写回，避免读到页缓存里的旧数据
file_accessed
mapping->a_ops->direct_IO(iocb, iter)
短读且不是 DAX、也还没到 EOF：剩下的字节改走 filemap_read
```

写是 `generic_file_direct_write`（`mm/filemap.c:3935`）调用同一个 `direct_IO`。没写完且不是 DAX 时，`direct_write_fallback` 再走缓冲写。所以一次 `O_DIRECT` 写仍可能在尾巴上产生脏页。那是 VFS 的回退，不是块层把 direct bio 改成了缓冲 bio。

`do_dentry_open` 只有在 `a_ops->direct_IO` 非空时才置 `FMODE_CAN_ODIRECT`（`fs/open.c:942`）。有的文件系统把这个函数设成 `noop_direct_IO`（`fs/libfs.c:1567`），它返回 `-EINVAL`，只为了让 open 通过，真正的 IO 走 iomap 而不是这个函数指针。注释在 `libfs.c:1569`。ext4、xfs 在 6.6 的缓冲/直接写主路径上用 iomap，下面第 3 节就是那条。

---

## 11.2 块设备文件：三档 direct

打开 `/dev/xxx` 时 `read_iter` / `write_iter` 是 `blkdev_read_iter` / `blkdev_write_iter`（`block/fops.c:700`、`:644`）。带 `IOCB_DIRECT` 时进 `blkdev_direct_IO`（`:366`）。

```text
iov_iter 为空：返回 0
nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS + 1)
nr_pages <= BIO_MAX_VECS（256，include/linux/bio.h:13）：
  同步 kiocb（ki_complete == NULL）：
    __blkdev_direct_IO_simple     :46
  否则：
    __blkdev_direct_IO_async      :301
更大：
  __blkdev_direct_IO              :162    可以拆成多个 bio
```

`BIO_MAX_VECS` 是一个 bio 内嵌或携带的段数上限。用户一次 write 超过 256 个不连续页，就要多个 bio。

`__blkdev_direct_IO_simple` 适合小的同步 IO：分配一个 bio，`bio_iov_iter_get_pages` 从迭代器钉页，`submit_bio_wait` 睡到完成。异步和多 bio 的版本在提交后返回 `-EIOCBQUEUED`（`:363`），真正的字节数由 `ki_complete` 以后再交。同步包装 `new_sync_read` 不允许这个返回值（`fs/read_write.c:390`），所以普通 `read` / `write` 系统调用走的是会睡觉的那一档，不是 `-EIOCBQUEUED`。

`io_uring` 的读写把 `ki_complete` 设成非空，于是块设备 direct 可以走 `__blkdev_direct_IO_async`，提交之后立刻回到环上，完成时再写 CQE。见 [12](./12-io-uring.md)。

---

## 11.3 iomap direct：文件系统的主路径

`iomap_dio_rw`（`fs/iomap/direct-io.c:757`）包着 `__iomap_dio_rw`（`:555`）。文件系统的 `direct_IO` 或 `write_iter` 调用它，并提供 `iomap_dio_ops`。

```text
__iomap_dio_rw
  trace_iomap_dio_rw_begin          :573
  按 iomap 区间循环
    映射类型是 bio 能提交的：iomap_dio_bio_iter   :286
iomap_dio_rw 在需要时等待完成，并做 O_DSYNC 的收尾
  注释 :535：O_[D]SYNC 写总会在这里完成同步，不管这次 IO 本身是不是异步提交
```

`iomap_dio_bio_iter`（`:286`）先拒绝没有按逻辑块对齐的位置或迭代器（`:302`），返回 `-EINVAL`。然后：

```text
bio_opf = iomap_dio_bio_opflags   :269
  写：REQ_OP_WRITE，可能再加 REQ_FUA、REQ_IDLE、REQ_SYNC
bio = iomap_dio_alloc_bio         :59 附近
  有 bioset 用 bio_alloc_bioset，否则 bio_alloc
bio_iov_iter_get_pages            block/bio.c:1331
  从 iov_iter 取页，填 bi_io_vec，推进迭代器
iomap_dio_submit_bio              direct-io.c:64
  atomic_inc(&dio->ref)           bio 在飞时 dio 不能释放
  IOCB_HIPRI 且不是同步 kiocb：
    bio_set_polled                带上 REQ_POLLED
    iocb->private = bio           轮询时找得回这个 bio
  文件系统提供了 submit_io：用它
  否则 submit_bio(bio)            :80
```

`bio_iov_iter_get_pages` 是 Direct IO 和块层之间最具体的交点。它按迭代器的类型处理用户 iovec、已经钉住的 bvec、或 io_uring 的固定缓冲。钉页失败会把已经拿到的页退回，bio 不会带着半截页提交。

完成不走页缓存的 `folio_end_writeback`。bio 的 `bi_end_io` 把字节数累进 `struct iomap_dio`，`dio->ref` 减到该返回时调用 `iomap_dio_complete`（`:83`）。那里可以再调文件系统的 `end_io`（未写完的 extent 转成已写、COW 收尾），然后推进 `ki_pos`，异步则 `ki_complete`。

`IOCB_NOWAIT` 时，等锁、等脏页、钉页需要睡眠的地方要返回 `-EAGAIN`，而不是睡。io_uring 看到 `-EAGAIN` 会把请求放进 io-wq 再试，或者武装 poll。块层这边对应的是 `REQ_NOWAIT`：`submit_bio_noacct` 在设备不支持 nowait 时直接 `BLK_STS_NOTSUPP`（`blk-core.c:744`），分配 tag 失败时 `bio_wouldblock_error`（`blk-mq.c:2967`）把状态设成 `BLK_STS_AGAIN`。

---

## 11.4 旧的 `__blockdev_direct_IO`

`fs/direct-io.c` 仍在树上。入口是 `__blockdev_direct_IO`（`:1104`）。它用文件系统的 `get_block` 把文件偏移映射成扇区，自己维护 `struct dio` 和 `struct dio_submit`，bio 通过 `dio_bio_submit`（`:424`）提交：

```text
bio->bi_private = dio
dio->refcount++                 在 bio_lock 下
异步读且 should_dirty：bio_set_pages_dirty
submit_bio(bio)                 :441
```

注释（`:420`）说：提交和 `end_io` 之间 bio 持有 dio 的引用；异步读若需要，完成路径会经 `bio_check_pages_dirty` 把页重新标脏。文件开头的长注释（`:1090` 附近）说明 `i_dio_count` 用来和 truncate 互斥：Direct IO 开始前增加，全部 bio 完成后再减，truncate 可以等到它归零。

新代码走 iomap。这条路径留给还没迁走的 `get_block` 文件系统。两条路径的 bio 在 `submit_bio` 之后没有区别。

---

## 11.5 和缓冲 IO 对照

| | 缓冲 | O_DIRECT |
|---|---|---|
| 用户字节先到哪 | 页缓存 folio（`generic_perform_write`） | `bio_iov_iter_get_pages` 钉住的页 |
| 谁调用 `submit_bio` | 未命中的读、flusher 的写（[10](./10-writeback-and-fsync.md)） | `iomap_dio_submit_bio`、`dio_bio_submit` 或 `blkdev_direct_IO` |
| `bi_end_io` 做什么 | `folio_end_writeback` / 标 uptodate | 累加 `iomap_dio` 或 dio 的字节数，再 `ki_complete` |
| 和页缓存的关系 | 就是页缓存 | 读之前 `kiocb_write_and_wait` 先把重叠脏页推下去。已经干净的缓存页还在不在，由文件系统的 direct 实现决定，VFS 只做这次等待 |
| 对齐 | 页缓存按页 | `iomap_dio_bio_iter` 要求位置和迭代器按逻辑块对齐，否则 `-EINVAL` |
| 同步 | 写进页就返回，回写是以后的事 | 同步 kiocb 睡到 bio 完成。`O_DSYNC` 还要再等元数据 |

缓冲读的磁盘 IO 只发生在缓存未命中。Direct 读每次都产生 bio（除非文件系统在 direct 实现里发现可以短路，那是文件系统的事，不是 `submit_bio` 的事）。

`REQ_POLLED` 在 direct 路径上由 `IOCB_HIPRI` 加上「这是异步 kiocb」两个条件一起打开（`iomap_dio_submit_bio` `:72`）。同步读即使带 HIPRI 也不走轮询，注释写明 sync dio 不能可靠地轮询。轮询的队列类型是 `HCTX_TYPE_POLL`，见 [08](./08-multiqueue-affinity.md)。

---

## 11.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `mm/filemap.c` | `generic_file_read_iter` | 2828 | `IOCB_DIRECT` 时先等脏页，再 `direct_IO` |
| `mm/filemap.c` | `generic_file_direct_write` | 3935 | 写方向的 `direct_IO` |
| `fs/libfs.c` | `noop_direct_IO` | 1567 | 占位，让 open 认为支持 `O_DIRECT` |
| `block/fops.c` | `blkdev_direct_IO` | 366 | 块设备文件按页数分三档 |
| `block/fops.c` | `__blkdev_direct_IO_simple` | 46 | 小的同步 direct |
| `block/fops.c` | `__blkdev_direct_IO_async` | 301 | 异步，返回 `-EIOCBQUEUED` |
| `block/fops.c` | `__blkdev_direct_IO` | 162 | 超过 256 段时拆多个 bio |
| `block/bio.c` | `bio_iov_iter_get_pages` | 1331 | 从 `iov_iter` 钉页并填 `bio_vec` |
| `fs/iomap/direct-io.c` | `__iomap_dio_rw` | 555 | iomap direct 的主体 |
| `fs/iomap/direct-io.c` | `iomap_dio_rw` | 757 | 对外包装，处理等待和 DSYNC |
| `fs/iomap/direct-io.c` | `iomap_dio_bio_iter` | 286 | 一个映射区间里组 bio |
| `fs/iomap/direct-io.c` | `iomap_dio_submit_bio` | 64 | `submit_bio`，HIPRI 时标成轮询 |
| `fs/iomap/direct-io.c` | `iomap_dio_complete` | 83 | 字节数、`end_io`、推进偏移 |
| `fs/direct-io.c` | `__blockdev_direct_IO` | 1104 | `get_block` 时代的 direct |
| `fs/direct-io.c` | `dio_bio_submit` | 424 | 增加 dio 引用后 `submit_bio` |

---

**导航**：[← 回写](./10-writeback-and-fsync.md) · [目录](./README.md) · [io_uring →](./12-io-uring.md)
