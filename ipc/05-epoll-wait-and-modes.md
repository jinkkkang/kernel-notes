# 5. `epoll_wait` 与 LT / ET / ONESHOT / EXCLUSIVE

**结论**：`epoll_wait` 睡在 `eventpoll.wq` 上，醒来后 `ep_send_events` 把 `rdllist` 上仍然满足兴趣的项拷给用户；水平触发会把仍就绪的项挂回就绪链，边缘触发不挂回，ONESHOT 把兴趣掩码收成只剩私有位。

> 入口：`SYSCALL_DEFINE4(epoll_wait, ...)`（`fs/eventpoll.c:2330`）→ `do_epoll_wait`（`:2288`）→ `ep_poll`（`:1798`）→ `ep_send_events`（`:1649`）。
> 用户位定义在 `include/uapi/linux/eventpoll.h`。

---

## 5.1 `do_epoll_wait` 与超时

`do_epoll_wait` 要求 `0 < maxevents <= EP_MAX_EVENTS`（`:96`，`INT_MAX / sizeof(struct epoll_event)`），并用 `access_ok` 检查用户缓冲可写。epfd 必须通过 `is_file_epoll`。然后调用 `ep_poll(ep, events, maxevents, to)`。

`epoll_wait` 的超时单位是毫秒。`ep_timeout_to_timespec`（`:1741`）：`ms < 0` 返回 NULL，表示一直睡；`ms == 0` 得到零 timespec，`ep_poll` 把它当成非阻塞；正数则加上 `ktime_get_ts64` 的现在时刻，变成绝对超时。

`epoll_pwait`（`:2364`）和 `epoll_pwait2`（`:2375`）都进 `do_epoll_pwait`（`:2343`）。后者先 `set_user_sigmask`（`:2353`），再 `do_epoll_wait`，然后 `restore_saved_sigmask_unless(error == -EINTR)`（`:2359`）。`-EINTR` 时保留用户要的掩码，让信号投递和系统调用重启按 [syscalls/08](../syscalls/08-restart.md) 的规则处理。`epoll_pwait` 的超时仍是毫秒；`epoll_pwait2` 用 `get_timespec64` 读 `__kernel_timespec`，再 `poll_select_set_timeout`（`fs/select.c:271`）做合法性检查。`timeout` 指针为 NULL 时 `epoll_pwait2` 一直睡。

---

## 5.2 `ep_poll`：没有事件就睡

```c
/* fs/eventpoll.c:1798 的控制流 */
if (timeout && (timeout->tv_sec | timeout->tv_nsec)) {
	slack = select_estimate_accuracy(timeout);   /* 和 select 共用的超时松弛 */
	expires = timespec64_to_ktime(*timeout);     /* 绝对时间 */
} else if (timeout) {
	timed_out = 1;                               /* 零超时：不进睡眠 */
}
/* timeout == NULL：一直睡，timed_out 保持 0 */

eavail = ep_events_available(ep);                  /* 锁外快看，允许漏看 */
for (;;) {
	if (eavail) {
		res = ep_send_events(ep, events, maxevents);
		if (res)
			return res;                  /* 正数：交给用户的条数；负数：出错 */
	}
	if (timed_out)
		return 0;

	eavail = ep_busy_loop(ep, timed_out);        /* 没配 NAPI 时直接返回 false */
	if (eavail)
		continue;
	if (signal_pending(current))
		return -EINTR;

	init_wait(&wait);                            /* func 先是 autoremove_wake_function */
	wait.func = ep_autoremove_wake_function;     /* :1768，醒来就摘链，不管 ttwu 是否成功 */

	spin_lock_irq(&ep->lock);
	__set_current_state(TASK_INTERRUPTIBLE);     /* 无屏障。两边都在 ep->lock 下，不必再用 set_current_state 的 smp_mb */
	eavail = ep_events_available(ep);
	if (!eavail)
		__add_wait_queue_exclusive(&ep->wq, &wait);
	spin_unlock_irq(&ep->lock);

	if (!eavail)
		timed_out = !schedule_hrtimeout_range(to, slack, HRTIMER_MODE_ABS);
	__set_current_state(TASK_RUNNING);

	eavail = 1;                                  /* 醒了就去收割；超时路径下面再修正 */
	if (!list_empty_careful(&wait.entry)) {
		spin_lock_irq(&ep->lock);
		if (timed_out)
			eavail = list_empty(&wait.entry);
		__remove_wait_queue(&ep->wq, &wait);
		spin_unlock_irq(&ep->lock);
	}
}
```

`ep_autoremove_wake_function`（`:1768`）先调 `default_wake_function`，再无条件 `list_del_init_careful`。注释说明：等待项不在循环之间复用；若进程被杀死，各线程被叫醒时也必须已经离队。多个线程堵在同一个 `ep->wq` 上时，每一次 `wake_up`（`nr_exclusive = 1`）打中队头那个独占等待者，把他摘掉，下一次唤醒才会打中下一个。等待项若不摘链，同一个人会反复消耗名额。

`__add_wait_queue_exclusive`（`include/linux/wait.h:188`）置 `WQ_FLAG_EXCLUSIVE`，再按 `__add_wait_queue` 插到优先级项之后、普通项的最前。`epoll_wait` 这边的独占，和目标 fd 上的 `EPOLLEXCLUSIVE` 是两层：前者限制「同一个 epfd 醒几个线程」，后者限制「同一个目标 fd 叫几个 epoll」。

`ep_busy_loop`（`:408`，要 `CONFIG_NET_RX_BUSY_POLL`）读 `ep->napi_id`。id 达到 `MIN_NAPI_ID` 且 `net_busy_loop_on()` 时调用 `napi_busy_loop`。超时后把 `napi_id` 清 0，等下次回调经 `ep_set_busy_poll_napi_id`（`:431`）从 `sk->sk_napi_id` 再记上。没开这个配置时函数直接返回 false（`:465`）。`napi_busy_loop` 的预算和软中断出口留给以后的 networking 章。

`ep_send_events` 开头若 `fatal_signal_pending(current)` 返回 `-EINTR`（`:1662`），避免致命信号路径上反复找到新事件、退不出去。

---

## 5.3 `ep_send_events`：LT、ET、ONESHOT 分叉的地方

函数持 `ep->mtx`，`ep_start_scan` 把 `rdllist` 偷到私有 `txlist`，并把 `ovflist` 打开（见 [03](./03-epoll-objects.md)）。扫描 `txlist` 时不持 `ep->lock`。`init_poll_funcptr(&pt, NULL)` 让复查用的 `poll_wait` 变成空操作，不会在这次 `epoll_wait` 里再挂一套 `eppoll_entry`。

```c
/* fs/eventpoll.c:1674 起，每一项 */
list_del_init(&epi->rdllink);

revents = ep_item_poll(epi, &pt, 1);    /* 再问一次 f_op->poll，_qproc 为空 */
if (!revents)
	continue;                           /* 就绪链上的过期项：丢掉，不挂回 */

events = epoll_put_uevent(revents, epi->event.data, events);
if (!events) {
	list_add(&epi->rdllink, &txlist);   /* 拷贝失败，放回，ep_done_scan 会归还 rdllist */
	ep_pm_stay_awake(epi);
	if (!res)
		res = -EFAULT;
	break;
}
res++;

if (epi->event.events & EPOLLONESHOT)
	epi->event.events &= EP_PRIVATE_BITS;          /* 清掉 EPOLLIN 等，保留行为位 */
else if (!(epi->event.events & EPOLLET)) {
	list_add_tail(&epi->rdllink, &ep->rdllist);    /* LT：下次 epoll_wait 还会复查 */
	ep_pm_stay_awake(epi);
}
/* ET：两个分支都不进。项留在就绪链之外，直到下一次 ep_poll_callback */
```

`epoll_put_uevent`（`include/linux/eventpoll.h:77`）对非 ARM OABI 就是两次 `__put_user`：`revents` 写到 `events`，`epi->event.data` 写到 `data`。用户看到的 `events` 是这次 `f_op->poll` 的返回值按兴趣掩码截过的，不是注册时原样存下来的那一长串位。

`ep_item_poll` 把 `pt->_key` 设成 `epi->event.events` 再调用 `vfs_poll`，返回值再和兴趣掩码相与（`:939`、`:945`）。ONESHOT 已经把 `EPOLLIN` 清掉之后，下一次复查的相与结果是 0，回调侧也会在 `EP_PRIVATE_BITS` 那道判断处直接返回。要重新武装，用户再发一次 `EPOLL_CTL_MOD`，`ep_modify` 写回掩码并立刻复查，仍然就绪就会重新入链。

LT 挂回 `rdllist` 时，注释（`:1720`）说明此刻只有本线程能插 `rdllist`：别的 `epoll_ctl` 被 `mtx` 挡住，并发的 `ep_poll_callback` 只会写 `ovflist`。`ep_done_scan` 把溢出链合并回来。

ET 不挂回。数据还在（pipe 里还有字节、eventfd 计数还大于 0）也没有关系：下一次 `epoll_wait` 看不到这项，除非目标再 `wake_up` 一次，回调重新入链。这就是边缘触发要求用户把 fd 读到 `EAGAIN` 的原因。

---

## 5.4 四种行为

位定义（`include/uapi/linux/eventpoll.h`）：

| 宏 | 行 | 值 | 作用在哪 |
|----|----|----|----------|
| `EPOLLEXCLUSIVE` | `:51` | bit 28 | `ADD` 时把 `eppoll_entry` 以独占方式挂到目标队列。`MOD` 不允许新加上这一位 |
| `EPOLLWAKEUP` | `:63` | bit 29 | 给 `epitem` 配 `wakeup_source`。需要 `CAP_BLOCK_SUSPEND` |
| `EPOLLONESHOT` | `:66` | bit 30 | 交付一次后兴趣掩码只留 `EP_PRIVATE_BITS` |
| `EPOLLET` | `:69` | bit 31 | 交付后不挂回 `rdllist` |
| `EPOLL_URING_WAKE` | `:48` | bit 27 | 内核内部。随 `poll_wait` 的唤醒掩码传给 io_uring，见 [04](./04-epoll-ctl-and-callback.md) |

| | 交付后就绪链 | 数据还在时下一次 `epoll_wait` | 典型用法 |
|---|---|---|---|
| 默认（LT） | 复查仍有事件则挂回 | 立刻再返回 | 读一点、处理一点，下轮还会看到 |
| `EPOLLET` | 不挂回 | 不再返回，直到目标再次唤醒 | 必须一次读到 `EAGAIN`，减少重复唤醒 |
| `EPOLLONESHOT` | 不挂回，且兴趣掩码被清 | 不再返回，直到 `EPOLL_CTL_MOD` | 多线程共享一个 epfd 时，保证一项只被一个线程领走 |
| `EPOLLEXCLUSIVE` | 由 LT/ET 决定，这一位不管就绪链 | 同上 | 多个 epfd 监视同一 listen fd，让目标队列只叫醒有限个 epoll |

`EPOLLEXCLUSIVE` 可以和 `EPOLLET` 同时置上（它在 `EPOLLEXCLUSIVE_OK_BITS` 里）。它不改变 `ep_send_events` 的挂回逻辑。

多线程对同一个 epfd 调用 `epoll_wait` 时，睡眠侧已经是独占的：一次 `ep_poll_callback` 里的 `wake_up(&ep->wq)` 只叫一个线程。LT 把项挂回之后，`ep_done_scan` 发现 `rdllist` 仍非空会再 `wake_up` 一次，下一个线程才能接到。ONESHOT 清掉兴趣位，挂回不会发生，其他线程看不到同一项，直到某人 `MOD`。

没有 `EPOLLEXCLUSIVE` 的 listen fd 被多个 epfd 监视时，接受队列上的一次唤醒会叫到每一个 epoll 的 `ep_poll_callback`（那些 `eppoll_entry` 是非独占的）。每个 epoll 再各叫一个自己的 `epoll_wait`。这是 accept 惊群的 epoll 形态。加上 `EPOLLEXCLUSIVE` 之后，目标队列的独占名额让这次 `wake_up` 只消耗一个匹配的 epoll。仍可能有多于一个线程被叫到：非独占的等待者（例如同时在 `accept` 里睡着的线程）会在独占名额之前被扫到。这一位收的是 epoll 之间的扇出，不是把整个系统收成恰好一个 `accept` 返回。

---

## 5.5 本机：LT 四次、ET 一次、ONESHOT 要 MOD

[demos/01-epoll-lt-vs-et.c](./demos/01-epoll-lt-vs-et.c) 对一根非阻塞管道写入 4 字节（ONESHOT 那次是 2 字节）。每次 `epoll_wait` 命中只 `read` 1 字节，超时 200ms 停止。ONESHOT 第一次命中后不读，第二次应超时，`EPOLL_CTL_MOD` 把同一掩码写回去之后应再命中一次。

sampled on 6.12.113+（`gcc -Wall -Wextra -O2`，结构行为以 v6.6.160 的 `ep_send_events` 为准）：

```text
kernel=6.12.113+
lt_hits=4
et_hits=1 et_unread_after_first=3
oneshot_first=1 oneshot_second=0 oneshot_after_mod=1
```

LT 的 4 次对应「每读走 1 字节，管道仍非空，项被挂回 `rdllist`」。ET 的 1 次之后还剩 3 字节，程序把它们用非阻塞 `read` 排空，第二次 `epoll_wait` 没有再返回。ONESHOT 在没有 `MOD` 时第二次为 0，`MOD` 之后 `ep_modify` 复查到管道仍可读，再次返回。

---

## 5.6 三个常见坑

**ET 没读完。** 上面的 `et_unread_after_first=3` 就是这个坑的计数。边缘触发之后内核不会因为「管道里还有字节」再把项放回就绪链。用户循环要读到 `EAGAIN`（或写到 `EAGAIN`）。只读一次就回到 `epoll_wait`，剩余字节一直留到下一次真正的 `wake_up`。

**`dup` 之后的 fd 号。** 红黑树键是 `(struct file *, fd)`。两个号是两项，两套 `eppoll_entry` 挂在同一组等待队列上。关掉其中一个号并不走到 `eventpoll_release_file`（`struct file` 还有引用）。那一项的 `ffd.fd` 仍是已经关闭的号，`epoll_wait` 还能把它报出来。`DEL` 要在 `close` 之前，用注册时的那个号。

**嵌套深度。** 把 epfd 加进另一个 epoll 会走 `ep_loop_check`，`EP_MAX_NESTS` 为 4。超过返回 `-ELOOP`。每一层的 `f_op->poll` 都会在持有外层 `mtx` 的情况下再锁内层 `mtx`（`mutex_lock_nested`），所以既限制环，也限制锁的嵌套层数。细节在 [04](./04-epoll-ctl-and-callback.md)。
