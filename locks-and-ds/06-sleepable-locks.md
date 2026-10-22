# 6. 可睡眠锁与序列锁

**结论**：临界区长、或里面要碰用户内存/IO/可能回收的分配 → 用 **mutex / rwsem**，别用 spinlock。读写不对称用 rwsem；读多写极少且数据不是指针网，可考虑 **seqlock**。选型表在本节末尾。

---

## 6.1 Mutex

```c
/* include/linux/mutex_types.h:41 非 RT */
context_lock_struct(mutex) {
	atomic_long_t		owner;       /* 中文：持有者 task；0 表示空闲 */
	raw_spinlock_t		wait_lock;   /* 中文：保护等待队列 */
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq;    /* 中文：owner 在跑时可乐观自旋一阵 */
#endif
	struct mutex_waiter	*first_waiter; /* 中文：等待队列头 */
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

语义（头文件注释写死的纪律）：

- 同一时刻一个持有者；**只有持有者能 unlock**
- **不可递归**；任务不能拿着 mutex 退出
- **不能**在硬中断 / softirq 里用

| API | 说明 |
|---|---|
| `mutex_init` / `DEFINE_MUTEX` | 初始化 |
| `mutex_lock` | 拿不到就睡眠 |
| `mutex_lock_interruptible` / `_killable` | 可被信号打断 |
| `mutex_trylock` | 非阻塞 |
| `mutex_unlock` | 释放 |
| `mutex_is_locked` | 查询（少用于同步逻辑） |

实现文件：`kernel/locking/mutex.c`（细节不展开）。快路径常是原子改 `owner`；失败则进入等待队列。

```c
mutex_lock(&inode->i_mutex风格的锁);  /* 现代 VFS 多用 rwsem，这里示意 */
/* 可睡眠操作：copy_from_user、GFP_KERNEL 分配…… */
mutex_unlock(&...);
```

---

## 6.2 rwsem（读写信号量）

```c
/* include/linux/rwsem.h:48 非 RT */
context_lock_struct(rw_semaphore) {
	atomic_long_t count;      /* 中文：读者计数 / 写者标志压缩在一个 long */
	atomic_long_t owner;
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq;
#endif
	raw_spinlock_t wait_lock;
	struct rwsem_waiter *first_waiter;
	/* debug / lockdep ... */
};
```

| API | 行号约 | 说明 |
|---|---|---|
| `down_read` / `up_read` | `:229` / `:252` | 共享读锁 |
| `down_write` / `up_write` | `:241` / `:257` | 独占写锁 |
| `down_read_trylock` / `down_write_trylock` | `:236` / `:247` | 尝试 |
| `down_*_killable` / `_interruptible` | 附近 | 可中断变体 |
| `downgrade_write` | `:288` | 写降级为读 |

**典型**：`mmap_lock`（曾经叫 `mmap_sem`）保护 VMA 布局——大量读路径 `down_read`，改映射时 `down_write`。

规则同样：**可睡眠上下文**；持锁可睡，但别在原子上下文 `down_read`。

---

## 6.3 percpu-rwsem

`include/linux/percpu-rwsem.h`：读侧极快（per-CPU），写侧很重（要等所有 CPU 的读侧安静）。

| API | 说明 |
|---|---|
| `percpu_down_read` / `percpu_up_read` | 读 |
| `percpu_down_write` / `percpu_up_write` | 写（贵） |

**何时用**：读极多、写极少（例如某些全局开关/注册表）。写路径延迟敏感就别用。

---

## 6.4 seqcount / seqlock：乐观读

```c
/* include/linux/seqlock_types.h */
typedef struct seqcount {
	unsigned sequence;   /* 中文：偶数=稳定；写者进入时打成奇数 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} seqcount_t;

context_lock_struct(seqlock) {
	seqcount_spinlock_t seqcount;  /* 中文：带 spinlock 的 seqcount */
	spinlock_t lock;               /* 中文：写者互斥 */
};
typedef struct seqlock seqlock_t;
```

| 机制 | 读侧 | 写侧 |
|---|---|---|
| `seqcount_t` | `read_seqcount_begin` → 读数据 → `read_seqcount_retry` | 自己保证写者互斥 + 非抢占 |
| `seqlock_t` | `read_seqbegin` / `read_seqretry` | `write_seqlock` / `write_sequnlock`（内含 spinlock） |

```c
do {
	seq = read_seqbegin(&s);
	/* 中文：无锁读一份快照字段 */
	x = shared->x;
	y = shared->y;
} while (read_seqretry(&s, seq));  /* 中文：若写者穿插过，重试 */
```

**限制（头文件写得很狠）**：

- 保护的数据里**不要有读者会跟着走的指针**（写者可能把你正在跟的指针腾空）
- 写侧要短；读者可能反复重试

适合：统计时钟、简单计数器组、无指针的小型状态。

---

## 6.5 选型总表

| 场景 | 选择 |
|---|---|
| 短临界区、原子上下文、中断里 | **spinlock**（必要时 `_irqsave`） |
| 长临界区、要睡眠、只进程上下文 | **mutex** |
| 读多写少、读写互斥语义清晰 | **rwsem** |
| 读极多写极少、写得起延迟 | **percpu-rwsem** |
| 读多写极少、数据无跟随指针、可重试 | **seqlock / seqcount** |
| 读极多、写者可复制更新结构 | **RCU**（下一节） |

```text
会睡眠吗？
  否 → spinlock（注意 IRQ/BH）
  是 → 只要互斥？ mutex
       读写不对称？ rwsem / percpu-rwsem / seqlock / RCU
```

---

## 6.6 函数表

| 名字 | 位置 | 说明 |
|---|---|---|
| `mutex_lock/unlock` | `include/linux/mutex.h` + `kernel/locking/mutex.c` | 可睡眠互斥 |
| `down_read/write` `up_*` | `include/linux/rwsem.h` | 读写锁 |
| `percpu_down_read/write` | `include/linux/percpu-rwsem.h` | per-CPU 读优化 |
| `read_seqbegin` / `write_seqlock` | `include/linux/seqlock.h` | 序列锁 |

---

*下一节：RCU——读者几乎不排队的发布-订阅式同步。*
