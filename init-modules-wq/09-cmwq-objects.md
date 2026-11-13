# 9. cmwq 对象

**结论**：一个工作项是 `work_struct`；一组线程是 `worker_pool`；某条队列在某个 pool 上的视图是 `pool_workqueue`；用户看见的名字和标志是 `workqueue_struct`。`worker_thread` 从 pool 的 `worklist` 上取工作，调用 `work->func`。

> 前四个结构在 `kernel/workqueue.c`（`worker_pool` `:155`，`pool_workqueue` `:229`，`workqueue_struct` `:288`）。`work_struct` 在 `include/linux/workqueue.h:98`。`struct worker` 在 `kernel/workqueue_internal.h:24`，头文件注释写它只给 workqueue 和 async 用。怎么在中断线程、softirq、`irq_work` 之间选择，见 [interrupts/09](../interrupts/09-deferred-work-choice.md)，本节不重写那张表。

字段注释里的字母是源码自己的锁规则（`workqueue.c:116` 起）：`I` 初始化之后只读，`L` 要 `pool->lock`，`WQ` 要 `wq->mutex`（`:144`），`WR` 写要 `wq->mutex`、读可以 RCU（`:146`），`PL` 要 `wq_pool_mutex`（`:135`），`MD` 要 `wq_mayday_lock`（`:148`），`A` 要 `wq_pool_attach_mutex`（`:133`），`K` 由 worker 在持有 `pool->lock` 时改（`:127`）。

---

## 9.1 关系

```text
workqueue_struct                 名字、WQ_* 标志、rescuer
  ├─ cpu_pwq[cpu] ──► pool_workqueue
  └─ dfl_pwq       ──► pool_workqueue     仅 unbound
                          │
                          ▼
                    worker_pool            一条 worklist、一组 worker
                          │
                          ▼
                    worker_thread          调用 work->func
```

工作项挂在链表上时，`work_struct.data` 的低位是标志，高位指向所属的 `pool_workqueue`（`pool_workqueue` 上面的注释，`workqueue.c:224`）。所以 `pool_workqueue` 按 `1 << WORK_STRUCT_FLAG_BITS` 对齐（`:271`）。

---

## 9.2 `work_struct`

```c
/* include/linux/workqueue.h:98 */
struct work_struct {
	atomic_long_t data;       /* 低位是 PENDING 等标志，在队列上时高位指向 pwq */
	struct list_head entry;   /* 挂在 pool->worklist 或 pwq->inactive_works 上 */
	work_func_t func;         /* void (*func)(struct work_struct *work) */
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;
#endif
};
```

`WORK_STRUCT_PENDING_BIT` 是 bit 0（`:31`）。`queue_work_on` 用 `test_and_set_bit` 抢这一位，已经置位说明还在队列上或还没执行完清理，这次 `queue_work` 返回 false。`WORK_STRUCT_INACTIVE_BIT`（bit 1）表示因为 `max_active` 被放进 `inactive_works`。`WORK_STRUCT_PWQ_BIT`（bit 2）表示 `data` 里存的是 pwq 指针。

```c
/* include/linux/workqueue.h:302 */
#define INIT_WORK(_work, _func)						\
	__INIT_WORK((_work), (_func), 0)
```

`__INIT_WORK_KEY`（`:286`，没有 lockdep 的那份）把 `data` 设成 `WORK_DATA_INIT()`（即 `WORK_STRUCT_NO_POOL`，`:107`），初始化 `entry`，写入 `func`。

可延迟的包装：

```c
/* include/linux/workqueue.h:111 */
struct delayed_work {
	struct work_struct work;
	struct timer_list timer;

	/* target workqueue and CPU ->timer uses to queue ->work */
	struct workqueue_struct *wq;
	int cpu;
};
```

定时器到期后由 `delayed_work_timer_fn` 把里面的 `work` 送进 `wq`。取消和 flush 的差别留给 [demos/README.md](./demos/README.md) 里还没写的 demo 04，本节不展开。

---

## 9.3 `worker_pool`

```c
/* kernel/workqueue.c:155 */
struct worker_pool {
	raw_spinlock_t		lock;		/* 保护本结构里标 L 的字段 */
	int			cpu;		/* I: 绑定的 CPU；unbound 池为负 */
	int			node;		/* I: NUMA 节点 */
	int			id;		/* I: 池子编号 */
	unsigned int		flags;		/* L: 含 POOL_DISASSOCIATED */

	unsigned long		watchdog_ts;	/* L: worklist 从空变成非空时更新 */
	bool			cpu_stall;	/* WD: watchdog 看到的 per-cpu 停顿 */

	int			nr_running;	/* 正在跑的 worker 数，并发管理用 */

	struct list_head	worklist;	/* L: 待执行的 work_struct */
	int			nr_workers;	/* L: worker 总数 */
	int			nr_idle;	/* L: 空闲 worker 数 */

	struct list_head	idle_list;	/* L: 空闲 worker */
	struct timer_list	idle_timer;	/* L: 空闲超时，IDLE_WORKER_TIMEOUT */
	struct work_struct      idle_cull_work; /* L: 回收多余空闲 worker */

	struct timer_list	mayday_timer;	/* L: 需要 rescuer 时的定时器 */

	DECLARE_HASHTABLE(busy_hash, BUSY_WORKER_HASH_ORDER);
						/* L: 正在执行的 worker，按 work 哈希 */

	struct worker		*manager;	/* L: 当前在管这个池的 worker */
	struct list_head	workers;	/* A: 挂上来的 worker */
	struct list_head        dying_workers;  /* A: 即将退出的 */
	struct completion	*detach_completion;

	struct ida		worker_ida;	/* 编进 kworker 名字的 id */
	struct workqueue_attrs	*attrs;		/* I: unbound 池的 cpumask / nice */
	struct hlist_node	hash_node;	/* PL: unbound_pool_hash */
	int			refcnt;		/* PL: unbound 池的引用 */

	struct rcu_head		rcu;		/* 释放走 RCU，get_work_pool() 可以读 */
};
```

每个 CPU 有 `NR_STD_WORKER_POOLS`（2，`:90`）个标准池：普通 nice 和高优先级 nice。`IDLE_WORKER_TIMEOUT` 是 `300 * HZ`（`:96`），注释写空闲 worker 留 5 分钟。`MAYDAY_INITIAL_TIMEOUT` 是 `HZ / 100` 且至少 2 个 tick（`:98`），然后每 `MAYDAY_INTERVAL`（`HZ / 10`，`:101`）再叫一次。

`POOL_DISASSOCIATED` 置上时，`process_one_work` 不再要求 `raw_smp_processor_id() == pool->cpu`（`:2581`）。unbound 池带着这个标志。`workqueue_init` 给 per-cpu 池清掉它（`:6804`），表示线程已经绑到那颗 CPU。

---

## 9.4 `pool_workqueue`

```c
/* kernel/workqueue.c:229 */
struct pool_workqueue {
	struct worker_pool	*pool;		/* I: 工作真正在哪个池子上跑 */
	struct workqueue_struct *wq;		/* I: 属于哪条队列 */
	int			work_color;	/* L: 当前颜色，flush 用 */
	int			flush_color;	/* L: 正在等的颜色 */
	int			refcnt;		/* L: 引用 */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: 每种颜色还没做完的个数 */

	int			nr_active;	/* L: 计入 max_active 的工作数 */
	int			max_active;	/* L: 同时算“活跃”的上限 */
	struct list_head	inactive_works;	/* L: 超过 max_active 后排在这里 */
	struct list_head	pwqs_node;	/* WR: 挂在 wq->pwqs */
	struct list_head	mayday_node;	/* MD: 挂在 wq->maydays */
	struct work_struct	mayday_cursor;

	u64			stats[PWQ_NR_STATS]; /* 见下方枚举 */

	struct kthread_work	release_work;	/* unbound pwq 的释放丢给 kthread */
	struct rcu_head		rcu;
} __aligned(1 << WORK_STRUCT_FLAG_BITS);
```

`nr_active >= max_active` 时，新工作项进 `inactive_works` 并带上 `WORK_STRUCT_INACTIVE`，不进 `pool->worklist`（结构体上方注释 `:238`）。flush 用的 `wq_barrier` 也会带 `INACTIVE`，但不是因为积压。颜色个数是 `WORK_NR_COLORS`（`workqueue.h:54`，`WORK_STRUCT_COLOR_BITS` 为 4，所以 16）。

`enum pool_workqueue_stats`（`workqueue.c:210`）的计数：`PWQ_STAT_STARTED`、`COMPLETED`、`CPU_TIME`、`CPU_INTENSIVE`、`CM_WAKEUP`、`REPATRIATED`、`MAYDAY`、`RESCUED`。源码注释写这些可以用 `tools/workqueue/wq_monitor.py` 看。本章没有跑过那个脚本。

---

## 9.5 `workqueue_struct`

```c
/* kernel/workqueue.c:288 */
struct workqueue_struct {
	struct list_head	pwqs;		/* WR: 这条队列的全部 pwq */
	struct list_head	list;		/* PR: 全局 workqueues 链表 */

	struct mutex		mutex;		/* 保护本结构里标 WQ 的字段 */
	int			work_color;	/* WQ: 当前颜色 */
	int			flush_color;	/* WQ: 正在 flush 的颜色 */
	atomic_t		nr_pwqs_to_flush;
	struct wq_flusher	*first_flusher;
	struct list_head	flusher_queue;
	struct list_head	flusher_overflow;

	struct list_head	maydays;	/* MD: 正在求救的 pwq */
	struct worker		*rescuer;	/* MD: WQ_MEM_RECLAIM 才有 */

	int			nr_drainers;	/* WQ: drain_workqueue 进行中 */
	int			saved_max_active; /* WQ: 创建时定下的 max_active */

	struct workqueue_attrs	*unbound_attrs;	/* PW: 仅 unbound */
	struct pool_workqueue	*dfl_pwq;	/* PW: 仅 unbound */

#ifdef CONFIG_SYSFS
	struct wq_device	*wq_dev;	/* I: WQ_SYSFS 的 device */
#endif
#ifdef CONFIG_LOCKDEP
	char			*lock_name;
	struct lock_class_key	key;
	struct lockdep_map	lockdep_map;
#endif
	char			name[WQ_NAME_LEN]; /* I: 缓冲长度 24，含结尾的 NUL */

	struct rcu_head		rcu;

	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* */
	struct pool_workqueue __percpu __rcu **cpu_pwq;
						/* I: 每 CPU 一个 pwq 指针 */
};
```

`WQ_NAME_LEN` 是 24（`:113`）。`alloc_workqueue` 用 `vsnprintf` 写进 `name`。同名的两条队列可以同时存在：`system_wq` 和 `system_percpu_wq` 都叫 `"events"`（见 [10](./10-cmwq-queue-and-flags.md)），它们是两个对象。

`flags` 单独放在一条缓存线上，注释写它是发工作项时的热字段（`:326`）。

---

## 9.6 `worker` 与 `worker_thread`

```c
/* kernel/workqueue_internal.h:24 */
struct worker {
	union {
		struct list_head	entry;	/* L: 空闲时挂 idle_list */
		struct hlist_node	hentry;	/* L: 忙碌时挂 busy_hash */
	};

	struct work_struct	*current_work;	/* K: 正在执行的工作项 */
	work_func_t		current_func;	/* K: 它的函数 */
	struct pool_workqueue	*current_pwq; /* K: 它所属的 pwq */
	u64			current_at;
	unsigned int		current_color;

	int			sleeping;	/* S: 是否在睡觉 */

	work_func_t		last_func;	/* K: 上一份工作的函数 */

	struct list_head	scheduled;	/* L: 已经派给这个 worker 的链表 */

	struct task_struct	*task;		/* I: 对应的任务 */
	struct worker_pool	*pool;		/* A: 所属池；rescuer 另算 */
	struct list_head	node;		/* A: 挂在 pool->workers */

	unsigned long		last_active;
	unsigned int		flags;		/* L: WORKER_* */
	int			id;		/* I: 出现在 comm 里 */

	char			desc[WORKER_DESC_LEN]; /* set_worker_desc()，长度 32 */

	struct workqueue_struct	*rescue_wq;	/* I: 只有 rescuer 使用 */
};
```

`current_wq_worker`（`:67`）在 `in_task()` 且 `current->flags & PF_WQ_WORKER` 时，用 `kthread_data(current)` 取回这个结构。`PF_WQ_WORKER` 是 `0x00000020`（`include/linux/sched.h:1752`）。

`worker_thread`（`workqueue.c:2760`）的循环：

```c
/* kernel/workqueue.c:2760 */
static int worker_thread(void *__worker)
{
	set_pf_worker(true);
woke_up:
	raw_spin_lock_irq(&pool->lock);
	if (unlikely(worker->flags & WORKER_DIE)) {
		/* 改名 kworker/dying，脱离池子，kfree(worker)，return 0 */
	}
	worker_leave_idle(worker);
recheck:
	if (!need_more_worker(pool))
		goto sleep;
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;

	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);
	do {
		work = list_first_entry(&pool->worklist, struct work_struct, entry);
		if (assign_work(work, worker, NULL))
			process_scheduled_works(worker);
	} while (keep_working(pool));

sleep:
	worker_enter_idle(worker);
	__set_current_state(TASK_IDLE);
	raw_spin_unlock_irq(&pool->lock);
	schedule();
	goto woke_up;
}
```

`process_scheduled_works` 最终到 `process_one_work`（[10](./10-cmwq-queue-and-flags.md)）。worker 睡觉用的是 `TASK_IDLE`，不是 `TASK_INTERRUPTIBLE`。需要新 worker 时 `manage_workers` → `maybe_create_worker`（`:2475`）→ `create_worker`（`:2189`）。

rescuer 是另一条线程 `rescuer_thread`（`:2884`），只为带 `WQ_MEM_RECLAIM` 的队列创建。它不参与并发管理（`WORKER_PREP` 一直留着，注释 `:2893`）。求救路径在 [12](./12-perf-analysis.md)。

---

## 9.7 关键函数

| 文件 | 符号 | 行号 | 作用 |
|------|------|------|------|
| `include/linux/workqueue.h` | `struct work_struct` | 98 | 工作项 |
| `include/linux/workqueue.h` | `INIT_WORK` | 302 | 填 `data` / `entry` / `func` |
| `include/linux/workqueue.h` | `struct delayed_work` | 111 | 工作项加一个定时器 |
| `kernel/workqueue.c` | `struct worker_pool` | 155 | 线程池与 `worklist` |
| `kernel/workqueue.c` | `struct pool_workqueue` | 229 | 一条队列在一个池上的限额 |
| `kernel/workqueue.c` | `struct workqueue_struct` | 288 | 对外的队列对象 |
| `kernel/workqueue_internal.h` | `struct worker` | 24 | 一个 kworker 任务 |
| `kernel/workqueue.c` | `worker_thread` | 2760 | 从 `worklist` 取工作并执行 |
| `kernel/workqueue.c` | `create_worker` | 2189 | 新建一个 worker 任务 |
| `kernel/workqueue.c` | `rescuer_thread` | 2884 | `WQ_MEM_RECLAIM` 的急救线程 |

---

**导航**：[← request_module](./08-kmod-request.md) · [目录](./README.md) · [入队与标志 →](./10-cmwq-queue-and-flags.md)
