# 5. 读和写

**结论**：`read` / `write` 先进 `vfs_read` / `vfs_write`。没有老的 `f_op->read` 时，就地造一个同步的 `kiocb` 和一段 `iov_iter`，调用 `f_op->read_iter` / `write_iter`。通用的缓冲实现把字节交给页缓存；`IOCB_DIRECT` 则调用 `a_ops->direct_IO`。

> 系统调用在 `fs/read_write.c`。页缓存那一侧的 `filemap_read`（`mm/filemap.c:2655`）和脏页限速在 [MM §08](../memory-management/08-page-cache-and-writeback.md)，这里只写到函数交界。

C 新手先看两个参数对象。`struct kiocb` 是「这一次 IO 的控制块」：哪个 file、从哪个偏移、做完了调谁。`struct iov_iter` 是「字节从哪来、到哪去」：用户缓冲、内核缓冲，或一段已经钉住的页。VFS 不关心缓冲是不是 `char *`，它只问迭代器还剩多少字节。

---

## 5.1 `kiocb` 与 `iov_iter`

```c
/* include/linux/fs.h:373 */
struct kiocb {
	struct file		*ki_filp;
	loff_t			ki_pos;        /* 这次 IO 的起始偏移 */
	void (*ki_complete)(struct kiocb *iocb, long ret); /* 非 NULL 表示异步完成 */
	void			*private;
	int			ki_flags;      /* IOCB_* */
	u16			ki_ioprio;
	union {
		/*
		 * 只给异步缓冲读。IOCB_WAITQ 置位时，指向完成这次读的页等待队列。
		 */
		struct wait_page_queue	*ki_waitq;
		/*
		 * 只给 O_DIRECT。发起者置了 IOCB_DIO_CALLER_COMP 才能用。
		 * 完成时调用方要自己看这个指针在不在。
		 */
		ssize_t (*dio_complete)(void *data);
	};
};
```

`is_sync_kiocb`（`fs.h:399`）为真的条件是 `ki_complete == NULL`。`read` / `write` 系统调用走的就是这种。`init_sync_kiocb`（`:2171`）只填 `ki_filp`、`ki_flags = filp->f_iocb_flags` 和当前 io 优先级。偏移由调用者另写进 `ki_pos`。

和本章有关的 `ki_flags`（`fs.h:326` 起）。低位直接复用用户态 `RWF_*`：

| 宏 | 值 | 含义 |
|---|---|---|
| `IOCB_HIPRI` | `RWF_HIPRI` | 高优先级，轮询完成时会用到 |
| `IOCB_DSYNC` | `RWF_DSYNC` | 写完要按数据同步 |
| `IOCB_SYNC` | `RWF_SYNC` | 写完要按文件同步 |
| `IOCB_NOWAIT` | `RWF_NOWAIT` | 不能睡眠等页或锁 |
| `IOCB_APPEND` | `RWF_APPEND` | 写到末尾 |
| `IOCB_DIRECT` | `1 << 17` | Direct IO，不走缓冲页的拷贝 |
| `IOCB_WRITE` | `1 << 18` | 这是一次写 |
| `IOCB_WAITQ` | `1 << 19` | 异步缓冲读，完成时叫醒等待队列 |
| `IOCB_NOIO` | `1 << 20` | 不许为这次读发新的 IO |
| `IOCB_DIO_CALLER_COMP` | `1 << 22` | Direct IO 的完成交回调用者 |

`iocb_flags()`（`include/linux/fs.h:3297`）在 `O_DIRECT` 置位时或上 `IOCB_DIRECT`（`:3302`）。`do_dentry_open` 把这个字抄进 `file->f_iocb_flags`（`fs/open.c:946`）。

```c
/* include/linux/uio.h:22 起的类型，:41 的迭代器 */
enum iter_type {
	ITER_IOVEC,    /* struct iovec 数组，readv/writev */
	ITER_KVEC,     /* 内核地址 */
	ITER_BVEC,     /* 已经是 struct page 的段 */
	ITER_XARRAY,   /* 页缓存 xarray 上的一段 */
	ITER_DISCARD,  /* 丢掉读到的字节 */
	ITER_UBUF,     /* 单段用户地址，普通 read/write */
};

struct iov_iter {
	u8 iter_type;
	bool copy_mc;          /* 拷贝时用机器检查，允许碰到坏页 */
	bool nofault;          /* 不许在拷贝时缺页 */
	bool data_source;      /* 真：数据从迭代器出来（写）。ITER_SOURCE = 1 */
	bool user_backed;
	union {
		size_t iov_offset; /* 当前这段里已经消费的字节 */
		int last_offset;
	};
	union {
		struct iovec __ubuf_iovec; /* ITER_UBUF：和下面的 iovec+count 叠在一起 */
		struct {
			union {
				const struct iovec *__iov;
				const struct kvec *kvec;
				const struct bio_vec *bvec;
				struct xarray *xarray;
				void __user *ubuf;
			};
			size_t count;      /* 还剩多少字节 */
		};
	};
	union {
		unsigned long nr_segs;
		loff_t xarray_start;
	};
};
```

`ITER_SOURCE` 是 1，注释写明等于写；`ITER_DEST` 是 0，等于读（`uio.h:32`）。普通 `read` 用 `iov_iter_ubuf(&iter, ITER_DEST, buf, len)`（`fs/read_write.c:387`）。普通 `write` 用 `ITER_SOURCE`（`:489`）。

---

## 5.2 `read` 系统调用

```text
read                                fs/read_write.c:621
  ksys_read                         :602
    fdget_pos                       拿 file 引用，必要时锁 f_pos
    vfs_read                        :450
    成功则把 kiocb 里推进后的偏移写回 f_pos
    fdput_pos

vfs_read
  没有 FMODE_READ：-EBADF
  没有 FMODE_CAN_READ：-EINVAL
  access_ok 失败：-EFAULT
  rw_verify_area                    :355
  count 超过 MAX_RW_COUNT 就截断
  f_op->read 非空：直接调它          老接口，自己处理用户缓冲
  否则 f_op->read_iter：
    new_sync_read                   :379
  读到字节：fsnotify_access，add_rchar
  inc_syscr                         进程的读系统调用计数
```

`new_sync_read`（`:379`）：

```text
init_sync_kiocb
ki_pos = *ppos
iov_iter_ubuf(ITER_DEST)
call_read_iter                      include/linux/fs.h:2010
  就是 file->f_op->read_iter(kiocb, iter)
返回值不许是 -EIOCBQUEUED           同步包装不能处理「以后再完成」
把 ki_pos 写回 *ppos
```

`pread64`（`fs/read_write.c:672`）不使用 `f_pos`，偏移来自参数。`readv`（`:1040`）走 `iov_iter` 的 `ITER_IOVEC`，入口是 `vfs_readv`（`:905`）和 `do_iter_read`（`:776`）。

---

## 5.3 缓冲读交到页缓存

很多磁盘文件系统把 `read_iter` 设成 `generic_file_read_iter`（`mm/filemap.c:2828`）。tmpfs 不用它，用自己的 `shmem_file_read_iter`，见 [10](./10-tmpfs-walkthrough.md)。

```text
generic_file_read_iter              mm/filemap.c:2828
  count == 0：返回 0，连 atime 都不碰
  ki_flags 含 IOCB_DIRECT：
    kiocb_write_and_wait            :2764    先等这一段上的脏页写完
    file_accessed
    mapping->a_ops->direct_IO
    短读且不是 DAX、也还没到 EOF：落到下面的缓冲读
  filemap_read                      :2655
```

`filemap_read` 的循环（`:2676` 起）每次 `filemap_get_pages`（`:2690`）。页已经在 xarray 里就拷贝；不在就分配并调用 `a_ops->read_folio`。`filemap_read_folio` 在 `:2403`。folio 怎么进 `address_space->i_pages`、预读窗口怎么涨，都在 [MM §08](../memory-management/08-page-cache-and-writeback.md) §8.2。缺页那条平行的路是 `filemap_fault`，见 [06](./06-mmap-and-file.md) 和 [MM §07](../memory-management/07-page-fault.md)。

缓冲读拷贝的是页缓存里的字节，不是用户进程直接对磁盘发命令。磁盘 IO 发生在缓存未命中、`read_folio` 把 folio 填上的时候。

---

## 5.4 `write` 与缓冲写

```text
write                               fs/read_write.c:646
  ksys_write                        :626
    vfs_write                       :564

vfs_write
  要有 FMODE_WRITE 和 FMODE_CAN_WRITE
  rw_verify_area(WRITE)
  file_start_write                  冻住超级块时会在这里等
  f_op->write 或 new_sync_write     :481
  写到字节：fsnotify_modify，add_wchar
  file_end_write
```

`new_sync_write` 用 `ITER_SOURCE`，调用 `call_write_iter`（`include/linux/fs.h:2016`）。

通用写是 `generic_file_write_iter`（`mm/filemap.c:4139`）：

```text
inode_lock                        独占 i_rwsem
generic_write_checks              检查偏移、大小、RLIMIT
__generic_file_write_iter         :4092
inode_unlock
写成功且需要同步：generic_write_sync
```

`__generic_file_write_iter`：

```text
file_remove_privs                 第一次写可能丢掉 setuid
file_update_time
IOCB_DIRECT：
  generic_file_direct_write       :3935
  没写完、也不是 DAX：direct_write_fallback 再 generic_perform_write
否则：
  generic_perform_write           :3990
```

`generic_perform_write` 按页循环（`:3999`）：

```text
fault_in_iov_iter_readable        先碰一下用户页，避免和「写到同一页」死锁
a_ops->write_begin                准备好一页，返回 struct page *
拷贝 iov_iter 到这一页
a_ops->write_end                  标脏、更新 i_size
```

标脏之后的 `balance_dirty_pages`、flusher 线程、`writepages`，在 [MM §08](../memory-management/08-page-cache-and-writeback.md) §8.3。本章停在 `write_end` 把页交给地址空间。

---

## 5.5 Direct IO

Direct IO 仍然从同一个 `read_iter` / `write_iter` 进来，差别只是 `IOCB_DIRECT`。

打开条件：`do_dentry_open` 看到 `f_mapping->a_ops->direct_IO` 非空才置 `FMODE_CAN_ODIRECT`（`fs/open.c:942`）。带着 `O_DIRECT` 却没有这个位，open 返回 `-EINVAL`（`:950`）。有的文件系统把 `direct_IO` 设成 `noop_direct_IO`（`fs/libfs.c:1567`），它返回 `-EINVAL`，只为了让 open 知道「这个文件声称支持 Direct IO」，真正的 IO 走别的路径（iomap）。注释写在 `:1569`。

读：

```text
generic_file_read_iter
  kiocb_write_and_wait            这一段若有脏页，先写回，避免读到旧缓存
  a_ops->direct_IO(iocb, iter)
  返回值 >= 0：推进 ki_pos
  短读、出错、DAX、或已经到 EOF：直接返回
  否则剩下的字节改走 filemap_read
```

写：

```text
generic_file_direct_write         mm/filemap.c:3935
  a_ops->direct_IO
没写完且不是 DAX：
  direct_write_fallback           :4118 的调用点
    再 generic_perform_write      缓冲写剩下的
```

Direct IO 绕过「把用户字节先拷进页缓存再回写」这条。它不保证页缓存里没有同一段的旧页：读之前的 `kiocb_write_and_wait` 就是为了把重叠的脏页先推下去。已经干净的缓存页还在不在，由具体的 `direct_IO` 实现决定，VFS 这一层只做上面的等待和回退。

`IOCB_NOWAIT` 时，等锁或等页的地方要返回 `-EAGAIN`，不能睡。同步的 `new_sync_read` 不允许 `-EIOCBQUEUED`（`read_write.c:390`）。异步完成是 `ki_complete != NULL` 的 kiocb，`io_uring` 用这条，本章不展开。

---

## 5.6 `fsync` 在 VFS 层停在哪

```text
fsync                               fs/sync.c:218
  do_fsync(fd, 0)                   :206
    vfs_fsync                       :200
      vfs_fsync_range               :180
        没有 f_op->fsync：-EINVAL
        非 datasync 且 inode 带 I_DIRTY_TIME：mark_inode_dirty_sync
        f_op->fsync(file, start, end, datasync)
fdatasync                           :223    do_fsync(fd, 1)
```

`vfs_fsync_range` 的范围是字节，`vfs_fsync` 传 `0` 到 `LLONG_MAX`（`:202`）。磁盘文件系统的 `fsync` 会等待回写；tmpfs 的 `fsync` 是 `noop_fsync`，直接返回 0，见 [10](./10-tmpfs-walkthrough.md)。回写线程本身在 MM §8.3。

---

## 5.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/fs.h` | `struct kiocb` | 373 | 一次 IO 的 file、偏移、完成回调 |
| `include/linux/fs.h` | `init_sync_kiocb` | 2171 | 同步 kiocb：`ki_complete` 留空 |
| `include/linux/fs.h` | `call_read_iter` / `call_write_iter` | 2010 / 2016 | 调 `f_op->read_iter` / `write_iter` |
| `include/linux/uio.h` | `struct iov_iter` | 41 | 字节流迭代器 |
| `fs/read_write.c` | `new_sync_read` | 379 | 单缓冲同步读 |
| `fs/read_write.c` | `vfs_read` | 450 | 校验后选择 `read` 或 `read_iter` |
| `fs/read_write.c` | `new_sync_write` | 481 | 单缓冲同步写 |
| `fs/read_write.c` | `vfs_write` | 564 | 校验后选择 `write` 或 `write_iter` |
| `fs/read_write.c` | `read` / `write` | 621 / 646 | 系统调用 |
| `fs/read_write.c` | `pread64` / `pwrite64` | 672 / 706 | 带显式偏移 |
| `mm/filemap.c` | `filemap_read` | 2655 | 缓冲读的页缓存循环 |
| `mm/filemap.c` | `generic_file_read_iter` | 2828 | Direct IO 或 `filemap_read` |
| `mm/filemap.c` | `generic_file_direct_write` | 3935 | 写方向的 `direct_IO` |
| `mm/filemap.c` | `generic_perform_write` | 3990 | `write_begin` / 拷贝 / `write_end` |
| `mm/filemap.c` | `__generic_file_write_iter` | 4092 | 去掉权限和时间后的写 |
| `mm/filemap.c` | `generic_file_write_iter` | 4139 | 加 `i_rwsem`，必要时 `generic_write_sync` |
| `fs/sync.c` | `vfs_fsync_range` | 180 | 调 `f_op->fsync` |
| `fs/libfs.c` | `noop_direct_IO` | 1567 | 占位，让 open 认为支持 `O_DIRECT` |
| `fs/libfs.c` | `noop_fsync` | 1561 | 返回 0 |

---

**导航**：[← fd 与 open](./04-fd-and-open.md) · [目录](./README.md) · [splice →](./05b-splice-and-zerocopy.md)
