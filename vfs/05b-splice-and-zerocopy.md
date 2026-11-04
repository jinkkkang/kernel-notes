# 5b. splice、sendfile 与 copy_file_range

**结论**：`read` / `write` 让字节经过用户缓冲。`splice` 把 `struct page` 放进管道槽，用户地址不在这条路上。两个普通文件做 `splice(2)` 会在 `do_splice` 里得到 `-EINVAL`。`sendfile` 用当前任务私有的 `splice_pipe` 把文件接到文件。`copy_file_range` 先交给文件系统自己的复制或 `remap_file_range`，没有成功的克隆再落到同一条 direct splice。

> 源码：Linux v6.6.160。管道在 `include/linux/pipe_fs_i.h`、`fs/pipe.c`。splice 在 `fs/splice.c`、`include/linux/splice.h`。`sendfile` 与 `copy_file_range` 在 `fs/read_write.c`。
> 对照：[elixir v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)。
> 导航：[上一节 读写](./05-read-write.md) · [目录](./README.md) · [下一节 mmap](./06-mmap-and-file.md)

`f_op->splice_read` / `splice_write` / `splice_eof` / `copy_file_range` 已在 [02](./02-core-objects.md) §2.6 的 `file_operations` 里列出，这里不重复整张表。缓冲 `read` / `write` 的主路径在 [05](./05-read-write.md)。tmpfs 的函数表在 [10](./10-tmpfs-walkthrough.md) §10.2：`splice_read` 是 `shmem_file_splice_read`，`splice_write` 是 `iter_file_splice_write`。

本篇写这三条系统调用在 VFS 里怎么选路、管道槽里放什么、什么时候退回字节拷贝。套接字 splice、`pipe(2)` 的创建细节、磁盘文件系统怎么实现区间克隆，都停在交界。

---

## 5b.1 零拷贝在 VFS 里指什么

```text
read + write                         fs/read_write.c
  vfs_read          :450             页缓存 → 用户缓冲
  vfs_write         :564             用户缓冲 → 目标页缓存

splice(2)                            fs/splice.c:1568 → do_splice :1243
  文件 → 管道                        页挂进 pipe_buffer
  管道 → 文件                        iter_file_splice_write → vfs_iter_write
  管道 → 管道                        挪整个 pipe_buffer
  文件 → 文件                        -EINVAL

sendfile(2)                          do_sendfile :1180
  输出是管道                         splice_file_to_pipe
  输出是普通文件                     do_splice_direct（任务私有管道）

copy_file_range(2)                   vfs_copy_file_range :1475
  文件系统 copy_file_range           调用方实现，VFS 不回落
  同超级块 remap_file_range          克隆区间，返回 > 0 就结束
  其余                               generic_copy_file_range → do_splice_direct
```

这三条路径里，字节不经过用户缓冲。管道到普通文件的通用写仍然把字节拷进目标文件的页缓存。`remap_file_range` 返回正数时，这一次调用克隆文件系统上的区间，VFS 不再调用 `do_splice_direct`。

---

## 5b.2 `pipe_buffer` / `pipe_inode_info`

```c
/* include/linux/pipe_fs_i.h:5 */
#define PIPE_DEF_BUFFERS	16
```

```c
/* include/linux/pipe_fs_i.h:26 */
struct pipe_buffer {
	struct page *page;                      /* 这一槽的数据页 */
	unsigned int offset, len;               /* 页内起点、有效字节 */
	const struct pipe_buf_operations *ops;  /* confirm / release / try_steal / get */
	unsigned int flags;                     /* PIPE_BUF_FLAG_*，:7 起 */
	unsigned long private;                  /* ops 自己的数据 */
};
```

```c
/* include/linux/pipe_fs_i.h:58。字段顺序以这份头文件为准，和上面的 kerneldoc 不完全同序 */
struct pipe_inode_info {
	struct mutex mutex;                    /* 保护整根管道 */
	wait_queue_head_t rd_wait, wr_wait;    /* 空：读者睡。满：写者睡 */
	unsigned int head;                      /* 下一生产槽。取槽时才按 ring_size 掩 */
	unsigned int tail;                      /* 下一消费槽 */
	unsigned int max_usage;                 /* 允许同时占用的槽数 */
	unsigned int ring_size;                /* bufs 的长度，2 的幂 */
#ifdef CONFIG_WATCH_QUEUE
	bool note_loss;                         /* 下一次 read 插入丢消息。本篇不展开 */
#endif
	unsigned int nr_accounted;              /* 记在 user->pipe_bufs 上的槽数 */
	unsigned int readers;
	unsigned int writers;
	unsigned int files;                     /* 指向这根管道的 struct file 个数 */
	unsigned int r_counter;
	unsigned int w_counter;
	bool poll_usage;                        /* epoll 用过，唤醒更勤 */
	struct page *tmp_page;                  /* 匿名页释放时可以留下的一页 */
	struct fasync_struct *fasync_readers;
	struct fasync_struct *fasync_writers;
	struct pipe_buffer *bufs;               /* 环形槽 */
	struct user_struct *user;
#ifdef CONFIG_WATCH_QUEUE
	struct watch_queue *watch_queue;       /* 通知管道。for_splice 时不当成 splice 管道 */
#endif
};
```

`head` / `tail` 在用到下标时才掩码。`pipe_buf`（`:180`）用 `slot & (ring_size - 1)`。`pipe_empty`（`:148`）是 `head == tail`。`pipe_occupancy`（`:158`）是 `head - tail`，无符号回绕仍然成立。`pipe_full`（`:169`）是占用数达到 `max_usage`。`fs/pipe.c:63` 的注释要求环长是 2 的幂，并且不超过 `2^31`。

`PIPE_SIZE`（`pipe_fs_i.h:259`）是 `PAGE_SIZE`，和 POSIX 的 `PIPE_BUF` 原子写保证不是同一个数。默认 16 个槽，容量是 `16 * PAGE_SIZE`。`alloc_pipe_info`（`fs/pipe.c:788`）把 `max_usage`、`ring_size`、`nr_accounted` 都设成这个槽数（`:820`）。非特权用户的管道太多时，新管道会缩到 `PIPE_MIN_DEF_BUFFERS`（2，`pipe.c:48`，用在 `:806`）。单根管道的上限是 `pipe_max_size`，初值 `1048576`（`:54`），sysctl `fs/pipe-max-size` 注册在 `:1478`。采样见 [12](./12-observability.md)。`F_GETPIPE_SZ` 返回 `max_usage * PAGE_SIZE`（`pipe_fcntl`，`:1408`）。

空和满睡在 `rd_wait` / `wr_wait` 上。`pipe_wait_readable` / `pipe_wait_writable`（声明在 `pipe_fs_i.h:267`）会先放下管道的 mutex。等待队列本身见 [locks 08](../locks-and-ds/08-wait-queue-and-completion.md)。

```c
/* include/linux/pipe_fs_i.h:95。try_steal 必须在 confirm 成功之后 */
struct pipe_buf_operations {
	int (*confirm)(struct pipe_inode_info *, struct pipe_buffer *);   /* 0：数据可读 */
	void (*release)(struct pipe_inode_info *, struct pipe_buffer *);  /* 消费者用完这一槽 */
	bool (*try_steal)(struct pipe_inode_info *, struct pipe_buffer *); /* 真：页已归调用者 */
	bool (*get)(struct pipe_inode_info *, struct pipe_buffer *);       /* 多拿一个引用 */
};
```

和文件 splice 有关的三套 ops：

| ops | 位置 | 用在 |
|---|---|---|
| `page_cache_pipe_buf_ops` | `fs/splice.c:155` | 页缓存页挂进管道。`confirm` 等 uptodate（`:119`），`release` 是 `put_page`（`:108`），`try_steal` 可以把它从原来的 `address_space` 摘掉（`:64`） |
| `default_pipe_buf_ops` | `:403` | `copy_splice_read` 新分配的页。`try_steal` 是 `generic_pipe_buf_try_steal`（`fs/pipe.c:165`）：页引用为 1 才锁页成功 |
| `zero_pipe_buf_ops` | `mm/shmem.c:2906` | tmpfs 的洞。`try_steal` 直接返回 false（`:2900`） |

`PIPE_BUF_FLAG_GIFT`（`0x04`）表示这一页可以送人。`vmsplice` 在带 `SPLICE_F_GIFT` 时把它写上（`splice.c:1480`）。`nosteal_pipe_buf_ops`（`:410`）没有 `try_steal`，注释写给套接字一类的缓冲。套接字路径不在本篇展开。

`get_pipe_info`（`fs/pipe.c:1382`）看的是 `file->f_op == &pipefifo_fops` 和 `file->private_data`。`inode` 里的 `i_pipe` 和 `i_cdev` 共用一个 union（[02](./02-core-objects.md) 的 `inode`），所以不能只看那个指针。`for_splice` 为真且这根管道是 watch queue 时，函数返回 `NULL`（`:1388`），后面的 `do_splice` 不会把它当管道。

---

## 5b.3 `do_splice`：四种组合

```c
/* include/linux/splice.h:17 */
#define SPLICE_F_MOVE	(0x01)	/* move pages instead of copying */
#define SPLICE_F_NONBLOCK (0x02) /* 管道这一端不睡；另一端的 fd 仍可能睡 */
#define SPLICE_F_MORE	(0x04)	/* 后面还有数据 */
#define SPLICE_F_GIFT	(0x08)	/* 送进来的页可以转手 */
```

`SPLICE_F_ALL` 在 `:24`。`splice` 系统调用（`fs/splice.c:1568`）看到 `len == 0` 直接返回 0（`:1575`）。旗标超出 `SPLICE_F_ALL` 返回 `-EINVAL`（`:1578`）。然后 `fdget` 两端，进 `__do_splice`（`:1336`）。

`__do_splice` 对管道端调用 `pipe_clear_nowait`（`:48`）：清掉这个 `struct file` 上的 `FMODE_NOWAIT`，避免 splice 把管道留在非阻塞模式里。偏移指针从用户地址拷进来，`do_splice` 返回后再拷回去。数据已经搬走、但新偏移 `copy_to_user` 失败时，系统调用返回 `-EFAULT`（`:1374`）。

`do_splice`（`:1243`）先要求输入 `FMODE_READ`、输出 `FMODE_WRITE`，否则 `-EBADF`。两端用 `get_pipe_info(..., true)` 分类：

| 输入 | 输出 | 去向 | 偏移 |
|---|---|---|---|
| 管道 | 管道 | `splice_pipe_to_pipe`（`:1673`） | 任一端带偏移：`-ESPIPE`。同一根管道：`-EINVAL` |
| 管道 | 普通文件 | `do_splice_from`（`:928`） | 输入带偏移：`-ESPIPE`。输出用 `off_out` 或 `f_pos`。`O_APPEND`：`-EINVAL`（`:1281`） |
| 普通文件 | 管道 | `splice_file_to_pipe`（`:1223`） | 输出带偏移：`-ESPIPE`。输入用 `off_in` 或 `f_pos` |
| 普通文件 | 普通文件 | `-EINVAL`（`:1320`） | 文件到文件用 `sendfile` 或 `copy_file_range` |

管道到管道（`:1747`）：`len` 盖住整槽时，把 `pipe_buffer` 整份挪到输出，源槽的 `ops` 置 `NULL`，源端不再 `release`。只拿走一部分时，`pipe_buf_get` 多拿一个引用，输出槽清掉 `PIPE_BUF_FLAG_GIFT` 和 `PIPE_BUF_FLAG_CAN_MERGE`，两边改 `offset` / `len`。成功后 `fsnotify_modify(out)` 再 `fsnotify_access(in)`（`:1329`）。这条系统调用本身不调用 `add_rchar` / `inc_syscr`。

`O_NONBLOCK` 会或上 `SPLICE_F_NONBLOCK`（管道两端看两边的旗标，`:1266`；单管道看管道那一端，`:1288` 或 `:1310`）。管道空或满时，`splice_from_pipe_next`（`:536`）和 `wait_for_space`（`:1211`）返回 `-EAGAIN`。有信号时返回 `-ERESTARTSYS`（`:525`、`:1213`）。

---

## 5b.4 `vfs_splice_read`、`do_splice_from`、`splice_to_pipe`

文件进管道的同步入口是 `vfs_splice_read`（`:962`）。调用者要自己拿着管道锁；`splice_file_to_pipe` 就是先 `pipe_lock`，`wait_for_space`，再调用它（`:1230`）。

```text
vfs_splice_read
  没有 FMODE_READ：-EBADF
  len == 0：0
  按管道剩余槽数截断 len          (max_usage - occupancy) << PAGE_SHIFT
  rw_verify_area(READ)
  len 超过 MAX_RW_COUNT 再截断    include/linux/fs.h:2475，INT_MAX 按页对齐
  没有 f_op->splice_read：warn_unsupported → -EINVAL   splice.c:917
  O_DIRECT 或 DAX：copy_splice_read   不调用 f_op->splice_read
  否则：f_op->splice_read
```

`copy_splice_read`（`:320`）按剩余槽分配页，组成 `ITER_DEST` 的 `bio_vec`，`call_read_iter` 把字节读进这些新页，再以 `default_pipe_buf_ops` 推进 `pipe->head`。读返回 `-EFAULT` 时改成 `-EAGAIN`（`:375`），因为 `splice_read` 的调用方把 `-EAGAIN` 当成「管道里放不进任何东西」。这条用的是 [05](./05-read-write.md) 的 `read_iter`，页不是页缓存里的那一页。

页缓存文件走另一条。`generic_ro_fops.splice_read` 直接是 `filemap_splice_read`（`fs/read_write.c:32`）。ext4 的 `ext4_file_splice_read`（`fs/ext4/file.c:150`）先看强制关机，再调用它；函数表在 `:948`。xfs 的 `xfs_file_splice_read`（`fs/xfs/xfs_file.c:340`）在共享 `XFS_IOLOCK` 下调用它（`:360`），函数表在 `:1486`。两边的 `splice_write` 都是 `iter_file_splice_write`。

`filemap_splice_read`（`mm/filemap.c:2926`）按剩余槽截断长度，循环调用 `filemap_get_pages`（定义 `:2573`，这里在 `:2957` 传入 `need_uptodate = true`），再 `splice_folio_into_pipe`（`:2876`）。后者按页切开 folio，每槽一条 `pipe_buffer`：`ops` 是 `page_cache_pipe_buf_ops`，`folio_get` 多一个引用，然后 `pipe->head++`。管道满、读到 `i_size`、或这一轮没有塞进任何字节就停。返回值可以短于请求：管道空间、文件尾、洞，都会让这一次 splice 提前结束。folio 怎样进入 `address_space`、脏页怎样回写，见 [MM §08](../memory-management/08-page-cache-and-writeback.md)。

tmpfs 不用 `filemap_splice_read`。`shmem_file_splice_read`（`mm/shmem.c:2934`，装在 `:4498`）调用 `shmem_get_folio(..., SGP_READ)`。拿到 folio 就 `splice_folio_into_pipe`，随后 `folio_put`：净引用留在管道那次 `folio_get` 上。洞（`folio == NULL`）走 `splice_zeropage_into_pipe`（`:2912`），槽里的页是 `ZERO_PAGE(0)`，ops 是 `zero_pipe_buf_ops`。

`splice_to_pipe`（`fs/splice.c:197`）把 `splice_pipe_desc` 里的页和 `partial` 填进空槽，没有读者就发 `SIGPIPE` 并返回 `-EPIPE`（`:209`），一个字节都没填进去则返回 `-EAGAIN`（`:235`）。6.6 的 `filemap_splice_read`、`copy_splice_read`、`shmem_file_splice_read` 都是直接写 `pipe->head`，不经过它。它仍是导出的 `EXPORT_SYMBOL_GPL`。relay 在 `kernel/relay.c:1188` 调用它。`vmsplice` 进管道用的是 `add_to_pipe`（`splice.c:245`），不是 `splice_to_pipe`。

出管道、进文件是 `do_splice_from`（`:928`）：没有 `f_op->splice_write` 就 `warn_unsupported`，返回 `-EINVAL`。有就调用那个函数。通用实现 `iter_file_splice_write`（`:666`）在持管道锁的循环里：

```text
splice_from_pipe_next          管道空：没写者或已经拷过则返回 0；NONBLOCK 则 -EAGAIN
pipe_buf_confirm
把槽收成 ITER_SOURCE 的 bio_vec
vfs_iter_write                 fs/read_write.c:896，进 f_op->write_iter
吃掉已经写走的字节：整槽 release，半槽改 offset/len
```

`sd.flags` 保存了传入的旗标，函数体没有再读 `SPLICE_F_MOVE`。写进目标文件的是 `vfs_iter_write` 那一次拷贝。`do_iter_write`（`:841`）成功时 `fsnotify_modify`，不调用 `add_wchar`，也不 `inc_syscw`。`page_cache_pipe_buf_try_steal` 仍然挂在页缓存 ops 上，这条通用写不调用 `pipe_buf_try_steal`。源 folio 留在原来的地址空间里，管道多持有的引用在 `release` 时放下。槽里的字节就是缓存里的字节，脏页也如此；这一步不先做回写。目标的 `write_iter` 把字节写进目标页缓存并标脏。

---

## 5b.5 文件到文件：`splice_direct_to_actor` / `do_splice_direct`

`splice(2)` 拒绝两个普通文件。内核里另有一条不暴露管道 fd 的路，给 `sendfile` 和 `generic_copy_file_range` 用。

```c
/* include/linux/splice.h:29 */
struct splice_desc {
	size_t total_len;          /* 还要搬多少 */
	unsigned int len;          /* 当前这一槽的长度 */
	unsigned int flags;        /* SPLICE_F_* */
	union {
		void __user *userptr;
		struct file *file; /* 目标文件 */
		void *data;
	} u;
	void (*splice_eof)(struct splice_desc *sd); /* 输入提前结束时的回调 */
	loff_t pos;                /* 输入位置 */
	loff_t *opos;              /* 输出位置。sendfile 传进来 */
	size_t num_spliced;        /* 已经搬到输出的字节 */
	bool need_wakeup;          /* 要叫醒管道写者 */
};
```

`splice_direct_to_actor`（`fs/splice.c:1010`）要求输入有 `FMODE_LSEEK`（`:1023`），避免把套接字这类不能定位的 fd 接成「文件到文件」。管道的 `open` 走 `stream_open`（`fs/pipe.c:955`、`fifo_open` 在 `:1138`），`stream_open` 清掉 `FMODE_LSEEK`（`fs/open.c:1655`）。所以管道不能当 direct splice 的输入。

内部管道放在 `current->splice_pipe`（`include/linux/sched.h:1337`）。这个字段的注解已经在 [process 02](../process-management/02-task-struct.md)。第一次用时 `alloc_pipe_info`，并把 `readers` 设成 1（`splice.c:1041`）。源码注释写的是紧接着 `splice_to_pipe` 把数据读走；6.6 的循环实际调用 `vfs_splice_read`。`readers = 1` 让「没有读者就发 `SIGPIPE`」的检查通过。这个任务之后的 direct splice 继续用同一根管道。

循环（`:1065`）：

```text
vfs_splice_read(in → 内部管道)     旗标仍可带 SPLICE_F_NONBLOCK，只约束输入
actor(管道 → 输出)                 direct_splice_actor 调用 do_splice_from
输出侧事先清掉 SPLICE_F_NONBLOCK    内部管道必须被抽干，不能卡在 -EAGAIN
输出短于刚读进的字节：释放管道里剩下的槽，按实际写出的量改输入位置
```

`direct_splice_actor`（`:1137`）把 `sd->u.file` 和 `sd->opos` 交给 `do_splice_from`。`do_splice_direct`（`:1170`）拒绝输出没有 `FMODE_WRITE` 或带了 `O_APPEND`（`:1187`），`rw_verify_area(WRITE)` 之后调用上面的 actor。返回正数时把输入偏移写回 `*ppos`（`:1196`）。输入在请求还没读完时得到 0，并且调用者当初没有 `SPLICE_F_MORE`、也已经写出过字节，会走 `splice_eof`（`:1116`）。文件上的 `f_op->splice_eof` 由此被调用（`direct_file_splice_eof`，`:1146`）。

---

## 5b.6 `do_sendfile`

`do_sendfile`（`fs/read_write.c:1180`）是 `sendfile`（`:1289`）和 `sendfile64`（`:1308`）的公共体。x86-64 的系统调用表把 40 号 `sendfile` 指到 `sys_sendfile64`（`arch/x86/entry/syscalls/syscall_64.tbl:51`）。demo 用的 `sys/sendfile.h` 在 `__USE_FILE_OFFSET64` 下把 `sendfile` 重定向到 `sendfile64`。

```text
fdget 输入，要 FMODE_READ
有显式偏移：还要 FMODE_PREAD，否则 -ESPIPE
rw_verify_area(READ)，count 截到 MAX_RW_COUNT
fdget 输出，要 FMODE_WRITE
输出不是管道：
  rw_verify_area(WRITE)
  file_start_write
  do_splice_direct
  file_end_write
输出是管道：
  输出 O_NONBLOCK 则或上 SPLICE_F_NONBLOCK
  splice_file_to_pipe
成功：
  add_rchar / add_wchar          include/linux/sched/xacct.h:12、:17
  fsnotify_access / fsnotify_modify
  写回输出 f_pos；显式偏移写回输入，否则写输入 f_pos
inc_syscr / inc_syscw            :1276、:1277。splice 返回之后才加
```

参数错误在走到 splice 之前就 `goto fput_out` / `fput_in`，那几条路径不加 `syscr`。`do_splice_direct` 返回负数时，函数仍会执行这两次自增，然后把这个负数返回给调用者。

`fl` 先是 0。源文件里有一段 `#if 0`（`:1238`）：没有把输入的 `O_NONBLOCK` 变成 `SPLICE_F_NONBLOCK`。注释写明 man page 对输出的 `EAGAIN` 还有争议，所以文件到文件这条目前不按输入的非阻塞旗标来。只有输出本身是管道且带 `O_NONBLOCK` 时才或上这个旗标（`:1258`）。

带 32 位偏移指针的 `sendfile` 把 `max` 设成 `MAX_NON_LFS`（`fs.h:1050`，`(1UL<<31) - 1`）。`sendfile64` 和偏移指针为 `NULL` 的 `sendfile` 传 `max = 0`，`do_sendfile` 再改用两个超级块 `s_maxbytes` 的较小值（`:1228`）。位置加上计数越过这个上限时，能缩短就缩短，起点已经越界则 `-EOVERFLOW`（`:1230`）。

`add_rchar` 加的是 `rchar`，`inc_syscr` 加的是 `syscr`。这两个字段在 `CONFIG_TASK_XACCT` 下才存在（`include/linux/task_io_accounting.h:13`）。`/proc/<pid>/io` 按这个顺序打印（`fs/proc/base.c:3042`）。`read_bytes` / `write_bytes` 是另一组，由 `task_io_account_read` / `task_io_account_write` 累加（`include/linux/task_io_accounting_ops.h:11`、`:25`），记的是存储层字节，要 `CONFIG_TASK_IO_ACCOUNTING`。走到 splice 返回的那次 `sendfile` 让 `syscr` 和 `syscw` 各加 1。用户态没有调用 `read` / `write`，这两个计数仍然增加。

---

## 5b.7 `vfs_copy_file_range` / `generic_copy_file_range`

系统调用 `copy_file_range`（`fs/read_write.c:1558`）要求 `flags == 0`（`:1592`），否则 `-EINVAL`。`COPY_FILE_SPLICE`（`include/linux/fs.h:1909`，值 `1 << 0`）的注释写明：这些旗标不经过系统调用给用户，`COPY_FILE_SPLICE` 表示直接走 splice，不走文件系统的克隆或 copy 方法。内核调用者（例如 nfsd）才能置它。

`generic_copy_file_checks`（`:1404`）先做 `generic_file_rw_checks`（`:1703`）：两端都得是普通文件，目录返回 `-EISDIR`，管道和套接字返回 `-EINVAL`，输出 `O_APPEND` 或缺少读写模式返回 `-EBADF`。然后：

```text
flags 含 COPY_FILE_SPLICE          允许跨超级块
否则输出有 copy_file_range         两端的函数指针必须是同一个，否则 -EXDEV
否则两端超级块不同                 -EXDEV
目标 IMMUTABLE                     -EPERM
任一端是 swap 文件                 -ETXTBSY
同一文件上区间重叠                 -EINVAL
起点已经在 i_size 之外             把长度改成 0
```

长度还会被 `generic_write_check_limits` 按 `RLIMIT_FSIZE` 和 `s_maxbytes` 缩短（`:1456`）。

`vfs_copy_file_range`（`:1475`）在 `file_start_write` 之后：

```text
没有 COPY_FILE_SPLICE，且输出有 f_op->copy_file_range
  调用它，然后 goto done
  返回值原样交给调用者，这里不再改走 splice

没有 COPY_FILE_SPLICE，两端同一超级块，且输入有 remap_file_range
  remap_file_range(..., REMAP_FILE_CAN_SHORTEN)    fs.h:1892
  返回 > 0：结束
  返回 0 或负数：丢掉这个返回值，继续下面的字节拷贝

generic_copy_file_range          :1386
  do_splice_direct，长度先截到 MAX_RW_COUNT
```

`generic_copy_file_range` 开头 `lockdep_assert(sb_write_started(...))`（`:1390`）。它假定调用者已经 `file_start_write`。`vfs_copy_file_range` 在 `:1501` 满足这一点。

`ret > 0` 时 `add_rchar` / `add_wchar`（`:1544`）。检查通过并执行过 `file_start_write` 之后，`done` 标签上总会 `inc_syscr` / `inc_syscw`（`:1549`），包括拷贝函数返回负数的情况。更早的 `-EINVAL` / `-EXDEV` 在 `file_start_write` 之前返回，不加这两项。

6.6.160 的 xfs 函数表有 `remap_file_range = xfs_file_remap_range`（`fs/xfs/xfs_file.c:1501`）。同一份树里 `fs/ext4/file.c` 的 `file_operations` 没有 `copy_file_range`，也没有 `remap_file_range`，同盘的 `copy_file_range(2)` 会落到 `do_splice_direct`。tmpfs 的 `shmem_file_operations`（`mm/shmem.c:4489`）同样没有这两个指针，所以两个 tmpfs 普通文件走 splice 拷贝。区间在磁盘上怎么克隆，留给具体文件系统，本篇停在 VFS 选择哪一个函数指针。

---

## 5b.8 限制

| 现象 | 6.6.160 里的结果 |
|---|---|
| 一次 splice 进管道，文件比管道大 | 按剩余槽数截断。默认 16 页。调用方循环 |
| 管道空，且没有写者；或请求已经部分完成 | 返回 0。0 是 EOF 或「这次就这些」，要和 `-EAGAIN` 分开 |
| `SPLICE_F_NONBLOCK`，管道空或满 | `-EAGAIN` |
| 没有 `splice_read` / `splice_write` | `warn_unsupported`，`-EINVAL`，`pr_debug_ratelimited` |
| 两个普通文件 `splice(2)` | `-EINVAL` |
| 管道端传入偏移指针 | `-ESPIPE` |
| 输出 `O_APPEND` | `do_splice` 与 `do_splice_direct` 都是 `-EINVAL` |
| 输入 `O_DIRECT` 或 DAX | `copy_splice_read`，不把页缓存 folio 挂进槽 |
| `SPLICE_F_MOVE` | 旗标还在。`iter_file_splice_write` 不看它 |
| `copy_file_range` 的用户态 flags 非 0 | `-EINVAL`。`COPY_FILE_SPLICE` 进不了系统调用 |
| 跨超级块，且两边没有同一个 `copy_file_range` | `-EXDEV` |
| 同一文件上源和目标区间重叠 | `-EINVAL` |

管道容量不够时，用户态必须循环。`sendfile` 和 `generic_copy_file_range` 的循环在内核里：`splice_direct_to_actor` 自己把内部管道抽干，一次系统调用可以盖住比 16 页更大的长度，直到 `MAX_RW_COUNT` 或文件尾。

---

## 5b.9 边界

`pipe(2)` 在 `fs/pipe.c:1040`，`pipe2(2)` 在 `:1035`，都进 `do_pipe2`（`:1013`）再 `alloc_pipe_info`。创建、fifo 的读者写者计数、`poll`，本篇不展开。

`splice_to_socket`（`fs/splice.c:791`，包在 `CONFIG_NET` 里）是管道到套接字的 `splice_write`。字节怎样进 TCP，留给网络章。

`tee(2)` 只接两根不同的管道（`do_tee`，`:1901`）。`link_pipe`（`:1810`）给输出槽一份 `pipe_buffer`，并用 `pipe_buf_get` 多拿引用；推进的是局部的 `i_tail`，不写 `ipipe->tail`，输入管道仍留着原来的槽。这是共享页，不是把字节再拷一份。

`vmsplice(2)`（`:1532`）从用户内存进管道时走 `iter_to_pipe`（`:1382`），里面是 `iov_iter_get_pages2`（`lib/iov_iter.c:1159`）。那一次怎么钉用户页，见 [MM 07b](../memory-management/07b-gup-and-pin.md)。反方向 `pipe_to_user`（`:1429`）是 `copy_page_to_iter`，注释写明这里放弃了真正的 splice，做成普通拷贝（`:1516`）。

`io_uring` 的 splice 操作数和固定缓冲不在这里。固定缓冲的页从哪来，见 [block-io 12](../block-io/12-io-uring.md) 和 MM 07b。

---

## 5b.10 Demo

[demos/01_splice_sendfile_copy.c](./demos/01_splice_sendfile_copy.c) 在 tmpfs 上写 256KiB 的模式，再分别用 4KiB 的 `read`+`write`、一根管道上的 `splice`、`sendfile`、`copy_file_range` 各拷一份，最后逐块比较。两个普通文件直接 `splice` 的那一次单独做，期望 `EINVAL`。

测量窗口前后各读一次 `/proc/self/io`。起点那次 `read` 的返回长度会进 `rchar`，并让 `syscr` 加 1。打印的数字已经减去这两项。`wchar`、`read_bytes`、`write_bytes` 不受这次读影响。

> **sampled on 6.12**。2026-11-04；Linux 6.12.94+；gcc 13.3.0；glibc 2.39。目录 `/dev/shm`，`statfs` 的 magic 是 `TMPFS_MAGIC`（`0x01021994`）。这台机器的 `/tmp` 是 ext4，没有拿来跑。符号和调用链仍以 v6.6.160 为准。下面的计数在 6.12 上采到，和 6.6 源码里 `add_rchar` / `inc_syscr` 的调用点一致；没有在 6.6 上重跑这份程序。

```text
fstype_magic=0x1021994 tmpfs=1 dir=/dev/shm/splice-demo-8s0KKn bytes=262144 pipe_sz=65536
splice_file_to_file=EINVAL errno=22
method=read_write ok=1 rchar=262144 wchar=262144 syscr=64 syscw=64 read_bytes=0 write_bytes=0 snap_bytes=99
method=splice ok=1 rchar=0 wchar=0 syscr=0 syscw=0 read_bytes=0 write_bytes=0 snap_bytes=103
method=sendfile ok=1 rchar=262144 wchar=262144 syscr=1 syscw=1 read_bytes=0 write_bytes=0 snap_bytes=103
method=copy_file_range ok=1 rchar=262144 wchar=262144 syscr=1 syscw=1 read_bytes=0 write_bytes=0 snap_bytes=103
calls read_write=64/64 splice=8 sendfile=1 copy_file_range=1
match read_write=1 splice=1 sendfile=1 copy_file_range=1
```

`pipe_sz=65536` 是默认 16 个槽乘 4096。256KiB 要填四次管道，每次「文件→管道」加「管道→文件」共 8 次 `splice`。`read`+`write` 的 4KiB 缓冲是 64 次读和 64 次写，`rchar` / `wchar` 各等于文件长度。

`splice` 的四个字符计数都是 0，文件内容仍然一致。`do_splice` 不调用 `add_rchar`，`iter_file_splice_write` 走到的 `vfs_iter_write` 也不调用 `add_wchar`。字节在内核里从源页缓存进了管道槽，再由 `write_iter` 写进目标。

`sendfile` 和 `copy_file_range` 各一次系统调用就拷完全长：循环在 `splice_direct_to_actor` 里。`rchar` 和 `wchar` 仍是 262144，因为 `do_sendfile`（`:1265`）和 `vfs_copy_file_range`（`:1544`）调用了 `add_rchar` / `add_wchar`。`syscr` / `syscw` 各加 1，来自这两处的 `inc_syscr` / `inc_syscw`，不是因为用户调用了 `read` / `write`。tmpfs 没有 bio，`read_bytes` / `write_bytes` 保持 0。

```bash
cd vfs/demos
gcc -Wall -Wextra -O2 -o /tmp/splice_demo 01_splice_sendfile_copy.c
/tmp/splice_demo
```

RECIPE，bpftrace / kprobe。这次环境没有 `bpftrace`，`/sys/kernel/tracing/kprobe_events` 是 `Permission denied`，下面没有执行，也没有输出。

`do_splice` 不是 `static`（`fs/splice.c:1243`），kprobe 用这个名字。`do_sendfile` 是 `static`（`fs/read_write.c:1180`），kallsyms 里不一定有这个符号；sendfile 用系统调用跟踪点。x86-64 进入的是 `sendfile64`（`SYSCALL_DEFINE4(sendfile64)`，`:1308`）。字段名来自 `syscall_enter_define_fields`（`kernel/trace/trace_syscalls.c:270`），参数名就是 `SYSCALL_DEFINE` 的形参。脚本在 [demos/splice_kprobe.bt](./demos/splice_kprobe.bt)。

```bash
# RECIPE
bpftrace vfs/demos/splice_kprobe.bt
```

---

## 5b.11 函数表

| 名字 | 位置 | 作用 |
|---|---|---|
| `PIPE_DEF_BUFFERS` | `include/linux/pipe_fs_i.h:5` | 默认 16 槽 |
| `struct pipe_buffer` | `:26` | 一槽：页、偏移、长度、ops |
| `struct pipe_inode_info` | `:58` | 环、`head`/`tail`、读者写者 |
| `struct pipe_buf_operations` | `:95` | `confirm` / `release` / `try_steal` / `get` |
| `PIPE_SIZE` | `:259` | `PAGE_SIZE` |
| `alloc_pipe_info` | `fs/pipe.c:788` | 分配环。软限制下可能只给 2 槽 |
| `pipe2` / `pipe` | `:1035` / `:1040` | 系统调用，进 `do_pipe2` |
| `pipefifo_fops` | `:1227` | 管道的 `f_op`。有 `splice_write`，没有 `splice_read` |
| `get_pipe_info` | `:1382` | `f_op` 必须是 `pipefifo_fops` |
| `SPLICE_F_MOVE` 等 | `include/linux/splice.h:17` | 系统调用旗标 |
| `struct splice_desc` | `:29` | actor 的长度、旗标、文件、偏移 |
| `splice_to_pipe` | `fs/splice.c:197` | 把 `splice_pipe_desc` 填进环 |
| `copy_splice_read` | `:320` | `O_DIRECT` / DAX，或没有页缓存可挂时 |
| `iter_file_splice_write` | `:666` | 管道 → 文件的通用 `splice_write` |
| `do_splice_from` | `:928` | 调用 `f_op->splice_write` |
| `vfs_splice_read` | `:962` | 文件 → 管道 |
| `splice_direct_to_actor` | `:1010` | 任务私有管道上的文件 → 文件 |
| `do_splice_direct` | `:1170` | `sendfile` 和通用 `copy_file_range` 的底层 |
| `splice_file_to_pipe` | `:1223` | 锁管道、等空位、`vfs_splice_read` |
| `do_splice` | `:1243` | 四种组合 |
| `splice` | `:1568` | 系统调用 |
| `splice_pipe_to_pipe` | `:1673` | 管道 → 管道，整槽挪走或拆开共享 |
| `do_tee` | `:1901` | 两根管道共享页。点到 |
| `do_sendfile` | `fs/read_write.c:1180` | `sendfile` / `sendfile64` |
| `sendfile64` | `:1308` | x86-64 的 `sendfile` 入口 |
| `generic_copy_file_range` | `:1386` | `do_splice_direct` |
| `vfs_copy_file_range` | `:1475` | 文件系统方法、remap，或 splice |
| `copy_file_range` | `:1558` | 系统调用。`flags` 必须是 0 |
| `COPY_FILE_SPLICE` | `include/linux/fs.h:1909` | 仅内核。跳过克隆，直接 splice |
| `REMAP_FILE_CAN_SHORTEN` | `fs.h:1892` | remap 可以短于请求 |
| `MAX_RW_COUNT` | `fs.h:2475` | 单次读写上限，按页对齐的 `INT_MAX` |
| `add_rchar` / `inc_syscr` | `include/linux/sched/xacct.h:12` / `:22` | `/proc/pid/io` 的 `rchar` / `syscr` |
| `splice_folio_into_pipe` | `mm/filemap.c:2876` | folio 拆进管道槽 |
| `filemap_splice_read` | `:2926` | 页缓存 → 管道 |
| `shmem_file_splice_read` | `mm/shmem.c:2934` | tmpfs，洞用 `ZERO_PAGE` |
| `current->splice_pipe` | `include/linux/sched.h:1337` | direct splice 的私有管道 |

---

[上一节 读写](./05-read-write.md) · [目录](./README.md) · [下一节 mmap](./06-mmap-and-file.md)
