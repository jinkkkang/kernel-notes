# 6. pipe：创建和读写唤醒

**结论**：`pipe2` 造出一对 `struct file`，共用一根 `pipe_inode_info`；读空、写满时在 `rd_wait` / `wr_wait` 上独占睡眠，写端或读端用 `wake_up_interruptible_sync_poll` 把 `EPOLLIN` / `EPOLLOUT` 送给 epoll。

> 系统调用：`fs/pipe.c` 的 `pipe2`（`:1035`）、`pipe`（`:1040`）。
> `pipe_buffer` 和 `pipe_inode_info` 的字段已经按头文件顺序注解在 [vfs/05b](../vfs/05b-splice-and-zerocopy.md)。本章不重复那两块结构，只写创建、睡眠和唤醒。splice 把页放进槽的路径也在那一节。

---

## 6.1 `pipe2` → `alloc_pipe_info`

```text
pipe2(fildes, flags)              SYSCALL_DEFINE2，pipe.c:1035
  do_pipe2                        :1013
    __do_pipe_flags               :960
      create_pipe_files           :917
        get_pipe_inode            :877
          alloc_pipe_info         :788
        alloc_file_pseudo         写端，O_WRONLY | (NONBLOCK|DIRECT)
        alloc_file_clone          读端，O_RDONLY | NONBLOCK
        两端 private_data = inode->i_pipe
        stream_open
      get_unused_fd_flags × 2
    copy_to_user 两个 fd
    fd_install
pipe(fildes)                      :1040，等价于 do_pipe2(fildes, 0)
```

`__do_pipe_flags` 只接受 `O_CLOEXEC | O_NONBLOCK | O_DIRECT | O_NOTIFICATION_PIPE`（`:965`）。`O_DIRECT` 让管道变成包模式：`pipe_write` 给槽置 `PIPE_BUF_FLAG_PACKET`（`:532`）。`O_NOTIFICATION_PIPE` 走 `watch_queue_init`（`:926`）。那是通知管道，`pipe_write` 对它直接返回 `-EXDEV`（`:437`）。本章不展开 `watch_queue`。

`alloc_pipe_info`（`:788`）从 `PIPE_DEF_BUFFERS`（`include/linux/pipe_fs_i.h:5`，16）个槽起算。槽数乘 `PAGE_SIZE` 超过 `pipe_max_size` 且调用者没有 `CAP_SYS_RESOURCE` 时，缩到 `pipe_max_size >> PAGE_SHIFT`。非特权用户的管道页记账超过软限制时，新管道缩到 `PIPE_MIN_DEF_BUFFERS`（2，`pipe.c:48`）。超过硬限制则创建失败。成功时 `max_usage`、`ring_size`、`nr_accounted` 都等于这个槽数，并 `init_waitqueue_head` 两把等待队列（`:817`）。

`pipe_max_size` 的初值是 1048576（`:54`）。sysctl `fs/pipe-max-size`、`fs/pipe-user-pages-hard`、`fs/pipe-user-pages-soft` 注册在 `:1476`，`register_sysctl_init("fs", ...)` 在 `:1514`。软限制的初值是 `PIPE_DEF_BUFFERS * INR_OPEN_CUR`（`:60`）。本机读数见 [09](./09-observability.md)。

容量在运行时用 `fcntl(F_SETPIPE_SZ)` / `F_GETPIPE_SZ`。`pipe_fcntl`（`:1393`）里 `F_GETPIPE_SZ` 返回 `max_usage * PAGE_SIZE`（`:1408`）。`F_SETPIPE_SZ` 进 `pipe_set_size`（`:1331`）。

两端的 `f_op` 都是 `pipefifo_fops`（`:1227`）：`read_iter = pipe_read`，`write_iter = pipe_write`，`poll = pipe_poll`。`get_pipe_info` 靠 `f_op == &pipefifo_fops` 识别管道，不能只看 `inode->i_pipe`，原因在 vfs/05b。

---

## 6.2 `pipe_read` / `pipe_write`：独占睡，sync 唤醒

空和满的判断在锁外用 `READ_ONCE` 读 `head` / `tail`：`pipe_readable`（`:221`）是「有数据，或者已经没有写端」；`pipe_writable`（`:406`）是「未满，或者已经没有读端」。

`pipe_read`（`:231`）持管道 mutex（`__pipe_lock`）。缓冲区非空就把槽里的字节 `copy_page_to_iter` 给用户。没有任何字节可读、还有写端、且不是非阻塞时，放下 mutex，必要时先把写端叫醒，然后：

```c
/* fs/pipe.c:379 */
wait_event_interruptible_exclusive(pipe->rd_wait, pipe_readable(pipe));
```

被信号打断返回 `-ERESTARTSYS`。读完若一开始管道是满的，退出时：

```c
/* fs/pipe.c:390 */
wake_up_interruptible_sync_poll(&pipe->wr_wait, EPOLLOUT | EPOLLWRNORM);
```

若这次读完仍有数据、并且循环里曾经把别的读者留在后面（`wake_next_reader`），再 sync 唤醒 `rd_wait` 上的下一个独占读者（`:393`）。

`pipe_write`（`:417`）对称。没有读端时给当前进程发 `SIGPIPE` 并返回 `-EPIPE`（`:446`）。能塞进最后一个可合并槽就合并；否则新取一页，在 `rd_wait.lock` 下把 `head` 加一，再拷贝。管道满且非非阻塞时：

```c
/* fs/pipe.c:574 */
if (was_empty)
	wake_up_interruptible_sync_poll(&pipe->rd_wait, EPOLLIN | EPOLLRDNORM);
wait_event_interruptible_exclusive(pipe->wr_wait, pipe_writable(pipe));
```

函数末尾（`:587` 的注释）说明 sync 唤醒是为了让读者马上把数据拿走，短写尤其如此。然后：

```c
/* fs/pipe.c:599 */
if (was_empty || pipe->poll_usage)
	wake_up_interruptible_sync_poll(&pipe->rd_wait, EPOLLIN | EPOLLRDNORM);
if (wake_next_writer)
	wake_up_interruptible_sync_poll(&pipe->wr_wait, EPOLLOUT | EPOLLWRNORM);
```

`wake_up_interruptible_sync_poll`（`include/linux/wait.h:248`）展开成 `__wake_up_sync_key(wq, TASK_INTERRUPTIBLE, poll_to_key(m))`。`nr_exclusive` 为 1，`wake_flags` 为 `WF_SYNC`。epoll 的 `ep_poll_callback` 收到这个 `key`，掩码里带 `EPOLLIN` 或 `EPOLLOUT`，于是匹配兴趣、入 `rdllist`、再 `wake_up_sync(&ep->wq)`。这就是 epoll 看见管道可读可写的来源。

`pipe_wait_readable` / `pipe_wait_writable`（`:1054`、`:1061`）是另一对帮助函数：放下 `pipe->mutex` 之后用普通的 `wait_event_interruptible`（非独占）再锁上。splice 那一侧用它们。`pipe_read` / `pipe_write` 自己用的是上面的独占等待，并在没把缓冲读完或写完时用 `wake_next_reader` / `wake_next_writer` 把名额让给下一个。

---

## 6.3 `pipe_poll` 与 `poll_usage`

```c
/* fs/pipe.c:655 */
pipe_poll(struct file *filp, poll_table *wait)
{
	WRITE_ONCE(pipe->poll_usage, true);          /* :662。一旦被 poll，就保持为真 */

	if (filp->f_mode & FMODE_READ)
		poll_wait(filp, &pipe->rd_wait, wait);
	if (filp->f_mode & FMODE_WRITE)
		poll_wait(filp, &pipe->wr_wait, wait);

	head = READ_ONCE(pipe->head);
	tail = READ_ONCE(pipe->tail);
	/* 读端：非空 → EPOLLIN|EPOLLRDNORM；没有写端且 w_counter 变过 → EPOLLHUP */
	/* 写端：未满 → EPOLLOUT|EPOLLWRNORM；没有读端 → EPOLLERR */
}
```

`poll_usage` 是 `pipe_inode_info` 的一个 `bool`（`include/linux/pipe_fs_i.h:74`）。`pipe_write` 末尾的注释写的是：epoll 无论这次写入之前管道是不是空的，都要一次唤醒。没有人 poll 过、且写入前已经有数据时，读者本来就醒着或已经被之前的写入叫醒，这里可以省掉一次 `rd_wait` 唤醒。epoll 的 `ADD` 和之后每次 `epoll_wait` 复查都会进 `pipe_poll`，把这个位写成 true，于是之后每次写都唤醒 `rd_wait`。

`epoll_wait` 复查时 `poll_table._qproc` 为 NULL，`poll_wait` 不会再挂新的 `eppoll_entry`。`poll_usage` 的写入发生在 `poll_wait` 之前，复查一样会把它置上。

读端和写端是两个 `struct file`，`f_mode` 不同。只把读端放进 epoll 时，`pipe_poll` 只对 `rd_wait` 调用 `poll_wait`，`ep_ptable_queue_proc` 只分配一个 `eppoll_entry`。写端的 `EPOLLOUT` 要另加写端 fd。

---

## 6.4 本机：写 1 字节，边缘触发一次

[demos/02-pipe-wakeup.c](./demos/02-pipe-wakeup.c) 用 `pipe2(O_CLOEXEC)` 建管道，把读端以 `EPOLLIN | EPOLLET` 加入 epoll，再写 1 字节 `'Z'`。读端保持阻塞。`epoll_wait` 超时 0 用在写入之前和读完之后。

sampled on 6.12.113+：

```text
kernel=6.12.113+
before_write hits=0 events=0x0
after_write hits=1 events=0x1
read_byte=Z
after_read hits=0 events=0x0
F_GETPIPE_SZ=65536
```

`events=0x1` 是 `EPOLLIN`。写入前和读完之后的 0 次命中，对应 ET：没有新的 `wake_up` 就不再返回。`F_GETPIPE_SZ=65536` 与 `PIPE_DEF_BUFFERS * PAGE_SIZE` 一致（本机 `PAGE_SIZE` 为 4096，16 × 4096）。fdinfo 全文在 [09](./09-observability.md)。

---

## 6.5 和 splice 的分界

零拷贝交接、`pipe_buffer` 的 `ops`、`splice` / `sendfile` / `copy_file_range` 怎样移动槽，见 [vfs/05b](../vfs/05b-splice-and-zerocopy.md)。那些路径在管道空或满时调用 `pipe_wait_readable` / `pipe_wait_writable`，唤醒仍是同一对 `rd_wait` / `wr_wait`。本章停在 `pipe_read` / `pipe_write` 和 `pipe_poll`。
