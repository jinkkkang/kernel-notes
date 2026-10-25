# 6. 可睡眠锁与序列锁

**结论**：临界区长、或里面要碰用户内存/IO/可能回收的分配 → 用 **mutex / rwsem**，别用 spinlock。读写不对称用 rwsem；读多写极少且数据不是指针网，可考虑 **seqlock**。选型表在本节末尾。

> 类型和 API：`include/linux/mutex.h`、`include/linux/rwsem.h`、`include/linux/seqlock.h`、`include/linux/percpu-rwsem.h`。

---

## 6.1 Mutex

非 RT 与 RT 是两套结构，都叫 `struct mutex`。

```c
/* include/linux/mutex.h:66 —— 非 CONFIG_PREEMPT_RT */
struct mutex {
	atomic_long_t		owner;       /* 中文：持有者 task；0 表示空闲 */
	raw_spinlock_t		wait_lock;   /* 中文：保护等待队列 */
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* 中文：owner 在跑时可乐观自旋一阵 */
#endif
	struct list_head	wait_list;   /* 中文：等待者的双向链表 */
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

等待者本人不放在 mutex 里。`kernel/locking/mutex.h:14` 的 `struct mutex_waiter` 在阻塞任务的内核栈上，用里面的 `list` 挂进 `wait_list`：

```c
struct mutex_waiter {
	struct list_head	list;     /* 中文：挂到 mutex->wait_list */
	struct task_struct	*task;    /* 中文：谁在等 */
	struct ww_acquire_ctx	*ww_ctx;
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
};
```

```c
/* include/linux/mutex.h:138 —— CONFIG_PREEMPT_RT */
struct mutex {
	struct rt_mutex_base	rtmutex;  /* 中文：RT 下 mutex 就是 rt 锁 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

语义（`mutex.h:39` 起的注释写死的纪律，非 RT）：

- 同一时刻一个持有者；**只有持有者能 unlock**
- **不可递归**；任务不能拿着 mutex 退出
- **不能**在硬中断 / softirq（tasklet、timer）里用
- 必须用 API 初始化，不能 `memset` 或整体赋值

| API | 位置 | 说明 |
|---|---|---|
| `DEFINE_MUTEX` | `mutex.h:118`（RT 为 `:151`） | 静态初始化 |
| `mutex_init` | `:104`（RT 为 `:168`） | 动态初始化 |
| `mutex_lock` | `CONFIG_DEBUG_LOCK_ALLOC` 时是宏 `:215`（转到 `mutex_lock_nested`）；否则声明在 `:227`。实现 `kernel/locking/mutex.c:281` | 拿不到就睡眠 |
| `mutex_lock_interruptible` | 声明 `:228`，实现 `mutex.c:977` | 可被信号打断 |
| `mutex_lock_killable` | 声明 `:229` | 只被致命信号打断 |
| `mutex_trylock` | 声明 `:245`，实现 `mutex.c:1081` | 非阻塞；成功返回 1 |
| `mutex_unlock` | 声明 `:246`，实现 `mutex.c:537` | 释放 |
| `mutex_is_locked` | 声明 `:130`；RT 是 `:160` 的宏 | 查询（少用于同步逻辑） |

快路径常是原子改 `owner`；失败则在 `wait_lock` 下把 `mutex_waiter` 链进 `wait_list` 然后睡觉。

VFS 的 inode **不是** mutex。`inode_lock()`（`include/linux/fs.h:802`）内部是 `down_write(&inode->i_rwsem)`。mutex 本身这样用：

```c
mutex_lock(&obj->lock);          /* 中文：拿不到就睡眠 */
/* 可睡眠：copy_from_user、kmalloc(GFP_KERNEL) */
mutex_unlock(&obj->lock);
```

---

## 6.2 rwsem（读写信号量）

```c
/* include/linux/rwsem.h:48 —— 非 CONFIG_PREEMPT_RT
 * count 与 owner 挨在一起，无竞争时少碰别的缓存行。
 */
struct rw_semaphore {
	atomic_long_t count;      /* 中文：读者计数 / 写者标志压在一个 long 里 */
	/*
	 * 写者，或某一位读者，外加状态标志。
	 * 乐观自旋时会反复读它，看写者是不是还在 CPU 上跑。
	 */
	atomic_long_t owner;
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* 中文：乐观自旋用的 MCS 锁 */
#endif
	raw_spinlock_t wait_lock;
	struct list_head wait_list;       /* 中文：等待者双向链表 */
#ifdef CONFIG_DEBUG_RWSEMS
	void *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

等待者是 `kernel/locking/rwsem.c:338` 的 `struct rwsem_waiter`（栈上），字段 `list` 挂进 `wait_list`。队头用 `rwsem_first_waiter()`（`:345`）取。

```c
/* include/linux/rwsem.h:129 —— CONFIG_PREEMPT_RT */
struct rw_semaphore {
	struct rwbase_rt	rwbase;   /* 中文：RT 读写锁基座 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

| API | 行号 | 说明 |
|---|---|---|
| `down_read` | `:175` | 共享读锁 |
| `down_read_interruptible` | `:176` | 读锁，可被信号打断 |
| `down_read_killable` | `:177` | 读锁，只被致命信号打断 |
| `down_read_trylock` | `:182` | 尝试读；成功返回 1 |
| `down_write` | `:187` | 独占写锁 |
| `down_write_killable` | `:188` | 写锁，可被致命信号打断 |
| `down_write_trylock` | `:193` | 尝试写；成功返回 1 |
| `up_read` | `:198` | 放读锁 |
| `up_write` | `:203` | 放写锁 |
| `downgrade_write` | `:215` | 写降级为读 |

**典型**：`mm_struct` 里的 `mmap_lock`（`include/linux/mm_types.h:828`，类型是 `struct rw_semaphore`）保护 VMA 布局——大量读路径 `down_read`，改映射时 `down_write`。

规则同样：**可睡眠上下文**；持锁可睡，但别在原子上下文 `down_read`。

---

## 6.3 percpu-rwsem

`include/linux/percpu-rwsem.h:12`：

```c
struct percpu_rw_semaphore {
	struct rcu_sync		rss;
	unsigned int __percpu	*read_count;  /* 中文：每 CPU 一份读者计数，读侧少抢全局缓存行 */
	struct rcuwait		writer;
	wait_queue_head_t	waiters;
	atomic_t		block;            /* 中文：写者要把新读者挡在慢路径上 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

| API | 行号 | 说明 |
|---|---|---|
| `percpu_down_read` | `:47` | 读；快路径只动本 CPU 计数 |
| `percpu_down_read_trylock` | `:73` | 尝试读 |
| `percpu_up_read` | `:97` | 放读锁 |
| `percpu_down_write` | `:125` | 写（贵：要等各 CPU 读侧安静） |
| `percpu_up_write` | `:126` | 放写锁 |

**何时用**：读极多、写极少（例如某些全局开关/注册表）。写路径延迟敏感就别用。

---

## 6.4 seqcount / seqlock：乐观读

类型全在 `include/linux/seqlock.h`。

```c
/* include/linux/seqlock.h:64 */
typedef struct seqcount {
	unsigned sequence;   /* 中文：偶数=稳定；写者进入时打成奇数 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} seqcount_t;
```

带关联锁的变体由 `SEQCOUNT_LOCKNAME` 宏生成（`:198`）。例如 `seqcount_spinlock_t` 里是 `seqcount_t seqcount`，再加一把锁指针（仅 `CONFIG_LOCKDEP` 或 `CONFIG_PREEMPT_RT` 时字段才真正存在，见 `:150` 的 `__SEQ_LOCK`）。

```c
/* include/linux/seqlock.h:853
 * PREEMPT_RT 上用 seqcount_spinlock_t，避免读者把写者饿死。
 */
typedef struct {
	/*
	 * 读者不要把写者饿死：这里用 seqcount_spinlock_t，不是裸 seqcount_t。
	 * 见同文件 __SEQ_LOCK()。
	 */
	seqcount_spinlock_t seqcount;
	spinlock_t lock;   /* 中文：写者互斥，并保证写侧不可抢占（非 RT） */
} seqlock_t;
```

| 机制 | 读侧 | 写侧 |
|---|---|---|
| `seqcount_t` | `read_seqcount_begin`（`:356`）→ 读数据 → `read_seqcount_retry`（`:443`） | 自己保证写者互斥 + 非抢占；`write_seqcount_begin`（`:529`）/ `write_seqcount_end`（`:551`） |
| `seqlock_t` | `read_seqbegin`（`:891`）/ `read_seqretry`（`:907`） | `write_seqlock`（`:931`）/ `write_sequnlock`（`:944`），里面拿 `spinlock_t` |

```c
do {
	seq = read_seqbegin(&s);
	/* 中文：无锁读一份快照字段 */
	x = shared->x;
	y = shared->y;
} while (read_seqretry(&s, seq));  /* 中文：若写者穿插过，重试 */
```

**限制**（`seqlock.h:51` 附近写得很明确）：

- 保护的数据里**不要有读者会跟着走的指针**（写者可能把你正在跟的指针腾空）
- 写侧要短；读者可能反复重试

适合：统计时钟、简单计数器组、无指针的小型状态。

---

## 6.5 选型总表

| 场景 | 选择 |
|---|---|
| 短临界区、原子上下文、中断里 | **spinlock**（必要时 `_irqsave`；RT 的原子上下文用 `raw_spinlock_t`） |
| 长临界区、要睡眠、只进程上下文 | **mutex** |
| 读多写少、读写互斥语义清晰 | **rwsem** |
| 读极多写极少、写得起延迟 | **percpu-rwsem** |
| 读多写极少、数据无跟随指针、可重试 | **seqlock / seqcount** |
| 读极多、写者可复制更新结构 | **RCU**（下一节） |

```text
会睡眠吗？
  否 → spinlock（注意 IRQ/BH；RT 原子上下文用 raw_spinlock）
  是 → 只要互斥？ mutex
       读写不对称？ rwsem / percpu-rwsem / seqlock / RCU
```

---

## 6.6 函数表

| 名字 | 位置 | 说明 |
|---|---|---|
| `struct mutex` | `include/linux/mutex.h:66`（RT `:138`） | 可睡眠互斥；非 RT 等待队列是 `wait_list` |
| `mutex_lock` / `mutex_unlock` | 声明 `mutex.h:227` / `:246`；实现 `kernel/locking/mutex.c:281` / `:537` | 睡 / 醒 |
| `struct rw_semaphore` | `include/linux/rwsem.h:48`（RT `:129`） | 读写锁；非 RT 等待队列是 `wait_list` |
| `down_read` / `down_write` / `up_read` / `up_write` | `rwsem.h:175` / `:187` / `:198` / `:203` | 加解锁 |
| `downgrade_write` | `rwsem.h:215` | 写降为读 |
| `percpu_down_read` / `percpu_down_write` | `include/linux/percpu-rwsem.h:47` / `:125` | per-CPU 读优化 |
| `seqcount_t` / `seqlock_t` | `include/linux/seqlock.h:64` / `:853` | 序列计数 / 带 spinlock 的序列锁 |
| `read_seqbegin` / `write_seqlock` | `seqlock.h:891` / `:931` | 乐观读 / 写侧加锁 |

---

*下一节：RCU——读者几乎不排队的发布-订阅式同步。*
