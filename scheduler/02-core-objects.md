# 2. 核心对象

**结论**：记住五层——**`task_struct`（任务）** 挂 **`sched_entity`（fair 记账实体）**，实体挂在某 CPU 的 **`cfs_rq`** 上，所有类的队列收在每 CPU **`struct rq`**；组调度时再叠一层 **`task_group`**（对接 cgroup cpu）。选人算法走 **`sched_class` 函数表**。

> 定义主战场：`kernel/sched/sched.h`、`include/linux/sched.h`

---

## 2.1 关系一张图

```text
per-CPU struct rq
  ├─ cfs  (struct cfs_rq) ── tasks_timeline (RB, 按 deadline)
  │         ▲
  │         │ on_rq 实体
  │    sched_entity  ◄── task_struct.se   （普通 fair 任务）
  │         │
  │         └─ parent / my_q  （组调度：实体代表整个 task_group）
  ├─ rt / dl /（可选）scx
  ├─ curr / donor / idle / stop
  └─ next_class

task_struct
  ├─ policy / prio / static_prio / normal_prio / rt_priority
  ├─ se / rt / dl
  ├─ sched_class*
  └─ sched_task_group*  → task_group → cgroup css（cpu 控制器）
```

---

## 2.2 `struct sched_entity`（fair 实体）

`include/linux/sched.h:572`。`avg` 的类型是 `struct sched_avg`（同文件 `:510`）。

```c
struct sched_entity {
	struct load_weight		load;		/* 权重 */
	struct load_weight		h_load;		/* 层次负载 */
	struct rb_node			run_node;	/* 挂在 cfs_rq->tasks_timeline */
	/*
	 * 虚拟截止时间 vd。EEVDF 按 earliest eligible deadline 排序。
	 */
	u64				deadline;
	u64				min_vruntime;	/* 子树最小 vruntime，供剪枝 */
	u64				min_slice;	/* 子树最小 slice，供剪枝 */
	u64				max_slice;	/* 子树最大 slice，供剪枝 */

	struct list_head		group_node;	/* 组相关链表节点 */
	unsigned char			on_rq;		/* 是否在 cfs_rq 上 */
	unsigned char			sched_delayed;	/* 延迟出队等状态 */
	unsigned char			rel_deadline;	/* 相对 deadline */
	unsigned char			custom_slice;	/* 自定义 slice */
					/* 对齐空洞 */

	u64				exec_start;		/* 本段墙上执行起点 */
	u64				sum_exec_runtime;	/* 累计墙上执行时间 */
	u64				prev_sum_exec_runtime;	/* 上次记账的累计墙上时间 */
	u64				vruntime;		/* 虚拟运行时间；权重越大涨得越慢 */
	/* 近似 virtual lag：相对平均虚拟时间的欠账或超账 */
	s64				vlag;
	/* 保护截止，给最小量子（protect_slice） */
	u64				vprot;
	u64				slice;		/* 本次请求大小，默认 sysctl_sched_base_slice */

	/*
	 * 跨 CPU 次数。set_task_cpu()（kernel/sched/core.c:3403）在
	 * task_cpu 真变了时加一；__sched_fork()（:4594）清零。
	 */
	u64				nr_migrations;

#ifdef CONFIG_FAIR_GROUP_SCHED
	int				depth;		/* 组层次深度 */
	struct sched_entity		*parent;	/* 父实体（上层 group se） */
	struct cfs_rq			*cfs_rq;	/* 当前所在 cfs_rq */
	struct cfs_rq			*my_q;		/* 本组拥有的 per-CPU cfs_rq */
	unsigned long			runnable_weight; /* 缓存的 runnable 权重（my_q->h_nr_runnable） */
#endif

	/* PELT 平均，单独占一条 cache line */
	struct sched_avg		avg;
};
```

任务与实体：普通 fair 任务的 `task_struct.se` 就是叶子实体；`entity_is_task(se)` 区分「真实任务」vs「代表 cgroup 的 group entity」。

---

## 2.3 `struct cfs_rq`

`kernel/sched/sched.h:681`

按组给出带注释的源码片段。只含笔记点名的成员；`throttled*` 按源码前缀展开。

```c
/* --- 计数 --- */
	unsigned int		nr_queued;		/* 本层排队数 */
	unsigned int		h_nr_queued;		/* 层次排队数，含 NORMAL/BATCH/IDLE */
	unsigned int		h_nr_runnable;		/* 层次可跑数 */
	unsigned int		h_nr_idle;		/* 层次 SCHED_IDLE 数 */
```

```c
/* --- EEVDF 平均 --- */
	s64			sum_w_vruntime;		/* 加权虚拟时间之和，用来构造 V */
	u64			sum_weight;		/* 权重之和，用来构造 V */
	u64			zero_vruntime;		/* 加权平均虚拟时间 V 的基准 */
	unsigned int		sum_shift;		/* 构造 V 时的移位 */
```

```c
/* --- 树 --- */
	struct rb_root_cached	tasks_timeline;		/* 按 deadline 排序的 runnable 实体树 */
```

```c
/* --- 当前 --- */
	struct sched_entity	*curr;			/* 当前跑的实体 */
	struct sched_entity	*next;			/* buddy */
	struct sched_entity	*h_curr;		/* 层次当前 */
```

```c
/* --- 负载 --- */
	struct load_weight	load;			/* 即时负载 */
	/* 此处省略本组未记录的成员 */
	struct sched_avg	avg;			/* PELT 平均 */
#ifndef CONFIG_64BIT
	/* 此处省略本组未记录的成员 */
#endif
	/*
	 * 实体离开后暂存、稍后减掉的负载与 PELT。
	 */
	struct {
		raw_spinlock_t	lock ____cacheline_aligned; /* 保护下面几项 */
		int		nr;		/* 待减次数 */
		unsigned long	load_avg;	/* 待减 load */
		unsigned long	util_avg;	/* 待减 util */
		unsigned long	runnable_avg;	/* 待减 runnable */
	} removed;
```

```c
/* --- 组 --- */
#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * 层次负载：h_load = weight * f(tg)，f(tg) 是组的递归权重份额。
	 */
	unsigned long		h_load;
	/* 此处省略本组未记录的成员 */
	struct rq		*rq;			/* 挂回 CPU rq */
	/* 此处省略本组未记录的成员 */
	struct list_head	leaf_cfs_rq_list;	/* 叶子 cfs_rq 链表，供负载均衡 */
	struct task_group	*tg;			/* 所属 task_group */
#endif
```

```c
/* --- 带宽 --- */
#ifdef CONFIG_FAIR_GROUP_SCHED
# ifdef CONFIG_CFS_BANDWIDTH
	int			runtime_enabled;	/* 本队列启用 cpu.max */
	s64			runtime_remaining;	/* 本周期剩余额度 */
	u64			throttled_pelt_idle;	/* 限流时用来冻结 PELT 的 idle 时钟 */
#  ifndef CONFIG_64BIT
	u64			throttled_pelt_idle_copy; /* 32 位：上一字段的副本 */
#  endif
	u64			throttled_clock;		/* 进入限流时的 rq 时钟 */
	u64			throttled_clock_pelt;		/* 进入限流时的 PELT 时钟 */
	u64			throttled_clock_pelt_time;	/* 累计被限流的 PELT 时间 */
	u64			throttled_clock_self;		/* 本层自己限流的时钟 */
	u64			throttled_clock_self_time;	/* 本层累计限流时间 */
	bool			throttled:1;			/* 当前被限流 */
	/* 此处省略本组未记录的成员 */
	struct list_head	throttled_list;		/* 带宽对象上的限流链 */
	struct list_head	throttled_csd_list;	/* 跨 CPU 解限流的 CSD 链 */
	struct list_head	throttled_limbo_list;	/* 限流 limbo 链 */
# endif
#endif
```

**直觉**：每个 CPU 上至少有一个根 `rq->cfs`；开了组调度后，每个 `task_group` 在每个 CPU 还有自己的 `cfs_rq`。

---

## 2.4 `struct rq`（每 CPU 运行队列）

`kernel/sched/sched.h:1158`（大结构，按组给出带注释的源码片段）

`clock*`、`numa_*` 按源码展开。`nr_numa_running` 和 `nr_preferred_running` 跟 `nr_running` 挤在同一条热 cache line；`numa_migrate_on` 在后面，靠近 `leaf_cfs_rq_list`。

```c
/* --- 热计数 --- */
	unsigned int		nr_running;		/* 可跑任务数 */
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		nr_numa_running;	/* 本 rq 上的 NUMA 任务数 */
	unsigned int		nr_preferred_running;	/* 其中跑在首选节点的数量 */
#endif
	unsigned int		ttwu_pending;		/* 挂起的唤醒 */
	unsigned long		cpu_capacity;		/* 容量 */
```

```c
/* --- 当前 --- */
#ifdef CONFIG_SCHED_PROXY_EXEC
	struct task_struct __rcu	*donor;		/* 调度上下文；proxy-exec 时 donor≠curr */
	struct task_struct __rcu	*curr;		/* 执行上下文 */
#else
	union {
		struct task_struct __rcu *donor;	/* 与 curr 叠成同一个指针 */
		struct task_struct __rcu *curr;		/* 正在执行者 */
	};
#endif
	struct task_struct		*idle;		/* 本 CPU idle */
	/* 此处省略本组未记录的成员 */
	struct task_struct		*stop;		/* stop 类任务 */
```

```c
/* --- 锁与切换 --- */
	u64			nr_switches	____cacheline_aligned; /* 切换计数 */
	raw_spinlock_t		__lock;			/* rq 自旋锁 */
```

```c
/* --- 子队列 --- */
	struct cfs_rq		cfs;			/* fair 队列 */
	struct rt_rq		rt;			/* RT 队列 */
	struct dl_rq		dl;			/* deadline 队列 */
#ifdef CONFIG_SCHED_CLASS_EXT
	struct scx_rq		scx;			/* sched_ext 队列 */
#endif
```

```c
/* --- 类提示 --- */
	const struct sched_class *next_class;		/* 下一次优先考虑的 sched_class */
```

```c
/* --- 其它 --- */
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct list_head	leaf_cfs_rq_list;	/* 本 CPU 叶子 cfs_rq，供负载均衡 */
	/* 此处省略本组未记录的成员 */
#endif
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		numa_migrate_on;	/* 本 rq 是否做 NUMA 迁移 */
#endif
	/* 此处省略本组未记录的成员 */
	u64			clock_task ____cacheline_aligned; /* 任务时钟（rq_clock_task） */
	u64			clock_pelt;		/* PELT 用的时钟 */
	u64			clock;			/* rq 时钟（rq_clock） */
	/* 此处省略本组未记录的成员 */
	unsigned int		clock_update_flags;	/* RQCF_*：本锁段是否已更新时钟 */
	u64			clock_pelt_idle;	/* idle 区间的 PELT 时钟 */
	u64			clock_idle;		/* idle 区间的 rq 时钟 */
#ifndef CONFIG_64BIT
	u64			clock_pelt_idle_copy;	/* 32 位：clock_pelt_idle 副本 */
	u64			clock_idle_copy;	/* 32 位：clock_idle 副本 */
#endif
```

锁约定：改队列、选人，通常持 **`rq->__lock`**（`raw_spinlock_t`）。唤醒路径还涉及 `p->pi_lock`。见 [locks-and-ds](../locks-and-ds/)。

---

## 2.5 `struct sched_class`

`kernel/sched/sched.h:2623` —— **函数表**，不是「类对象里塞状态」。下面只列笔记点名的回调，签名按源码，顺序也按源码。这些回调在结构体里彼此相邻。

```c
	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags); /* 任务入该类队列 */
	bool (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags); /* 任务出该类队列 */
	void (*yield_task)   (struct rq *rq);			/* 主动让出 */
	bool (*yield_to_task)(struct rq *rq, struct task_struct *p); /* 让给指定任务 */
	void (*wakeup_preempt)(struct rq *rq, struct task_struct *p, int flags); /* 唤醒后是否抢占当前 */
	int (*balance)(struct rq *rq, struct rq_flags *rf);	/* 选人前的均衡机会 */
	struct task_struct *(*pick_task)(struct rq *rq, struct rq_flags *rf); /* 选出下一个要跑的 task */
	void (*put_prev_task)(struct rq *rq, struct task_struct *p, struct task_struct *next); /* 切换前簿记 */
	void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first); /* 切换后簿记 */
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags); /* 选 CPU */
	void (*migrate_task_rq)(struct task_struct *p, int new_cpu); /* 迁移后处理 */
	void (*task_woken)(struct rq *this_rq, struct task_struct *task); /* 唤醒后钩子 */
	void (*set_cpus_allowed)(struct task_struct *p, struct affinity_context *ctx); /* 亲和性变更 */
```

fair 的实例：`DEFINE_SCHED_CLASS(fair)` 在 `fair.c:15855`。

---

## 2.6 `struct task_group`

`kernel/sched/sched.h:478` —— **cpu 控制器在调度器里的实体**。整个结构体包在 `CONFIG_CGROUP_SCHED` 里。`cfs_bandwidth` 的定义在 `kernel/sched/sched.h:451`，这里只出现嵌入的那一个成员。

```c
#ifdef CONFIG_CGROUP_SCHED
	struct cgroup_subsys_state css;	/* 嵌入 css；container_of 找回 tg */
#ifdef CONFIG_GROUP_SCHED_WEIGHT
	/* 此处省略本组未记录的成员 */
#endif
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct cfs_rq __percpu	*cfs_rq;	/* 每 CPU 一组 fair 队列 */
	unsigned long		shares;		/* 权重份额（对接 cpu.weight） */
	/*
	 * 组级 load 平均。tick 上争用，和上面的字段分 cache line。
	 */
	atomic_long_t		load_avg ____cacheline_aligned;
	atomic_long_t		runnable_avg;	/* 组级 runnable 平均 */
#endif
#ifdef CONFIG_RT_GROUP_SCHED
	/* 此处省略本组未记录的成员 */
#endif
	struct scx_task_group	scx;		/* sched_ext 侧组状态 */
	/* 此处省略本组未记录的成员 */
	struct task_group	*parent;	/* 父组 */
	struct list_head	siblings;	/* 兄弟链表 */
	struct list_head	children;	/* 子组链表 */
#ifdef CONFIG_SCHED_AUTOGROUP
	/* 此处省略本组未记录的成员 */
#endif
	struct cfs_bandwidth	cfs_bandwidth;	/* quota/period 带宽 */
#ifdef CONFIG_UCLAMP_TASK_GROUP
	unsigned int		uclamp_pct[UCLAMP_CNT];	/* 用户请求的夹具百分比 */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];	/* 请求的利用率夹具 */
	struct uclamp_se	uclamp[UCLAMP_CNT];	/* 组上生效的利用率夹具 */
#endif
#endif
```

回链：[cgroup/05-cpu-cpuset.md](../cgroup/05-cpu-cpuset.md)、本章 [07](./07-cgroup-cpu.md)。

---

## 2.7 `task_struct` 上的调度字段

`include/linux/sched.h`（约 `:885`–`:938`）

```c
	int				static_prio;	/* nice 静态优先级 */
	int				normal_prio;	/* 正常优先级 */
	unsigned int			rt_priority;	/* RT 优先级 */

	struct sched_entity		se;		/* fair 调度实体 */
	struct sched_rt_entity		rt;		/* RT 调度实体 */
	struct sched_dl_entity		dl;		/* deadline 调度实体 */
	/* 此处省略本组未记录的成员 */
	const struct sched_class	*sched_class;	/* 当前所属调度类 */
#ifdef CONFIG_SCHED_CORE
	/* 此处省略本组未记录的成员 */
#endif
#ifdef CONFIG_CGROUP_SCHED
	struct task_group		*sched_task_group; /* 所属调度组 */
#endif
	/* 此处省略本组未记录的成员 */
	unsigned int			policy;		/* SCHED_* 策略 */
```

策略切换会改 `sched_class` 指针（见 `core.c` 里 `SCHED_EXT`/`FIFO`/`NORMAL` 分支，约 `:4873`）。

---

## 2.8 关键函数 / 宏

| 符号 | 位置 | 作用 |
|---|---|---|
| `cpu_rq(cpu)` / `this_rq()` | sched.h | 取 per-CPU rq |
| `task_rq(p)` | sched.h | 任务当前 rq |
| `cfs_rq_of(se)` | fair 侧 | 实体所在 cfs_rq |
| `task_of(se)` | fair 侧 | 叶子实体 → task |
| `for_each_sched_entity(se)` | 组调度 | 沿 parent 向上 |
| `css_tg(css)` | sched.h:551 | css → task_group |

---

**导航**：[← 总览](./01-overview.md) · [章节目录](./README.md) · [EEVDF →](./03-eevdf.md)
