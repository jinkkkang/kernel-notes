# 2. select / poll 与 `poll_table`

**结论**：select 和 poll 每次系统调用都把当前任务挂进每个被监视 fd 的等待队列，返回前全部摘掉；兴趣集合放在用户缓冲里，下次调用再拷进来。

> 头文件：`include/linux/poll.h`。实现：`fs/select.c`。
> 具体套接字的 `tcp_poll` 不在本章。例子用 pipe 的 `pipe_poll` 和 eventfd 的 `eventfd_poll`，见 [06](./06-pipe.md)、[07](./07-eventfd-signalfd-timerfd.md)。

---

## 2.1 三个积木

```c
/* include/linux/poll.h:37 */
typedef struct poll_table_struct {
	poll_queue_proc _qproc;   /* 非空：本次 poll 要把当前等待者挂进队列 */
	__poll_t _key;            /* 调用方关心的事件位 */
} poll_table;
```

`poll_queue_proc`（`:31`）的签名是 `(struct file *, wait_queue_head_t *, struct poll_table_struct *)`。文件的 `f_op->poll` 在自己的每个等待队列头上调用 `poll_wait`：

```c
/* include/linux/poll.h:42 */
static inline void poll_wait(struct file *filp, wait_queue_head_t *wait_address, poll_table *p)
{
	if (p && p->_qproc && wait_address) {
		p->_qproc(filp, wait_address, p);
		smp_mb();   /* 和 wq_has_sleeper() 配对，见 locks-and-ds/08 */
	}
}
```

`_qproc == NULL` 时 `poll_wait` 什么都不挂。select/poll 在已经发现就绪 fd 之后会把 `_qproc` 清掉，后面的 fd 只查状态、不再登记。`poll_does_not_wait`（`:61`）就是在看这个指针。`poll_requested_events`（`:72`）读 `_key`；`p == NULL` 时返回全部位。

`vfs_poll`（`:88`）是统一入口：

```c
/* include/linux/poll.h:88 */
static inline __poll_t vfs_poll(struct file *file, struct poll_table_struct *pt)
{
	if (unlikely(!file->f_op->poll))
		return DEFAULT_POLLMASK;          /* :24，IN|OUT|RDNORM|WRNORM */
	return file->f_op->poll(file, pt);
}
```

没有 `f_op->poll` 的文件（普通磁盘文件常见）对 select/poll 来说永远可读可写。`epoll_ctl` 拒绝这种文件，见 [04](./04-epoll-ctl-and-callback.md) 的 `-EPERM`。

`init_poll_funcptr`（`:77`）把 `_qproc` 设成给定函数，并把 `_key` 设成所有位。select 和 poll 随后按每个 fd 收窄 `_key`。

---

## 2.2 等待表：`poll_wqueues`

```c
/* include/linux/poll.h:95 */
struct poll_table_entry {
	struct file *filp;                 /* get_file 拿到的引用，free 时 fput */
	__poll_t key;                      /* 这一项关心的事件，pollwake 用来过滤 */
	wait_queue_entry_t wait;           /* func = pollwake，private = poll_wqueues * */
	wait_queue_head_t *wait_address;   /* 挂在哪个队列头上 */
};

/* include/linux/poll.h:105 */
struct poll_wqueues {
	poll_table pt;
	struct poll_table_page *table;    /* 栈上槽用完之后按页续的表 */
	struct task_struct *polling_task; /* 被叫醒的是这个任务 */
	int triggered;                     /* pollwake 把它写成 1 */
	int error;
	int inline_index;
	struct poll_table_entry inline_entries[N_INLINE_POLL_ENTRIES];
};
```

`N_INLINE_POLL_ENTRIES`（`:22`）是 `(MAX_STACK_ALLOC - FRONTEND_STACK_ALLOC) / sizeof(struct poll_table_entry)`。`MAX_STACK_ALLOC` 为 832，`FRONTEND_STACK_ALLOC` 为 256（`:17`–`:19`）。超出的项放在 `poll_table_page` 里，一页一个（`poll_get_entry`，`select.c:158`）。

| 函数 | 位置 | 作用 |
|------|------|------|
| `poll_initwait` | `select.c:119` | `_qproc = __pollwait`，`polling_task = current`，`triggered = 0` |
| `__pollwait` | `:219` | 分配 `poll_table_entry`，`init_waitqueue_func_entry(..., pollwake)`，`add_wait_queue` |
| `pollwake` | `:208` | `key` 与 `entry->key` 无交集则返回 0；否则 `__pollwake` |
| `__pollwake` | `:182` | `triggered = 1`，再用一个栈上的 dummy entry 调 `default_wake_function` 叫醒 `polling_task` |
| `poll_schedule_timeout` | `:234` | `triggered` 仍为 0 才 `schedule_hrtimeout_range`；醒来后 `smp_store_mb(triggered, 0)` |
| `poll_freewait` | `:136` | 对每一项 `remove_wait_queue` + `fput` |

`pollwake` 的 `private` 指向整张 `poll_wqueues`，不是任务。真正送进 `try_to_wake_up` 的是 dummy entry 上的 `polling_task`。这和 epoll 把 `epitem` 从 `container_of` 找出来是同一类手法：自定义 `func` 的 `private` 可以不是任务指针。`default_wake_function` 只作用在那个 dummy 上。

`key` 从 `wake_up_poll` 一类宏进来（`poll_to_key` / `key_to_poll`，`include/linux/wait.h:238`）。pipe 唤醒时带的是 `EPOLLIN | EPOLLRDNORM` 或 `EPOLLOUT | EPOLLWRNORM`。只关心另一方向的 select/poll 项在 `pollwake` 里直接返回 0，扫描继续。

---

## 2.3 `do_select` 与 `do_sys_poll`

select 的系统调用 `select`（`select.c:723`）进 `core_sys_select`（`:622`）。后者准备 6 张位图（in/out/ex 的输入和结果），能放进栈上 `SELECT_STACK_ALLOC` 的就放栈，否则 `kvmalloc`。`get_fd_set`（`:387`）把用户 `fd_set` 拷进来，`do_select` 填结果，`set_fd_set` 拷回去。

`do_select`（`:476`）的循环：

1. `poll_initwait`。超时为 0 时立刻把 `_qproc` 清掉，只查状态。
2. 对每个被置位的 fd：`fdget`，`wait_key_set`（`:465`）按 in/out/ex 拼 `_key`，`vfs_poll`。
3. 掩码命中则 `retval++`，并 `__qproc = NULL`。这一轮里更后面的 fd 不再挂队列。
4. 一整轮结束再把 `_qproc` 置 NULL（`:575`）。等待项已经在队列上，下一轮醒来后只重新 `vfs_poll`。
5. 没有命中、没有超时、没有信号：`poll_schedule_timeout(..., TASK_INTERRUPTIBLE, ...)`。
6. 退出前 `poll_freewait`，把这一次挂上的项全部摘掉。

poll 的系统调用 `poll`（`:1065`）进 `do_sys_poll`（`:971`）。它把用户 `pollfd[]` 拷进栈上的 `poll_list`，放不下再 `kmalloc`。`nfds` 超过 `RLIMIT_NOFILE` 返回 `-EINVAL`（`:984`）。`do_poll`（`:882`）对每一项调用 `do_pollfd`（`:852`）：用户 16 位 `events` 经 `demangle_poll`（`poll.h:140`）换成内核 `__poll_t`，并或上 `EPOLLERR | EPOLLHUP`。返回用户前 `mangle_poll`（`:130`）把内核位压回 16 位 `revents`。

`poll` 被信号打断时返回 `-ERESTARTNOHAND`，系统调用层把它记进 `restart_block` 并 `set_restart_fn(..., do_restart_poll)`（`do_restart_poll` 在 `:1044`，`poll` 系统调用里的登记在 `:1093`）。重启语义见 [syscalls/08](../syscalls/08-restart.md)。`ppoll`（`:1098`）在此之上接受 `timespec` 和信号掩码。

两边都有一段 `net_busy_loop_on()` 时看 `POLL_BUSY_LOOP`（`include/uapi/asm-generic/poll.h:34`，值 `0x8000`）的忙等。那是套接字的 NAPI 忙等钩子，循环本身留给以后的 networking 章。本章只需要知道：select/poll 的主路径是「拷贝集合 → `vfs_poll` → 睡 → 再扫」。

---

## 2.4 和 epoll 对照

| | select / poll | epoll |
|---|---|---|
| 监视状态放在哪 | 这一次系统调用栈上的 `poll_wqueues`，返回前销毁 | `struct eventpoll` 活到 epfd 关闭 |
| 谁把等待项挂上目标队列 | 每次 `poll_initwait` → `__pollwait` | `EPOLL_CTL_ADD` 时 `ep_ptable_queue_proc`，之后留着 |
| 唤醒回调 | `pollwake`：置 `triggered`，叫醒 `polling_task` | `ep_poll_callback`：把 `epitem` 放进 `rdllist`，叫醒 `ep->wq` |
| 用户缓冲 | 每次拷整张 `fd_set` 或 `pollfd[]` | `epoll_ctl` 拷一条 `epoll_event`；`epoll_wait` 只拷就绪项 |
| 一次调用的扫描量 | 随 `nfds` | 随当前就绪项 |
| `EPOLLET` / `EPOLLONESHOT` / `EPOLLEXCLUSIVE` | 用户位图里没有这些行为位 | 有，见 [05](./05-epoll-wait-and-modes.md) |
| 嵌套「等另一个多路复用 fd」 | 把那个 fd 放进集合即可，每次重新 poll | 同一套 `f_op->poll`，另加环检测 |

fd 个数少、而且每次兴趣集合都换一批时，select/poll 少一次长期对象的维护。fd 个数大、集合稳定时，epoll 把挂队列的成本摊到 `ADD` 上。数量级上的取舍见 [10](./10-perf-analysis.md)，这里不编时间。

---

## 2.5 `poll(2)` 和 `f_op->poll` 是两层

`poll(2)` 是系统调用，入口 `SYSCALL_DEFINE3(poll, ...)`（`select.c:1065`）。它分配 `poll_wqueues`，对用户给出的每个 fd 调用 `vfs_poll`。

`f_op->poll` 是文件操作表上的函数指针。pipe 填 `pipe_poll`，eventfd 填 `eventfd_poll`，epoll 文件自己填 `ep_eventpoll_poll`（`fs/eventpoll.c:948`）。select、poll、epoll 的 `ADD` 和 `epoll_wait` 里的复查，最后都调用到这个指针。

因此「把 epfd 放进另一个 epoll」和「对 epfd 调用 `poll(2)`」走的是同一个 `ep_eventpoll_poll`：它在 `eventpoll.poll_wait` 上 `poll_wait`，再看 `rdllist` 里有没有仍满足兴趣的项。嵌套上限是 epoll 自己加的，见 [04](./04-epoll-ctl-and-callback.md)。`poll(2)` 没有 `EP_MAX_NESTS` 这道检查。
