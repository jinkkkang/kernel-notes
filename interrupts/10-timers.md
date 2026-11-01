# 10. 定时器

**结论**：低精度定时器进时间轮，由 `TIMER_SOFTIRQ` 到期执行；高精度定时器是 per-CPU 的红黑树，硬到期在 clockevent 的中断里跑，软到期进 `HRTIMER_SOFTIRQ`。x86-64 上把 clockevent 编程进硬件的是本地 APIC 定时器，能用 TSC-deadline 时名字变成 `lapic-deadline`。

> 时间轮：`kernel/time/timer.c`。hrtimer：`kernel/time/hrtimer.c`。tick：`kernel/time/tick-sched.c`。LAPIC：`arch/x86/kernel/apic/apic.c`。

---

## 10.1 时间轮

每个 `timer_list` 挂进某一层的某个桶。桶是哈希表，不是按到期时间排好的链表，所以插入是 O(1)，到期时再把这一桶扫一遍。

```c
/* include/linux/timer.h:11 */
struct timer_list {
	/* 运行中会改的字段放在同一条 cache line */
	struct hlist_node	entry;       /* 挂在 timer_base.vectors[桶] 上 */
	unsigned long		expires;     /* 到期的 jiffies */
	void			(*function)(struct timer_list *);
	u32			flags;
#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lockdep_map;
#endif
};
```

轮子的尺寸全是宏（`timer.c:167` 起）：

```c
#define LVL_BITS	6
#define LVL_SIZE	(1UL << LVL_BITS)          /* 每层 64 个桶 */
#if HZ > 100
# define LVL_DEPTH	9
#else
# define LVL_DEPTH	8
#endif
#define WHEEL_SIZE	(LVL_SIZE * LVL_DEPTH)     /* HZ>100 时 576 个桶 */
```

层越深，一个桶代表的时间越长。最近的到期放在第 0 层，远的放在高层。`calc_index` 在入队时选层（调用点在 `:553` 一带）。

```c
/* kernel/time/timer.c:199 */
struct timer_base {
	raw_spinlock_t		lock;
	struct timer_list	*running_timer;   /* 正在执行的那一个 */
#ifdef CONFIG_PREEMPT_RT
	spinlock_t		expiry_lock;
	atomic_t		timer_waiters;
#endif
	unsigned long		clk;              /* 这个 base 已经处理到的 jiffies */
	unsigned long		next_expiry;
	unsigned int		cpu;
	bool			next_expiry_recalc;
	bool			is_idle;
	bool			timers_pending;
	DECLARE_BITMAP(pending_map, WHEEL_SIZE); /* 哪些桶非空 */
	struct hlist_head	vectors[WHEEL_SIZE];
} ____cacheline_aligned;
```

`NR_BASES`（`:186` 起）：开了 `CONFIG_NO_HZ_COMMON` 时是 2，`BASE_STD` 与 `BASE_DEF`（可推迟的 `TIMER_DEFERRABLE`）分开；否则是 1。每 CPU 一份 `timer_bases[NR_BASES]`（`:216`）。

到期路径：`TIMER_SOFTIRQ` → `run_timer_softirq`（`:2032`）→ 按 `clk` 收集这一拍到期的桶（`collect_expired_timers` 在 `:1768` 一带）→ 调用 `function`。`function` 在 softirq 里跑，不能睡眠。

`timer_migration` 是 sysctl，不是模块参数：`kernel/timer_migration`（`timer.c:255`），默认 `sysctl_timer_migration = 1`（`:227`）。它只在 `CONFIG_NO_HZ_COMMON` 且 `CONFIG_SMP` 下存在，用来把空闲 CPU 上的定时器挪走。

---

## 10.2 hrtimer

```c
/* include/linux/hrtimer.h:118 */
struct hrtimer {
	struct timerqueue_node		node;          /* 按到期时间放进红黑树 */
	ktime_t				_softexpires;  /* 软到期；硬到期可以再晚一点以便合并 */
	enum hrtimer_restart		(*function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;
	u8				state;
	u8				is_rel;
	u8				is_soft;       /* 在 HRTIMER_SOFTIRQ 里到期 */
	u8				is_hard;       /* 即使 RT 也走硬中断 */
};
```

`function` 返回 `HRTIMER_RESTART` 或 `HRTIMER_NORESTART`。

两条到期路：

| 路 | 函数 | 行号 | 上下文 |
|---|---|---|---|
| 硬 | `hrtimer_interrupt` | `hrtimer.c:1850` | clockevent 的中断处理函数 |
| 硬队列 | `__hrtimer_run_queues(..., HRTIMER_ACTIVE_HARD)` | `:1880` | 上面那个中断里 |
| 软 | `hrtimer_run_softirq` | `:1825` | `HRTIMER_SOFTIRQ` |
| 注册 | `open_softirq(HRTIMER_SOFTIRQ, hrtimer_run_softirq)` | `:2312` | 启动 |

`hrtimer_interrupt` 由每 CPU 的 clockevent 设备在到期时调用。它跑完硬定时器后，若还有软定时器，会把 `HRTIMER_SOFTIRQ` 拉起来。

---

## 10.3 clockevent

clockevent 是「能在未来某个时刻打断这颗 CPU」的设备。定时器子系统不直接写 APIC 寄存器，而是调这张函数表。

```c
/* include/linux/clockchips.h:100
 * 只列出和编程有关的前半；后面是状态回调。
 */
struct clock_event_device {
	void			(*event_handler)(struct clock_event_device *);
	int			(*set_next_event)(unsigned long evt, struct clock_event_device *);
	int			(*set_next_ktime)(ktime_t expires, struct clock_event_device *);
	ktime_t			next_event;
	u64			max_delta_ns;
	u64			min_delta_ns;
	u32			mult;
	u32			shift;
	enum clock_event_state	state_use_accessors;
	unsigned int		features;
	unsigned long		retries;
	int			(*set_state_periodic)(struct clock_event_device *);
	int			(*set_state_oneshot)(struct clock_event_device *);
	int			(*set_state_oneshot_stopped)(struct clock_event_device *);
	int			(*set_state_shutdown)(struct clock_event_device *);
	int			(*tick_resume)(struct clock_event_device *);
	void			(*broadcast)(const struct cpumask *mask);
	void			(*suspend)(struct clock_event_device *);
	void			(*resume)(struct clock_event_device *);
	unsigned long		min_delta_ticks;
	unsigned long		max_delta_ticks;
	const char		*name;
	int			rating;
	int			irq;
	int			bound_on;
	const struct cpumask	*cpumask;
	struct list_head	list;
	struct module		*owner;
} ____cacheline_aligned;
```

`event_handler` 是到期时要调用的函数。周期性 tick 和 oneshot 的 NO_HZ 在这里分叉。

---

## 10.4 本地 APIC 定时器与 TSC-deadline

```c
/* arch/x86/kernel/apic/apic.c:527 */
static struct clock_event_device lapic_clockevent = {
	.name				= "lapic",
	.features			= CLOCK_EVT_FEAT_PERIODIC |
					  CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_C3STOP
					  | CLOCK_EVT_FEAT_DUMMY,
	.shift				= 32,
	.set_state_shutdown		= lapic_timer_shutdown,       /* :465 */
	.set_state_periodic		= lapic_timer_set_periodic,   /* :503 */
	.set_state_oneshot		= lapic_timer_set_oneshot,    /* :508 */
	.set_next_event			= lapic_next_event,           /* :445 */
	.broadcast			= lapic_timer_broadcast,
	.rating				= 100,
	.irq				= -1,
};
```

`setup_APIC_timer`（`:604`）把这份模板拷到每 CPU 的 `lapic_events`。若 TSC-deadline 可用，`set_next_event` 换成 `lapic_next_deadline`（`:452`），名字改成 `"lapic-deadline"`（`:618`），rating 提到 150（`:611`）。deadline 模式写的是 TSC 时间戳，不是 APIC 当前计数的一个减法。

中断从 `LOCAL_TIMER_VECTOR`（`0xec`）进来，`local_apic_timer_interrupt`（`:1049`）取出本 CPU 的 `clock_event_device` 并调用它的 `event_handler`。没有初始化完就收到中断时，函数会把这次丢掉（注释在 `:1053`，kdump 留下的旧定时器）。

校准在 `calibrate_APIC_clock`（`:822`）。和 PM-Timer 对不上时打印 `APIC calibration not consistent with PM-Timer`（`:741`）。

---

## 10.5 tick 与 NO_HZ

周期 tick 是「每 HZ 一次」的 clockevent。NO_HZ 在 CPU 空闲或整个 CPU 只有一个任务时把下一次 tick 推远，回来时再补 jiffies。

| 函数 | 行号 | 作用 |
|---|---|---|
| `tick_nohz_handler` | `tick-sched.c:1369` | 没有高分辨率定时器时的 oneshot tick 处理函数 |
| `tick_sched_timer` | `:1478` | `CONFIG_HIGH_RES_TIMERS` 下用 hrtimer 模拟 tick |
| `tick_setup_sched_timer` | `:1518` | `hrtimer_init(..., HRTIMER_MODE_ABS_HARD)` 后把 `function` 设成 `tick_sched_timer` |
| `tick_nohz_stop_tick` | `:884` | 空闲时停掉下一次 tick |
| `tick_nohz_irq_exit` | `:1185` | 中断退出时看要不要重新进入 NO_HZ |
| `tick_irq_enter` | 被 `irq_enter_rcu` 调用 | 见 [02](./02-x86-entry.md) |

`tick_sched_timer` 用 `container_of` 从 `hrtimer` 找外层的 `tick_sched`（`:1480`）。它在硬中断里更新 jiffies、跑时间轮需要的记账，并可能给当前任务标上需要调度。

`skew_tick` 命令行（`early_param`，`:1513`）把每 CPU 的 tick 相位错开，减轻 `jiffies_lock` 在同一拍上的争用（`:1532`）。

NO_HZ 停 tick 之后，下一次硬中断（设备或 IPI）会经 `irq_enter_rcu` → `tick_irq_enter` 把 jiffies 补上。所以「完全没有中断的 CPU」上的时间不会自己走，要靠别的 CPU 的广播定时器。广播设备的回调在 `clock_event_device.broadcast`。

---

## 10.6 和 softirq 的关系

```text
LAPIC 定时器中断 (LOCAL_TIMER_VECTOR)
  -> local_apic_timer_interrupt
  -> event_handler：tick_nohz_handler 或 hrtimer_interrupt
       -> 硬 hrtimer
       -> 可能 raise HRTIMER_SOFTIRQ
       -> 更新 jiffies 后，TIMER_SOFTIRQ 里 run_timer_softirq
```

`TIMER_SOFTIRQ` 跑 `timer_list`。`HRTIMER_SOFTIRQ` 跑标了软到期的 `hrtimer`。两者都在 [08](./08-softirq-tasklet.md) 的十条里面，优先级分别是 1 和 8。

---

**导航**：[← 推迟执行](./09-deferred-work-choice.md) · [目录](./README.md) · [KVM →](./11-kvm-irq-virt.md)
