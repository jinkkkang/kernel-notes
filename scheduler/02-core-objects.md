# 2. 核心对象

**结论**：记住五层——**`task_struct`（任务）** 里嵌着 **`sched_entity`（fair 记账实体）**，实体挂在某 CPU 的 **`cfs_rq`** 上，所有类的队列收在每 CPU **`struct rq`**；组调度时再叠一层 **`task_group`**（对接 cgroup cpu）。选人算法走 **`sched_class` 函数表**。

> 定义主战场：`kernel/sched/sched.h`、`include/linux/sched.h`

---

## 2.1 关系一张图

```text
per-CPU struct rq                         kernel/sched/sched.h:963
  ├─ cfs  (struct cfs_rq)                 :999
  │     tasks_timeline                    红黑树，键是 vruntime
  │     每个节点另存 min_deadline         给 EEVDF 在树上找最早 deadline
  │     curr / next                       正在跑的实体 / NEXT_BUDDY
  │         ▲
  │         │ on_rq 的实体
  │    sched_entity  ◄── task_struct.se   普通 fair 任务（嵌在 task 里，不是指针）
  │         │
  │         └─ parent / my_q              组调度：实体代表整个 task_group
  ├─ rt / dl
  ├─ curr / idle / stop
  └─ leaf_cfs_rq_list                     本 CPU 上的叶子 cfs_rq（组调度）

task_struct
  ├─ on_rq / prio / static_prio / normal_prio / rt_priority
  ├─ se / rt / dl
  ├─ sched_class*
  └─ sched_task_group*  → task_group → cgroup css（cpu 控制器）
```

`entity_is_task(se)`（`sched.h:771`，要 `CONFIG_FAIR_GROUP_SCHED`）看 `!se->my_q`：没有自己的队列就是真实任务，有就是代表 cgroup 的组实体。没开组调度时这个宏恒为 1。

---

## 2.2 先认两个小结构

`sched_entity` 的第一个成员、以及 PELT 平均，类型都在 `include/linux/sched.h`。

```c
/* include/linux/sched.h:411 */
struct load_weight {
	unsigned long			weight;		/* 权重，nice 0 对应 NICE_0_LOAD */
	u32				inv_weight;	/* 1/weight 的定点倒数，给 calc_delta_fair 用 */
};
```

```c
/* include/linux/sched.h:445
 * 任务的 util 估计。enqueued 的最高位 UTIL_AVG_UNCHANGED 用来和 util_avg 对齐。
 */
struct util_est {
	unsigned int			enqueued;	/* 任务：上次出队时的 util_avg；cfs_rq：可跑任务之和 */
	unsigned int			ewma;		/* 只对任务：util 的指数滑动平均 */
#define UTIL_EST_WEIGHT_SHIFT		2
#define UTIL_AVG_UNCHANGED		0x80000000
} __attribute__((__aligned__(sizeof(u64))));
```

```c
/* include/linux/sched.h:497
 * PELT 平均。单独占一条 cache line，避免和上面只读字段抢同一行。
 */
struct sched_avg {
	u64				last_update_time;	/* 上次用调度时钟更新的时刻 */
	u64				load_sum;		/* 负载的衰减累加 */
	u64				runnable_sum;		/* 可跑时间的衰减累加 */
	u32				util_sum;		/* 正在跑时间的衰减累加 */
	u32				period_contrib;		/* 当前 1024us 窗口里已走过的部分 */
	unsigned long			load_avg;		/* 衰减后的负载 */
	unsigned long			runnable_avg;		/* 衰减后的可跑比例 */
	unsigned long			util_avg;		/* 衰减后的利用率 */
	struct util_est			util_est;		/* 出队时记下的 util 估计 */
} ____cacheline_aligned;
```

---

## 2.3 `struct sched_entity`（fair 实体）

`include/linux/sched.h:551`。字段顺序、类型、`#ifdef` 与 v6.6.160 一致。

```c
struct sched_entity {
	/* --- 排队 --- */
	/* For load-balancing: */
	struct load_weight		load;		/* 本实体权重 */
	struct rb_node			run_node;	/* 挂进 cfs_rq->tasks_timeline */
	u64				deadline;	/* 虚拟截止 vd；EEVDF 在合格者里挑最小的 */
	u64				min_deadline;	/* 以本节点为根的子树里最小的 deadline */

	struct list_head		group_node;	/* SMP：挂在 rq->cfs_tasks，均衡时扫任务 */
	unsigned int			on_rq;		/* 1 表示实体在这个 cfs_rq 上 */

	/* --- 记账 --- */
	u64				exec_start;		/* 本段墙上执行起点（rq_clock_task） */
	u64				sum_exec_runtime;	/* 累计墙上执行时间 */
	u64				prev_sum_exec_runtime;	/* 上次被选中时的累计墙上时间 */
	u64				vruntime;		/* 虚拟运行时间；权重越大涨得越慢 */
	s64				vlag;			/* 虚拟 lag：V - v_i，出队时写入；被选中时另有用途，见 03 */
	u64				slice;			/* 本次请求的墙上长度，默认 sysctl_sched_base_slice */

	u64				nr_migrations;		/* 跨 CPU 次数 */

	/* --- 组调度 --- */
#ifdef CONFIG_FAIR_GROUP_SCHED
	int				depth;		/* 组层次深度 */
	struct sched_entity		*parent;	/* 上一层 group se */
	/* rq on which this entity is (to be) queued: */
	struct cfs_rq			*cfs_rq;	/* 自己正挂在哪个 cfs_rq */
	/* rq "owned" by this entity/group: */
	struct cfs_rq			*my_q;		/* 本组在这个 CPU 上的队列；任务为 NULL */
	/* cached value of my_q->h_nr_running */
	unsigned long			runnable_weight; /* 组实体：缓存 my_q->h_nr_running */
#endif

	/* --- PELT --- */
#ifdef CONFIG_SMP
	/*
	 * Per entity load average tracking.
	 * Put into separate cache line so it does not
	 * collide with read-mostly values above.
	 */
	struct sched_avg		avg;		/* 本实体的 PELT；非 SMP 构建没有这个成员 */
#endif
};
```

普通 fair 任务的 `task_struct.se` 就是叶子实体。`on_rq` 在 `enqueue_entity` 里写成 1、在 `dequeue_entity` 里写成 0（`fair.c:5348`、`:5418`）。任务自己的 `task_struct.on_rq` 是另一套：`TASK_ON_RQ_QUEUED`（1）和 `TASK_ON_RQ_MIGRATING`（2），见 `sched.h:107`。

---

## 2.4 `struct cfs_rq`

`kernel/sched/sched.h:547`。每个 CPU 的 `rq` 里嵌一个根 `cfs`；开了组调度后，每个 `task_group` 在每个 CPU 还有自己的 `cfs_rq`。

```c
/* CFS-related fields in a runqueue */
struct cfs_rq {
	/* --- 负载与人数 --- */
	struct load_weight	load;			/* 本队列上实体 weight 之和 */
	unsigned int		nr_running;		/* 本层排队实体数 */
	unsigned int		h_nr_running;		/* 层次上的任务数，含 NORMAL/BATCH/IDLE */
	unsigned int		idle_nr_running;	/* 本层 SCHED_IDLE 实体数 */
	unsigned int		idle_h_nr_running;	/* 层次上的 SCHED_IDLE 任务数 */

	/* --- EEVDF 平均 --- */
	s64			avg_vruntime;		/* Σ (v_i - min_vruntime) * w_i，不含正在跑的 curr */
	u64			avg_load;		/* 上面那个和所用的 Σ w_i */

	u64			exec_clock;		/* 本队列累计执行时钟 */
	u64			min_vruntime;		/* 单调前进的虚拟时间零点 v0 */
#ifdef CONFIG_SCHED_CORE
	unsigned int		forceidle_seq;		/* core scheduling 的 force-idle 序号 */
	u64			min_vruntime_fi;	/* force-idle 下另记的 min_vruntime */
#endif

#ifndef CONFIG_64BIT
	u64			min_vruntime_copy;	/* 32 位：min_vruntime 的副本，防撕裂读 */
#endif

	/* --- 树 --- */
	struct rb_root_cached	tasks_timeline;		/* 按 vruntime 排序；节点带 min_deadline */

	/*
	 * 'curr' points to currently running entity on this cfs_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
	struct sched_entity	*curr;			/* 正在跑；它不在红黑树里 */
	struct sched_entity	*next;			/* NEXT_BUDDY：希望下一个被选中的实体 */

#ifdef CONFIG_SCHED_DEBUG
	unsigned int		nr_spread_over;		/* 调试：vruntime 散得过开的次数 */
#endif

	/* --- SMP / PELT --- */
#ifdef CONFIG_SMP
	struct sched_avg	avg;			/* 本 cfs_rq 的 PELT */
#ifndef CONFIG_64BIT
	u64			last_update_time_copy;	/* 32 位：avg.last_update_time 的副本 */
#endif
	/*
	 * 实体已经离开，但负载还没从本队列减掉。远程 CPU 先记在这里，持有者再减。
	 */
	struct {
		raw_spinlock_t	lock ____cacheline_aligned; /* 保护下面几项 */
		int		nr;		/* 待减次数 */
		unsigned long	load_avg;	/* 待减 load */
		unsigned long	util_avg;	/* 待减 util */
		unsigned long	runnable_avg;	/* 待减 runnable */
	} removed;

#ifdef CONFIG_FAIR_GROUP_SCHED
	unsigned long		tg_load_avg_contrib;	/* 本队列对 task_group->load_avg 的贡献 */
	long			propagate;		/* 要向上传的负载增量 */
	long			prop_runnable_sum;	/* 要向上传的 runnable_sum */

	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
	unsigned long		h_load;			/* 层次负载，均衡时用 */
	u64			last_h_load_update;	/* 上次更新 h_load 的时间 */
	struct sched_entity	*h_load_next;		/* 更新 h_load 时沿层次走的游标 */
#endif /* CONFIG_FAIR_GROUP_SCHED */
#endif /* CONFIG_SMP */

	/* --- 组与带宽 --- */
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct rq		*rq;	/* 这个 cfs_rq 挂在哪个 CPU rq 上 */

	/*
	 * leaf cfs_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_cfs_rq_list ties together list of leaf cfs_rq's in a CPU.
	 * This list is used during load balance.
	 */
	int			on_list;		/* 是否已挂上叶子链表 */
	struct list_head	leaf_cfs_rq_list;	/* 串到 rq->leaf_cfs_rq_list */
	struct task_group	*tg;	/* 拥有这个队列的 task_group */

	/* Locally cached copy of our task_group's idle value */
	int			idle;			/* 本组是不是 SCHED_IDLE 组的缓存 */

#ifdef CONFIG_CFS_BANDWIDTH
	int			runtime_enabled;	/* 本队列启用了 quota */
	s64			runtime_remaining;	/* 本周期还剩多少额度（纳秒） */

	u64			throttled_pelt_idle;	/* 限流时冻结 PELT 用的 idle 时钟 */
#ifndef CONFIG_64BIT
	u64                     throttled_pelt_idle_copy; /* 32 位副本 */
#endif
	u64			throttled_clock;		/* 进入限流时的 rq 时钟 */
	u64			throttled_clock_pelt;		/* 进入限流时的 PELT 时钟 */
	u64			throttled_clock_pelt_time;	/* 累计被限流的 PELT 时间 */
	u64			throttled_clock_self;		/* 本层自己限流的起点 */
	u64			throttled_clock_self_time;	/* 本层累计限流时间 */
	int			throttled;			/* 当前被限流 */
	int			throttle_count;			/* 层次上有多少祖先也在限流 */
	struct list_head	throttled_list;			/* 挂到 cfs_bandwidth->throttled_cfs_rq */
#ifdef CONFIG_SMP
	struct list_head	throttled_csd_list;		/* 跨 CPU 解限流用的 CSD 链 */
#endif
#endif /* CONFIG_CFS_BANDWIDTH */
#endif /* CONFIG_FAIR_GROUP_SCHED */
};
```

**直觉**：`avg_vruntime` / `avg_load` 让 `avg_vruntime()` 不用每次把树扫一遍。正在跑的 `curr` 不在树上，算平均时要临时加回去（`fair.c:677`）。

---

## 2.5 `struct rq`（每 CPU 运行队列）

`kernel/sched/sched.h:963`。改队列、选人，通常持 **`rq->__lock`**（`raw_spinlock_t`）。唤醒路径还要先拿 `p->pi_lock`。见 [locks-and-ds](../locks-and-ds/)。

```c
struct rq {
	/* --- 锁与热计数 --- */
	/* runqueue lock: */
	raw_spinlock_t		__lock;			/* rq 自旋锁 */

	/*
	 * nr_running and cpu_load should be in the same cacheline because
	 * remote CPUs use both these fields when doing load calculation.
	 */
	unsigned int		nr_running;		/* 本 CPU 可跑任务数（各类合计） */
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		nr_numa_running;	/* 本 rq 上的 NUMA 任务数 */
	unsigned int		nr_preferred_running;	/* 其中跑在首选节点的数量 */
	unsigned int		numa_migrate_on;	/* 本 rq 是否做 NUMA 迁移 */
#endif
#ifdef CONFIG_NO_HZ_COMMON
#ifdef CONFIG_SMP
	unsigned long		last_blocked_load_update_tick; /* 上次刷新 blocked load 的 tick */
	unsigned int		has_blocked_load;	/* idle 上还有没衰减完的负载 */
	call_single_data_t	nohz_csd;		/* nohz 均衡用的 IPI 描述符 */
#endif /* CONFIG_SMP */
	unsigned int		nohz_tick_stopped;	/* 本 CPU 的 tick 是否停了 */
	atomic_t		nohz_flags;		/* NOHZ_* kick 位 */
#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_SMP
	unsigned int		ttwu_pending;		/* 挂起、还没在本 CPU 上激活的唤醒 */
#endif
	u64			nr_switches;		/* 上下文切换次数 */

	/* --- 利用率夹具 --- */
#ifdef CONFIG_UCLAMP_TASK
	/* Utilization clamp values based on CPU's RUNNABLE tasks */
	struct uclamp_rq	uclamp[UCLAMP_CNT] ____cacheline_aligned; /* 本 rq 上的 min/max 夹具 */
	unsigned int		uclamp_flags;		/* UCLAMP_FLAG_IDLE 等 */
#define UCLAMP_FLAG_IDLE 0x01
#endif

	/* --- 三类队列 --- */
	struct cfs_rq		cfs;			/* fair */
	struct rt_rq		rt;			/* RT */
	struct dl_rq		dl;			/* deadline */

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* list of leaf cfs_rq on this CPU: */
	struct list_head	leaf_cfs_rq_list;	/* 本 CPU 叶子 cfs_rq，供负载均衡 */
	struct list_head	*tmp_alone_branch;	/* 插入叶子链表时的暂存指针 */
#endif /* CONFIG_FAIR_GROUP_SCHED */

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	unsigned long 		nr_uninterruptible;	/* 贡献给 loadavg 的不可打断睡眠 */

	/* --- 当前任务 --- */
	struct task_struct __rcu	*curr;		/* 正在执行者 */
	struct task_struct	*idle;			/* 本 CPU idle 线程 */
	struct task_struct	*stop;			/* stop 类任务 */
	unsigned long		next_balance;		/* 下次周期均衡的 jiffies */
	struct mm_struct	*prev_mm;		/* 切走内核线程时暂存的旧 mm */

	/* --- 时钟 --- */
	unsigned int		clock_update_flags;	/* RQCF_*：本锁段是否已更新时钟 */
	u64			clock;			/* rq 时钟（rq_clock） */
	/* Ensure that all clocks are in the same cache line */
	u64			clock_task ____cacheline_aligned; /* 任务时钟（rq_clock_task） */
	u64			clock_pelt;		/* PELT 用的时钟 */
	unsigned long		lost_idle_time;		/* idle 期间丢掉的时间 */
	u64			clock_pelt_idle;	/* idle 区间的 PELT 时钟 */
	u64			clock_idle;		/* idle 区间的 rq 时钟 */
#ifndef CONFIG_64BIT
	u64			clock_pelt_idle_copy;	/* 32 位副本 */
	u64			clock_idle_copy;	/* 32 位副本 */
#endif

	atomic_t		nr_iowait;		/* 本 rq 上的 iowait 任务数 */

#ifdef CONFIG_SCHED_DEBUG
	u64 last_seen_need_resched_ns;		/* 上次看到 NEED_RESCHED 的时间，给延迟告警 */
	int ticks_without_resched;		/* 连续多少 tick 没重调度 */
#endif

#ifdef CONFIG_MEMBARRIER
	int membarrier_state;			/* membarrier 状态 */
#endif

	/* --- SMP：域、容量、均衡 --- */
#ifdef CONFIG_SMP
	struct root_domain		*rd;		/* 根域（cpuset 划分出的岛） */
	struct sched_domain __rcu	*sd;		/* 本 CPU 的调度域链 */

	unsigned long		cpu_capacity;		/* 本 CPU 当前容量 */

	struct balance_callback *balance_callback;	/* 放下 rq 锁之后要跑的均衡回调 */

	unsigned char		nohz_idle_balance;	/* 这次 softirq 要不要做 nohz idle 均衡 */
	unsigned char		idle_balance;		/* 触发 softirq 时本 CPU 是否 idle */

	unsigned long		misfit_task_load;	/* 当前任务塞不进本 CPU 容量时记下的负载 */

	/* For active balancing */
	int			active_balance;		/* 正在用 stopper 推任务 */
	int			push_cpu;		/* 推到哪个 CPU */
	struct cpu_stop_work	active_balance_work;	/* stopper 工作项 */

	/* CPU of this runqueue: */
	int			cpu;			/* 本 rq 的 CPU 号 */
	int			online;			/* 本 CPU 是否 online */

	struct list_head cfs_tasks;			/* 本 rq 上 fair 任务的链表（se->group_node） */

	struct sched_avg	avg_rt;			/* RT 占用的 PELT */
	struct sched_avg	avg_dl;			/* DL 占用的 PELT */
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;		/* 中断占用的 PELT */
#endif
#ifdef CONFIG_SCHED_THERMAL_PRESSURE
	struct sched_avg	avg_thermal;		/* 热限频压力 */
#endif
	u64			idle_stamp;		/* 进入 idle 的时间戳 */
	u64			avg_idle;		/* 平均 idle 长度 */

	unsigned long		wake_stamp;		/* 最近一次唤醒相关时间戳 */
	u64			wake_avg_idle;		/* 唤醒路径上看到的平均 idle */

	/* This is used to determine avg_idle's max value */
	u64			max_idle_balance_cost;	/* newidle 均衡花过的最长时间 */

#ifdef CONFIG_HOTPLUG_CPU
	struct rcuwait		hotplug_wait;		/* 热插拔等待 */
#endif
#endif /* CONFIG_SMP */

	/* --- 中断 / 偷时间 --- */
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64			prev_irq_time;		/* 上次中断记账时间 */
	u64			psi_irq_time;		/* 交给 PSI 的中断时间 */
#endif
#ifdef CONFIG_PARAVIRT
	u64			prev_steal_time;	/* 上次被 hypervisor 偷走的时间 */
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64			prev_steal_time_rq;	/* rq 视角的 steal 时间 */
#endif

	/* --- loadavg --- */
	/* calc_load related fields */
	unsigned long		calc_load_update;	/* 下次折叠进全局 loadavg 的 jiffies */
	long			calc_load_active;	/* 本 rq 对 loadavg 的贡献 */

	/* --- 高精度 tick --- */
#ifdef CONFIG_SCHED_HRTICK
#ifdef CONFIG_SMP
	call_single_data_t	hrtick_csd;		/* 跨 CPU 启动 hrtick */
#endif
	struct hrtimer		hrtick_timer;		/* 到点抢占的高精度定时器 */
	ktime_t 		hrtick_time;		/* 定时器到期时刻 */
#endif

	/* --- schedstat --- */
#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	struct sched_info	rq_sched_info;		/* rq 级等待统计 */
	unsigned long long	rq_cpu_time;		/* rq 上累计 CPU 时间 */
	/* could above be rq->cfs_rq.exec_clock + rq->rt_rq.rt_runtime ? */

	/* sys_sched_yield() stats */
	unsigned int		yld_count;		/* yield 次数 */

	/* schedule() stats */
	unsigned int		sched_count;		/* 进入 schedule 的次数 */
	unsigned int		sched_goidle;		/* 切到 idle 的次数 */

	/* try_to_wake_up() stats */
	unsigned int		ttwu_count;		/* 唤醒次数 */
	unsigned int		ttwu_local;		/* 其中唤醒到本 CPU 的次数 */
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state	*idle_state;		/* 当前 cpuidle 状态 */
#endif

#ifdef CONFIG_SMP
	unsigned int		nr_pinned;		/* 钉死在本 CPU、不能迁走的任务数 */
#endif
	unsigned int		push_busy;		/* RT/DL 推任务正在进行 */
	struct cpu_stop_work	push_work;		/* 推任务的 stopper 工作项 */

	/* --- core scheduling --- */
#ifdef CONFIG_SCHED_CORE
	/* per rq */
	struct rq		*core;			/* SMT 核心里负责一起选人的那个 rq */
	struct task_struct	*core_pick;		/* 这次核心范围选出的任务 */
	unsigned int		core_enabled;		/* 本 rq 是否启用 core scheduling */
	unsigned int		core_sched_seq;		/* 本 rq 已经执行到的选择序号 */
	struct rb_root		core_tree;		/* 按 cookie 排的任务树 */

	/* shared state -- careful with sched_core_cpu_deactivate() */
	unsigned int		core_task_seq;		/* 入队出队改变后的序号 */
	unsigned int		core_pick_seq;		/* 最近一次核心范围选择的序号 */
	unsigned long		core_cookie;		/* 当前核心正在跑的 cookie */
	unsigned int		core_forceidle_count;	/* force-idle 计数 */
	unsigned int		core_forceidle_seq;	/* force-idle 序号 */
	unsigned int		core_forceidle_occupation; /* force-idle 时占着几个 sibling */
	u64			core_forceidle_start;	/* force-idle 起点 */
	unsigned int		core_pick_in_flight;	/* 正在做核心范围选择的次数 */
#endif

	/* Scratch cpumask to be temporarily used under rq_lock */
	cpumask_var_t		scratch_mask;		/* 持锁时借用的临时 cpumask */

#if defined(CONFIG_CFS_BANDWIDTH) && defined(CONFIG_SMP)
	call_single_data_t	cfsb_csd;		/* 带宽解限流的 IPI */
	struct list_head	cfsb_csd_list;		/* 等待这次 IPI 处理的 cfs_rq */
#endif
};
```

取队列的宏（`sched.h:1217`）：`cpu_rq(cpu)`、`this_rq()`、`task_rq(p)`。

---

## 2.6 `struct sched_class`

`kernel/sched/sched.h:2247`。这是**函数表**，不是「类对象里塞状态」。各类用 `DEFINE_SCHED_CLASS(name)`（`:2333`）放进链接器段，地址小的优先级高。下面按源码顺序列出全部回调。

```c
struct sched_class {

#ifdef CONFIG_UCLAMP_TASK
	int uclamp_enabled;		/* 这个类是否参与 uclamp */
#endif

	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags); /* 任务入该类队列 */
	void (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags); /* 任务出该类队列 */
	void (*yield_task)   (struct rq *rq);			/* 主动让出 */
	bool (*yield_to_task)(struct rq *rq, struct task_struct *p); /* 让给指定任务 */

	void (*wakeup_preempt)(struct rq *rq, struct task_struct *p, int flags); /* 唤醒后要不要抢当前 */

	struct task_struct *(*pick_next_task)(struct rq *rq);	/* 选出下一个要跑的 task */

	void (*put_prev_task)(struct rq *rq, struct task_struct *p); /* 切走前记账 */
	void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first); /* 切上来后记账 */

#ifdef CONFIG_SMP
	int (*balance)(struct rq *rq, struct task_struct *prev, struct rq_flags *rf); /* 选人前的均衡机会 */
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags); /* 选 CPU */

	struct task_struct * (*pick_task)(struct rq *rq);	/* core scheduling 用的「只看本 rq」挑选 */

	void (*migrate_task_rq)(struct task_struct *p, int new_cpu); /* 迁移前把 PELT 摘干净 */

	void (*task_woken)(struct rq *this_rq, struct task_struct *task); /* 唤醒完成、锁仍可放下之后 */

	void (*set_cpus_allowed)(struct task_struct *p, struct affinity_context *ctx); /* 亲和性变更 */

	void (*rq_online)(struct rq *rq);			/* CPU 上线 */
	void (*rq_offline)(struct rq *rq);			/* CPU 下线 */

	struct rq *(*find_lock_rq)(struct task_struct *p, struct rq *rq); /* 找到并锁住任务所在 rq */
#endif

	void (*task_tick)(struct rq *rq, struct task_struct *p, int queued); /* 周期 tick */
	void (*task_fork)(struct task_struct *p);		/* fork 时给新实体摆位置 */
	void (*task_dead)(struct task_struct *p);		/* 任务死亡，拆掉 PELT */

	/*
	 * The switched_from() call is allowed to drop rq->lock, therefore we
	 * cannot assume the switched_from/switched_to pair is serialized by
	 * rq->lock. They are however serialized by p->pi_lock.
	 */
	void (*switched_from)(struct rq *this_rq, struct task_struct *task); /* 离开这个类 */
	void (*switched_to)  (struct rq *this_rq, struct task_struct *task); /* 进入这个类 */
	void (*prio_changed) (struct rq *this_rq, struct task_struct *task,
			      int oldprio);			/* 优先级变了 */

	unsigned int (*get_rr_interval)(struct rq *rq,
					struct task_struct *task); /* 报告时间片，单位 jiffies */

	void (*update_curr)(struct rq *rq);			/* 给别的类推进 fair 账本的入口 */

#ifdef CONFIG_FAIR_GROUP_SCHED
	void (*task_change_group)(struct task_struct *p);	/* 任务换 cgroup */
#endif

#ifdef CONFIG_SCHED_CORE
	int (*task_is_throttled)(struct task_struct *p, int cpu); /* core scheduling：这任务是不是被限流 */
#endif
};
```

fair 的实例：`DEFINE_SCHED_CLASS(fair)` 在 `fair.c:13225`。它填了这些槽（没写的在 SMP 下是 NULL，例如 `task_woken`、`find_lock_rq`）：

| 槽 | fair 的函数 |
|---|---|
| `enqueue_task` | `enqueue_task_fair` |
| `dequeue_task` | `dequeue_task_fair` |
| `yield_task` | `yield_task_fair` |
| `yield_to_task` | `yield_to_task_fair` |
| `wakeup_preempt` | `check_preempt_wakeup_fair` |
| `pick_next_task` | `__pick_next_task_fair` |
| `put_prev_task` | `put_prev_task_fair` |
| `set_next_task` | `set_next_task_fair` |
| `balance` | `balance_fair`（`CONFIG_SMP`） |
| `pick_task` | `pick_task_fair` |
| `select_task_rq` | `select_task_rq_fair` |
| `migrate_task_rq` | `migrate_task_rq_fair` |
| `rq_online` / `rq_offline` | `rq_online_fair` / `rq_offline_fair` |
| `task_dead` | `task_dead_fair` |
| `set_cpus_allowed` | `set_cpus_allowed_common` |
| `task_tick` | `task_tick_fair` |
| `task_fork` | `task_fork_fair` |
| `prio_changed` | `prio_changed_fair` |
| `switched_from` / `switched_to` | `switched_from_fair` / `switched_to_fair` |
| `get_rr_interval` | `get_rr_interval_fair` |
| `update_curr` | `update_curr_fair` |
| `task_change_group` | `task_change_group_fair`（`CONFIG_FAIR_GROUP_SCHED`） |
| `task_is_throttled` | `task_is_throttled_fair`（`CONFIG_SCHED_CORE`） |
| `uclamp_enabled` | `1`（`CONFIG_UCLAMP_TASK`） |

入队/出队 flag（`sched.h:2216`）本章会用到的：

| 宏 | 值 | 含义 |
|---|---|---|
| `DEQUEUE_SLEEP` | `0x01` | 真的要睡，不是临时摘下 |
| `DEQUEUE_SAVE` | `0x02` | 和 `ENQUEUE_RESTORE` 成对，尽量保住位置 |
| `DEQUEUE_MOVE` | `0x04` | 和 `ENQUEUE_MOVE` 成对，允许改位置 |
| `DEQUEUE_NOCLOCK` | `0x08` | 调用方已经更新过 rq 时钟 |
| `DEQUEUE_MIGRATING` | `0x100` | deadline 迁移中 |
| `ENQUEUE_WAKEUP` | `0x01` | 从睡眠醒来 |
| `ENQUEUE_RESTORE` | `0x02` | 恢复先前 SAVE 的实体 |
| `ENQUEUE_MOVE` | `0x04` | 允许改位置 |
| `ENQUEUE_NOCLOCK` | `0x08` | 跳过 `update_rq_clock` |
| `ENQUEUE_HEAD` | `0x10` | 放到队头（fair 基本不用这个语义） |
| `ENQUEUE_REPLENISH` | `0x20` | deadline 补充 runtime |
| `ENQUEUE_MIGRATED` | `0x40`（仅 SMP） | 这次唤醒换了 CPU |
| `ENQUEUE_INITIAL` | `0x80` | 新任务第一次入队 |
| `ENQUEUE_MIGRATING` | `0x100` | 迁移中的入队 |

---

## 2.7 `struct task_group`

`kernel/sched/sched.h:364`。整个结构体包在 `CONFIG_CGROUP_SCHED` 里。`css_tg()` 不在这个头文件里，而在 `kernel/sched/core.c:10602`：`container_of(css, struct task_group, css)`。

```c
/* kernel/sched/sched.h:337 —— 嵌在 task_group 末尾的那个成员的类型 */
struct cfs_bandwidth {
#ifdef CONFIG_CFS_BANDWIDTH
	raw_spinlock_t		lock;			/* 保护本带宽对象 */
	ktime_t			period;			/* 周期长度 */
	u64			quota;			/* 每周期配额；RUNTIME_INF 表示不限 */
	u64			runtime;		/* 全局池里还没分下去的额度 */
	u64			burst;			/* 允许透支的 burst */
	u64			runtime_snap;		/* 本周期开始时的 runtime，用来算 burst */
	s64			hierarchical_quota;	/* 算上祖先之后的有效配额 */

	u8			idle;			/* 全局池暂时空闲 */
	u8			period_active;		/* 周期定时器在跑 */
	u8			slack_started;		/* slack 定时器已启动 */
	struct hrtimer		period_timer;		/* 周期到点：sched_cfs_period_timer */
	struct hrtimer		slack_timer;		/* 提前归还没用完的额度 */
	struct list_head	throttled_cfs_rq;	/* 被限流的 per-CPU cfs_rq */

	/* Statistics: */
	int			nr_periods;		/* 走过多少个周期 */
	int			nr_throttled;		/* 被限流多少次 */
	int			nr_burst;		/* 用到 burst 多少次 */
	u64			throttled_time;		/* 累计限流时间（纳秒） */
	u64			burst_time;		/* 累计 burst 时间（纳秒） */
#endif
};
```

```c
/* Task group related information */
struct task_group {
	struct cgroup_subsys_state css;		/* 嵌入 css；container_of 找回 tg */

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* schedulable entities of this group on each CPU */
	struct sched_entity	**se;		/* 每 CPU 一个组实体 */
	/* runqueue "owned" by this group on each CPU */
	struct cfs_rq		**cfs_rq;	/* 每 CPU 一个组队列 */
	unsigned long		shares;		/* 权重份额，对接 cpu.weight / cpu.shares */

	/* A positive value indicates that this is a SCHED_IDLE group. */
	int			idle;		/* 非 0 表示这是 SCHED_IDLE 组 */

#ifdef	CONFIG_SMP
	/*
	 * load_avg can be heavily contended at clock tick time, so put
	 * it in its own cacheline separated from the fields above which
	 * will also be accessed at each tick.
	 */
	atomic_long_t		load_avg ____cacheline_aligned; /* 组级 load 平均 */
#endif
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity	**rt_se;	/* 每 CPU 一个 RT 组实体 */
	struct rt_rq		**rt_rq;	/* 每 CPU 一个 RT 组队列 */

	struct rt_bandwidth	rt_bandwidth;	/* RT 组带宽 */
#endif

	struct rcu_head		rcu;		/* 延迟释放 */
	struct list_head	list;		/* 全局 task_groups 链表 */

	struct task_group	*parent;	/* 父组 */
	struct list_head	siblings;	/* 兄弟链表 */
	struct list_head	children;	/* 子组链表 */

#ifdef CONFIG_SCHED_AUTOGROUP
	struct autogroup	*autogroup;	/* tty 自动组；没有则为 NULL */
#endif

	struct cfs_bandwidth	cfs_bandwidth;	/* quota/period/burst */

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* The two decimal precision [%] value requested from user-space */
	unsigned int		uclamp_pct[UCLAMP_CNT];	/* 用户请求的夹具百分比 */
	/* Clamp values requested for a task group */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];	/* 请求的利用率夹具 */
	/* Effective clamp values used for a task group */
	struct uclamp_se	uclamp[UCLAMP_CNT];	/* 组上生效的利用率夹具 */
#endif

};
```

回链：[cgroup/05-cpu-cpuset.md](../cgroup/05-cpu-cpuset.md)、本章 [07](./07-cgroup-cpu.md)。

---

## 2.8 `task_struct` 上的调度字段

`include/linux/sched.h:771` 起这一段是连着的。前后还有别的成员，这里只展开调度直接用到的这一段，顺序与源码一致。

```c
#ifdef CONFIG_SMP
	int				on_cpu;		/* 正在某个 CPU 上跑 */
	struct __call_single_node	wake_entry;	/* 远程唤醒队列的节点 */
	unsigned int			wakee_flips;	/* 最近唤醒对象来回翻的次数 */
	unsigned long			wakee_flip_decay_ts; /* 上面计数的衰减时间 */
	struct task_struct		*last_wakee;	/* 上次被本任务唤醒的人 */

	/*
	 * recent_used_cpu 记下「刚被唤醒、又把别人唤醒」时用过的 CPU，
	 * 下次找空闲 CPU 可以先看它。
	 */
	int				recent_used_cpu;
	int				wake_cpu;	/* 希望被唤醒到的 CPU */
#endif
	int				on_rq;		/* TASK_ON_RQ_QUEUED / MIGRATING，或 0 */

	int				prio;		/* 当前生效优先级，数字小更高 */
	int				static_prio;	/* nice 换算来的静态优先级 */
	int				normal_prio;	/* 不考虑 PI 提升时的优先级 */
	unsigned int			rt_priority;	/* 用户看到的 RT 优先级 */

	struct sched_entity		se;		/* fair 实体，嵌在 task 里 */
	struct sched_rt_entity		rt;		/* RT 实体 */
	struct sched_dl_entity		dl;		/* deadline 实体 */
	const struct sched_class	*sched_class;	/* 当前所属调度类 */

#ifdef CONFIG_SCHED_CORE
	struct rb_node			core_node;	/* 挂在 rq->core_tree 上 */
	unsigned long			core_cookie;	/* 必须和谁一起上 SMT */
	unsigned int			core_occupation; /* cookie 占几个 sibling */
#endif

#ifdef CONFIG_CGROUP_SCHED
	struct task_group		*sched_task_group; /* 所属调度组 */
#endif

#ifdef CONFIG_UCLAMP_TASK
	/* Clamp values requested for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	struct uclamp_se		uclamp_req[UCLAMP_CNT]; /* 任务自己请求的夹具 */
	/* Effective clamp values used for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	struct uclamp_se		uclamp[UCLAMP_CNT];	/* 和组夹具取过之后的生效值 */
#endif

	struct sched_statistics         stats;		/* 字段都包在 CONFIG_SCHEDSTATS 里，见 include/linux/sched.h:509 */

#ifdef CONFIG_PREEMPT_NOTIFIERS
	/* List of struct preempt_notifier: */
	struct hlist_head		preempt_notifiers; /* 抢占通知链 */
#endif

#ifdef CONFIG_BLK_DEV_IO_TRACE
	unsigned int			btrace_seq;	/* blktrace 序号 */
#endif

	unsigned int			policy;		/* SCHED_NORMAL / FIFO / RR / BATCH / IDLE / DEADLINE */
	int				nr_cpus_allowed; /* cpus_mask 里有几位 */
	const cpumask_t			*cpus_ptr;	/* 当前允许的 CPU，通常指向 cpus_mask */
	cpumask_t			*user_cpus_ptr;	/* 用户最近一次 sched_setaffinity 的掩码 */
	cpumask_t			cpus_mask;	/* 允许掩码本体 */
	void				*migration_pending; /* 正在进行的迁移 */
#ifdef CONFIG_SMP
	unsigned short			migration_disabled; /* 非 0：这段时间不许迁走 */
#endif
	unsigned short			migration_flags; /* 迁移辅助标志 */
```

策略切换改 `sched_class` 指针：`__setscheduler_prio`（`core.c:7048`）按 `dl_prio` / `rt_prio` 指到 deadline、rt 或 fair。

`sched_info`（等待多久才上 CPU）是另一个小结构，`include/linux/sched.h:376`，整段包在 `CONFIG_SCHED_INFO` 里：`pcount`、`run_delay`、`last_arrival`、`last_queued`。

---

## 2.9 关键函数 / 宏

| 符号 | 位置 | 作用 |
|---|---|---|
| `cpu_rq(cpu)` / `this_rq()` / `task_rq(p)` | `sched.h:1217` | 取 per-CPU rq |
| `rq_of(cfs_rq)` | `sched.h:1176` | 组调度时读 `cfs_rq->rq`；否则 `container_of` |
| `entity_is_task(se)` | `sched.h:771` | 叶子任务还是组实体 |
| `for_each_sched_entity(se)` | `fair.c:328` / `:495` | 组调度时沿 `parent` 向上；没开组调度时循环体只跑一次 |
| `for_each_class(class)` | `sched.h:2345` | 从最高类走到最低类 |
| `sched_class_above(a, b)` | `sched.h:2348` | 地址更小则优先级更高 |
| `css_tg(css)` | `core.c:10602` | css → `task_group` |
| `cfs_rq_of(se)` | `sched.h:1440` | 组调度读 `se->cfs_rq`；否则从 task 找回 `rq->cfs` |
| `task_of(se)` | `sched.h:1428` | 叶子实体 → `container_of` 出 `task_struct` |

---

**导航**：[← 总览](./01-overview.md) · [章节目录](./README.md) · [EEVDF →](./03-eevdf.md)
