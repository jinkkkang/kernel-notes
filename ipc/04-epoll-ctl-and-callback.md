# 4. `epoll_ctl` 与 `ep_poll_callback`

**结论**：`EPOLL_CTL_ADD` 调用目标的 `f_op->poll`，把 `ep_poll_callback` 留在目标等待队列上；之后目标一被 `wake_up*`，回调就在持有那把队列锁的上下文里把 `epitem` 放进就绪链，并叫醒 `ep->wq`。

> 入口：`SYSCALL_DEFINE4(epoll_ctl, ...)`（`fs/eventpoll.c:2272`）→ `do_epoll_ctl`（`:2116`）。
> 回调：`ep_poll_callback`（`:1133`）。它是 [locks-and-ds/08](../locks-and-ds/08-wait-queue-and-completion.md) 里自定义 `wait_queue_entry.func` 的例子：`private` 保持 NULL，对象用 `container_of` 从 `eppoll_entry.wait` 找回来。

[demos/01-epoll-lt-vs-et.c](./demos/01-epoll-lt-vs-et.c) 的 `epoll_ctl(ADD)` 走本节的 `ep_insert`。LT / ET / ONESHOT 的差别在交付时才出现，见 [05](./05-epoll-wait-and-modes.md)。

---

## 4.1 `do_epoll_ctl` 的门禁

| 检查 | 失败返回 | 位置 |
|------|----------|------|
| `fdget(epfd)` 或 `fdget(fd)` 失败 | `-EBADF` | `:2127`、`:2132` |
| 目标没有 `f_op->poll` | `-EPERM` | `:2137`，`file_can_poll` |
| epfd 与目标是同一个 `struct file`，或 epfd 不是 epoll | `-EINVAL` | `:2151` |
| `EPOLLEXCLUSIVE` 用在 `MOD` 上 | `-EINVAL` | `:2160` |
| `ADD` 且带 `EPOLLEXCLUSIVE`，但目标是 epoll，或事件位超出 `EPOLLEXCLUSIVE_OK_BITS` | `-EINVAL` | `:2162` |
| `ADD` 时树上已有同一 `(file, fd)` | `-EEXIST` | `:2226` |
| `DEL` / `MOD` 找不到项 | `-ENOENT` | `:2237`、`:2247` |
| 嵌套成环或深度超过 `EP_MAX_NESTS` | `-ELOOP` | `:2202` |
| 用户监视数达到 `max_user_watches` | `-ENOSPC` | `ep_insert:1466` |

`EPOLLEXCLUSIVE_OK_BITS`（`:89`）是 `EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLWAKEUP | EPOLLET | EPOLLEXCLUSIVE`。带独占位时不能再要 `EPOLLPRI`、`EPOLLRDHUP` 这一类。

`ep_op_has_event`（`include/linux/eventpoll.h:60`）对 `ADD` 和 `MOD` 为真，`DEL` 为假。有事件参数时，`epoll_ctl` 先 `copy_from_user` 一整份 `struct epoll_event`（`:2277`）。`EPOLLWAKEUP` 再过 `ep_take_care_of_epollwakeup`（`:2092`）：`CONFIG_PM_SLEEP` 下没有 `CAP_BLOCK_SUSPEND` 就清掉这一位；没开电源管理配置时直接清掉。

`ADD` 和 `MOD` 在调用 `ep_insert` / `ep_modify` 之前把 `EPOLLERR | EPOLLHUP` 或进用户掩码（`:2223`、`:2243`）。fdinfo 里因此总能看到这两位，即使用户没写。`MOD` 若现有项已经带 `EPOLLEXCLUSIVE`，函数不改掩码，`error` 保持进入 `switch` 时的 `-EINVAL`。

非阻塞变体由 `do_epoll_ctl` 的 `nonblock` 参数控制：`epoll_mutex_lock`（`:2104`）失败返回 `-EAGAIN`。系统调用 `epoll_ctl` 传入 `false`。

---

## 4.2 `ADD`：`ep_insert`

调用时已持有 `ep->mtx`。`full_check` 为真表示这次拿过全局 `epnested_mutex`，下面要做环和唤醒路径检查。

```text
ep_insert                          eventpoll.c:1450
  percpu_counter 对照 max_user_watches，然后 inc
  kmem_cache_zalloc(epi_cache)
  填 ep、ffd、event，next = EP_UNACTIVE_PTR，INIT_LIST_HEAD(rdllink)
  目标是 epoll 时 mutex_lock_nested(&tep->mtx, 1)
  attach_epitem                    挂 fllink，见 03
  ep_rbtree_insert                 :1271，键 (file, fd)，rb_insert_color_cached
  ep_get(ep)                       这项活着就多握一次 ep 引用
  full_check 时 reverse_path_check，失败 -EINVAL
  带 EPOLLWAKEUP 则 ep_create_wakeup_source
  epq.epi = epi
  init_poll_funcptr(&epq.pt, ep_ptable_queue_proc)
  revents = ep_item_poll(epi, &epq.pt, 1)     这里目标 poll 会调用 poll_wait
  epq.epi 被写成 NULL：pwq 分配失败，ep_remove，返回 -ENOMEM
  持 ep->lock：revents 非 0 且尚未入链 → list_add_tail(rdllist)，必要时 wake_up(wq)
```

`ep_item_poll`（`:926`）先 `epi_fget` 给目标 `struct file` 加引用。目标不是 epoll 时调用 `vfs_poll`；目标是 epoll 时调用 `__ep_eventpoll_poll`，避免在已持有的 `mtx` 上再走一遍 `ep_eventpoll_poll` 的加锁注解。返回值再和 `epi->event.events` 按位与。`pt->_key` 被设成用户兴趣掩码，所以目标的 `poll` 实现可以只对关心的队列头调用 `poll_wait`。pipe 的 `pipe_poll` 仍按 `f_mode` 把读等待和写等待都挂上，见 [06](./06-pipe.md)。

`MOD` 走 `ep_modify`（`:1577`）。它不重新挂 `pwqlist`，只换 `event`，必要时创建或拆掉 `wakeup_source`，然后 `smp_mb()`，再 `ep_item_poll` 一次（这次 `_qproc` 为 NULL，只复查、不新挂队列）。复查为真就把项放进 `rdllist`。屏障的两条用途写在 `:1602`：让别的 CPU 上的 `ep_poll_callback` 看见新掩码，并和 `wq_has_sleeper` 配对，避免漏掉「poll 之前就已经就绪」的状态。

`DEL` 走 `ep_remove`（`:802`）。它先 `ep_unregister_pollwait`，看到 `dying` 就返回（关闭路径已经在摘），否则 `epi_fget` 成功后在 `file->f_lock` 下摘 `fllink` 和红黑树节点。`ep_remove_epi`（`:774`）对 `epi` 做 `call_rcu`，并 `percpu_counter_dec` 监视数。

---

## 4.3 `ep_ptable_queue_proc`：把回调挂上去

```c
/* fs/eventpoll.c:1244。由目标 f_op->poll 经 poll_wait 调用。
 * 此时 ep->mtx 已持有。分配使用 GFP_KERNEL，所以这条路径必须能睡眠。
 */
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
				 poll_table *pt)
{
	struct ep_pqueue *epq = container_of(pt, struct ep_pqueue, pt);
	struct epitem *epi = epq->epi;
	struct eppoll_entry *pwq;

	if (unlikely(!epi))
		return;                              /* 更早的一次分配已经失败 */

	pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL);
	if (unlikely(!pwq)) {
		epq->epi = NULL;                     /* 告诉 ep_insert：安装失败 */
		return;
	}

	init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);
	pwq->whead = whead;
	pwq->base = epi;
	if (epi->event.events & EPOLLEXCLUSIVE)
		add_wait_queue_exclusive(whead, &pwq->wait);   /* 队尾，WQ_FLAG_EXCLUSIVE */
	else
		add_wait_queue(whead, &pwq->wait);
	pwq->next = epi->pwqlist;
	epi->pwqlist = pwq;
}
```

`add_wait_queue_exclusive`（`kernel/sched/wait.c:28`）置 `WQ_FLAG_EXCLUSIVE` 后 `list_add_tail`。目标文件下次 `wake_up` / `wake_up_poll`（`nr_exclusive == 1`）时，非独占项全被调用，独占项只消耗一个名额。多个 epoll 用 `EPOLLEXCLUSIVE` 监视同一 listen fd，就是靠这个名额把「每个 epoll 各醒一次」收成有限次。返回值怎样消耗名额，见下一节。

---

## 4.4 `ep_poll_callback`

`__wake_up_common`（`kernel/sched/wait.c:80`）持有目标 `wait_queue_head.lock`，再调用 `wait->func`。所以 `ep_poll_callback` 进来时已经持有目标队列的锁，并且可能处在硬中断上下文。它自己再拿 `ep->lock`（另一把自旋锁），不能睡眠，也不能在这个上下文里 `copy_to_user`。

```c
/* fs/eventpoll.c:1133 的控制流 */
epi = ep_item_from_wait(wait);          /* container_of → eppoll_entry.base */
ep  = epi->ep;
pollflags = key_to_poll(key);           /* 唤醒者经 poll_to_key 塞进来的事件位 */
spin_lock_irqsave(&ep->lock, flags);

ep_set_busy_poll_napi_id(epi);          /* 有 sk_napi_id 就记到 ep->napi_id */

/* ONESHOT 交付后兴趣掩码只剩 EP_PRIVATE_BITS，这项暂时禁用 */
if (!(epi->event.events & ~EP_PRIVATE_BITS))
	goto out_unlock;

/* key 非 0 且和兴趣没有交集：不是这次要的方向 */
if (pollflags && !(pollflags & epi->event.events))
	goto out_unlock;

if (READ_ONCE(ep->ovflist) != EP_UNACTIVE_PTR) {
	/* ep_send_events 正在拷用户缓冲，插到溢出单链表 */
	if (epi->next == EP_UNACTIVE_PTR) {
		epi->next = READ_ONCE(ep->ovflist);
		WRITE_ONCE(ep->ovflist, epi);
		ep_pm_stay_awake_rcu(epi);
	}
} else if (!ep_is_linked(epi)) {
	list_add_tail(&epi->rdllink, &ep->rdllist);
	ep_pm_stay_awake_rcu(epi);
}

if (waitqueue_active(&ep->wq)) {
	/* 下面按 pollflags 决定独占项要不要消耗目标队列上的那一个名额 */
	if ((epi->event.events & EPOLLEXCLUSIVE) && !(pollflags & POLLFREE)) {
		switch (pollflags & EPOLLINOUT_BITS) {   /* EPOLLIN | EPOLLOUT */
		case EPOLLIN:  if (epi->event.events & EPOLLIN)  ewake = 1; break;
		case EPOLLOUT: if (epi->event.events & EPOLLOUT) ewake = 1; break;
		case 0:        ewake = 1; break;          /* 两个方向位都没有 */
		}
	}
	if (sync)
		wake_up_sync(&ep->wq);   /* 第三个参数是 wake_flags，非 0 走 sync */
	else
		wake_up(&ep->wq);        /* nr_exclusive = 1，见下节 */
}
if (waitqueue_active(&ep->poll_wait))
	pwake++;                         /* 锁外再 ep_poll_safewake */

out_unlock:
spin_unlock_irqrestore(&ep->lock, flags);
if (pwake)
	ep_poll_safewake(ep, epi, pollflags & EPOLL_URING_WAKE);

if (!(epi->event.events & EPOLLEXCLUSIVE))
	ewake = 1;

if (pollflags & POLLFREE) {
	list_del_init(&wait->entry);     /* 队列即将释放，等待者必须自己摘链 */
	smp_store_release(&ep_pwq_from_wait(wait)->whead, NULL);
}
return ewake;
```

第三个参数在 `wait_queue_func_t` 里是 `wake_flags`。pipe 的 `wake_up_interruptible_sync_poll` 经 `__wake_up_sync_key`（`wait.c:207`）传入 `WF_SYNC`，这里非 0，于是叫醒 `epoll_wait` 时用 `wake_up_sync`。`WF_SYNC` 的含义（叫醒之后自己马上要睡、尽量不迁 CPU）在 locks-and-ds/08。

`ep_pm_stay_awake_rcu`（`:603`）在 RCU 读侧取出 `epi->ws`，非空则 `__pm_stay_awake`。没有 `EPOLLWAKEUP` 时 `ws` 为空，这几行是空操作。

`POLLFREE`（`include/uapi/asm-generic/poll.h:32`，值 `0x4000`）表示这个等待队列头正在被拆掉。回调必须在持有队列锁时把自己摘下来，并把 `whead` 置 NULL。`ep_remove_wait_queue`（`:550`）读到 NULL 就不再 `remove_wait_queue`。配对用的是 `smp_store_release` / `smp_load_acquire`。

### 返回值怎样限制独占唤醒

`__wake_up_common` 的规则：`func` 返回负值就停止扫描；返回正值并且该项带 `WQ_FLAG_EXCLUSIVE` 时，`nr_exclusive` 减一，减到 0 就停止。`wake_up` 和 `wake_up_poll` 的 `nr_exclusive` 都是 1。

因此：

- 兴趣里没有 `EPOLLEXCLUSIVE` 时，函数末尾把 `ewake` 写成 1。这一项是 `add_wait_queue` 挂上的，没有 `WQ_FLAG_EXCLUSIVE`，返回 1 不消耗名额，扫描继续，同一队列上的其他 epoll 都会被调用。
- 有 `EPOLLEXCLUSIVE` 时，只有「纯 `EPOLLIN` 且兴趣含 `EPOLLIN`」「纯 `EPOLLOUT` 且兴趣含 `EPOLLOUT`」或「两个方向位都没有」才返回 1，消耗掉这唯一的名额，后面的独占项不再被调用。`pollflags` 同时带 `EPOLLIN` 和 `EPOLLOUT` 时 `switch` 没有对应的 `case`，`ewake` 保持 0，名额留给后面的独占项。
- 事件位和兴趣不相交而走到 `out_unlock` 时，独占项还没把 `ewake` 写成 1，返回 0，不消耗名额。

这只决定「目标文件的等待队列上，这次 `wake_up` 叫到几个 epoll」。每个被叫到的 epoll 再对它自己的 `ep->wq` 做一次 `wake_up`（同样 `nr_exclusive = 1`）。同一 epfd 上睡着的多个 `epoll_wait` 怎样一个一个醒，见 [05](./05-epoll-wait-and-modes.md)。

`ep->poll_wait` 的唤醒放在放下 `ep->lock` 之后，函数是 `ep_poll_safewake`（`:542`；开了 lockdep 时是 `:503` 的嵌套版本）。它再 `wake_up_poll(&ep->poll_wait, EPOLLIN | pollflags)`。传入的 `pollflags` 只保留 `EPOLL_URING_WAKE`（`uapi/linux/eventpoll.h:48`，bit 27）。这一位让 io_uring 的 poll 回调认出「这次唤醒是自己引起的」，避免递归。`io_poll_add` 的对象在 [block-io/12](../block-io/12-io-uring.md)，这里不展开。

开 lockdep 时，`ep_poll_safewake` 用 `ep->nests` 给 `poll_wait.lock` 套 `spin_lock_irqsave_nested`，并在注释里把嵌套上限和 `EP_MAX_NESTS` 放在一起（`:495`）：唤醒链可以是「设备 → 内层 epoll → 外层 epoll」，层数不能把栈打穿。

---

## 4.5 嵌套：`ep_loop_check` 与 `EP_MAX_NESTS`

`EP_MAX_NESTS` 是 4（`:93`）。`ADD` 的目标是另一个 epoll，或者本 epfd 已经被别人监视（`f_ep` 非空），或者 `ep->gen` 刚好等于当前 `loop_check_gen` 时，`do_epoll_ctl` 先放下 `ep->mtx`，拿全局 `epnested_mutex`，把 `loop_check_gen` 加一，`full_check = 1`。

目标是 epoll 时调用 `ep_loop_check(ep, tep)`（`:2001`）：

1. `inserting_into = ep`（即将被插进去的那个外层）。
2. `ep_loop_check_proc(tep, 0)`（`:1933`）从目标向下走。碰到 `inserting_into` 或者 `depth > EP_MAX_NESTS`，深度记成 `EP_MAX_NESTS + 1`。叶子若不是 epoll，挂到 `tfile_check_list`，留给 `reverse_path_check` 限制从一个文件扇出的唤醒路径条数。
3. `ep_get_upwards_depth_proc`（`:1976`）在 RCU 下沿 `refs` 从外层向上走。
4. `depth + 1 + upwards_depth > EP_MAX_NESTS` 则返回 `-1`，系统调用返回 `-ELOOP`。

两条 `ADD` 同时进行（A 加 B、B 加 A）时，谁没拿到 `epnested_mutex` 谁就看不见对方的半成品。这是这把全局锁存在的原因，文件头 `:63` 写了这个竞态。

`reverse_path_check`（`:1359`）和 `path_count_inc`（`:1309`）限制的是「从一个被监视文件出发、长度为 1..5 的唤醒路径条数」，避免一个 fd 扇出太多 epoll。超限同样在 `ep_insert` 里变成 `-EINVAL`。路径条数表在 `:1294` 一带，本章不逐项展开。

把 epfd `ADD` 进它自己，在更早的 `f.file == tf.file` 就被 `-EINVAL` 拒绝，到不了环检测。

---

## 4.6 `EPOLLWAKEUP`

用户位 `EPOLLWAKEUP`（`uapi/linux/eventpoll.h:63`，bit 29）要求处理事件期间阻止系统自动休眠。`ep_create_wakeup_source`（`:1375`）在第一次需要时给 `ep->ws` 注册名字 `"eventpoll"`，再按目标 dentry 的名字给 `epi->ws` 注册一个。`ep_send_events` 在把项交给用户之前对 `epi->ws` 做 `__pm_relax`，若它当时是 active 的，则对 `ep->ws` 做 `__pm_stay_awake`，避免两项交接的空档触发自动休眠（`:1681` 的注释）。

`DEL` 或 `MOD` 去掉该位时走 `ep_destroy_wakeup_source`（`:1398`）：`RCU_INIT_POINTER` 清掉 `epi->ws`，`synchronize_rcu` 之后 `wakeup_source_unregister`。电源管理子系统的休眠状态机不在本章。

---

## 4.7 和等待队列一节的衔接

| | 通用 `wait_event` | epoll 的目标侧 | epoll 的 `epoll_wait` 侧 |
|---|---|---|---|
| 条目放在哪 | 调用者栈上 | `eppoll_entry`，slab `eventpoll_pwq` | `ep_poll` 的栈上 `wait_queue_entry_t` |
| `func` | `autoremove_wake_function` | `ep_poll_callback` | `ep_autoremove_wake_function` |
| `private` | `current` | `NULL` | `current` |
| 被叫醒时做什么 | `try_to_wake_up` | 把 `epitem` 放进就绪链，再 `wake_up(ep->wq)` | `default_wake_function`，然后无条件摘链 |

目标侧这个 `func` 不能换成 `default_wake_function`：`private` 是 NULL，送进去会把空指针当成 `task_struct`。对象身份在 `eppoll_entry.base`。
