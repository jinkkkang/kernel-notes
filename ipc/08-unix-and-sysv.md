# 8. AF_UNIX 就绪，以及 SysV / POSIX mqueue

**结论**：`AF_UNIX` 在本章只回答「`unix_poll` 怎样向 epoll 报告可读可写」；System V 的消息、信号量和共享内存，以及 POSIX mqueue，各留一条创建调用链和睡眠点，不展开它们的队列算法。

> `sk_buff`、发送队列长度怎样累加、`unix_stream_sendmsg` 里的字节账，在 [networking/10](../networking/10-af-unix.md)。
> 对象所在的 `ipc_namespace` 见 [containerization/04](../containerization/04-pid-user-uts-ipc-time-ns.md)。

新代码要在进程之间交一次事件，优先用 [07](./07-eventfd-signalfd-timerfd.md) 的 eventfd，或 [06](./06-pipe.md) 的管道。下面三条是内核里仍在的遗留接口，读代码时能对上号即可。

---

## 8.1 `AF_UNIX`：创建点到 `unix_create`，就绪看 `unix_poll`

`unix_create`（`net/unix/af_unix.c:1053`）按 `sock->type` 填操作表：

| 类型 | `proto_ops` | `.poll` |
|------|-------------|---------|
| `SOCK_STREAM` | `unix_stream_ops`（`:876`） | `unix_poll`（`:885`） |
| `SOCK_DGRAM`，以及 `SOCK_RAW`（被改成 `SOCK_DGRAM`） | `unix_dgram_ops`（`:901`） | `unix_dgram_poll`（`:910`） |
| `SOCK_SEQPACKET` | `unix_seqpacket_ops` | `unix_dgram_poll`（`:934`） |

`protocol` 只允许 0 或 `PF_UNIX`。`unix_create1`（`:999`）分配 `sock`。`unix_socketpair`（`:1698`）把两个 `unix_peer` 互相指上，两边 `sk_state = TCP_ESTABLISHED`，`socket->state = SS_CONNECTED`。`socketpair(AF_UNIX, SOCK_STREAM, 0, fds)` 走的就是这条，不经过文件系统上的路径名。`bind` 到路径名是 `unix_bind`（`:1357`），本章不展开路径和抽象命名空间的哈希。

流套接字的就绪：

```c
/* net/unix/af_unix.c:3190 */
unix_poll(file, sock, wait)
{
	sock_poll_wait(file, sock, wait);     /* include/net/sock.h:2463，挂到 sock->wq.wait */
	/* sk_err                        → EPOLLERR */
	/* shutdown == SHUTDOWN_MASK     → EPOLLHUP */
	/* shutdown & RCV_SHUTDOWN       → EPOLLRDHUP | EPOLLIN | EPOLLRDNORM */
	/* sk_receive_queue 非空         → EPOLLIN | EPOLLRDNORM */
	/* sk_is_readable(sk)            → EPOLLIN | EPOLLRDNORM */
	/* 流或 seqpacket 且 TCP_CLOSE   → EPOLLHUP */
	/* unix_writable(sk, state)      → EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND */
}
```

`sock_poll_wait` 在 `poll_does_not_wait` 为假时调用 `poll_wait(filp, &sock->wq.wait, p)`，并跟一条 `smp_mb()`。epoll 的 `ADD` 因此把 `eppoll_entry` 挂在套接字等待队列上，和 pipe 挂在 `rd_wait` 上是同一套 `ep_ptable_queue_proc`。

`unix_writable`（`:566`）为真的条件是：状态不是 `TCP_LISTEN`，且 `(sk_wmem_alloc << 2) <= sk_sndbuf`。写端有空间时 `unix_write_space`（`:572`）对 `sk_wq->wait` 做 `wake_up_interruptible_sync_poll(..., EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)`。读端有数据时的唤醒由协议的接收路径发出，队列仍是这把 `sock->wq.wait`。队列里的 `sk_buff` 怎样入队，本章停在 `skb_queue_empty_lockless(&sk->sk_receive_queue)` 这个判断上。

数据报的 `unix_dgram_poll`（`:3235`）同样先 `sock_poll_wait`。可读条件仍看接收队列和 `sk_is_readable`。可写在 `unix_writable` 之外还要看对端：对端存在、对端的 peer 不是自己、且 `unix_recvq_full_lockless(other)` 时，可能把 writable 清掉并 `unix_dgram_peer_wake_me`（`:3274`）。这是「对端接收队列满了，本地先不要报 `EPOLLOUT`」。

`SCM_RIGHTS`（`include/linux/socket.h:177`，值 `0x01`）用来在消息上捎带文件描述符。`unix_stream_sendmsg`（`:2227`）调用 `scm_send`（`:2239`）；需要附带 fd 时 `unix_attach_fds`（`:1821`）把它们记到这条消息上。接收侧流的 `scm_recv_unix` 在 `unix_stream_read_generic`（`af_unix.c:2940`），数据报在 `unix_dgram_recvmsg`（`:2499`）。fd 怎样安装进接收进程的 fd 表，在 [networking/10](../networking/10-af-unix.md)。这里只需要知道：传 fd 的载体是 `AF_UNIX` 消息，不是 pipe。

---

## 8.2 SysV：`ipc_ids` 与 `kern_ipc_perm`

每个 `ipc_namespace` 有三张表（`include/linux/ipc_namespace.h:32` 的 `ids[3]`）：

| 下标 | 宏 | 访问 |
|------|----|------|
| 0 | `IPC_SEM_IDS`（`ipc/util.h:122`） | `sem_ids(ns)`，`ipc/sem.c:169` |
| 1 | `IPC_MSG_IDS`（`:123`） | `msg_ids(ns)`，`ipc/msg.c:100` |
| 2 | `IPC_SHM_IDS`（`:124`） | `shm_ids(ns)`，`ipc/shm.c:95` |

```c
/* include/linux/ipc_namespace.h:18 */
struct ipc_ids {
	int in_use;
	unsigned short seq;             /* 和槽下标拼进用户看到的 id */
	struct rw_semaphore rwsem;
	struct idr ipcs_idr;            /* 槽 */
	int max_idx;
	int last_idx;
#ifdef CONFIG_CHECKPOINT_RESTORE
	int next_id;
#endif
	struct rhashtable key_ht;       /* key 到对象。IPC_PRIVATE 不进这张表的查找 */
};
```

id 的低位是槽下标，高位是序号，避免槽复用之后旧 id 仍能命中。`ipcid_to_idx` / `ipcid_to_seqx` 在 `ipc/util.h:126`。默认序号从 bit 15 开始（`IPCMNI_SHIFT`，`:28`）；打开 IPCMNI 扩展后从 bit 24 开始。本章不展开 sysctl 切换。

每类对象把 `kern_ipc_perm` 嵌在自己的结构体头部：

```c
/* include/linux/ipc.h:12 */
struct kern_ipc_perm {
	spinlock_t	lock;
	bool		deleted;
	int		id;
	key_t		key;
	kuid_t		uid;
	kgid_t		gid;
	kuid_t		cuid;
	kgid_t		cgid;
	umode_t		mode;
	unsigned long	seq;
	void		*security;
	struct rhash_head khtnode;
	struct rcu_head rcu;
	refcount_t refcount;
} ____cacheline_aligned_in_smp __randomize_layout;
```

`get` / `ctl` / `op` 三族的入口：

| 操作 | 系统调用 | 落到 |
|------|----------|------|
| 创建或按 key 查找消息队列 | `msgget` `:315` | `ksys_msgget` → `ipcget`（`ipc/util.c:673`），`ops.getnew = newque` |
| 发送 | `msgsnd` `:971` | `do_msgsnd`（`:848`）。队列满时在 `:916` `schedule()` |
| 接收 | `msgrcv` `:1270` | `do_msgrcv`（`:1098`）。没有匹配消息时在 `:1198` `schedule()` |
| 创建信号量数组 | `semget` `sem.c:624` | `ipcget`，`ops.getnew = newary` |
| 操作 | `semop` `:2296` | `do_semtimedop`（`:2222`）→ `__do_semtimedop`（`:1983`）。做不成时把栈上的 `sem_queue` 挂进 pending 列表，再 `schedule_hrtimeout_range`（`:2166`） |
| 创建共享内存 | `shmget` `shm.c:855` | `ksys_shmget`（`:836`）→ `ipcget`，`getnew = newseg`（`:710`） |
| 映射 | `shmat` `:1701` | `do_shmat`（`:1527`）对 `shp->shm_file` `get_file`，再 `do_mmap`（`:1670`） |

`newseg` 用 `shmem_kernel_file_setup`（`:772`）做出背后的 shmem 文件。映射之后的缺页和换出在 [memory-management/08](../memory-management/08-page-cache-and-writeback.md) 的 shmem 一节，不在这里重写。

`ipcget`（`util.c:673`）看到 `IPC_PRIVATE` 就 `ipcget_new`，否则 `ipcget_public` 按 key 在 `key_ht` 里找，没有再创建。权限检查走 `ipcperms`（`util.h:140` 声明）。`ctl` 一族（`msgctl`、`semctl`、`shmctl`）改的是 `kern_ipc_perm` 里的 uid/gid/mode 和各类上限，命令字很多，本章不逐条列。

睡眠都不走 `poll_wait`。SysV 消息和信号量没有 `f_op->poll`，不能放进 epoll。要和别的 fd 一起等，用 POSIX mqueue 或 eventfd。

---

## 8.3 POSIX mqueue：伪文件系统上的 inode

`mq_open`（`ipc/mqueue.c:943`）进 `do_mq_open`（`:894`）。挂载点是 `current->nsproxy->ipc_ns->mq_mnt`（`:897`）。它在这个伪文件系统的根上 `lookup_one_len`，`prepare_open` 之后 `dentry_open`。新建队列时 inode 的 `i_private` 一类上下文是 `mqueue_inode_info`，`MQUEUE_I` 用 `container_of` 从 `vfs_inode` 找回来（`:167`）。

```c
/* ipc/mqueue.c:134。只留和睡眠、epoll 有关的成员，其余字段仍按源码顺序都在。 */
struct mqueue_inode_info {
	spinlock_t lock;
	struct inode vfs_inode;
	wait_queue_head_t wait_q;          /* mqueue_poll_file 挂在这里 */

	struct rb_root msg_tree;           /* 按优先级分的消息树。算法不展开 */
	struct rb_node *msg_tree_rightmost;
	struct posix_msg_tree_node *node_cache;
	struct mq_attr attr;

	struct sigevent notify;
	struct pid *notify_owner;
	u32 notify_self_exec_id;
	struct user_namespace *notify_user_ns;
	struct ucounts *ucounts;
	struct sock *notify_sock;
	struct sk_buff *notify_cookie;

	/* 下标 0/1：等空位的发送者和等消息的接收者，按任务优先级插入 */
	struct ext_wait_queue e_wait_q[2];

	unsigned long qsize;
};
```

`mqueue_file_operations.poll` 是 `mqueue_poll_file`（`:1662` 的操作表，函数在 `:676`）。它 `poll_wait` 到 `wait_q`：`mq_curmsgs > 0` 给 `EPOLLIN | EPOLLRDNORM`，`mq_curmsgs < mq_maxmsg` 给 `EPOLLOUT | EPOLLWRNORM`。因此 mqueue 的 fd 可以放进 epoll。

真正堵住发送和接收的是 `e_wait_q`，不是 `wait_q`。`do_mq_timedsend`（`:1061`）和 `do_mq_timedreceive`（`:1180`）在队列满或空时调用 `wq_sleep`（`:714`）：把当前任务放进对应的 `ext_wait_queue`，`TASK_INTERRUPTIBLE`，`schedule_hrtimeout_range_clock`。被叫醒的条件是该项 `state == STATE_READY`。poll 侧的 `wait_q` 在两处被敲：`__do_notify`（`:783`）末尾 `wake_up(&info->wait_q)`（`:841`），接收侧没有正在等空位的发送者时 `pipelined_receive` 做 `wake_up_interruptible(&info->wait_q)`（`:1052`）。优先级树怎样按 `m_type` 分层，停在 `msg_insert`（`:191`）这个名字上。

---

## 8.4 和本章其余部分的关系

| 机制 | 能否 `epoll_ctl` | 唤醒打到哪 |
|------|------------------|------------|
| pipe | 能 | `rd_wait` / `wr_wait` |
| eventfd / signalfd / timerfd | 能 | 各自的 `wqh` 或 `signalfd_wqh` |
| `AF_UNIX` | 能 | `sock->wq.wait`，`unix_poll` 解释队列空满 |
| POSIX mqueue | 能 | `mqueue_inode_info.wait_q` |
| SysV msg / sem | 不能 | 自己的 `schedule`，没有 `f_op->poll` |
| SysV shm | 映射后是内存，不是「就绪事件」 | 缺页路径，见内存章 |
| futex | 不能 | 哈希桶上的 `futex_q`，见 locks-and-ds/09 |
