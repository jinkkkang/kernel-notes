# 3. epoll 的四个对象

**结论**：一次 `epoll_create1` 得到一个 `struct eventpoll`；每 `EPOLL_CTL_ADD` 一个 fd 得到一个 `struct epitem`，挂在以 `(struct file *, fd)` 为键的红黑树上，并用 `struct eppoll_entry` 把回调留在目标文件的等待队列上。

> 对象都写在 `fs/eventpoll.c` 里，没有对外的 `struct eventpoll` 头文件。用户可见的事件位在 `include/uapi/linux/eventpoll.h`。
> 等待队列条目本身的字段见 [locks-and-ds/08](../locks-and-ds/08-wait-queue-and-completion.md)。本章只解释 epoll 怎么用它。

---

## 3.1 `epoll_create1`：匿名 inode 加 `eventpoll_fops`

```text
epoll_create1(flags)          SYSCALL_DEFINE1，eventpoll.c:2078
  do_epoll_create             :2035
    ep_alloc                  :1032，kzalloc 一个 eventpoll
    get_unused_fd_flags(O_RDWR | (flags & O_CLOEXEC))
    anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep, 同样的旗标)   :2061
    ep->file = file
    fd_install
```

`flags` 里只允许 `EPOLL_CLOEXEC`（`uapi/linux/eventpoll.h:23`，等于 `O_CLOEXEC`）。`epoll_create(size)`（`:2083`）在 `size <= 0` 时返回 `-EINVAL`，否则调用 `do_epoll_create(0)`。`size` 不参与分配。

`ep_alloc`（`:1032`）把 `mtx`、`lock`、`wq`、`poll_wait`、`rdllist` 初始化好，`rbr = RB_ROOT_CACHED`，`ovflist = EP_UNACTIVE_PTR`（`(void *)-1L`，`:97`），`user = get_current_user()`，`refcount = 1`。

```c
/* fs/eventpoll.c:978 */
static const struct file_operations eventpoll_fops = {
	.show_fdinfo	= ep_show_fdinfo,     /* CONFIG_PROC_FS 时才有，:954 */
	.release	= ep_eventpoll_release, /* :871，ep_clear_and_put */
	.poll		= ep_eventpoll_poll,    /* :948，嵌套 epoll 和 poll(2) 都走这里 */
	.llseek		= noop_llseek,
};
```

`is_file_epoll`（`:342`）比较 `f_op == &eventpoll_fops`。后续代码用它判断目标是不是另一个 epoll，而不是看文件名。

`anon_inode_getfile` 创建的文件共用一个匿名 inode（`fs/anon_inodes.c:152` 的注释，函数在 `:156`）。`/proc/<pid>/fdinfo/<epfd>` 头上的 `ino` 因此是这个共享 inode 的号，监视项自己的 inode 打在 `tfd` 那一行里，见 [09](./09-observability.md)。

---

## 3.2 `struct epoll_filefd` 与 `struct eppoll_entry`

红黑树的键是一对 `(file, fd)`，不是单独的 fd 号，也不是单独的 `struct file`。

```c
/* fs/eventpoll.c:101 */
struct epoll_filefd {
	struct file *file;   /* 目标 struct file，不是 fd 表里的槽 */
	int fd;              /* 注册时的描述符号。dup 出的另一个号是另一项 */
} __packed;
```

比较函数 `ep_cmp_ffd`（`:356`）先比 `file` 指针，指针相同再比 `fd`。`dup` 得到的两个 fd 指向同一个 `struct file`，在树上是两个 `epitem`。`ep_find`（`:1060`）必须持有 `ep->mtx`。

```c
/* fs/eventpoll.c:107 */
struct eppoll_entry {
	struct eppoll_entry *next;     /* 串在 epitem.pwqlist 上 */

	struct epitem *base;          /* 容器。ep_item_from_wait 用 container_of 找到它 */

	/*
	 * 挂进目标文件的 wait_queue_head。
	 * init_waitqueue_func_entry 把 func 设为 ep_poll_callback，private 设为 NULL
	 * （include/linux/wait.h:90）。回调不读 private，它从 wait 成员反查本结构。
	 */
	wait_queue_entry_t wait;

	wait_queue_head_t *whead;      /* 上面 wait 所在的队列头。POLLFREE 时会被置 NULL */
};
```

一个目标文件的 `f_op->poll` 可能对多个队列头调用 `poll_wait`（pipe 的读端和写端就是两个）。每调用一次分配一个 `eppoll_entry`，所以一个 `epitem` 的 `pwqlist` 可以有多项。分配走 slab `eventpoll_pwq`（`pwq_cache`，`eventpoll_init` `:2475`）。

---

## 3.3 `struct epitem`

```c
/* fs/eventpoll.c:130。eventpoll_init 用 BUILD_BUG_ON 要求
 * sizeof(void *) <= 8 时本结构不超过 128 字节（:2468）。
 */
struct epitem {
	union {
		struct rb_node rbn;    /* 链进 eventpoll.rbr。活着的时候用这个成员 */
		struct rcu_head rcu;   /* ep_remove 之后 call_rcu 归还 epi_cache */
	};

	struct list_head rdllink;      /* 空：不在就绪链上。非空：在 rdllist 或某次扫描的 txlist 上 */

	/*
	 * 和 eventpoll.ovflist 一起用的单链表指针。
	 * 不在溢出链上时是 EP_UNACTIVE_PTR。
	 */
	struct epitem *next;

	struct epoll_filefd ffd;       /* 红黑树键，也是 fdinfo 里的 tfd */

	/*
	 * 由 file->f_lock 保护。eventpoll_release_file 先把它写成 true，
	 * 再拿 ep->mtx，避免和 ep_clear_and_put 同时改 fllink。
	 */
	bool dying;

	struct eppoll_entry *pwqlist;  /* 挂在目标等待队列上的桥 */

	struct eventpoll *ep;         /* 所属的 epoll */

	struct hlist_node fllink;      /* 挂进 file->f_ep，或嵌套时挂进内层 eventpoll.refs */

	struct wakeup_source __rcu *ws; /* EPOLLWAKEUP 时按 dentry 名注册。没有该位则为空 */

	struct epoll_event event;      /* 用户兴趣掩码和 data。ONESHOT 交付后会清掉非私有位 */
};
```

`ep_is_linked`（`:364`）看 `rdllink` 是否非空。回调和 `ep_send_events` 都用它避免把同一项挂两次。

`EP_PRIVATE_BITS`（`:85`）是 `EPOLLWAKEUP | EPOLLONESHOT | EPOLLET | EPOLLEXCLUSIVE`。兴趣掩码去掉这些位之后若为空，`ep_poll_callback` 认为这项已被 ONESHOT 关掉，不再入就绪链。

---

## 3.4 `struct eventpoll`

```c
/* fs/eventpoll.c:178。放在 file->private_data。 */
struct eventpoll {
	/*
	 * 收集事件、epoll_ctl、eventpoll_release_file、ep 退出时持有。可以睡眠。
	 * 文件头 LOCKING（:43）规定的顺序是 epnested_mutex → ep->mtx → ep->lock。
	 * 嵌套 epoll 时按嵌套深度使用 mutex_lock_nested，深度同时当 lockdep 的子类。
	 */
	struct mutex mtx;

	wait_queue_head_t wq;         /* epoll_wait 把当前任务加到这里 */
	wait_queue_head_t poll_wait;  /* 别的 poll/epoll 监视本 epfd 时挂这里 */

	struct list_head rdllist;     /* 就绪 epitem，由 lock 保护 */
	spinlock_t lock;              /* 保护 rdllist 和 ovflist。回调可能来自硬中断，不能睡 */

	struct rb_root_cached rbr;    /* 全部监视项 */

	/*
	 * 单链表。平时是 EP_UNACTIVE_PTR。
	 * ep_start_scan 把它写成 NULL，表示「现在正在把就绪项拷给用户，
	 * 新来的 epitem 请串到这条链上」。ep_done_scan 再搬回 rdllist。
	 */
	struct epitem *ovflist;

	struct wakeup_source *ws;     /* 名字 "eventpoll"。发送事件期间 __pm_stay_awake */
	struct user_struct *user;     /* 创建者。限额记在 user->epoll_watches */
	struct file *file;

	u64 gen;                      /* 跟上全局 loop_check_gen，环检测用来跳过已访问节点 */
	struct hlist_head refs;       /* 哪些外层 epitem 把本 epfd 嵌了进去 */
	u8 loop_check_depth;

	/*
	 * ep_insert 成功路径会再 ep_get 一次。
	 * ep_remove 放下那一次引用；epfd 的 release 放下创建时的那一次。
	 * 和 epitem.dying 一起决定能不能 ep_free。
	 */
	refcount_t refcount;

	struct rcu_head rcu;          /* 向上走嵌套深度的 RCU 读侧还可能握着 epi->ep */

#ifdef CONFIG_NET_RX_BUSY_POLL
	/* 最近一个带 sk_napi_id 的就绪套接字。ep_busy_loop 只用这个 id。 */
	unsigned int napi_id;
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	u8 nests;                     /* ep_poll_safewake 给 poll_wait.lock 标的嵌套层 */
#endif
};
```

三把锁的分工可以收成一句：`mtx` 让 `epoll_ctl` 和「把事件拷到用户」可以睡眠；`lock` 让硬中断里的 `ep_poll_callback` 能改就绪链；`epnested_mutex` 让两个方向同时 `ADD` 时无法各看各的、拼出一个环。

`ep_events_available`（`:388`）在不上 `lock` 的情况下看 `rdllist` 非空，或 `ovflist != EP_UNACTIVE_PTR`。`list_empty_careful` 允许和持锁的插入并发。`epoll_wait` 用它做快路径，真正睡觉前会再在 `ep->lock` 下看一次。

---

## 3.5 `rdllist` 与 `ovflist`

`ep_start_scan`（`:620`）持 `ep->lock`：把 `rdllist` 整表拼到调用者私有的 `txlist`，再 `WRITE_ONCE(ovflist, NULL)`。注释写明这时不能让回调直接插 `rdllist`，因为扫描途中会 `copy_to_user`，不能一直持自旋锁。

这扇窗口里 `ep_poll_callback` 看到 `ovflist != EP_UNACTIVE_PTR`，就把 `epitem` 头插到 `ovflist`（后进先出）。同一项已经在溢出链上时（`epi->next != EP_UNACTIVE_PTR`）不重复插入。

`ep_done_scan`（`:637`）再持 `ep->lock`：把 `ovflist` 倒序插回 `rdllist`（恢复先进先出），然后把 `ovflist` 写回 `EP_UNACTIVE_PTR`，再把 `txlist` 上没送完的项拼回 `rdllist`。`rdllist` 仍非空且 `wq` 上有人，就 `wake_up(&ep->wq)`。

所以就绪链有两条，对外只表现为「`epoll_wait` 不会在拷贝用户缓冲的窗口里弄丢一次唤醒」。

---

## 3.6 `file->f_ep`：关闭文件时谁来摘

```c
/* include/linux/fs.h:1025，CONFIG_EPOLL */
struct file {
	/* ... */
	spinlock_t		f_lock;   /* :1005，保护 f_ep 和 f_flags。禁止在硬中断里拿 */
	/* ... */
	struct hlist_head	*f_ep;    /* :1027。空指针表示没有任何 epoll 监视这个 file */
};
```

`attach_epitem`（`eventpoll.c:1413`）在 `ADD` 时把 `epi->fllink` 挂上去：

- 目标是普通文件：`f_ep` 指向一块 `epitems_head`（slab 名 `ep_head`），里面的 `epitems` 是 hlist。
- 目标自己就是 epoll：`f_ep` 直接指向那个 `eventpoll.refs`。外层项因此出现在内层的 `refs` 上，环检测向上走时用它。

`eventpoll_release`（`include/linux/eventpoll.h:34`）在 `f_ep` 非空时调用 `eventpoll_release_file`（`eventpoll.c:992`）。后者在 `file->f_lock` 下取出 hlist 上的一个 `epitem`，把 `dying` 写成 true，再拿对应的 `ep->mtx`，`ep_unregister_pollwait` 摘掉 `pwqlist`，然后 `ep_remove_file` + `ep_remove_epi`。hlist 上还有项就回到 `again`。

`__fput` 把 `eventpoll_release` 放在清理链的前面，vfs 章 [04](../vfs/04-fd-and-open.md) 已经点过这个顺序。本章要记住的是：摘项发生在 `struct file` 的最后一次释放，而不是某一个 fd 号的 `close`。`dup` 之后只关掉其中一个号，另一个号还握着引用，`f_ep` 上的项都还在，其中包含那个已经关掉的 fd 号。用户要在 `close` 之前自己 `EPOLL_CTL_DEL`，否则 `epoll_wait` 仍可能报出那个号。

---

## 3.7 限额和三块 slab

`eventpoll_init`（`:2452`，`fs_initcall`）把 `max_user_watches` 设成：

```text
(((totalram - totalhigh) / 25) << PAGE_SHIFT) / EP_ITEM_COST
```

`si_meminfo` 的 `totalram - totalhigh` 是低端内存页数，除以 25 即百分之四，左移 `PAGE_SHIFT` 换成字节。`EP_ITEM_COST`（`:99`）是 `sizeof(struct epitem) + sizeof(struct eppoll_entry)`。结果可以通过 `/proc/sys/fs/epoll/max_user_watches` 改（`epoll_table`，`:319`，`proc_doulongvec_minmax`，范围 0 到 `LONG_MAX`）。本机读数见 [09](./09-observability.md)，那个数是 6.12 上的采样，不要回写成 6.6 的 `totalram`。

记账在创建者的 `user_struct` 上：

```c
/* include/linux/sched/user.h:17，CONFIG_EPOLL */
struct percpu_counter epoll_watches; /* 该用户当前被监视的 fd 数 */
```

`ep_insert` 用 `percpu_counter_compare(..., max_user_watches) >= 0` 判定，满了返回 `-ENOSPC`（`:1464`）。比较发生在 `percpu_counter_inc` 之前。

| slab 名 | 对象 | 创建点 |
|---------|------|--------|
| `eventpoll_epi` | `struct epitem` | `eventpoll_init:2471`，`SLAB_HWCACHE_ALIGN \| SLAB_PANIC \| SLAB_ACCOUNT` |
| `eventpoll_pwq` | `struct eppoll_entry` | `:2475`，`SLAB_PANIC \| SLAB_ACCOUNT` |
| `ep_head` | `struct epitems_head` | `:2479`，普通文件的 `f_ep` 头 |

`struct eventpoll` 本身是 `ep_alloc` 里的 `kzalloc`，不走这三块 slab。

---

## 3.8 包装：`struct ep_pqueue`

```c
/* fs/eventpoll.c:243 */
struct ep_pqueue {
	poll_table pt;       /* _qproc = ep_ptable_queue_proc */
	struct epitem *epi;  /* 这次 ADD 正在安装的项。分配 pwq 失败时被写成 NULL */
};
```

`ep_insert` 把 `epq` 放在栈上，`init_poll_funcptr(&epq.pt, ep_ptable_queue_proc)`，再交给 `ep_item_poll`。目标的 `f_op->poll` 调用 `poll_wait` 时，`_qproc` 从 `poll_table` 用 `container_of` 找回 `ep_pqueue`。这一步的代码在 [04](./04-epoll-ctl-and-callback.md)。
