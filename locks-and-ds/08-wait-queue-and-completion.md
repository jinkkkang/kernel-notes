# 8. 等待队列与 completion

**结论**：等的是**一件事**，用 `wait_queue_head` + `wait_event`，或者用 `completion`。等的是**一把锁**，仍用第 6 节的 mutex / rwsem，它们自带 `wait_list`，不走这套队列。6.6 的 `struct completion` 内部睡在 **swait** 上，字段类型是 `struct swait_queue_head`，不是 `wait_queue_head_t`。

> 头文件：`include/linux/wait.h`、`include/linux/completion.h`、`include/linux/swait.h`。
> 实现：`kernel/sched/wait.c`、`kernel/sched/completion.c`、`kernel/sched/swait.c`。
> 读序插在 [06 可睡眠锁](./06-sleepable-locks.md) 与 [07 RCU](./07-rcu.md) 之间。下一篇是 [09 futex](./09-futex.md)。章末观测仍是 [`08-observability.md`](./08-observability.md)（文件名暂不改，读序最后）。

本篇只写通用睡/醒和 completion。用户态 futex 见 [09](./09-futex.md)。`wait_on_bit` 的哈希表、`wait_woken` 那套自定义协议，点到名字为止。

---

## 8.1 为什么 mutex 的 `wait_list` 不够

mutex 的等待者是栈上的 `struct mutex_waiter`，用它的 `list` 挂进 `mutex.wait_list`（结构在 [06](./06-sleepable-locks.md)）。这条链只回答一个问题：谁想拿**这把锁**。唤醒资格也绑在锁上——非 RT 的 mutex 只有持有者能 `mutex_unlock`。

事件等待是另一问：条件已经成立了吗。条件可以由别的任务、软中断、硬中断改掉，等待者之间没有「持有者」。多人可以等同一个条件，唤醒可以是一个，也可以是一队。醒来之后必须**再判一次条件**，因为唤醒和条件之间没有锁语义帮你兜住。

同一章里已经能看见这两套东西并排存在。`percpu_rw_semaphore` 的 `waiters` 字段类型就是 `wait_queue_head_t`（`include/linux/percpu-rwsem.h:16`，结构见 06）。那是通用等待队列嵌进一把锁。mutex 的 `wait_list` 仍然是 `struct list_head`，元素是 `mutex_waiter`，不是 `wait_queue_entry`。

睡眠从这里进调度器：`wait_event` 的 `cmd` 是 `schedule()`（`kernel/sched/core.c:6789`）。谁被选中接着跑，见调度章 [04](../scheduler/04-enqueue-wakeup.md) 和进程章 [08](../process-management/08-context-switch.md)。本篇停在入队、改 `__state`、调用 `schedule` / `try_to_wake_up`。

```text
mutex.wait_list          谁在等这把锁（mutex_waiter）
wait_queue_head.head     谁在等这件事（wait_queue_entry，func 回调）
completion.done + wait   这件事做完了几次（6.6 的 wait 是 swait）
```

---

## 8.2 `wait_queue_head` / `wait_queue_entry`

```c
/* include/linux/wait.h:30 */
struct wait_queue_entry {
	unsigned int		flags;    /* 中文：WQ_FLAG_* */
	void			*private; /* 中文：默认是 task_struct *；自定义 func 可以另用 */
	wait_queue_func_t	func;     /* 中文：唤醒回调；默认 default_wake_function */
	struct list_head	entry;    /* 中文：挂进 wait_queue_head.head */
};

/* include/linux/wait.h:37 */
struct wait_queue_head {
	spinlock_t		lock;     /* 中文：保护 head。非 RT 不睡；RT 上 spinlock_t 的替换见第 5 节 */
	struct list_head	head;     /* 中文：等待者链表 */
};
```

`wait_queue_entry_t` 是 `:14` 的 typedef，`wait_queue_head_t` 是 `:41`。回调类型在 `:16`：

```c
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry,
				 unsigned mode, int flags, void *key);
```

`flags`（`:20` 起）本篇用到的几位：

| 标志 | 值 | 作用 |
|---|---|---|
| `WQ_FLAG_EXCLUSIVE` | `0x01` | 占用 `nr_exclusive` 名额；插入时放到队尾 |
| `WQ_FLAG_WOKEN` | `0x02` | `wait_woken` 协议用，本篇不展开 |
| `WQ_FLAG_BOOKMARK` | `0x04` | 唤醒扫描中途放下锁的书签，不是真正的等待者 |
| `WQ_FLAG_PRIORITY` | `0x20` | 插在非优先级项前面；`add_wait_queue_priority` 会同时置上 EXCLUSIVE |

`WQ_FLAG_CUSTOM`（`0x08`）和 `WQ_FLAG_DONE`（`0x10`）在这个头里定义了。通用 `wait_event` 路径不靠它们表达「独占 / 非独占」。

队列锁是 `spinlock_t`。非 RT 上 `add_wait_queue` / `__wake_up` 用 `spin_lock_irqsave`（`wait.c:22`、`:137`），硬中断里可以叫醒人。RT 上这把 `spinlock_t` 可以睡，原子上下文改用 `raw_spinlock_t` 的规则见第 5 节。等的那一侧无论是否 RT 都必须能 `schedule`。`wait_event` 开头的 `might_sleep()`（`wait.h:346`）在原子上下文会走到观测章那条 `sleeping function called from invalid context`（`kernel/sched/core.c:10241`）。

### 初始化与入队

| API | 位置 | 做什么 |
|---|---|---|
| `DECLARE_WAIT_QUEUE_HEAD` | `wait.h:61` | 静态头：`spinlock` 未锁 + `LIST_HEAD_INIT` |
| `init_waitqueue_head` | `wait.h:66` | 动态头。展开成 `__init_waitqueue_head`（`wait.c:8`）：`spin_lock_init`、给 lockdep 登记类名、`INIT_LIST_HEAD` |
| `DECLARE_WAITQUEUE` | `wait.h:54` | 栈上/静态 entry：`private = task`，`func = default_wake_function` |
| `init_waitqueue_entry` | `wait.h:82` | 同上，`flags = 0` |
| `init_waitqueue_func_entry` | `wait.h:90` | 自定义 `func`，`private = NULL` |
| `init_wait_entry` | `wait.c:301` | `___wait_event` 用的入口：`private = current`，`func = autoremove_wake_function`，`INIT_LIST_HEAD` |
| `add_wait_queue` | `wait.c:17` | 清掉 EXCLUSIVE，经 `__add_wait_queue`（`wait.h:171`）插到优先级段之后、普通项的前面 |
| `add_wait_queue_exclusive` | `wait.c:28` | 置 EXCLUSIVE，`list_add_tail` 到队尾 |
| `add_wait_queue_priority` | `wait.c:39` | `EXCLUSIVE \| PRIORITY`，插在优先级段末尾（仍在普通项之前） |
| `remove_wait_queue` | `wait.c:50` | 持锁 `list_del` |
| `DEFINE_WAIT` | `wait.h:1212` | `func = autoremove_wake_function`，`private = current` |

`__add_wait_queue` 的插入点：从队头跳过所有 `WQ_FLAG_PRIORITY`，然后 `list_add`。非独占的新来者排在普通段的最前，先被扫描到。独占项走队尾，先到先被独占唤醒。这就是 `wait.c:68` 那段注释说的：非独占在前，独占在后，优先级项可以在队头把事件吃掉。

`waitqueue_active`（`wait.h:127`）只看链表非空，**不上锁**。注释写了标准误用：把「条件已成立」的存储重排到 `waitqueue_active` 的负载之后，唤醒者看见空队列，等待者又没看见条件。要做这个优化，条件存储和 `waitqueue_active` 之间得有 `smp_mb()`，或者干脆持着 `wait_queue_head.lock`。`wq_has_sleeper`（`:153`）自己带了这道 `smp_mb()`。

---

## 8.3 睡：`prepare_to_wait_event` → `___wait_event` → `finish_wait`

`wait_event` 家族收进同一个宏 `___wait_event`（`wait.h:304`）。控制流是：

```c
/* include/linux/wait.h:304 的控制流 */
init_wait_entry(&__wq_entry, exclusive ? WQ_FLAG_EXCLUSIVE : 0);
for (;;) {
	__int = prepare_to_wait_event(&wq_head, &__wq_entry, state);
	if (condition)
		break;                          /* 中文：已经挂上队列、状态已改成睡眠，再看条件 */
	if (___wait_is_interruptible(state) && __int) {
		__ret = __int;                  /* 中文：-ERESTARTSYS；entry 已摘掉 */
		goto __out;                     /* 中文：不再走 finish_wait */
	}
	cmd;                                    /* 中文：schedule() 或 schedule_timeout() */
}
finish_wait(&wq_head, &__wq_entry);
```

`___wait_is_interruptible`（`:286`）在编译期能看穿 `state` 时，要求里面带 `TASK_INTERRUPTIBLE` 或 `TASK_WAKEKILL`。`TASK_UNINTERRUPTIBLE` 的等待不会因为普通信号从这里返回。

### `prepare_to_wait_event`（`wait.c:310`）

持 `wq_head->lock`（`spin_lock_irqsave`）：

1. `signal_pending_state(state, current)`（`include/linux/sched/signal.h:414`）为真：`list_del_init` 把自己拿掉，返回 `-ERESTARTSYS`，**不**改任务状态。独占等待者如果留在队列上，下一次 `wake_up` 会把名额算到一个已经决定放弃的人身上；在同一把锁里摘链，并发的唤醒看不见他。
2. 否则队列上还没有自己：独占走 `__add_wait_queue_entry_tail`（`wait.h:194`），非独占走 `__add_wait_queue`。`WQ_FLAG_EXCLUSIVE` 已经由 `init_wait_entry` 写上。`__add_wait_queue_entry_tail_exclusive`（`:200`）会再置一次这个标志，给别的调用方用。
3. `set_current_state(state)`（`include/linux/sched.h:217`）。它是 `smp_store_mb(current->__state, state)`，存储带全屏障。`sched.h:175` 的注释就是为这个循环写的：先写「我要睡」，再看条件；对侧是先改条件，再 `try_to_wake_up`。屏障对不上就会丢唤醒。条件和唤醒在**同一把锁**里完成时，用没有屏障的 `__set_current_state`（`:211`）就够——completion 走的是这条，见 8.5。

`prepare_to_wait`（`wait.c:269`）是老的手写循环用的：它清掉 EXCLUSIVE、入队、`set_current_state`，**不**看信号。`___wait_event` 不调用它。

### 条件要在「准备睡之后」再看一次

`wait_event`（`wait.h:344`）本身有一条快路径：进来时条件已经为真，直接 `break`，不入队。慢路径进 `__wait_event`（`:328`）：`TASK_UNINTERRUPTIBLE`、非独占、`cmd` 是 `schedule()`。

丢唤醒的形态是：先看条件为假，再被对侧改条件并 `wake_up`（当时队列还是空的），然后自己才入队睡觉。宏把「入队 + 改状态」放在判条件**之前**，就是为了堵住这个窗口。对侧的纪律写在 `wait_event` 的注释里（`:341`）：改任何会影响条件的变量之后再 `wake_up()`。

信号路径的 `goto __out` 跳过 `finish_wait`，因为 `prepare_to_wait_event` 已经 `list_del_init`，而且没有把状态改成睡眠。条件为真时就算同时有信号，也是 `break` 去 `finish_wait`，独占等待者把这次事件吃掉，而不是返回 `-ERESTARTSYS`。

### `finish_wait`（`wait.c:396`）

1. `__set_current_state(TASK_RUNNING)`。被叫醒时 `try_to_wake_up` 已经把状态写成 `TASK_RUNNING`；没睡成的路径也要回到 `TASK_RUNNING`。
2. 用 `list_empty_careful` 在锁外看自己还在不在队上。`autoremove_wake_function` 成功时会先摘链，这里经常是空的。
3. 还挂着才 `spin_lock_irqsave` + `list_del_init`。

### 常用宏

都进 `___wait_event`。`exclusive` 参数为 0 就是非独占。

| 宏 | 行号 | `state` | `cmd` | 返回 |
|---|---|---|---|---|
| `wait_event` | `wait.h:344` | `TASK_UNINTERRUPTIBLE` | `schedule()` | 无（条件成立才出来） |
| `wait_event_interruptible` | `:500` | `TASK_INTERRUPTIBLE` | `schedule()` | 0，或 `-ERESTARTSYS` |
| `wait_event_killable` | `:930` | `TASK_KILLABLE` | `schedule()` | 0，或致命信号时 `-ERESTARTSYS` |
| `wait_event_timeout` | `:416` | `TASK_UNINTERRUPTIBLE` | `schedule_timeout` | 见下 |
| `wait_event_interruptible_timeout` | `:534` | `TASK_INTERRUPTIBLE` | `schedule_timeout` | 超时 0、剩余 jiffies、条件在超时后才成立则是 1、信号则 `-ERESTARTSYS` |
| `wait_event_interruptible_exclusive` | `:625` | `TASK_INTERRUPTIBLE`，`exclusive = 1` | `schedule()` | 同 interruptible |
| `io_wait_event` | `:359` | `TASK_UNINTERRUPTIBLE` | `io_schedule()` | 无；按 IO 等待记账 |

`wait_event_timeout` 的返回（`:410` 的注释）：超时后条件仍假是 **0**；超时后条件为真是 **1**；时限用完之前条件为真是**剩余 jiffies，至少为 1**。`___wait_cond_timeout`（`:278`）在条件为真且 `__ret` 还是 0 时把 `__ret` 改成 1，避免和「超时」撞成同一个 0。

`schedule_timeout` 在 `kernel/time/timer.c:2129`。超时值是 `MAX_SCHEDULE_TIMEOUT`（`sched.h:296`，即 `LONG_MAX`）时它退化成一次 `schedule()`（`timer.c:2136`）。

同文件还有 `wait_event_freezable`、`wait_event_hrtimeout`、`wait_event_state`（`:958`）、持锁等待的 `wait_event_lock_irq` 等。它们只是给 `___wait_event` 换 `state` / `cmd`，本篇不逐个展开。

任务状态位（`include/linux/sched.h`）：

| 名字 | 行 | 值 |
|---|---|---|
| `TASK_INTERRUPTIBLE` | `:86` | `0x00000001` |
| `TASK_UNINTERRUPTIBLE` | `:87` | `0x00000002` |
| `TASK_WAKEKILL` | `:97` | `0x00000100` |
| `TASK_KILLABLE` | `:115` | `TASK_WAKEKILL \| TASK_UNINTERRUPTIBLE` |
| `TASK_NORMAL` | `:122` | `TASK_INTERRUPTIBLE \| TASK_UNINTERRUPTIBLE` |

`wake_up` 用 `TASK_NORMAL`，所以可中断和不可中断的睡眠它都匹配。`wake_up_interruptible` 只匹配 `TASK_INTERRUPTIBLE`。

---

## 8.4 醒：`__wake_up` / exclusive vs non-exclusive / sync

```text
wake_up(x)                 __wake_up(x, TASK_NORMAL, 1, NULL)     wait.h:223
wake_up_all(x)             __wake_up(x, TASK_NORMAL, 0, NULL)     :225
wake_up_interruptible(x)   __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)  :230
wake_up_sync(x)            __wake_up_sync(x, TASK_NORMAL)         :228
```

`__wake_up`（`wait.c:157`）进 `__wake_up_common_lock`（`:124`），持 `wq_head->lock` 调用 `__wake_up_common`（`:80`）。返回值是**被叫醒的独占等待者个数**（`nr_exclusive - remaining`，`:143`）。非独占项不计入这个数。

`__wake_up_common` 沿 `head` 走，跳过 `WQ_FLAG_BOOKMARK`，对每个 entry 调 `func(entry, mode, wake_flags, key)`：

- 返回值 `< 0`：停止扫描
- 返回值 `> 0` 且带 `WQ_FLAG_EXCLUSIVE`：`nr_exclusive` 减 1，减到 0 就停
- 返回值是 0：`try_to_wake_up` 没把人从睡眠状态拉起来（注释 `:76`：任务已经在跑的稀有情况）。**不消耗**独占名额，继续往后看
- 非独占项：叫醒，但不减 `nr_exclusive`，扫描继续

所以 `wake_up`（`nr_exclusive == 1`）的意思是：前面的**非独占项全叫醒**，再叫醒 **1 个**独占项。它不是「队列里只叫一个人」。`wait_event` 默认 `exclusive = 0`，一队 `wait_event` 配一次 `wake_up` 会整队醒来，各自再判条件，条件仍假的重新睡。只要一个人起来，等的一侧用 `wait_event_interruptible_exclusive` 或 `add_wait_queue_exclusive`。

`wake_up_all` 把 `nr_exclusive` 传 0。循环里对独占项做 `--nr_exclusive` 会穿过 0，`!--nr_exclusive` 为假，于是独占项也不再限额。

优先级项排在队头，并且 `add_wait_queue_priority` 给了 EXCLUSIVE。`nr_exclusive == 1` 时，它的 `func` 若返回正数，会把这一个名额用掉，后面的人这次醒不来（`wait.c:72`）。

队列很长时，扫描超过 `WAITQUEUE_WALK_BREAK_CNT`（`wait.c:65`，64）个 entry，就把一个栈上的 bookmark 插进链表、放下 `lock`，外层循环再拿锁从书签接着扫。这是锁持有时间的上限，驱动不用自己造 bookmark。

### 已经持锁的变体

调用者持有 `wq_head->lock` 时用 `_locked`，它们直接进 `__wake_up_common`，不再加锁：

| 函数 | 行 | `nr` | `key` |
|---|---|---|---|
| `__wake_up_locked` | `wait.c:172` | 参数 | `NULL` |
| `__wake_up_locked_key` | `:178` | 1 | 参数 |
| `__wake_up_locked_key_bookmark` | `:184` | 1 | 参数，可传入 bookmark |
| `__wake_up_locked_sync_key` | `:233` | 1 | 参数，并带 `WF_SYNC` |

对应宏：`wake_up_locked`（`wait.h:226`）是 `TASK_NORMAL`、`nr = 1`；`wake_up_all_locked`（`:227`）是 `nr = 0`。

### sync

`__wake_up_sync_key`（`wait.c:207`）把 `wake_flags` 设成 `WF_SYNC`，`nr_exclusive` 固定为 1。`WF_SYNC` 在 `kernel/sched/sched.h:2168`，值 `0x10`，注释是 waker 叫醒之后自己马上要睡。`try_to_wake_up` 看到这个标志就不必把被叫醒的任务搬到别的 CPU。`__wake_up_sync`（`wait.c:243`）是 `key = NULL` 的包装。`wake_up_interruptible_sync`（`wait.h:233`）用 `TASK_INTERRUPTIBLE`。

还有 `WF_CURRENT_CPU`（`kernel/sched/sched.h:2170`，`0x40`）：`__wake_up_on_current_cpu`（`wait.c:164`）用它。`default_wake_function` 在 `CONFIG_SCHED_DEBUG` 下会警告出现这两种之外的 `wake_flags`（`core.c:7043`）。

### `autoremove_wake_function` 与 `key`

`init_wait_entry` 把 `func` 设成 `autoremove_wake_function`（`wait.c:422`）：

1. 调 `default_wake_function`（`kernel/sched/core.c:7040`）
2. 返回值非 0 时 `list_del_init_careful`，在队列锁里把自己摘掉

`default_wake_function` 忽略 `key`，只做 `try_to_wake_up(curr->private, mode, wake_flags)`。`try_to_wake_up` 在 `core.c:4213`。它按 `mode & task->__state` 决定叫不叫醒，匹配上就把状态改成 `TASK_RUNNING` 再入队。入队和选 CPU 的细节在调度章 04，这里不重复。

`key` 原样传给 `func`。`poll_to_key`（`wait.h:238`）/ `key_to_poll`（`:239`）把 poll 事件掩码塞进这个指针。`wake_up_poll`（`:240`）因此可以只叫醒关心这几位的等待者——前提是 `func` 自己看 `key`。默认回调不看。

`private` 在默认路径上就是 `task_struct *`。自定义 `func` 时它可以是别的对象；那种 entry 不要交给 `default_wake_function`，它会把 `private` 当成任务指针送进 `try_to_wake_up`。

---

## 8.5 `completion`：一次性「事做完了」

completion 是计数，不是互斥。`kernel/sched/completion.c:11` 写得很直：信号量容易被拿去当排除手段，completion 要表达的是同步点。没有持有者，谁都可以 `complete`。

```c
/* include/linux/completion.h:26 */
struct completion {
	unsigned int done;                 /* 中文：还没被等待者消耗的完成次数；complete_all 写成 UINT_MAX */
	struct swait_queue_head wait;      /* 中文：6.6 嵌的是 swait，不是 wait_queue_head_t */
};
```

swait 是另一套更瘦的队列。`include/linux/swait.h:11` 起的注释列出它砍掉的东西：不能在同一条队列上混用可中断和不可中断（唤醒一律按 `TASK_NORMAL`）；没有非独占模式，所以一次 `swake_up_one` 只叫一个人；没有自定义 `func`；队列锁是 `raw_spinlock_t`，RT 上也能在不可睡眠的上下文里拿。同一段注释的结论是：普通代码继续用 `wait.h` 的队列。completion 是这套简化队列的主要使用者。

```c
/* include/linux/swait.h:43 */
struct swait_queue_head {
	raw_spinlock_t		lock;       /* 中文：raw 自旋锁，保护 task_list 和 completion.done */
	struct list_head	task_list;  /* 中文：FIFO */
};

/* include/linux/swait.h:48 */
struct swait_queue {
	struct task_struct	*task;      /* 中文：没有 func / private，等待者就是这个任务 */
	struct list_head	task_list;  /* 中文：挂进 swait_queue_head.task_list */
};
```

公开的 swait 宏从 `swait.h:186` 的 `swait_event_exclusive` 起。completion **不**走那些宏，它直接调 `kernel/sched/sched.h:3273` 声明的 `__prepare_to_swait`。

### 初始化

| API | 行 | 作用 |
|---|---|---|
| `DECLARE_COMPLETION` | `completion.h:52` | `done = 0`，swait 头静态初始化 |
| `DECLARE_COMPLETION_ONSTACK` | `:68`（`CONFIG_LOCKDEP`） | 栈上对象走 `init_completion`，好让 lockdep 登记；没开 lockdep 时退回 `DECLARE_COMPLETION`（`:73`） |
| `init_completion` | `:84` | `done = 0`，`init_swait_queue_head` |
| `reinit_completion` | `:97` | **只**把 `done` 清 0。`complete_all` 之后要复用必须走它，而且得等已经醒来的等待者都离开 |

### `complete` / `complete_all`

`complete`（`completion.c:45`）→ `complete_with_flags`（`:16`），`wake_flags = 0`：

1. `raw_spin_lock_irqsave(&x->wait.lock)`
2. `done != UINT_MAX` 时 `done++`。已经 `complete_all` 过就不再加
3. `swake_up_locked`（`swait.c:21`）：空队列直接返回；否则取队头，`try_to_wake_up(task, TASK_NORMAL, wake_flags)`，再 `list_del_init`
4. 放锁

一次 `complete` 只醒一个等待者，顺序与 `__prepare_to_swait` 的 `list_add_tail` 一致（FIFO）。函数自己不睡，用的是 irqsave，硬中断里可以调。`complete_on_current_cpu`（`completion.c:28`，声明 `completion.h:119`）把 `wake_flags` 换成 `WF_CURRENT_CPU`。

`completion.c:42` 的注释：这次调用如果真的叫醒了任务，访问任务状态之前有一道全屏障。屏障在 `try_to_wake_up` 里。

`complete_all`（`completion.c:67`）：

1. `lockdep_assert_RT_in_threaded_ctx()`（`include/linux/lockdep.h:677`）。只有 `CONFIG_PROVE_RAW_LOCK_NESTING` 时才检查：非线程化的硬中断上下文会 `WARN_ONCE`。该选项关掉时宏是空的（`:686`）
2. `done = UINT_MAX`
3. `swake_up_all_locked`（`swait.c:41`）：持着同一把 raw 锁，对队列里每一个人做 `swake_up_locked(..., 0)`

`swake_up_all_locked` 的注释（`swait.c:35`）说它和 `swake_up_all`（`:61`，每叫一个就放锁）不同，就是为了 completion 能在硬中断和关中断区域使用。`complete_all` 把完成状态**钉死**：之后的 `wait_for_completion*` 都会成功，直到 `reinit_completion`。`complete_all` 的注释写明，这之后不能靠 `completion_done()` 判断还有没有等待者没离开（`completion.c:64`）。

### `wait_for_completion*`

声明从 `completion.h:102` 起，实现都进 `wait_for_common`（`completion.c:125`）或 `wait_for_common_io`（`:131`）。真正睡觉的是 `do_wait_for_common`（`:81`），调用时已经持有 `x->wait.lock`（`__wait_for_common`，`:115`）：

```c
/* kernel/sched/completion.c:81 的控制流；锁由调用者持有 */
if (!x->done) {
	DECLARE_SWAITQUEUE(wait);          /* swait.h:58：task = current */
	do {
		if (signal_pending_state(state, current)) {
			timeout = -ERESTARTSYS;
			break;
		}
		__prepare_to_swait(&x->wait, &wait);   /* swait.c:84：队尾 */
		__set_current_state(state);            /* done 和唤醒在同一把 raw 锁下，不必 smp_store_mb */
		raw_spin_unlock_irq(&x->wait.lock);
		timeout = action(timeout);             /* schedule_timeout 或 io_schedule_timeout */
		raw_spin_lock_irq(&x->wait.lock);
	} while (!x->done && timeout);
	__finish_swait(&x->wait, &wait);   /* swait.c:125：TASK_RUNNING，还在队上就摘掉 */
	if (!x->done)
		return timeout;
}
if (x->done != UINT_MAX)
	x->done--;
return timeout ?: 1;                       /* 中文：刚好剩 0 个 jiffy 但 done 已成立 → 返回 1 */
```

`done` 在进 `do_wait_for_common` 之前已经在锁里看过。等待者要么看见旧的 `done > 0` 直接减一返回，要么先入队再放锁去睡；唤醒者必须拿同一把锁才能 `done++`。这和 `wait_event` 把条件放在队列锁外面、靠 `set_current_state` 的屏障配对，是两条路。

`timeout ?: 1` 和 `wait_event_timeout` 的「至少返回 1」是同一件事：调用方用 0 表示超时。

| API | 声明 | 实现 | 状态 | 返回 |
|---|---|---|---|---|
| `wait_for_completion` | `completion.h:102` | `completion.c:146` | `TASK_UNINTERRUPTIBLE`，`MAX_SCHEDULE_TIMEOUT` | 无 |
| `wait_for_completion_timeout` | `:107` | `:165` | 同上，超时单位 jiffies | 0 超时；否则至少 1，或剩余 jiffies |
| `wait_for_completion_io` | `:103` | `:179` | 同上，`io_schedule_timeout` | 无；按 IO 等待记账 |
| `wait_for_completion_io_timeout` | `:109` | `:199` | 同上 | 同 timeout |
| `wait_for_completion_interruptible` | `:104` | `:214` | `TASK_INTERRUPTIBLE` | 0 完成，`-ERESTARTSYS` 被信号打断 |
| `wait_for_completion_interruptible_timeout` | `:111` | `:236` | 同上 | 信号 `-ERESTARTSYS`，超时 0，否则正数 |
| `wait_for_completion_killable` | `:105` | `:252` | `TASK_KILLABLE` | 0 或 `-ERESTARTSYS` |
| `wait_for_completion_killable_timeout` | `:113` | `:285` | 同上 | 同 interruptible_timeout |
| `wait_for_completion_state` | `:106` | `:262` | 调用者给的 `state` | 0 或 `-ERESTARTSYS` |

`try_wait_for_completion`（`completion.c:304`）：先 `READ_ONCE(done)`，为 0 就返回 false，不上锁。非 0 再拿锁；若仍非 0 且不是 `UINT_MAX` 就减一。返回 true 表示这次没有阻塞也消耗掉（或碰到了 `complete_all` 的饱和值）。

`completion_done`（`:337`）：`done == 0` 返回 false。非 0 时把 `wait.lock` 拿起来再马上放开，等可能还在 `complete()` 里的那次引用结束，避免等待方释放 completion 对象时唤醒方还拿着它的锁。`complete_all` 之后它一直返回 true。

计数语义：`complete` 两次、还没人等，`done == 2`，接下来两次 `wait_for_completion` 都不睡。这是「事已经做完」能留给后来的等待者。`wake_up` 在队列为空时什么都不记录，晚到的 `wait_event` 如果条件变量没另外保存这件事，就会睡死。

进程章里的现成例子：vfork 的父进程在 `wait_for_vfork_done` 里调用 `wait_for_completion_state`（`kernel/fork.c:1604`），状态是 `TASK_UNINTERRUPTIBLE|TASK_KILLABLE|TASK_FREEZABLE`。子进程 exec/exit 时 `complete_vfork_done`（`:1584`）对栈上的 completion 做 `complete`（`:1592`）。字段 `task_struct.vfork_done` 的注解在进程章 [02](../process-management/02-task-struct.md)，创建路径在 [04](../process-management/04-fork-clone.md)。本篇不重写 fork。

---

## 8.6 选型一眼

| 你要的语义 | 用 |
|---|---|
| 临界区互斥，拿不到就睡，只有持有者能放 | mutex / rwsem（06）。等待链是 `wait_list` |
| 一个可反复变化的条件，醒来必须重判表达式 | `wait_event*` + `wake_up*`。条件变量由你自己放 |
| 「做完了」一次或若干次，晚到的等待者也要看见 | `completion`。`done` 替你记着 |
| 读者不停，写者等旧读者走完 | RCU（07）。那不是等待队列 |
| 用户态地址上的睡眠 | futex 的 WAIT/WAKE，见 [09](./09-futex.md) |

`wait_event` 默认非独占：`wake_up` 会叫整队。完成事件要一个一个交接时，completion 的 `swake_up_locked` 本来就是一次一个；wait queue 上则把等待者标成 `WQ_FLAG_EXCLUSIVE`。

`complete_all` 适合「再也不会回到未完成」：关机、对象拆掉之前把所有人放走。还要复用同一对象，等他们离开后 `reinit_completion`。把它当布尔标志反复 toggle，会在 `UINT_MAX` 上卡住。

栈上的 completion 必须活过最后一个 `wait_for_*` 和 `complete`。`DECLARE_COMPLETION_ONSTACK` 解决的是 lockdep 的初始化，不解决寿命。

---

## 8.7 观测

正确性仍然先看 lockdep，热点再看 lock_stat / trace。工具和「原子上下文里睡觉」那条日志在 [08-observability](./08-observability.md)（文件名 `08-observability.md`，读序放在本章和 RCU 之后）。本篇不另起 ftrace 课。

卡住时先看任务状态。进程章 [10](../process-management/10-observability.md) 的 `wchan` 会停在 `schedule`、`wait_for_completion` 这一类符号上。那只说明人睡在等待队列或 completion 里；条件是谁改的、有没有人 `wake_up` / `complete`，回到本篇的对侧纪律。用户态 futex 的 `wchan`（例如 `futex_wait_queue`）见 [09](./09-futex.md)。

```bash
# RECIPE：未在本机执行。需要能挂 kprobe 的权限。
# bpftrace -e 'kprobe:__wake_up { @[kstack] = count(); }'
# bpftrace -e 'kprobe:wait_for_completion { printf("%s\n", comm); }'
```

对照源码时直接看 vfork 那一处即可：`kernel/fork.c:1604` 的 `wait_for_completion_state`，以及 `include/linux/wait.h:304` 的 `___wait_event`。内核树不在本仓库。

小模块（completion 的「先完成再等」和「没人 complete 就超时」）：[demos/kmod_completion_timeout.c](./demos/kmod_completion_timeout.c)。用的 `init_completion`、`reinit_completion`、`complete`、`wait_for_completion_timeout`、`kthread_run`、`kthread_stop`、`msleep`、`msecs_to_jiffies` 都在 v6.6.160 的头文件里。下面是 **RECIPE**（没有对 v6.6.160 做 `make` / `insmod`）：

```bash
# RECIPE
cd locks-and-ds/demos
make -f Makefile.example
# sudo insmod kmod_completion_timeout.ko    # 仅实验机
# dmesg | tail
# sudo rmmod kmod_completion_timeout
```

`wait_for_completion_timeout` 在完成时返回剩余 jiffies（至少 1），超时返回 0。模块先等一个 50ms 后 `complete` 的线程（上限 200ms），`kthread_stop` 把它收回来，再 `reinit_completion` 后用 20ms 空等，第二次应打印 `left=0`。

### 踩坑

| 坑 | 后果 | 做法 |
|---|---|---|
| 先看条件为假，再入队 | 中间被人 `wake_up` 过，队列当时是空的，自己睡死 | 用 `wait_event`，或手写时遵守 `prepare_to_wait` → 再判条件 → `schedule` |
| 先 `wake_up` 再写条件 | 等待者醒来看见旧条件，重新睡，之后没人再叫 | 先写条件，再 `wake_up`（`wait.h:341`） |
| 把 `wake_up` 当成「只叫一个」 | 非独占的整队被叫醒 | 等待侧加 `WQ_FLAG_EXCLUSIVE`，或改用 completion |
| 在原子上下文 / 持非 RT `spinlock` 时 `wait_event` | `might_sleep`，lockdep | 叫醒可以在中断里；等必须在进程上下文 |
| `complete_all` 之后当普通 completion 再用 | `done` 停在 `UINT_MAX`，等待者不再阻塞 | `reinit_completion`，且先等已醒来的人离开 |
| 用 completion 保护临界区 | 没有持有者，谁都能 `complete`，也不能防止并发进入 | 互斥用 mutex |
| 把 mutex 的 `wait_list` 当成 `wait_queue_head` | 挂错对象 | 锁等待留在 06；事件等待用本节的队列 |
| 释放栈上或刚 `kfree` 的 completion，而 `complete()` 还持着 `wait.lock` | 唤醒方用已释放的锁 | `completion_done` 拿放一次锁就是在等这次引用结束；栈对象要活过 wait 和 complete |

`wait_on_bit`（`include/linux/wait_bit.h:71`）是按比特位哈希进一张公用 `wait_queue_head` 的封装，本篇不展开。`wait_woken`（`wait.c:453`）配合 `WQ_FLAG_WOKEN`，给不能在 `func` 里直接摘链的调用方用，同样不展开。

---

## 8.8 函数表

| 名字 | 位置 | 说明 |
|---|---|---|
| `struct wait_queue_entry` / `wait_queue_head` | `include/linux/wait.h:30` / `:37` | 等待者 / 队列头（`spinlock` + `list_head`） |
| `init_waitqueue_head` | `wait.h:66`；实现 `kernel/sched/wait.c:8` | 动态初始化并登记 lockdep |
| `add_wait_queue` / `add_wait_queue_exclusive` | `wait.c:17` / `:28` | 非独占靠前插入 / 独占插尾 |
| `prepare_to_wait_event` | `wait.c:310` | 睡前入队或因信号摘链；`set_current_state` |
| `finish_wait` | `wait.c:396` | 回到 `TASK_RUNNING` 并摘链 |
| `___wait_event` | `wait.h:304` | `wait_event*` 的公共循环 |
| `wait_event` / `wait_event_interruptible` | `wait.h:344` / `:500` | 不可中断 / 可中断 |
| `wait_event_timeout` | `wait.h:416` | jiffies 超时；0 / 1 / 剩余 |
| `__wake_up` | `wait.c:157` | `mode`、`nr_exclusive`、`key` |
| `__wake_up_locked` / `__wake_up_locked_key` | `wait.c:172` / `:178` | 调用者已持队列锁 |
| `__wake_up_sync_key` | `wait.c:207` | `WF_SYNC`，`nr_exclusive = 1` |
| `autoremove_wake_function` | `wait.c:422` | 默认 `func`：叫醒后摘链 |
| `default_wake_function` | `kernel/sched/core.c:7040` | `try_to_wake_up(private, mode, wake_flags)` |
| `struct completion` | `include/linux/completion.h:26` | `done` + `swait_queue_head wait` |
| `complete` / `complete_all` | `kernel/sched/completion.c:45` / `:67` | `done++` 并醒一个 / `done = UINT_MAX` 并全醒 |
| `wait_for_completion` | 声明 `completion.h:102`；实现 `completion.c:146` | 不可中断，无超时 |
| `wait_for_completion_timeout` | 声明 `:107`；实现 `completion.c:165` | 超时返回 0 |
| `wait_for_completion_interruptible` / `_killable` | 声明 `completion.h:104` / `:105` | 信号 / 致命信号 → `-ERESTARTSYS` |
| `swake_up_locked` / `__prepare_to_swait` | `kernel/sched/swait.c:21` / `:84` | completion 实际的醒与入队 |
| `schedule` / `try_to_wake_up` | `kernel/sched/core.c:6789` / `:4213` | 睡与醒的调度入口；选人另章 |

---

*下一节：[futex](./09-futex.md)（文件名 09，读序在本节之后、RCU 之前）。观测、lockdep 与踩坑清单在 [`08-observability.md`](./08-observability.md)（现文件名 08，读序最后）。*
