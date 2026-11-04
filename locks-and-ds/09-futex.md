# 9. futex

**结论**：用户态锁的快路径是自己的原子操作，对不上才进 `futex` 系统调用。内核把用户地址收成 `union futex_key`，放进全局哈希桶里的 `struct futex_q`，然后 `schedule`。唤醒方用同一把键把人摘下来，经 `wake_up_q` 叫醒。桶上的队列是 `plist`，不是 [08](./08-wait-queue-and-completion.md) 的 `wait_queue_head`。`futex_q` 的注释写明：用这张哈希队列代替普通的 `wait_queue_entry_t`，才能只叫醒这个地址上的人。

> UAPI：`include/uapi/linux/futex.h`。
> 对外薄头：`include/linux/futex.h`（`union futex_key`、`do_futex` 声明、任务初始化）。**没有** `struct futex_q`。
> 内部头：`kernel/futex/futex.h`（`futex_hash_bucket`、`futex_q`、`futex_wait` / `futex_wake` 的声明）。
> 实现：`kernel/futex/syscalls.c`、`core.c`、`waitwake.c`。`pi.c`、`requeue.c` 本篇只点名。
> 读序在 [08 等待队列](./08-wait-queue-and-completion.md) 之后、[07 RCU](./07-rcu.md) 之前。章末观测仍是 [`08-observability.md`](./08-observability.md)（文件名 08，读序最后）。

本篇写 `FUTEX_WAIT` / `FUTEX_WAKE` 的慢路径：分发、键、桶、入队、睡眠、唤醒，以及退出时和进程章的衔接。优先级继承（`FUTEX_LOCK_PI*`）、requeue、`futex_waitv` 的多地址状态机只点名字。

x86_64 系统调用号：`arch/x86/entry/syscalls/syscall_64.tbl:213` 是 `202 common futex`，`:373` 是 `449 common futex_waitv`。别的架构号不同，机制在 `kernel/futex/`。

---

## 9.1 用户态锁为何落内核

快路径不进内核。拿锁的一方用原子指令把用户空间里的一个 `u32` 从「空」改成「我持有」。成功就返回。失败说明已经有人持有，或者字上已经标了等待者，这时才调用 `futex`。

放锁的一方先改这个字。改完发现没有等待者，也直接返回。有等待者才 `FUTEX_WAKE`。所以无争用的 mutex 在 `strace -e futex` 里是空白的。争用之后才出现成对的 WAIT / WAKE。本机样本在 [9.9](#99-demo)。

内核要兜住的窗口写在 `kernel/futex/waitwake.c:15` 起。等待方如果先读到旧值、还没入队，唤醒方已经把字改掉并且发现队列是空的，这次唤醒就丢了，等待方再睡下去没人叫。收缩成四步：

```text
等待方                              唤醒方
waiters++，然后一条全屏障           先改用户字
拿桶锁，在锁里再读一次用户字        全屏障，再读 waiters
字已经变了：不入队，waiters--       waiters == 0：不拿桶锁，直接返回
字没变：入队，放锁，schedule        waiters != 0：拿桶锁，按 key 摘人，再 wake_q
```

屏障的两侧是 `futex_hb_waiters_inc`（`kernel/futex/futex.h:184`，`atomic_inc` 之后 `smp_mb__after_atomic`）和 `futex_hb_waiters_pending`（`:206`，先 `smp_mb` 再 `atomic_read`）。注释里标成 (A) 和 (B)。`CONFIG_SMP` 关掉时 `futex_hb_waiters_pending` 固定返回 1（`:215`），空桶优化消失，每次唤醒都拿锁。

用户态和内核的分工在 `futex_wait_setup` 的注释里（`waitwake.c:587`）：

```text
等待方：val = *uaddr；条件成立才 futex_wait(uaddr, val)
唤醒方：先把 *uaddr 改掉，再 futex_wake(uaddr)
```

内核在**已经拿着桶锁**之后再读一次 `*uaddr`。相等才把 `futex_q` 挂上去。不等就返回 `-EWOULDBLOCK`，调用方在用户态重试原子操作。`-EWOULDBLOCK` 就是 `EAGAIN`（`include/uapi/asm-generic/errno.h:22`）。

睡眠本身是 `schedule()`（`waitwake.c:355`）。谁被选中接着跑，见调度章 [04](../scheduler/04-enqueue-wakeup.md)。`set_current_state` 的 `smp_store_mb` 和 `try_to_wake_up` 的配对已经在 [08](./08-wait-queue-and-completion.md) 写过，这里不重讲等待队列。futex 不走 `__wake_up`：叫醒发生在放下桶锁之后，经 `wake_up_q`（`kernel/sched/core.c:1026`）进 `wake_up_process`（`:4490`，即 `try_to_wake_up(p, TASK_NORMAL, 0)`，`try_to_wake_up` 在 `:4213`）。

`wchan` 停在 `futex_wait_queue` 时，人在这张哈希队列上，不在 `wait_queue_head` 上。

---

## 9.2 UAPI：`FUTEX_WAIT` / `FUTEX_WAKE` / `FUTEX_PRIVATE_FLAG`

命令在 `include/uapi/linux/futex.h:11` 起。低位是操作，`FUTEX_CMD_MASK`（`:28`）把两个旗标剥掉：

| 宏 | 行 | 值 | 本篇 |
|---|---|---|---|
| `FUTEX_WAIT` | `:11` | 0 | 展开。相对超时 |
| `FUTEX_WAKE` | `:12` | 1 | 展开。`val` = 最多叫醒几个人 |
| `FUTEX_FD` | `:13` | 2 | `do_futex` 没有这个 case，落到 `-ENOSYS` |
| `FUTEX_REQUEUE` | `:14` | 3 | 点名。把等待者挪到另一个地址 |
| `FUTEX_CMP_REQUEUE` | `:15` | 4 | 点名。挪之前先比较用户字 |
| `FUTEX_WAKE_OP` | `:16` | 5 | 点名。对第二个字做原子运算再决定叫醒 |
| `FUTEX_LOCK_PI` | `:17` | 6 | 点名。优先级继承，见 9.8 |
| `FUTEX_UNLOCK_PI` | `:18` | 7 | 点名 |
| `FUTEX_TRYLOCK_PI` | `:19` | 8 | 点名 |
| `FUTEX_WAIT_BITSET` | `:20` | 9 | 和 `FUTEX_WAIT` 进同一个 `futex_wait`，多一个位图，超时是绝对时间 |
| `FUTEX_WAKE_BITSET` | `:21` | 10 | 和 `FUTEX_WAKE` 进同一个 `futex_wake` |
| `FUTEX_WAIT_REQUEUE_PI` | `:22` | 11 | 点名 |
| `FUTEX_CMP_REQUEUE_PI` | `:23` | 12 | 点名 |
| `FUTEX_LOCK_PI2` | `:24` | 13 | 点名。`FUTEX_LOCK_PI` 会落到它上面，并强制 realtime 时钟 |
| `FUTEX_PRIVATE_FLAG` | `:26` | 128 | 私有键。没有它，`do_futex` 置 `FLAGS_SHARED` |
| `FUTEX_CLOCK_REALTIME` | `:27` | 256 | 绝对超时用 `CLOCK_REALTIME` |

`FUTEX_WAIT_PRIVATE`（`:30`）就是 `FUTEX_WAIT | FUTEX_PRIVATE_FLAG`，数值 128。`FUTEX_WAKE_PRIVATE`（`:31`）是 129。glibc 的进程私有 mutex 走这两条，样本见 9.9。

位图「全匹配」是 `FUTEX_BITSET_MATCH_ANY`（`:152`，`0xffffffff`）。普通 `FUTEX_WAIT` / `FUTEX_WAKE` 不带位图参数，内核自己填上这个值，于是和位图版走同一条函数。

robust 字的三个掩码也在这个头里，退出路径用得到（9.7）：

| 宏 | 行 | 值 | 作用 |
|---|---|---|---|
| `FUTEX_WAITERS` | `:127` | `0x80000000` | 用户态标「有人在等」。死亡处理看见它才 `futex_wake` |
| `FUTEX_OWNER_DIED` | `:135` | `0x40000000` | 内核标「持有者死时没放开」 |
| `FUTEX_TID_MASK` | `:140` | `0x3fffffff` | 剩下的位是持有者 TID |
| `ROBUST_LIST_LIMIT` | `:146` | 2048 | 退出时最多走这么多个链表节点，挡住环 |

`FUTEX_32`（`:50`，值 2）和 `struct futex_waitv`（`:64`）只给 `futex_waitv` 用，见 9.8。

---

## 9.3 `do_futex` 分发

`SYSCALL_DEFINE6(futex)` 在 `kernel/futex/syscalls.c:192`：

```c
/* kernel/futex/syscalls.c:192 的实参 */
u32 __user *uaddr          /* 中文：用户字地址 */
int op                     /* 中文：命令，可或上 PRIVATE / CLOCK_REALTIME */
u32 val                    /* 中文：WAIT 时期望值；WAKE 时是 nr_wake */
const struct __kernel_timespec __user *utime
                           /* 中文：WAIT* 的超时；REQUEUE 时这个指针值被当成 val2 */
u32 __user *uaddr2         /* 中文：第二个用户字。WAIT/WAKE 不用 */
u32 val3                   /* 中文：位图或 WAKE_OP 的编码。普通 WAIT/WAKE 会被内核覆盖 */
```

有超时的命令才把 `utime` 读成 `timespec64`（`futex_cmd_has_timeout`，`:165`）：`FUTEX_WAIT`、`FUTEX_LOCK_PI`、`FUTEX_LOCK_PI2`、`FUTEX_WAIT_BITSET`、`FUTEX_WAIT_REQUEUE_PI`。然后 `futex_init_timeout`（`:179`）：

- `FUTEX_WAIT`：超时是**相对**的，`ktime_add_safe(ktime_get(), *t)`（`:186`）变成绝对的 monotonic 时间
- 其余带超时的命令：用户给的是绝对时间。没带 `FUTEX_CLOCK_REALTIME`、又不是 `FUTEX_LOCK_PI` 时，再过一遍 `timens_ktime_to_host(CLOCK_MONOTONIC, *t)`（`:188`）

`do_futex`（`:113`，声明在薄头 `include/linux/futex.h:78`）先看旗标：

```c
/* kernel/futex/syscalls.c:113 */
int cmd = op & FUTEX_CMD_MASK;
unsigned int flags = 0;

if (!(op & FUTEX_PRIVATE_FLAG))
	flags |= FLAGS_SHARED;          /* 中文：没带 PRIVATE 就是共享键 */
if (op & FUTEX_CLOCK_REALTIME) {
	flags |= FLAGS_CLOCKRT;
	if (cmd != FUTEX_WAIT_BITSET && cmd != FUTEX_WAIT_REQUEUE_PI &&
	    cmd != FUTEX_LOCK_PI2)
		return -ENOSYS;        /* 中文：普通 FUTEX_WAIT 不能带这个旗标 */
}
```

`FLAGS_SHARED` / `FLAGS_CLOCKRT` / `FLAGS_HAS_TIMEOUT` 在 `kernel/futex/futex.h:20`、`:28`、`:29`。有 MMU 时 `FLAGS_SHARED` 是 `0x01`；`CONFIG_MMU` 关掉时这个宏是 `0x00`（`:26`），共享和私有收成同一把键。

和本篇相关的分发（`:129`）：

| `cmd` | 调用 |
|---|---|
| `FUTEX_WAIT` | 把 `val3` 改成 `FUTEX_BITSET_MATCH_ANY`，落到下一行 |
| `FUTEX_WAIT_BITSET` | `futex_wait(uaddr, flags, val, timeout, val3)` |
| `FUTEX_WAKE` | 同样先填满位图 |
| `FUTEX_WAKE_BITSET` | `futex_wake(uaddr, flags, val, val3)`。这里的 `val` 是 `nr_wake` |
| `FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE` | `futex_requeue`（实现 `kernel/futex/requeue.c:377`）。不展开 |
| `FUTEX_WAKE_OP` | `futex_wake_op`（`waitwake.c:238`）。不展开 |
| `FUTEX_LOCK_PI` | 强制 `FLAGS_CLOCKRT`，落到 `FUTEX_LOCK_PI2` |
| `FUTEX_LOCK_PI2` / `FUTEX_TRYLOCK_PI` | `futex_lock_pi`（`kernel/futex/pi.c:930`）。不展开 |
| `FUTEX_UNLOCK_PI` | `futex_unlock_pi`（`pi.c:1101`）。不展开 |
| `FUTEX_WAIT_REQUEUE_PI` / `FUTEX_CMP_REQUEUE_PI` | requeue-PI。不展开 |
| 其余 | `-ENOSYS` |

`FUTEX_REQUEUE` 没有独立的「再排队个数」寄存器。系统调用把第 4 个参数的指针值 `(unsigned long)utime` 当成 `do_futex` 的 `val2`（`syscalls.c:211`）。WAIT 不读 `val2`。这是这条 6 参数系统调用的复用，不是超时。

32 位 `timespec` 的入口是 `sys_futex_time32`（`syscalls.c:365`，`CONFIG_COMPAT_32BIT_TIME`），最后也进 `do_futex`。

---

## 9.4 键与桶：`union futex_key`、`futex_hash_bucket`、`get_futex_key`

`union futex_key` 在薄头里，不在 `kernel/futex/futex.h`。内部头 `:69` 那一行只是 `futex_pi_state` 嵌进去的一个成员。

```c
/* include/linux/futex.h:32
 * 头注释 :17 仍写 hash_futex()。6.6 的函数是 futex_hash（core.c:115）。
 * 成员顺序参与哈希，不能重排。
 */
union futex_key {
	struct {
		u64 i_seq;                 /* 中文：文件页：inode 的序列号 */
		unsigned long pgoff;       /* 中文：这一页在文件中的偏移 */
		unsigned int offset;       /* 中文：页内字节偏移；低两位兼做类型 */
	} shared;
	struct {
		union {
			struct mm_struct *mm; /* 中文：私有或匿名共享：mm */
			u64 __tmp;            /* 中文：和上面的指针同宽，方便当 u64 看 */
		};
		unsigned long address;     /* 中文：按页对齐后的用户地址 */
		unsigned int offset;       /* 中文：页内字节偏移 */
	} private;
	struct {
		u64 ptr;                   /* 中文：与前两个布局重叠，给比较和 jhash2 */
		unsigned long word;
		unsigned int offset;
	} both;
};
```

`offset` 的低两位是类型（同文件 `:20` 的注释）：

| 低两位 | 宏 | 含义 |
|---|---|---|
| 00 | （不设） | 进程私有。键里是 `mm` + 虚拟地址，不另外抓 inode 或 mm 的引用 |
| 01 | `FUT_OFF_INODE`（`:29`，1） | 共享，落在文件页上。键是 `i_seq` + `pgoff` |
| 10 | `FUT_OFF_MMSHARED`（`:30`，2） | 共享，但是匿名映射。键仍是 `mm` + 虚拟地址 |

页内偏移本身 4 字节对齐，所以低两位让出来做类型不会和「页内偏移」撞车。`FUTEX_KEY_INIT`（`:53`）把 `both.ptr` 清 0。

`get_futex_key`（`kernel/futex/core.c:220`，声明 `futex.h:119`）把用户地址收成上面的键。地址不是 4 字节对齐返回 `-EINVAL`（`:233`）。`access_ok` 失败返回 `-EFAULT`（`:237`）。

`fshared == false`（调用方没置 `FLAGS_SHARED`，也就是用户带了 `FUTEX_PRIVATE_FLAG`）走 `:250`：有 MMU 时 `key->private.mm = current->mm`，`key->private.address` 是按页对齐的地址，然后返回。不找 VMA，不调用 `get_user_pages_fast`。等待期间也不钉用户页。

`fshared == true` 走 `:271` 的 `get_user_pages_fast`。`FUTEX_READ` 写失败时再试只读（`:276`）。然后：

- 匿名页（`:351`）：`offset |= FUT_OFF_MMSHARED`，`key->private.mm = current->mm`，地址同样按页对齐。只读的匿名页直接 `-EFAULT`（`:356`），这种页上的字不会再变。同行注释写 “ref taken on mm”。函数体这里只有赋值和或标志，没有 `mmgrab`
- 文件页（`:365`）：`offset |= FUT_OFF_INODE`，`key->shared.i_seq = get_inode_sequence_number(inode)`（`:171`），`key->shared.pgoff = page_to_pgoff(tail)`

两条共享路径都在返回前 `put_page`（`:403`）。键是序列号或 `mm` + 地址，不是一枚被钉住的 `struct page`。`lock_page` 可能睡（`:218` 的注释），所以 `get_futex_key` 在拿桶锁**之前**调用。

`futex_hash`（`core.c:115`）对键做 `jhash2`。长度是 `offsetof(key, both.offset) / 4` 个 `u32`，种子是 `key->both.offset`，再按表长掩码选桶。`futex_match`（`futex.h:135`）比较 `both.word`、`both.ptr`、`both.offset` 三个字段。不同的键可以落在同一个桶里，唤醒时必须再比一次键。

```c
/* kernel/futex/futex.h:45 */
struct futex_hash_bucket {
	atomic_t waiters;          /* 中文：即将或正在这只桶上睡的人数。空桶唤醒可以不拿锁 */
	spinlock_t lock;           /* 中文：保护 chain。非 RT 不睡；RT 上 spinlock_t 的替换见第 5 节 */
	struct plist_head chain;   /* 中文：优先级链表，不是 wait_queue_head */
} ____cacheline_aligned_in_smp;
```

`plist_head` 只有一条 `list_head node_list`（`include/linux/plist.h:82`）。`plist_node`（`:86`）是 `prio` + 两条 `list_head`（同一优先级一条，全部节点一条）。桶和桶之间按缓存行对齐，避免相邻桶的锁互相碰缓存行。

整张表是 `core.c:49` 的 `__futex_data`：`queues` 指针和 `hashsize` 放在同一条缓存线里。`futex_init`（`:1163`，`core_initcall` 在 `:1188`）里，`CONFIG_BASE_SMALL` 为真时表长 16；否则先取 `roundup_pow_of_two(256 * num_possible_cpus())`，再交给 `alloc_large_system_hash`，最后 `hashsize = 1UL << futex_shift`。每个桶 `waiters = 0`，`plist_head_init`，`spin_lock_init`（`:1180`）。

---

## 9.5 排队对象：`struct futex_q`

```c
/* kernel/futex/futex.h:96
 * 定义在这个内部头。include/linux/futex.h 里没有它。
 */
struct futex_q {
	struct plist_node list;            /* 中文：挂进 futex_hash_bucket.chain */

	struct task_struct *task;          /* 中文：等待的任务。__futex_queue 写成 current */
	spinlock_t *lock_ptr;              /* 中文：指向所在桶的 lock。唤醒方用 smp_store_release 写成 NULL */
	union futex_key key;               /* 中文：这一个等待者的键 */
	struct futex_pi_state *pi_state;   /* 中文：普通 WAIT 是 NULL。非空表示 PI */
	struct rt_mutex_waiter *rt_waiter; /* 中文：requeue_pi 用。普通 WAIT 是 NULL */
	union futex_key *requeue_pi_key;   /* 中文：requeue_pi 的目标键 */
	u32 bitset;                         /* 中文：位图。普通 WAIT 由 futex_q_init 填成 MATCH_ANY */
	atomic_t requeue_state;             /* 中文：requeue_pi 状态机，本篇不展开 */
#ifdef CONFIG_PREEMPT_RT
	struct rcuwait requeue_wait;       /* 中文：仅 RT，配 requeue_pi */
#endif
} __randomize_layout;
```

源码顺序如上。打开结构体随机化时字段在二进制里会重排，读代码以这个顺序为准。

初始值 `futex_q_init` 在 `kernel/futex/requeue.c:59`：`key` 用 `FUTEX_KEY_INIT`，`bitset` 是 `FUTEX_BITSET_MATCH_ANY`，`requeue_state` 是 `Q_REQUEUE_PI_NONE`。`list` 留到入队时再初始化。`futex_wait` 在栈上写 `struct futex_q q = futex_q_init`（`waitwake.c:637`）。

「已经醒了」的判定在结构注释里（`futex.h:88`）：`plist_node_empty(&q->list)` 或 `q->lock_ptr == 0`。顺序是先把节点从 plist 摘掉，再清 `lock_ptr`。

入队 `__futex_queue`（`core.c:553`）把优先级设成 `min(current->normal_prio, MAX_RT_PRIO)`。`MAX_RT_PRIO` 是 100（`include/linux/sched/prio.h:16`）。RT 线程用自己的 `normal_prio`，排在前面；普通线程都变成 100，同一优先级按 FIFO。这是**叫醒顺序**，不是优先级继承。继承走 `pi_state` 里的 `rt_mutex_base`，普通 `futex_wake` 碰到 `pi_state` 或 `rt_waiter` 非空会直接 `-EINVAL`（`waitwake.c:168`）。

`futex_queue`（`futex.h:170`）是内联：调用 `__futex_queue`，然后 `spin_unlock(&hb->lock)`。调用方进来时必须已经拿着桶锁。它和一次 `futex_unqueue` 配对；PI 的摘队是例外，用 `futex_unqueue_pi`（`core.c:630`），本篇不展开。

---

## 9.6 慢路径：`futex_wait` → queue → 睡；`futex_wake` → 出队 → wake

### 等待

`futex_wait`（`waitwake.c:632`，声明 `futex.h:267`）：

1. `bitset == 0` 返回 `-EINVAL`（`:640`）。否则 `q.bitset = bitset`。普通 `FUTEX_WAIT` 的 bitset 已经是 `FUTEX_BITSET_MATCH_ANY`
2. `futex_setup_timer`（`core.c:134`）。`abs_time == NULL` 就没有定时器。有超时的话按 `FLAGS_CLOCKRT` 选 `CLOCK_REALTIME` 或 `CLOCK_MONOTONIC`，范围用 `current->timer_slack_ns`（调用点 `waitwake.c:644`）
3. `futex_wait_setup`（`waitwake.c:577`）。成功时**拿着桶锁**返回 0；失败时锁已放下

`futex_wait_setup` 的循环：

```c
/* kernel/futex/waitwake.c:602 起，锁的持有见各步 */
get_futex_key(...);                 /* 中文：还没拿桶锁。共享键这里可能睡 */
hb = futex_q_lock(q);               /* 中文：先 waiters++，再 spin_lock。core.c:523 */
futex_get_value_locked(&uval, uaddr); /* 中文：关缺页，再读一次用户字。core.c:461 */
if (读失败) { futex_q_unlock(hb); get_user 后再试; }
if (uval != val) { futex_q_unlock(hb); return -EWOULDBLOCK; }
return 0;                           /* 中文：锁还在调用方手里 */
```

`futex_q_lock` 在拿锁之前 `futex_hb_waiters_inc`。注释（`core.c:530`）说明原因：任务正卡在 `spin_lock` 上时，唤醒方已经该看见这个未来的等待者。值对不上或读失败时 `futex_q_unlock`（`:546`）放锁并把 `waiters` 减回去。这就是 9.1 图里的「不入队，waiters--」。

4. `futex_wait_queue`（`waitwake.c:328`）。调用方持有桶锁

```c
/* kernel/futex/waitwake.c:337 */
set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE);
futex_queue(q, hb);                 /* 中文：plist 入队，并放下桶锁 */
if (timeout)
	hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);
if (!plist_node_empty(&q->list)) {
	if (!timeout || timeout->task)
		schedule();             /* 中文：kernel/sched/core.c:6789。选人见调度章 */
}
__set_current_state(TASK_RUNNING);
```

`TASK_INTERRUPTIBLE` 是 `include/linux/sched.h:86`（`0x1`），`TASK_FREEZABLE` 是 `:102`（`0x2000`）。用户字的比较在 `futex_wait_setup` 里、还拿着桶锁时已经做完。这里再 `set_current_state`，然后入队并放锁。唤醒方摘人必须拿同一把桶锁，插不进「看过字」和「入队」之间。`set_current_state` 的 `smp_store_mb` 加上 `futex_queue` 里的 `spin_unlock`，把「我要睡」排在别人能够叫醒之前（`waitwake.c:331` 的注释）。08 的 `wait_event` 把条件放在队列锁外面，靠状态位和 `wake_up` 配对；futex 把这次比较收进桶锁。

`plist` 已经空，说明放下桶锁之后、读链表之前，唤醒方已经把节点摘掉了，跳过 `schedule`。定时器已经到期（`timeout->task` 被清空）也跳过。

5. 从 `futex_wait_queue` 回来之后 `futex_unqueue`（`core.c:583`）

| `futex_unqueue` 返回值 | 含义 |
|---|---|
| 0 | 唤醒方已经把 `lock_ptr` 写成 NULL。这次等待成功，`futex_wait` 返回 0 |
| 1 | 节点还在桶上，本函数自己摘掉了。接着看定时器和信号 |

还在队上时：定时器触发过则 `-ETIMEDOUT`（`waitwake.c:662`）；没有信号则当成假唤醒，`goto retry`（`:670`）；有信号且没有超时则 `-ERESTARTSYS`（`:673`）。有绝对超时又被信号打断时，把 `uaddr`、`val`、时间、bitset、`flags | FLAGS_HAS_TIMEOUT` 写进 `current->restart_block`，`set_restart_fn(..., futex_wait_restart)`（`:684`）。`futex_wait_restart` 在 `:694`，重新调用 `futex_wait`。

`__futex_unqueue`（`core.c:509`）要求调用方拿着 `q->lock_ptr`。它用 `container_of` 从锁指针找回桶，`plist_del`，再 `futex_hb_waiters_dec`。`futex_unqueue` 自己用 `READ_ONCE(q->lock_ptr)` 拿锁，锁上之后再比一次指针（`:611`）：requeue 可能在读指针和 `spin_lock` 之间把人挪到另一只桶，拿错锁就放掉重试。

### 唤醒

`futex_wake`（`waitwake.c:143`，声明 `futex.h:285`）：

1. `bitset == 0` 返回 `-EINVAL`（`:151`）
2. `get_futex_key(..., FUTEX_READ)`，`futex_hash`
3. `futex_hb_waiters_pending(hb)` 为 0：不拿桶锁，返回 0。这是 9.1 图的右侧快路径
4. 拿 `hb->lock`，沿 `plist` 走。`futex_match` 命中、位图相与非 0、并且不是 PI 等待者，就 `futex_wake_mark`。计数达到 `nr_wake` 停止
5. 放锁，`wake_up_q(&wake_q)`，返回叫醒的个数。碰到 PI 等待者则返回 `-EINVAL`

`futex_wake_mark`（`waitwake.c:115`）在**持有桶锁**时做三件事：`get_task_struct`，`__futex_unqueue`，`smp_store_release(&q->lock_ptr, NULL)`，然后 `wake_q_add_safe`。真正的 `try_to_wake_up` 留到 `wake_up_q`，那时桶锁已经放下。`lock_ptr` 用 release 写，是为了别让这个 NULL 跑到 `plist_del` 前面：等待方看见 NULL 就可以把栈上的 `futex_q` 扔掉（`:125` 的注释）。

一次 `FUTEX_WAKE` 的 `nr_wake` 是几，就最多摘几个匹配的等待者。plist 上同优先级是 FIFO，所以普通线程按入队顺序被叫。它不把整桶都叫醒：同一只桶里别的键留着。

---

## 9.7 与 process 衔接

任务上的字段已经注解在进程章 [02](../process-management/02-task-struct.md)，这里不重抄 `task_struct`。内核行号：

| 字段 | `include/linux/sched.h` | 进程章 02 里的角色 |
|---|---|---|
| `clear_child_tid` | `:1022` | `CLONE_CHILD_CLEARTID`。退出时写 0 并 futex 唤醒 |
| `robust_list` | `:1242` | 用户态 robust 链表头（`CONFIG_FUTEX`） |
| `compat_robust_list` | `:1244` | 32 位兼容链表 |
| `pi_state_list` | `:1246` | 本任务持有的 PI 状态。清理点名，不展开 |
| `pi_state_cache` | `:1247` | 缓存的一枚 `futex_pi_state` |
| `futex_exit_mutex` | `:1248` | 退出清理和后来的等待者互斥 |
| `futex_state` | `:1249` | `FUTEX_STATE_OK` / `EXITING` / `DEAD` |

这三个状态在 `include/linux/futex.h:56`。`futex_init_task`（`:62`）在 `copy_process` 里调用（`kernel/fork.c:2555`）：链表头初始化，`futex_state = FUTEX_STATE_OK`，`mutex_init` 那把退出锁。

`clear_child_tid` 的赋值在 `copy_process`（`fork.c:2363`）：`CLONE_CHILD_CLEARTID` 才把 `args->child_tid` 记下来。创建路径见进程章 [04](../process-management/04-fork-clone.md)。

退出的顺序在 `exit_mm`（`kernel/exit.c:543`）→ `exit_mm_release`（`fork.c:1663`）：

```c
/* kernel/fork.c:1663 */
void exit_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exit_release(tsk);   /* 中文：先走 robust 链表和 PI 状态 */
	mm_release(tsk, mm);       /* 中文：再处理 clear_child_tid */
}
```

`futex_exit_release`（`core.c:1156`）进 `futex_cleanup_begin`：先 `mutex_lock(&tsk->futex_exit_mutex)`（`:1106`），再在 `tsk->pi_lock` 下把 `futex_state` 写成 `FUTEX_STATE_EXITING`（`:1120`）。然后 `futex_cleanup`（`:1055`），最后写成 `FUTEX_STATE_DEAD` 并放下 `futex_exit_mutex`。`futex_exec_release`（`:1138`）是同一套清理，结束时状态回到 `FUTEX_STATE_OK`，给 `exec_mm_release`（`fork.c:1669`）用。oops 路径 `futex_exit_recursive`（`core.c:1090`，调用点 `kernel/exit.c:987`）只把状态打成 `DEAD`。

### `clear_child_tid`

`mm_release`（`fork.c:1630`）：`clear_child_tid` 非空，并且 `mm->mm_users > 1`（还有别的用户抓着这个地址空间）时，`put_user(0, clear_child_tid)`，然后

```c
do_futex(tsk->clear_child_tid, FUTEX_WAKE, 1, NULL, NULL, 0, 0);
```

（`fork.c:1649`）。操作是 `FUTEX_WAKE`，**没有** `FUTEX_PRIVATE_FLAG`，所以 `do_futex` 置 `FLAGS_SHARED`。`val = 1`，只叫一个。最后一个线程离开这个 `mm` 时 `mm_users` 不再大于 1，这条唤醒不做。pthread 的 join 能醒，是因为同组别的线程还抓着 `mm`。进程章 [07](../process-management/07-exit-reap.md) 的 `exit_mm` 讲到这里为止；完整的 `do_exit` 仍留在那一节。

这次 `do_futex` 是内核在退出路径里直接调的，不是退出线程自己再进一次 `syscall`。`strace -e futex` 看不见对应的 `FUTEX_WAKE`，只能看见 join 那一侧的等待。9.9 的样本就是这样。

### robust list

用户态用 `sys_set_robust_list`（`syscalls.c:29`）把链表头指针放进 `current->robust_list`。长度必须等于 `sizeof(struct robust_list_head)`，否则 `-EINVAL`。这是另一条系统调用，`strace -e futex` 不会打印它。

```c
/* include/uapi/linux/futex.h:85 */
struct robust_list {
	struct robust_list __user *next;   /* 中文：用户态链表。内核只顺着 next 走 */
};

/* include/uapi/linux/futex.h:97 */
struct robust_list_head {
	struct robust_list list;                  /* 中文：空链表时 next 指回自己 */
	long futex_offset;                        /* 中文：从这个节点到 futex 字的字节偏移 */
	struct robust_list __user *list_op_pending; /* 中文：正在加锁、还没链进来的那一把 */
};
```

`futex_cleanup` 看见 `robust_list` 非空就调用 `exit_robust_list`（`core.c:813`）。它先读 `list_op_pending`，再沿 `next` 走，最多 `ROBUST_LIST_LIMIT` 步。每个节点加上 `futex_offset` 得到用户字，交给 `handle_futex_death`（`:647`）。compat 链表走 `compat_exit_robust_list`（`:907`），形状相同。

`handle_futex_death` 用 `FUTEX_TID_MASK` 取出 TID，和 `task_pid_vnr(curr)` 比。相等说明这把锁确实是正在死的线程持有的：`futex_cmpxchg_value_locked` 把字改成 `(uval & FUTEX_WAITERS) | FUTEX_OWNER_DIED`（`:748`）。非 PI 并且原来带 `FUTEX_WAITERS` 时，`futex_wake(uaddr, 1, 1, FUTEX_BITSET_MATCH_ANY)`（`:784`）。第一个 `1` 是内部 `flags` 参数：有 MMU 时 `FLAGS_SHARED` 就是 `0x01`，于是 `flags & FLAGS_SHARED` 为真，走共享键。第二个 `1` 是 `nr_wake`。用户态接下来看见 `FUTEX_OWNER_DIED`，自己做恢复。PI 那一支不在这里叫醒，注释指向 `exit_pi_state_list`。

`pi_state_list` 非空时 `futex_cleanup` 调用 `exit_pi_state_list`（`core.c:983`，包在 `CONFIG_FUTEX_PI` 里）。它把 `rt_mutex` 的持有者拆掉。等待者若看见 `FUTEX_STATE_EXITING`，会在 `wait_for_owner_exiting`（`core.c:479`）里堵住 `futex_exit_mutex`。这两条是 PI 的退出挂钩，锁的获取协议在 `pi.c`，不在本篇。

---

## 9.8 边界表

| 本篇写了 | 不写 |
|---|---|
| `FUTEX_WAIT` / `FUTEX_WAKE`（含 BITSET 共用的那条函数） | `FUTEX_LOCK_PI` / `FUTEX_LOCK_PI2` / `FUTEX_UNLOCK_PI` / `FUTEX_TRYLOCK_PI` 的获取协议 |
| 私有键与共享键、哈希桶、`futex_q` | `rt_mutex` 的优先级继承。可跟调度章 RT 一起后置 |
| 退出时 robust 字的 `OWNER_DIED` + 一次 `futex_wake` | `exit_pi_state_list` 的锁顺序 |
| `clear_child_tid` 上的一次 `FUTEX_WAKE` | `do_exit` / `waitpid` 全路径（进程章 07） |
| `futex_waitv` 的入口在哪 | `futex_wait_multiple` 里多把锁的入队重试 |

PI 对象长这样，好和进程章的 `pi_state_cache` 对上号。获取、继承、解锁不往下走：

```c
/* kernel/futex/futex.h:54 */
struct futex_pi_state {
	struct list_head list;             /* 中文：挂在持有者的 pi_state_list 上 */
	struct rt_mutex_base pi_mutex;     /* 中文：真正的 PI 等待挂在这里 */
	struct task_struct *owner;         /* 中文：当前拥有者 */
	refcount_t refcount;               /* 中文：引用计数 */
	union futex_key key;               /* 中文：这把 PI futex 的键 */
} __randomize_layout;
```

`futex_lock_pi_atomic` 在 `pi.c:514`。普通 `futex_wake` 拒绝带 `pi_state` 的 `futex_q`。

requeue 把一只桶里的 `futex_q` 挪到另一把键上，入口 `futex_requeue`（`requeue.c:377`）。挪的时候要同时拿两只桶的锁（`double_lock_hb`，`futex.h:239`）。比较、唤醒个数、PI 目标这些参数组合不在这里展开。

`futex_waitv` 是独立系统调用，不是 `do_futex` 的一个 `cmd`：

```c
/* include/uapi/linux/futex.h:64 */
struct futex_waitv {
	__u64 val;          /* 中文：这个地址上期望的值 */
	__u64 uaddr;        /* 中文：用户地址 */
	__u32 flags;        /* 中文：必须含 FUTEX_32，可或上 FUTEX_PRIVATE_FLAG */
	__u32 __reserved;   /* 中文：必须是 0 */
};
```

`SYSCALL_DEFINE5(futex_waitv)` 在 `syscalls.c:274`。个数为 0 或大于 `FUTEX_WAITV_MAX`（128，`uapi:55`）返回 `-EINVAL`。每个元素的旗标必须落在 `FUTEX_32 | FUTEX_PRIVATE_FLAG` 里，并且带 `FUTEX_32`（`futex_parse_waitv`，`:225` 与 `:239`）。内核侧把 UAPI 结构和一把 `futex_q` 拼成 `struct futex_vector`（`futex.h:277`），交给 `futex_wait_multiple`（`waitwake.c:524`）。多地址的入队、哪一个下标先醒、超时和信号怎么跟单地址的 `futex_wait` 不一样，本篇不展开。

和相邻章节的分工：

| 现象 | 去哪 |
|---|---|
| 内核里等一个条件、等一件事做完 | [08](./08-wait-queue-and-completion.md) 的 `wait_event` / `completion` |
| 内核 mutex 的 `wait_list` | [06](./06-sleepable-locks.md) |
| 用户地址上的字，快路径在用户态 | 本篇 |
| `schedule` 选出下一个任务 | 调度章 [04](../scheduler/04-enqueue-wakeup.md) |
| `task_struct` 字段全文、`do_exit` | 进程章 [02](../process-management/02-task-struct.md)、[07](../process-management/07-exit-reap.md) |

---

## 9.9 Demo

用户态程序 [demos/05_pthread_mutex_futex.c](./demos/05_pthread_mutex_futex.c)：主线程先独自 `pthread_mutex_lock` / `unlock` 1000 次，再持锁拉起 4 个线程，睡 200ms，然后放开。等待线程记录自己堵在 `pthread_mutex_lock` 里的时间。

> **sampled on 6.12**。环境：2026-11-04；Linux 6.12.94+；gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0；glibc 2.39；strace 6.8。
> 内核符号钉 v6.6.160。这次运行和 `strace` 在 6.12 上，不在 6.6 上。阻塞毫秒数随调度差 1ms 以内。地址和 tid 每次运行都变。

```text
phase=uncontended locks=1000
phase=unlock waiters=4
waiter=0 blocked_ms=201
waiter=1 blocked_ms=201
waiter=2 blocked_ms=201
waiter=3 blocked_ms=201
phase=done entered=4 worst_blocked_ms=201
```

```bash
cd locks-and-ds/demos
gcc -Wall -O2 -pthread -o 05_pthread_mutex_futex 05_pthread_mutex_futex.c
./05_pthread_mutex_futex

# strace -e futex 只留 futex 系统调用。等待者在子线程里，要 -f 才看得到 WAIT。
# 下面这条在上述 6.12 环境跑过。
strace -f -e futex ./05_pthread_mutex_futex

# RECIPE：本机没有 bpftrace，没跑。
# 字段名来自 SYSCALL_DEFINE6(futex) 的形参，见 demos/futex_enter.bt。
bpftrace -e 'tracepoint:syscalls:sys_enter_futex { @[args->op] = count(); }'
```

同一次 `strace -f -e futex` 的计数：

| 记录到的 op | 次数 | 对应什么 |
|---|---|---|
| （无争用的 1000 次 lock/unlock） | 0 | 快路径没有系统调用 |
| `FUTEX_WAIT_PRIVATE` | 4 | 四个人堵在**同一把** mutex 字上，期望值是 2 |
| `FUTEX_WAKE_PRIVATE` | 5 | 四次返回 1，把等待者逐个叫起来；最后一次返回 0，队已经空了 |
| `FUTEX_WAIT_BITSET\|FUTEX_CLOCK_REALTIME` | 4 | 主线程 `pthread_join`。字是子线程 tid，**没有** `FUTEX_PRIVATE_FLAG` |

mutex 那一段（pid 3284 是主线程，地址是这次运行的 mutex）：

```text
3285  futex(0x559b12241060, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>
3286  futex(0x559b12241060, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>
3287  futex(0x559b12241060, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>
3288  futex(0x559b12241060, FUTEX_WAIT_PRIVATE, 2, NULL <unfinished ...>
3284  futex(0x559b12241060, FUTEX_WAKE_PRIVATE, 1) = 1
3285  <... futex resumed>)              = 0
```

`FUTEX_WAIT_PRIVATE` 是 `FUTEX_WAIT | FUTEX_PRIVATE_FLAG`。`do_futex` 不置 `FLAGS_SHARED`，键是私有的 `(mm, address)`。期望值 2 是这份 glibc 传进来的 `val`，不是 UAPI 常量；内核只做 `uval == val`。`FUTEX_WAKE_PRIVATE` 的 `1` 是 `nr_wake`：每次只叫一个人，所以四个等待者是串起来被叫的，不是一队同时醒。这和 08 里 `wake_up` 对非独占等待者「整队叫醒」不同。

join 那一段的地址不是 mutex，值是子线程的 tid：

```text
3284  futex(0x7fe3dd3ff990, FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME, 3285, NULL, FUTEX_BITSET_MATCH_ANY <unfinished ...>
3284  <... futex resumed>)              = 0
```

这条没有 `FUTEX_PRIVATE_FLAG`，和 `mm_release` 里那次 `do_futex(..., FUTEX_WAKE, 1, ...)` 一样走共享键（9.7）。退出线程不会再执行一条用户态 `futex` 系统调用，所以 strace 里没有配对的 `FUTEX_WAKE`，等待直接返回 0。

bpftrace 的 `args->op` 是或上旗标之后的整数：`FUTEX_WAIT_PRIVATE` 为 128，`FUTEX_WAKE_PRIVATE` 为 129，`FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME` 为 `9|256` = 265。事件要内核打开 `CONFIG_FTRACE_SYSCALLS`。字段怎么来的写在 [demos/futex_enter.bt](./demos/futex_enter.bt) 头部：`kernel/trace/trace_syscalls.c:270` 的 `syscall_enter_define_fields` 用 `SYSCALL_DEFINE6(futex)` 的形参名做 trace 字段。

---

## 9.10 函数表

| 名字 | 位置 | 说明 |
|---|---|---|
| `FUTEX_WAIT` / `FUTEX_WAKE` / `FUTEX_PRIVATE_FLAG` | `include/uapi/linux/futex.h:11` / `:12` / `:26` | 0 / 1 / 128 |
| `FUTEX_CMD_MASK` | `uapi/.../futex.h:28` | 剥掉 PRIVATE 和 CLOCK_REALTIME |
| `union futex_key` | `include/linux/futex.h:32` | 私有、文件共享、匿名共享三套布局。不在内部头 |
| `do_futex` | 定义 `kernel/futex/syscalls.c:113`；声明 `include/linux/futex.h:78` | 按 `cmd` 分发，顺手置 `FLAGS_SHARED` |
| `SYSCALL_DEFINE6(futex)` | `syscalls.c:192` | 系统调用入口。x86_64 号 202 |
| `SYSCALL_DEFINE5(futex_waitv)` | `syscalls.c:274` | 多地址等待的入口。状态机不展开。x86_64 号 449 |
| `struct futex_hash_bucket` | `kernel/futex/futex.h:45` | `waiters` + `spinlock` + `plist` |
| `struct futex_q` | `kernel/futex/futex.h:96` | 一个等待者。不在 `include/linux/futex.h` |
| `futex_q_init` | `kernel/futex/requeue.c:59` | bitset 默认 `FUTEX_BITSET_MATCH_ANY` |
| `get_futex_key` | `kernel/futex/core.c:220` | 用户地址 → 键。私有路径不取页 |
| `futex_hash` | `core.c:115` | `jhash2` 选桶 |
| `futex_match` | `kernel/futex/futex.h:135` | 三字段都相等才算同一把 futex |
| `futex_q_lock` / `futex_q_unlock` | `core.c:523` / `:546` | 先 `waiters++` 再拿锁；失败路径把计数还回去 |
| `__futex_queue` / `futex_queue` | `core.c:553` / 内联 `futex.h:170` | 入 plist，内联版顺手放锁 |
| `futex_unqueue` / `__futex_unqueue` | `core.c:583` / `:509` | 0 = 已被唤醒方摘掉；1 = 自己摘掉 |
| `futex_wait_setup` | `waitwake.c:577` | 桶锁内重读用户字。不等则 `-EWOULDBLOCK` |
| `futex_wait_queue` | `waitwake.c:328` | 改状态、入队、`schedule` |
| `futex_wait` | 声明 `futex.h:267`；实现 `waitwake.c:632` | WAIT / WAIT_BITSET |
| `futex_wake_mark` | `waitwake.c:115` | 持桶锁摘队，把任务放进 `wake_q` |
| `futex_wake` | 声明 `futex.h:285`；实现 `waitwake.c:143` | WAKE / WAKE_BITSET |
| `wake_up_q` / `wake_up_process` | `kernel/sched/core.c:1026` / `:4490` | 放下桶锁之后才 `try_to_wake_up` |
| `futex_wait_multiple` | `waitwake.c:524` | `futex_waitv` 的实现。不展开 |
| `sys_set_robust_list` | `syscalls.c:29` | 登记 `current->robust_list` |
| `exit_robust_list` / `handle_futex_death` | `core.c:813` / `:647` | 退出时标 `OWNER_DIED` 并可能 `futex_wake` 一个 |
| `futex_exit_release` | `core.c:1156` | `exit_mm_release` 里、`clear_child_tid` 之前 |
| `mm_release` 的 `do_futex(FUTEX_WAKE)` | `kernel/fork.c:1649` | `clear_child_tid` 写 0 之后叫一个人 |
| `futex_lock_pi` / `futex_requeue` | `pi.c:930` / `requeue.c:377` | 点名。PI 与 requeue 不展开 |

---

*下一节：[RCU](./07-rcu.md)。观测、lockdep 与踩坑清单在 [`08-observability.md`](./08-observability.md)（现文件名 08，读序最后）。*
