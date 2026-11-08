# 7. eventfd / signalfd / timerfd

**结论**：这三个 fd 各自有一块小上下文和一把等待队列，`read` 取走计数、信号或到期次数，`f_op->poll` 在计数非零时返回 `EPOLLIN`，因此都能放进 epoll。

> 实现：`fs/eventfd.c`、`fs/signalfd.c`、`fs/timerfd.c`。三个都用 `anon_inode_getfile` / `anon_inode_getfd`。

---

## 7.1 eventfd：一个 64 位计数

```c
/* fs/eventfd.c:30 */
struct eventfd_ctx {
	struct kref kref;
	wait_queue_head_t wqh;
	/*
	 * write 把写入的 __u64 加到 count 上并唤醒 wqh。
	 * 没有 EFD_SEMAPHORE 时，read 把 count 原样返回并清零。
	 * 有 EFD_SEMAPHORE 且 count 非 0 时，read 返回 1，count 减 1。
	 * 内核侧 eventfd_signal 同样是加 count 再唤醒。
	 */
	__u64 count;
	unsigned int flags;    /* 含 EFD_SEMAPHORE 与打开旗标 */
	int id;                /* ida 分配，fdinfo 和调试用 */
};
```

`eventfd2`（`:430`）和 `eventfd`（`:435`，旗标为 0）进 `do_eventfd`（`:386`）。允许的旗标是 `EFD_FLAGS_SET`（`include/linux/eventfd.h:27`）：`EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK`。`EFD_SEMAPHORE` 在 `include/uapi/linux/eventfd.h:7`，值 `1 << 0`。创建时 `count` 等于用户传入的初值，文件名 `"[eventfd]"`。

`eventfd_read`（`:223`）要求缓冲区至少 8 字节。`count` 为 0 且非阻塞则 `-EAGAIN`，否则 `wait_event_interruptible_locked_irq`。然后 `eventfd_ctx_do_read`（`:185`，必须已持 `wqh.lock`）：

```c
*cnt = ((ctx->flags & EFD_SEMAPHORE) && ctx->count) ? 1 : ctx->count;
ctx->count -= *cnt;
```

读完若队列上还有人，`wake_up_locked_poll(&ctx->wqh, EPOLLOUT)`。`eventfd_write`（`:256`）拒绝值为 `ULLONG_MAX` 的写入（`-EINVAL`）。相加会溢出且是阻塞 fd 时，睡到有空间；成功后 `count += ucnt`，`wake_up_locked_poll(..., EPOLLIN)`。

`eventfd_poll`（`:127`）先 `poll_wait` 到 `wqh`，再 `READ_ONCE(count)`：`count > 0` 给 `EPOLLIN`，`count == ULLONG_MAX` 给 `EPOLLERR`，`count` 离 `ULLONG_MAX` 还差至少 1 给 `EPOLLOUT`。函数上面的注释说明这次读必须落在 `poll_wait` 拿到的队列锁之后，否则会和写入错过一次唤醒。

内核子系统用 `eventfd_signal`（`:85`）→ `eventfd_signal_mask`（`:46`）。它在 `wqh.lock` 下把 `count` 加上 `n`（封顶 `ULLONG_MAX`），再 `wake_up_locked_poll(..., EPOLLIN | mask)`。调用期间 `current->in_eventfd` 为 1，重入会 `WARN_ON_ONCE` 并返回 0，用来避开等待队列回调里的递归。KVM 的 irqfd 和 vhost 怎样消费这次信号，见 [interrupts/11](../interrupts/11-kvm-irq-virt.md)。

---

## 7.2 signalfd：从信号队列里读

```c
/* fs/signalfd.c:41 */
struct signalfd_ctx {
	sigset_t sigmask;    /* 取反之后的屏蔽：位为 1 的信号不从本 fd 送出 */
};
```

`signalfd4`（`:302`）进 `do_signalfd4`（`:253`）。`ufd == -1` 时 `kmalloc` 上下文，`anon_inode_getfd("[signalfd]", ...)`。`ufd` 已是一个 signalfd 时，在 `sighand->siglock` 下换掉 `sigmask`，并 `wake_up(&current->sighand->signalfd_wqh)`，让正堵在旧掩码上的读者按新掩码再试一次。`SIGKILL` 和 `SIGSTOP` 会从用户掩码里删掉，然后 `signotset`：上下文里存的是「哪些信号不要」。

`signalfd_poll`（`:51`）把等待项挂到 `current->sighand->signalfd_wqh`（这个字段在 [process/09](../process-management/09-signals.md) 的 `sighand_struct` 里）。然后持 `siglock`，若 `next_signal` 在本线程 `pending` 或线程组 `shared_pending` 里看到掩码允许的信号，返回 `EPOLLIN`。

`signalfd_read`（`:202`）循环调用 `signalfd_dequeue`（`:155`）。dequeue 在 `siglock` 下调用 `dequeue_signal`（`kernel/signal.c:636`）。那条函数先取本线程私有队列，再取组共享队列；注释写明别的任务不能从这里偷走调用者的私有信号。取到之后 `signalfd_copyinfo`（`:71`）把 `kernel_siginfo` 填进用户的 `signalfd_siginfo`。`BUILD_BUG_ON` 要求这个用户结构是 128 字节（`:76`）。一次 `read` 可以取多条，缓冲区按 128 字节对齐，条数是 `count / 128`。

信号入队时谁来敲这把等待队列：`signalfd_notify`（`include/linux/signalfd.h:19`）在 `signalfd_wqh` 上有人时 `wake_up`。`kernel/signal.c` 在把信号放进挂起集合的出口调用它（`:1179`、`:2055`）。`sighand` 被拆掉时 `signalfd_cleanup`（`signalfd.c:36`）做 `wake_up_pollfree`，让还挂着的 poll 项走 `POLLFREE` 摘链。信号怎样产生、怎样选择线程，仍以进程章为准；本章只接到「入队之后叫醒 signalfd，读的时候由 `dequeue_signal` 把 `siginfo` 拿走」。

---

## 7.3 timerfd：hrtimer 或 alarm

```c
/* fs/timerfd.c:31 */
struct timerfd_ctx {
	union {
		struct hrtimer tmr;     /* CLOCK_MONOTONIC / REALTIME / BOOTTIME */
		struct alarm alarm;     /* *_ALARM 两个时钟 */
	} t;
	ktime_t tintv;                  /* it_interval。0 表示单次 */
	ktime_t moffs;                  /* 用来发现实时时钟被调过 */
	wait_queue_head_t wqh;
	u64 ticks;                      /* 到期次数。read 把它清零并返回 */
	int clockid;
	short unsigned expired;         /* 至少到期过一次 */
	short unsigned settime_flags;   /* fdinfo 里打印的 TFD_TIMER_* */
	struct rcu_head rcu;
	struct list_head clist;        /* TFD_TIMER_CANCEL_ON_SET 时挂在全局取消表上 */
	spinlock_t cancel_lock;
	bool might_cancel;
};
```

`timerfd_create`（`:406`）接受的 `clockid` 是 `CLOCK_MONOTONIC`、`CLOCK_REALTIME`、`CLOCK_BOOTTIME`、`CLOCK_REALTIME_ALARM`、`CLOCK_BOOTTIME_ALARM`。后两个还要 `CAP_WAKE_ALARM`，否则 `-EPERM`。旗标只允许 `TFD_CLOEXEC | TFD_NONBLOCK`（`TFD_CREATE_FLAGS`，`include/linux/timerfd.h:16`）。文件名 `"[timerfd]"`。

`timerfd_settime`（`:558`）进 `do_timerfd_settime`（`:454`）→ `timerfd_setup`（`:189`）。`TFD_TIMER_ABSTIME`（`include/uapi/linux/timerfd.h:30`，`1 << 0`）选择 `HRTIMER_MODE_ABS`，否则 `HRTIMER_MODE_REL`。绝对时间还会过 `timens_ktime_to_host`。`TFD_TIMER_CANCEL_ON_SET`（`:31`，`1 << 1`）让时钟被设置时取消等待，读端得到 `-ECANCELED`。这两个位组成 `TFD_SETTIME_FLAGS`（`include/linux/timerfd.h:18`）。

到期回调：

```text
hrtimer          timerfd_tmrproc     :74    container_of 到 t.tmr
alarmtimer       timerfd_alarmproc   :82    container_of 到 t.alarm
                 timerfd_triggered   :63
                   持 wqh.lock
                   expired = 1，ticks++
                   wake_up_locked_poll(&wqh, EPOLLIN)
                 返回 HRTIMER_NORESTART / ALARMTIMER_NORESTART
```

周期定时器的下一次武装发生在用户 `read` 看见到期之后，而不是在回调里。`timerfd_poll`（`:249`）在 `ticks != 0` 时返回 `EPOLLIN`。`read`（`:265`）把 `ticks` 作为一个 `uint64_t` 交给用户并清零。

`CLOCK_MONOTONIC` 不受 `settimeofday` 影响。`CLOCK_REALTIME` 会。需要「墙上时钟被拨动就取消」时用 `TFD_TIMER_CANCEL_ON_SET`，回调是 `timerfd_clock_was_set`（`:97`）。

---

## 7.4 三者怎样进 epoll

| | `.poll` | 挂上的队列 | `EPOLLIN` 的条件 | 内核侧再唤醒的入口 |
|---|---|---|---|---|
| eventfd | `eventfd_poll:127` | `eventfd_ctx.wqh` | `count > 0` | `eventfd_signal_mask:46`，以及 `eventfd_write` |
| signalfd | `signalfd_poll:51` | `sighand->signalfd_wqh` | 私有或共享挂起集合里有掩码允许的信号 | `signalfd_notify` |
| timerfd | `timerfd_poll:249` | `timerfd_ctx.wqh` | `ticks != 0` | `timerfd_triggered:63` |

`epoll_ctl(ADD)` 时这三份 `poll` 都调用 `poll_wait`，于是各得到一个 `eppoll_entry`，`func` 都是 `ep_poll_callback`。之后的唤醒宏是 `wake_up_locked_poll` 或 `wake_up`，`key` 里带 `EPOLLIN`，回调侧的兴趣匹配和 [04](./04-epoll-ctl-and-callback.md) 相同。

---

## 7.5 本机：另一个线程写入 eventfd

[demos/03-eventfd-cross-thread.c](./demos/03-eventfd-cross-thread.c) 用 `eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)`，主线程在 `epoll_ctl(ADD)` 之后创建等待线程。等待线程把 `armed` 写成 1 再 `epoll_wait`（水平触发，超时 2s）。主线程看见 `armed` 后写入 `uint64_t` 值 1。

sampled on 6.12.113+：

```text
kernel=6.12.113+
waiter_events=0x1 count=1
```

`events=0x1` 是 `EPOLLIN`。`count=1` 是没有 `EFD_SEMAPHORE` 时 `read` 取走的计数。这次写入发生在 `ADD` 之后。等待线程若还没进入 `epoll_wait`，写操作仍会走 `ep_poll_callback` 把 `epitem` 放进 `rdllist`，随后的 `epoll_wait` 在快路径上拿到它。输出说明跨线程的这一次计数被 epoll 交付了；本环境没有 `perf`，不额外声称当时一定已经睡在 `schedule` 里。
