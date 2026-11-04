# 8. FUSE

**结论**：FUSE 把 VFS 操作编成 `fuse_req`，挂到连接的 `fuse_iqueue.pending` 上，用户态守护进程从 `/dev/fuse` 把请求读走，处理完再写回答。内核在 `fuse_req->waitq` 上睡觉，直到回答到来或连接中断。容器里用它做两类事：镜像的惰性拉取（守护进程按块取 blob），以及把用户态文件系统当卷挂进挂载命名空间。队列在内核，数据路径多一次用户态往返。

> 结构在 `fs/fuse/fuse_i.h`。入队与 `/dev/fuse` 读写在 `fs/fuse/dev.c`。控制文件系统在 `fs/fuse/control.c`。

---

## 8.1 请求

```c
/* fs/fuse/fuse_i.h:275 */
struct fuse_args {
	uint64_t nodeid;
	uint32_t opcode;             /* FUSE_LOOKUP、FUSE_READ、FUSE_WRITE … */
	uint8_t in_numargs;
	uint8_t out_numargs;
	uint8_t ext_idx;
	bool force:1;
	bool noreply:1;
	bool nocreds:1;
	bool in_pages:1;
	bool out_pages:1;
	bool user_pages:1;
	bool out_argvar:1;
	bool page_zeroing:1;
	bool page_replace:1;
	bool may_block:1;
	bool is_ext:1;
	bool is_pinned:1;
	bool invalidate_vmap:1;
	struct fuse_in_arg in_args[3];
	struct fuse_arg out_args[2];
	void (*end)(struct fuse_mount *fm, struct fuse_args *args, int error);
	void *vmap_base;
};

/* :382 */
struct fuse_req {
	struct list_head list;           /* pending、processing 或 io 链表 */
	struct list_head intr_entry;     /* 中断链表 */
	struct fuse_args *args;
	refcount_t count;
	unsigned long flags;             /* FR_* ，test_bit 更新 */
	struct {
		struct fuse_in_header h;
	} in;
	struct {
		struct fuse_out_header h;
	} out;
	wait_queue_head_t waitq;         /* 等回答的任务睡在这里 */
#if IS_ENABLED(CONFIG_VIRTIO_FS)
	void *argbuf;                    /* virtio-fs 的连续缓冲 */
#endif
	struct fuse_mount *fm;
};
```

`enum fuse_req_flag`（`:360`）里和队列有关的是：`FR_PENDING`（还没到用户态）、`FR_SENT`（已在用户态，等回答）、`FR_FINISHED`、`FR_INTERRUPTED`、`FR_ABORTED`、`FR_BACKGROUND`（后台，不堵调用者）、`FR_ISREPLY`（需要回答）。

`fuse_conn`（`fuse_i.h:581`）是一条连接。队列成员是 `struct fuse_iqueue iq`（`:618`）。`max_background`（`:627`）和 `congestion_threshold`（`:630`）限制后台请求堆积。`num_waiting` 在 `:845`，设备链表 `devices` 在 `:881`。`fuse_conn` 字段很多，这里不整表展开。和容器有关的是队列、这两个上限，以及 `pid_ns`（`:600`）、`user_ns`（`:603`）。`fuse_get_req`（`fs/fuse/dev.c:106`）把请求头里的 uid/gid 用 `from_kuid(fc->user_ns, ...)` 填上，pid 用 `pid_nr_ns(..., fc->pid_ns)`（`:138`）。守护进程读到的身份是这两个命名空间里的号。

```c
/* fuse_i.h:459 */
struct fuse_iqueue {
	unsigned connected;
	spinlock_t lock;
	wait_queue_head_t waitq;             /* /dev/fuse 的读者睡在这里 */
	u64 reqctr;
	struct list_head pending;            /* 待用户态读取 */
	struct list_head interrupts;
	struct fuse_forget_link forget_list_head;
	struct fuse_forget_link *forget_list_tail;
	int forget_batch;
	struct fasync_struct *fasync;
	const struct fuse_iqueue_ops *ops;   /* 默认 fuse_dev_fiq_ops */
	void *priv;
};
```

`/dev/fuse` 每个打开者还有自己的 `fuse_dev`（`:515`），里面的 `fuse_pqueue pq` 放正在处理的请求哈希和 io 链表。一个连接可以有多个设备 fd（`FUSE_DEV_IOC_CLONE`，`fs/fuse/dev.c:2294`）。

---

## 8.2 同步请求怎么走

目录、属性、一部分读写走 `fuse_simple_request`（`fs/fuse/dev.c:485`）：

```text
fuse_simple_request                fs/fuse/dev.c:485
  fuse_get_req                     分配 fuse_req，连接中断则返回错误
  fuse_args_to_req                 把 fuse_args 填进 req
  __fuse_request_send              :410
    链上 fiq->pending
    ops->wake_pending_and_unlock   叫醒 /dev/fuse 的读者
    调用者在 req->waitq 上等待
fuse_dev_read                      :1371    守护进程 read /dev/fuse
  fuse_dev_do_read                 :1222
    在 fiq->waitq 上等到 pending 非空
    把请求从 pending 搬到本设备的 processing
    拷贝 fuse_in_header 和参数到用户缓冲
守护进程处理完 write 回答
  fuse_request_end                 :280     置 FR_FINISHED，唤醒 waitq
```

`fuse_dev_do_read` 要求读缓冲至少装得下固定头加 `max_write`（`:1246`）。太小返回 `-EINVAL`。非阻塞读在没有请求时返回 `-EAGAIN`（`:1259`）。

`fuse_simple_background`（`:550`）把请求标成后台，调用者不等 `waitq`。写回、forget 用这条。后台请求数受 `max_background` 限制，超过就让写路径感觉到拥塞（`congestion_threshold`）。

守护进程死掉或写 `fusectl` 的 `abort` 时，连接上的请求以错误结束，VFS 调用方从 `fuse_simple_request` 拿到负的 `req->out.h.error`。容器里卷的守护进程若被 cgroup 杀掉，挂载点上的读写会全部失败，见 [12](./12-perf-and-pitfalls.md)。

---

## 8.3 和容器镜像、卷的关系

镜像的惰性拉取把 overlay 的 lower（或单独的挂载）做成 FUSE。读一个还没在本地的块时，内核发出 `FUSE_READ`，守护进程再去仓库取。upper 仍是普通文件系统上的目录，copy-up 发生之后写不再进 FUSE。这种挂法的延迟在第一次读，不在 copy-up 之后的写。

用户提供的卷可以是 FUSE 挂载，由 runc 在 `prepareRootfs` 里按挂载表挂进容器的 mnt ns（见 [09](./09-runc-create-path.md)）。挂载之后它和其他文件系统一样出现在容器的 `/proc/self/mountinfo` 里。守护进程通常留在宿主机的 pid/mnt ns，靠 FUSE 连接上的 `user_ns`/`pid_ns` 解释容器里的身份。

`virtio-fs` 在 `CONFIG_VIRTIO_FS` 下复用 `fuse_req`，`argbuf` 是给 virtio 的连续缓冲。那是虚拟机和宿主之间的另一条传输，队列结构相同，唤醒回调换成 virtio 的 `fuse_iqueue_ops`。容器笔记只点到这个字段存在。

6.6 的 FUSE 设备控制是 `FUSE_DEV_IOC_CLONE` 这种 ioctl。请求仍经过 `/dev/fuse` 的读和写。

---

## 8.4 fusectl

`fs/fuse/control.c` 注册文件系统 `fusectl`（`:368`）。每个连接在挂载点下有一个目录，里面是 `waiting`、`abort`、`max_background`、`congestion_threshold`（`fuse_ctl_add_conn`，`:256`）。写 `abort` 走 `fuse_conn_abort_write`（`:34`）。本机这个文件系统挂在 `/sys/fs/fuse/connections`，样例见 [11](./11-observability.md)。类型名是 `fusectl`，挂载点放在 `/sys/fs` 下面。

---

## 8.5 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/fuse/fuse_i.h` | `struct fuse_args` | 275 | 一次操作的参数 |
| `fs/fuse/fuse_i.h` | `struct fuse_req` | 382 | 在队列里排队的请求 |
| `fs/fuse/fuse_i.h` | `struct fuse_iqueue` | 459 | pending 与读者等待队列 |
| `fs/fuse/fuse_i.h` | `struct fuse_conn` | 581 | 连接；`iq` 在 `:618` |
| `fs/fuse/dev.c` | `fuse_simple_request` | 485 | 同步请求 |
| `fs/fuse/dev.c` | `__fuse_request_send` | 410 | 挂上 pending 并等待 |
| `fs/fuse/dev.c` | `fuse_simple_background` | 550 | 后台请求 |
| `fs/fuse/dev.c` | `fuse_dev_do_read` | 1222 | 守护进程读走一个请求 |
| `fs/fuse/dev.c` | `fuse_request_end` | 280 | 完成并唤醒 |
| `fs/fuse/control.c` | `fuse_ctl_add_conn` | 256 | fusectl 下的每连接目录 |
| `fs/fuse/control.c` | `fuse_ctl_fs_type` | 368 | 名字 `fusectl` |

---

**导航**：[← overlayfs](./07-overlayfs.md) · [目录](./README.md) · [runc 创建路径 →](./09-runc-create-path.md)
