# 10. 入队与标志

**结论**：`alloc_workqueue` 造出 `workqueue_struct` 并挂上 pwq；`queue_work_on` 在关中断下把 `PENDING` 置位后调用 `__queue_work`；没超过 `max_active` 就进 `pool->worklist` 并 `kick_pool`，否则进 `inactive_works`。`schedule_work` 固定使用 `system_percpu_wq`。

> 标志在 `include/linux/workqueue.h:364`。`schedule_work` 在 `:626`。`alloc_workqueue` 定义在 `kernel/workqueue.c:4755`，`__queue_work` 在 `:1726`，`queue_work_on` 在 `:1849`，`process_one_work` 在 `:2560`。

---

## 10.1 `WQ_*`

```c
/* include/linux/workqueue.h:364 */
enum {
	WQ_UNBOUND		= 1 << 1, /* 不绑在某一个 CPU 的 worker 上 */
	WQ_FREEZABLE		= 1 << 2, /* 挂起时可以冻住 */
	WQ_MEM_RECLAIM		= 1 << 3, /* 内存回收路径会用，因此有 rescuer */
	WQ_HIGHPRI		= 1 << 4, /* 高优先级池 */
	WQ_CPU_INTENSIVE	= 1 << 5, /* 不参加并发管理 */
	WQ_SYSFS		= 1 << 6, /* 在 sysfs 里露出一个 device */
	WQ_POWER_EFFICIENT	= 1 << 7, /* workqueue.power_efficient 打开时变成 unbound */

	__WQ_DESTROYING		= 1 << 15, /* destroy_workqueue 进行中 */
	__WQ_DRAINING		= 1 << 16, /* drain_workqueue 进行中 */
	__WQ_ORDERED		= 1 << 17,
	__WQ_LEGACY		= 1 << 18, /* create_*_workqueue() */
	__WQ_ORDERED_EXPLICIT	= 1 << 19, /* alloc_ordered_workqueue() */

	WQ_MAX_ACTIVE		= 512,
	WQ_UNBOUND_MAX_ACTIVE	= WQ_MAX_ACTIVE,
	WQ_DFL_ACTIVE		= WQ_MAX_ACTIVE / 2,   /* 256 */
};
```

`max_active` 传 0 时 `alloc_workqueue` 换成 `WQ_DFL_ACTIVE`（`workqueue.c:4791`）。`wq_clamp_max_active`（`:4708`）把小于 1 或大于 `WQ_MAX_ACTIVE` 的值夹到 `[1, 512]`，并 `pr_warn`。

`WQ_UNBOUND` 且 `max_active == 1` 时，`alloc_workqueue` 再或上 `__WQ_ORDERED`（`:4769`），用来保住旧的「单线程」语义。显式有序队列用 `alloc_ordered_workqueue`（`workqueue.h:478`），它额外或上 `__WQ_ORDERED_EXPLICIT` 并把 `max_active` 写成 1。带 `EXPLICIT` 的队列不能注册 sysfs（`workqueue_sysfs_register` 在 `:6324` `WARN_ON` 并返回 `-EINVAL`）。

`WQ_POWER_EFFICIENT` 且模块参数 `power_efficient` 为真时，创建路径或上 `WQ_UNBOUND`（`:4773`）。变量在 `workqueue.c:367`，`module_param_named(power_efficient, ...)` 在 `:368`，perm `0444`。

`WQ_MEM_RECLAIM` 让 `init_rescuer`（`:4722`）创建名为 `kworker/R-<wq名>` 的线程（`:4738`）。没有这个标志的队列在内存回收里 `flush` 另一条也没有这个标志的队列，会碰到 `check_flush_dependency`（`:3004`）的警告。rescuer 什么时候被叫醒见 [12](./12-perf-analysis.md)。

`WQ_CPU_INTENSIVE` 的工作项在 `process_one_work` 里给 worker 设 `WORKER_CPU_INTENSIVE`（`:2608`），并发管理不再把它算进 `nr_running`。跑得比 `cpu_intensive_thresh_us` 还久的 per-cpu 工作项也会被自动算进这一类（`:357` 的注释）。阈值默认先按 10ms，BogoMIPS 低于 4000 时放大，上限 1 秒（`wq_cpu_intensive_thresh_init`，`:6751`）。

---

## 10.2 系统队列

`workqueue_init_early`（`:6645`）里一次分配：

| 符号 | 行 | 名字参数 | 标志 | max_active |
|------|----|----------|------|------------|
| `system_wq` | 6705 | `"events"` | 0 | 0 → 256 |
| `system_percpu_wq` | 6706 | `"events"` | 0 | 0 → 256 |
| `system_highpri_wq` | 6707 | `"events_highpri"` | `WQ_HIGHPRI` | 0 |
| `system_long_wq` | 6708 | `"events_long"` | 0 | 0 |
| `system_unbound_wq` | 6709 | `"events_unbound"` | `WQ_UNBOUND` | `WQ_MAX_ACTIVE` |
| `system_dfl_wq` | 6710 | `"events_unbound"` | `WQ_UNBOUND` | `WQ_MAX_ACTIVE` |
| `system_freezable_wq` | 6711 | `"events_freezable"` | `WQ_FREEZABLE` | 0 |
| `system_power_efficient_wq` | 6713 | `"events_power_efficient"` | `WQ_POWER_EFFICIENT` | 0 |
| `system_freezable_power_efficient_wq` | 6715 | `"events_freezable_power_efficient"` | `WQ_FREEZABLE \| WQ_POWER_EFFICIENT` | 0 |

头文件注释（`workqueue.h:413`）写 `system_percpu_wq` 是 `schedule[_delayed]_work[_on]()` 用的那条。`system_wq` 的声明（`:438`）写着 `use system_percpu_wq, this will be removed`。两条都叫 `"events"`，是两次 `alloc_workqueue`，不是同一个指针。

```c
/* include/linux/workqueue.h:626 */
static inline bool schedule_work(struct work_struct *work)
{
	return queue_work(system_percpu_wq, work);
}
```

`queue_work`（`:564`）再调用 `queue_work_on(WORK_CPU_UNBOUND, wq, work)`。`WORK_CPU_UNBOUND` 是 `NR_CPUS`（`workqueue.h:57`）。`schedule_work_on`（`:607`）同样把队列写成 `system_percpu_wq`，只是 CPU 由调用者指定。

`flush_scheduled_work`（`:642`）会先 `__warn_flushing_systemwide_wq()`，再 `__flush_workqueue(system_percpu_wq)`。注释要求不要再 flush 这些全局队列。

---

## 10.3 `__queue_work`

```c
/* kernel/workqueue.c:1849 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	local_irq_save(flags);
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}
	local_irq_restore(flags);
	return ret;
}
```

关中断是因为 `PENDING` 已经置上、工作项还没挂上链表的这段窗口里，别的任务会忙等（`__queue_work` 上方注释 `:1735`）。抢不到 `PENDING` 就返回 false，工作项留在原来的位置。

`__queue_work`（`:1726`）接着：

1. 队列正在 `__WQ_DESTROYING` 或 `__WQ_DRAINING`，并且调用者不是这条队列上正在执行的链式工作，就 `WARN_ON_ONCE` 并返回（`:1748`）。
2. `cpu == WORK_CPU_UNBOUND` 时，unbound 队列用 `wq_select_unbound_cpu(raw_smp_processor_id())`（`:1756`），其它队列用 `raw_smp_processor_id()`（`:1758`）。`raw_smp_processor_id` 在 `CONFIG_SMP` 下由 `include/linux/smp.h:113` 包含的 `asm/smp.h` 提供；x86 是 `this_cpu_read(pcpu_hot.cpu_number)`（`arch/x86/include/asm/smp.h:146`）。非 SMP 的定义是常量 0（`include/linux/smp.h:198`）。
3. `pwq = rcu_dereference(*per_cpu_ptr(wq->cpu_pwq, cpu))`（`:1761`）。
4. 若这个 work 上次在另一个 pool 上、而且那个 worker 还在执行它，就改排到那个 pwq，保证同一个 `work_struct` 不会两处同时跑（`:1769`）。
5. `trace_workqueue_queue_work`（`:1809`）。
6. `pwq->nr_active < pwq->max_active` 时 `insert_work` 到 `pool->worklist` 并 `kick_pool`（`:1817`）。否则或上 `WORK_STRUCT_INACTIVE`，插到 `pwq->inactive_works`（`:1826`）。

`insert_work`（`:1668`）`set_work_pwq`、`list_add_tail`、`get_pwq`。`kick_pool` 在需要时唤醒一个空闲 worker。

unbound 的 CPU 选择：本 CPU 在 `wq_unbound_cpumask` 里就用本 CPU（`:1708`）。否则从 `wq_rr_cpu_last` 往后轮转（`:1714`）。`CONFIG_DEBUG_WQ_FORCE_RR_CPU` 会强迫轮转并 `pr_warn_once`（`:1710`）。

---

## 10.4 `process_one_work`

```c
/* kernel/workqueue.c:2560 */
static void process_one_work(struct worker *worker, struct work_struct *work)
{
	pwq = get_work_pwq(work);
	pool = worker->pool;

	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     raw_smp_processor_id() != pool->cpu);

	hash_add(pool->busy_hash, &worker->hentry, (unsigned long)work);
	worker->current_work = work;
	worker->current_func = work->func;
	worker->current_pwq = pwq;
	strscpy(worker->desc, pwq->wq->name, WORKER_DESC_LEN);
	list_del_init(&work->entry);

	if (unlikely(pwq->wq->flags & WQ_CPU_INTENSIVE))
		worker_set_flags(worker, WORKER_CPU_INTENSIVE);

	kick_pool(pool);
	set_work_pool_and_clear_pending(work, pool->id);  /* 清 PENDING */
	pwq->stats[PWQ_STAT_STARTED]++;
	raw_spin_unlock_irq(&pool->lock);

	trace_workqueue_execute_start(work);             /* :2656 */
	worker->current_func(work);                      /* :2657 */
	trace_workqueue_execute_end(work, worker->current_func);
	pwq->stats[PWQ_STAT_COMPLETED]++;
	/* 回到 pool->lock，检查原子计数、lockdep、RCU 深度有没有漏 */
}
```

`work->func` 在进程上下文运行，可以睡觉。函数返回后不能再碰调用者可能已经释放的 `work`；tracepoint 只记录地址（注释 `:2659`）。若 `in_atomic()` 或 lockdep / RCU 深度和进入时不同，`pr_err("BUG: workqueue leaked atomic, lock or RCU: ...")`（`:2670`）并 `dump_stack`。

清掉 `PENDING` 发生在调用 `func` 之前（`:2625`）。所以 `func` 里面可以再次 `queue_work` 同一个对象，那一次会成功挂到下次执行。

---

## 10.5 `alloc_workqueue` 与 `destroy_workqueue`

```c
/* kernel/workqueue.c:4755 */
struct workqueue_struct *alloc_workqueue(const char *fmt,
					 unsigned int flags,
					 int max_active, ...)
{
	/* UNBOUND && max_active==1 → 或上 __WQ_ORDERED */
	/* POWER_EFFICIENT && wq_power_efficient → 或上 WQ_UNBOUND */
	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	max_active = max_active ?: WQ_DFL_ACTIVE;
	max_active = wq_clamp_max_active(max_active, flags, wq->name);
	wq->flags = flags;
	wq->saved_max_active = max_active;
	alloc_and_link_pwqs(wq);
	if (wq_online && init_rescuer(wq) < 0)
		goto err_destroy;
	if ((wq->flags & WQ_SYSFS) && workqueue_sysfs_register(wq))
		goto err_destroy;
	/* 挂上全局 workqueues 链表 */
}
```

`wq_online` 在 `workqueue_init` 末尾才变成 true（`:6812`）。更早的 `alloc_workqueue`（包括 `workqueue_init_early` 里的系统队列）不会在这里建 rescuer；`workqueue_init` 会再扫一遍链表补上（`:6793`）。

```c
/* kernel/workqueue.c:4869 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	workqueue_sysfs_unregister(wq);
	wq->flags |= __WQ_DESTROYING;
	drain_workqueue(wq);          /* :4886 先把已经排队的做完 */
	/* 有 rescuer 就 kthread_stop */
	/* 检查每个 pwq 不再 busy，然后拆掉 */
}
```

注释（`:4867`）写：当前还挂着的工作会先做完。`drain_workqueue` 比单次 flush 更进一步，它挡住队列外面新来的非链式工作（`__queue_work` 看到 `__WQ_DRAINING`）。demo 01 的 exit 靠这个顺序：`destroy_workqueue` 返回之后，init 里排队的那个函数已经跑完。

---

## 10.6 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `include/linux/workqueue.h` | `WQ_*` | 364 | 对外和内部标志 |
| `include/linux/workqueue.h` | `schedule_work` | 626 | `queue_work(system_percpu_wq, work)` |
| `include/linux/workqueue.h` | `queue_work` | 564 | CPU 参数用 `WORK_CPU_UNBOUND` |
| `kernel/workqueue.c` | `alloc_workqueue` | 4755 | 分配、链接 pwq、可选 sysfs 和 rescuer |
| `kernel/workqueue.c` | `destroy_workqueue` | 4869 | 先 `drain_workqueue` |
| `kernel/workqueue.c` | `queue_work_on` | 1849 | 关中断，抢 `PENDING` |
| `kernel/workqueue.c` | `__queue_work` | 1726 | 选 pwq，插入 worklist 或 inactive |
| `kernel/workqueue.c` | `insert_work` | 1668 | 把 work 链上并 `get_pwq` |
| `kernel/workqueue.c` | `process_one_work` | 2560 | 清 `PENDING`，调用 `func` |
| `kernel/workqueue.c` | `init_rescuer` | 4722 | 仅 `WQ_MEM_RECLAIM` |
| `kernel/workqueue.c` | `workqueue_init_early` | 6645 | 分配上表那些系统队列 |

---

**导航**：[← cmwq 对象](./09-cmwq-objects.md) · [目录](./README.md) · [观测 →](./11-observability.md)
