# 12. io_uring

**结论**：io_uring 让用户态用一块共享内存里的提交环（SQ）和完成环（CQ）批量提交 IO。读写操作码最终仍是 `kiocb` + `read_iter` / `write_iter`，进到和系统调用相同的 `direct_IO` 或页缓存路径；固定缓冲和固定文件省掉每次提交时的查找和钉页。

> 用户接口在 `include/uapi/linux/io_uring.h`。实现在 `io_uring/` 目录，不在 `fs/io_uring.c`。系统调用 `io_uring_setup`（`io_uring/io_uring.c:4204`）、`io_uring_enter`（`:3719`）、`io_uring_register`（`:4670`）。操作码表在 `io_uring/opdef.c`。

C 新手先记两个环。SQE（submission queue entry）是用户填的一张「请做这件事」的表项。CQE（completion queue entry）是内核填的「这件事的结果」。两边用头尾下标推进，不需要每次 `read` / `write` 都进一次内核。`io_uring_enter` 仍是一次系统调用，但它可以一次提交多个 SQE，并一次取回多个完成。

本章只写会碰到块层的部分：读写、固定缓冲、固定文件、IOPOLL、`URING_CMD`。网络操作码（send/recv/accept）停在名字上。

---

## 12.1 三个系统调用

```text
io_uring_setup(entries, params)          io_uring.c:4204
  io_uring_allowed                       :4186
    io_uring_disabled == 2：-EPERM
    == 0 或 CAP_SYS_ADMIN：允许
    == 1：调用者必须在 io_uring_group 里
  io_uring_setup                         :4160    校验 flags 和 resv
  io_uring_create                        :3949    分配 ctx、环、返回 fd

io_uring_enter(fd, to_submit, min_complete, flags, argp, argsz)   :3719
  to_submit > 0：io_submit_sqes           :2406
  IORING_ENTER_GETEVENTS：等到至少 min_complete 个 CQE

io_uring_register(fd, opcode, arg, nr_args)   :4670
  固定缓冲、固定文件、事件 fd、探针等
```

`/proc/sys/kernel/io_uring_disabled` 和 `io_uring_group` 由 `register_sysctl_init("kernel", ...)` 登记（`:4811`）。`disabled` 的取值被 `proc_dointvec_minmax` 夹在 0 和 2（`:165`）。这是 6.6 里能从 proc 读到的 io_uring 开关，不是每环的统计。

`params->flags` 里和块 IO 有关的（`io_uring.h:140` 起）：

| 宏 | 位 | 含义 |
|---|---|---|
| `IORING_SETUP_IOPOLL` | 0 | 完成靠轮询，不靠 IRQ。对应块层 `REQ_POLLED` |
| `IORING_SETUP_SQPOLL` | 1 | 内核线程替进程提交 SQ |
| `IORING_SETUP_SQ_AFF` | 2 | `sq_thread_cpu` 有效 |
| `IORING_SETUP_SQE128` | 10 | SQE 128 字节，给 `uring_cmd` 的额外数据 |
| `IORING_SETUP_CQE32` | 11 | CQE 32 字节 |
| `IORING_SETUP_COOP_TASKRUN` | 8 | 完成时尽量不 IPI 对方 |
| `IORING_SETUP_DEFER_TASKRUN` | 13 | task_work 推迟到下一次 enter |
| `IORING_SETUP_SINGLE_ISSUER` | 12 | 只有一个任务提交 |

`io_uring_setup` 拒绝这张掩码以外的 flag（`:4172`）。6.6 的掩码到 `IORING_SETUP_NO_SQARRAY`（位 16）为止。

环在内存里的布局由 `struct io_uring_params`（`:473`）返回的偏移描述。`io_sqring_offsets`（`:423`）有 `head`、`tail`、`ring_mask`、`ring_entries`、`flags`、`dropped`、`array`。`io_cqring_offsets`（`:442`）有 `head`、`tail`、`ring_mask`、`ring_entries`、`overflow`、`cqes`、`flags`。mmap 的魔数偏移是 `IORING_OFF_SQ_RING`（0）、`IORING_OFF_CQ_RING`（`0x8000000`）、`IORING_OFF_SQES`（`0x10000000`）（`:413`）。`IORING_SETUP_NO_MMAP` 时改用用户自己提供的地址，偏移结构里的 `user_addr` 就是给这条用的。

---

## 12.2 SQE 与 CQE

```c
/* include/uapi/linux/io_uring.h:30。64 字节。SQE128 时后面还有 cmd[] */
struct io_uring_sqe {
	__u8	opcode;     /* IORING_OP_* */
	__u8	flags;      /* IOSQE_* */
	__u16	ioprio;     /* 写进 kiocb，再进 bio->bi_ioprio */
	__s32	fd;         /* 普通 fd，或 IOSQE_FIXED_FILE 时的固定文件下标 */
	union {
		__u64	off;          /* 文件偏移。FEAT_RW_CUR_POS 时 -1 表示当前偏移 */
		__u64	addr2;
		struct {
			__u32	cmd_op;   /* IORING_OP_URING_CMD 的子命令 */
			__u32	__pad1;
		};
	};
	union {
		__u64	addr;         /* 缓冲或 iovec 指针 */
		__u64	splice_off_in;
	};
	__u32	len;        /* 字节数，或 iovec 个数 */
	union {
		__u32	rw_flags;     /* RWF_* */
		__u32	fsync_flags;
		__u32	uring_cmd_flags;
		/* 同位置还有 poll、timeout、accept 等旗标 */
	};
	__u64	user_data;  /* 完成时原样回到 CQE */
	union {
		__u16	buf_index;    /* 固定缓冲下标 */
		__u16	buf_group;    /* BUFFER_SELECT 时的组 */
	} __attribute__((packed));
	__u16	personality;
	union {
		__s32	splice_fd_in;
		__u32	file_index;
		struct {
			__u16	addr_len;
			__u16	__pad3[1];
		};
	};
	union {
		struct {
			__u64	addr3;
			__u64	__pad2[1];
		};
		/* IORING_SETUP_SQE128：从这里起 80 字节命令数据 */
		__u8	cmd[0];
	};
};
```

`IOSQE_*`（`:123`）：`FIXED_FILE` 用固定文件表，`IO_DRAIN` 等前面的请求都完成，`IO_LINK` / `IO_HARDLINK` 把下一个 SQE 串成链，`ASYNC` 强制进 io-wq，`BUFFER_SELECT` 从 `buf_group` 选缓冲，`CQE_SKIP_SUCCESS` 成功时不写 CQE。

```c
/* :380。16 字节。IORING_SETUP_CQE32 时末尾再加 16 字节 big_cqe */
struct io_uring_cqe {
	__u64	user_data;  /* 来自 SQE */
	__s32	res;        /* 字节数或负的 errno */
	__u32	flags;      /* IORING_CQE_F_* */
	__u64	big_cqe[];
};
```

`IORING_CQE_F_BUFFER`（`:401`）置位时高 16 位是选中的缓冲 id。`IORING_CQE_F_MORE` 表示这条 SQE 还会再产生 CQE。`IORING_CQE_F_NOTIF` 用来区分零拷贝发送的通知。

和块层读写直接相关的操作码（`enum io_uring_op`，`:193`）：

| 操作码 | `opdef.c` 里的 issue |
|---|---|
| `IORING_OP_READV` / `READ_FIXED` / `READ` | `io_read`（`opdef.c:67`、`:98`、`:239`） |
| `IORING_OP_WRITEV` / `WRITE_FIXED` / `WRITE` | `io_write`（`:80`、`:111`、`:252`） |
| `IORING_OP_FSYNC` | 文件系统 `fsync`，不是一条新的 bio 类型 |
| `IORING_OP_URING_CMD` | `io_uring_cmd`（`:403`） |

`IORING_OP_READ` 的定义（`opdef.c:228`）标明 `plug = 1`、`ioprio = 1`、`iopoll = 1`。`plug` 表示一批读写会套在 `blk_plug` 里再 unplug，和系统调用里的 plug 是同一个 `struct blk_plug`。

---

## 12.3 从 SQE 到 kiocb

```text
io_submit_sqes                      io_uring.c:2406
  从 SQ 环取出最多 nr 个 SQE
  每个分配一个 io_kiocb（请求对象，不是块层的 request）
  def->prep                         读写是 io_prep_rw（rw.c:86）
    填 kiocb：文件、偏移、rw_flags → IOCB_*
    ioprio 写进 kiocb
    FIXED_FILE：从 ctx 的固定文件表取 struct file
  io_queue_sqe                      :2048   声明在 :148
    io_issue_sqe                    :1849
      def = io_issue_defs[opcode]
      def->issue(req, issue_flags)  读写即 io_read / io_write
```

`io_read`（`io_uring/rw.c:888`）调用 `__io_read`。那条路径准备 `iov_iter`，然后 `call_read_iter`，也就是 `file->f_op->read_iter(kiocb, iter)`。这和 `new_sync_read` 调用的是同一个函数指针。差别是 kiocb 的 `ki_complete` 不是 NULL：`io_complete_rw`（`:309`）或轮询时的 `io_complete_rw_iopoll`（`:327`）。

因此块设备上的一次 `IORING_OP_READ` 会进 `blkdev_read_iter` → `blkdev_direct_IO`。异步 kiocb 走 `__blkdev_direct_IO_async`，返回 `-EIOCBQUEUED`，`io_issue_sqe` 把这个返回值当成「先别写 CQE」（`IOU_ISSUE_SKIP_COMPLETE`，见 `io_issue_sqe` `:1890`）。bio 完成时 `ki_complete` 把字节数或错误写进 CQE。

文件在页缓存上的读写同样进 `generic_file_read_iter` / `generic_file_write_iter`。`O_DIRECT` 的文件走 [11](./11-direct-io.md) 的 iomap；没带 direct 的走页缓存，未命中才有 bio。io_uring 不发明第三条数据路径。

issue 时若 `IO_URING_F_NONBLOCK` 且文件系统返回 `-EAGAIN`，请求进 io-wq（`io_queue_iowq`，`io_uring.c:476`；工作函数 `io_wq_submit_work`，`:1920`）。io-wq 是内核线程，可以睡眠。`IOSQE_ASYNC` 则一开始就进这条。SQPOLL 线程在 `io_sq_thread`（`io_uring/sqpoll.c:223`），它替提交者调用 `io_submit_sqes`，提交者自己可以不再 enter，除非环上的 `IORING_SQ_NEED_WAKEUP` 置位。

---

## 12.4 固定缓冲和固定文件

每次 `read` 系统调用都要按用户地址找页并钉住。固定缓冲把这件事提前到 `io_uring_register`：

```text
IORING_REGISTER_BUFFERS / BUFFERS2
  io_sqe_buffers_register           io_uring/rsrc.c:970
    io_sqe_buffer_register          :876
      钉页、建 struct io_mapped_ubuf，放进 ctx->user_bufs
```

`IORING_OP_READ_FIXED` / `WRITE_FIXED` 的 SQE 用 `buf_index` 选中一块。`io_import_fixed`（`rsrc.c:1024`）把这块已经钉住的内存包成 `ITER_BVEC` 的 `iov_iter`，不再按用户地址缺页。`bio_iov_iter_get_pages` 看到的是现成的 `bio_vec`。

固定文件：

```text
IORING_REGISTER_FILES / FILES2
  io_sqe_files_register             rsrc.c:701
SQE 带 IOSQE_FIXED_FILE 时 fd 字段是下标
  io_assign_file 从文件表取 struct file，不再 fget
```

`IORING_OP_PROVIDE_BUFFERS` 是另一套：一组一次性的缓冲，完成时内核选一块，把 id 放进 CQE 的 `IORING_CQE_F_BUFFER`。登记在 `io_provide_buffers`（`io_uring/kbuf.c:421`）。它和 `REGISTER_BUFFERS` 的固定映射不是同一个表。

---

## 12.5 轮询

两层轮询不要混。

**IOPOLL**（`IORING_SETUP_IOPOLL`）轮询的是块设备的完成队列。`opdef` 里 `iopoll = 1` 的读写会把 kiocb 标成高优先级。iomap 在 `IOCB_HIPRI` 且 kiocb 异步时 `bio_set_polled`（[11](./11-direct-io.md)）。请求进 `HCTX_TYPE_POLL`。完成不靠 `nvme_irq`，而靠 `io_do_iopoll`（`io_uring/rw.c:1055`）调用驱动的 `blk_mq_ops.poll`。NVMe 的 poll 是 `nvme_poll`（`drivers/nvme/host/pci.c:1118`），virtio 的是 `virtblk_poll`（`virtio_blk.c:1259`）。

**POLL_ADD** 是另一回事：把一个文件的 poll 等待装进 io_uring，可读或可写时写一条 CQE。实现在 `io_uring/poll.c` 的 `io_poll_add`（`:964`）。套接字常用它。块设备的完成轮询不走 `io_poll_add`。

`io_arm_poll_handler`（`poll.c:711`）是第三条相关路径：非 IOPOLL 的读写返回 `-EAGAIN` 时，内核能先武装一次 poll，等设备或套接字就绪再重试，避免立刻进 io-wq。

---

## 12.6 `IORING_OP_URING_CMD`

NVMe 这类驱动可以不把命令伪装成 read/write，而实现 `file_operations.uring_cmd`（`include/linux/fs.h:1955`）。VFS 章只点到这个指针存在。

```text
io_uring_cmd_prep                 io_uring/uring_cmd.c:86
  读 uring_cmd_flags，只允许 IORING_URING_CMD_FIXED
  FIXED：buf_index 必须小于 nr_user_bufs，记下 io_mapped_ubuf
  cmd_op = sqe->cmd_op
io_uring_cmd                      :113
  没有 f_op->uring_cmd：-EOPNOTSUPP
  security_uring_cmd
  IORING_SETUP_IOPOLL 还要求 f_op->uring_cmd_iopoll，否则 -EOPNOTSUPP
  file->f_op->uring_cmd(ioucmd, issue_flags)
  返回 -EAGAIN：拷一份 SQE 到 async_data，交给 io-wq
  返回 -EIOCBQUEUED：先不写 CQE（IOU_ISSUE_SKIP_COMPLETE）
```

SQE128 时 `sqe->cmd[]` 有 80 字节给驱动。NVMe 的 passthrough 用它装 NVMe 命令字，再走驱动自己的 `queue_rq`，不一定经过 `read_iter`。块层仍是 `struct request`：`REQ_OP_DRV_IN` / `REQ_OP_DRV_OUT`（`blk_types.h:398`）就是给这种直通准备的。`submit_bio_noacct` 拒绝把这两个操作码当普通 bio 提交（`blk-core.c:815`），它们从驱动的 ioctl / uring_cmd 路径分配 request。

---

## 12.7 和经典 AIO 对照

经典 AIO 在 `fs/aio.c`，6.6 里仍然有系统调用。

| | `io_submit`（AIO） | io_uring |
|---|---|---|
| 建立 | `io_setup`（`fs/aio.c:1342`）返回 `aio_context_t` | `io_uring_setup` 返回 fd |
| 提交 | `io_submit`（`:2041`）每次从用户拷贝 `struct iocb` | SQ 在共享内存，enter 时内核读环 |
| 完成 | `io_getevents`（`:2211`）从 AIO 环拷贝 `io_event` | CQ 在共享内存，用户自己读 head/tail |
| 请求对象 | `struct kiocb`，`aio_read`（`:1540`）里 `call_read_iter` | `struct io_kiocb` 里嵌着 `kiocb`，同样 `call_read_iter` |
| 固定缓冲 | 没有对应的 register | `IORING_REGISTER_BUFFERS` |
| 块层轮询 | 没有 `IORING_SETUP_IOPOLL` 这一套 | `REQ_POLLED` + `mq_ops->poll` |
| 取消 | `io_cancel`（`:2135`），依赖 `kiocb_set_cancel_fn`（`:591`） | `IORING_OP_ASYNC_CANCEL` |

两条路在文件系统下面汇合：都是带 `ki_complete` 的 kiocb。缓冲和 direct 的分叉仍由 `IOCB_DIRECT` 决定，不由「这是 AIO 还是 io_uring」决定。

AIO 的环是内核里的 `kioctx`，用户通过 `io_getevents` 把事件拷出来。io_uring 的 CQ 用户可以自己 mmap 来读，enter 只在需要提交或睡觉等完成时才进内核。`IORING_FEAT_NODROP`（`io_uring.h:490`）表示 CQ 满时不丢事件，溢出进 `overflow` 链表，fdinfo 里能看到。tracepoint `io_uring_cqe_overflow` 在 `include/trace/events/io_uring.h:571`。

---

## 12.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/uapi/linux/io_uring.h` | `struct io_uring_sqe` | 30 | 提交项 |
| `include/uapi/linux/io_uring.h` | `struct io_uring_cqe` | 380 | 完成项 |
| `include/uapi/linux/io_uring.h` | `struct io_uring_params` | 473 | setup 的参数和环偏移 |
| `include/uapi/linux/io_uring.h` | `enum io_uring_op` | 193 | 操作码，`URING_CMD` 在 `:240` |
| `io_uring/io_uring.c` | `io_uring_setup` 系统调用 | 4204 | 建环 |
| `io_uring/io_uring.c` | `io_uring_enter` | 3719 | 提交并等待 |
| `io_uring/io_uring.c` | `io_submit_sqes` | 2406 | 从 SQ 取 SQE |
| `io_uring/io_uring.c` | `io_issue_sqe` | 1849 | 按 `io_issue_defs` 调用 issue |
| `io_uring/opdef.c` | `IORING_OP_READ` | 228 | `prep = io_prep_rw`，`issue = io_read` |
| `io_uring/rw.c` | `io_prep_rw` | 86 | SQE → kiocb |
| `io_uring/rw.c` | `io_read` / `io_write` | 888 / 918 | `read_iter` / `write_iter` |
| `io_uring/rw.c` | `io_complete_rw` | 309 | bio 完成后写 CQE |
| `io_uring/rw.c` | `io_do_iopoll` | 1055 | 轮询已提交的 HIPRI 请求 |
| `io_uring/rsrc.c` | `io_sqe_buffers_register` | 970 | 固定缓冲 |
| `io_uring/rsrc.c` | `io_import_fixed` | 1024 | 固定缓冲 → `iov_iter` |
| `io_uring/uring_cmd.c` | `io_uring_cmd` | 113 | 调 `f_op->uring_cmd` |
| `io_uring/sqpoll.c` | `io_sq_thread` | 223 | SQPOLL 内核线程 |
| `fs/aio.c` | `io_submit` | 2041 | 经典 AIO 的提交 |
| `include/linux/fs.h` | `file_operations.uring_cmd` | 1955 | 驱动挂 passthrough 的位置 |

---

**导航**：[← Direct IO](./11-direct-io.md) · [目录](./README.md) · [NVMe →](./13-nvme-walkthrough.md)
