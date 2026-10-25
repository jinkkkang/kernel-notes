# 8. 上下文切换（进程管理视角）

**结论**：本章只关心「什么时候进调度器，切换时 mm 和寄存器怎么换」。**下一个是谁**由 `pick_next_task` 问各个调度类。6.6 的公平类是 EEVDF，实体是 `sched_entity`（`vruntime`、`vlag`、`deadline`、`slice` 都在 `sched.h:551` 起）。选人算法留给调度那一章。

## 8.1 谁调用 `__schedule`

`__schedule(unsigned int sched_mode)` 在 `kernel/sched/core.c:6600`，是 `static` 的。常见入口：

| 场景 | 路径 |
|---|---|
| 主动阻塞 | `schedule()`（`:6789`）。`wait_event`、锁睡眠、阻塞 IO 都走到它 |
| 抢占 | 返回用户态，或 `preempt_enable` 时发现需要重调度 → `preempt_schedule`（`:6905`）/ `preempt_schedule_irq`（`:7020`）→ `__schedule` |
| 没有抢占配置时 | `__cond_resched`（`:8609`），或系统调用 / 中断返回用户态时再看 |
| `sched_yield` | `sys_sched_yield`（`:8602`）→ `do_sched_yield`（`:8577`）→ `schedule` |
| 退出 | `do_exit` 末尾 `do_task_dead`（`:6730`）再进 `__schedule`，不再返回 |

`__schedule` 里选出 `next` 的函数是 `pick_next_task`（`:6081`）。它会问 `fair_sched_class`、实时类、deadline 类、idle 类。本章停在它被调用之前。

## 8.2 `context_switch`（`core.c:5337`）

```c
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	prepare_task_switch(rq, prev, next);          /* :5192 */
	arch_start_context_switch(prev);

	/*
	 * 内核 → 内核：lazy TLB，把 active_mm 交过去
	 * 用户 → 内核：lazy TLB，mmgrab_lazy_tlb
	 * 内核 → 用户：switch_mm，稍后 mmdrop_lazy_tlb
	 * 用户 → 用户：switch_mm
	 */
	if (!next->mm) {                              /* 切入内核线程 */
		enter_lazy_tlb(prev->active_mm, next);
		next->active_mm = prev->active_mm;    /* 借用，不换页表 */
		if (prev->mm)                         /* 上一个是用户任务 */
			mmgrab_lazy_tlb(prev->active_mm);
		else
			prev->active_mm = NULL;       /* 上一个也是内核线程，引用已经转走 */
	} else {                                      /* 切入用户任务 */
		membarrier_switch_mm(rq, prev->active_mm, next->mm);
		switch_mm_irqs_off(prev->active_mm, next->mm, next);
		lru_gen_use_mm(next->mm);

		if (!prev->mm) {                      /* 上一个是内核线程 */
			rq->prev_mm = prev->active_mm; /* finish_task_switch 里再 mmdrop_lazy_tlb */
			prev->active_mm = NULL;
		}
	}

	switch_mm_cid(rq, prev, next);
	prepare_lock_switch(rq, next, rf);

	/* 真正换栈和寄存器。这行之后的代码在 next 的上下文里「返回」 */
	switch_to(prev, next, prev);
	barrier();
	return finish_task_switch(prev);              /* :5224 */
}
```

四种 mm 组合，源码注释（`:5350`）就是上面那四行：

| prev → next | mm 动作 |
|---|---|
| 内核 → 内核 | lazy TLB，把 `active_mm` 交过去 |
| 用户 → 内核 | lazy TLB，并对借来的 mm `mmgrab_lazy_tlb` |
| 内核 → 用户 | `switch_mm_irqs_off`，旧的 `active_mm` 记在 `rq->prev_mm`，稍后放下 |
| 用户 → 用户 | `switch_mm_irqs_off`。两个任务的 mm 相同（同一进程的两个线程）时，架构代码会少做一次页表切换 |

内核线程的 `mm` 是 NULL，所以切到它不会换页表，只是继续用上一个用户 mm。这就是 `active_mm` 和 `mm` 要分成两个字段的原因。

## 8.3 本章停在入口

- `pick_next_task` → 各 `sched_class`：不在本章展开。
- 公平类看 `sched_entity` 的 `vruntime` / `vlag` / `deadline` / `slice`。运行队列结构的名字仍是 `cfs_rq`。
- 实时任务走 `sched_rt_entity`，deadline 任务走 `sched_dl_entity`。这两个实体都嵌在 `task_struct` 里，和第 2 章对得上。

## 8.4 关键函数

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `kernel/sched/core.c` | `__schedule` | 6600 | 调度主体 |
| | `schedule` | 6789 | 主动让出 |
| | `do_task_dead` | 6730 | 退出任务的最后一次调度 |
| | `preempt_schedule` | 6905 | 内核抢占 |
| | `preempt_schedule_irq` | 7020 | 中断返回路径上的抢占 |
| | `do_sched_yield` / `sys_sched_yield` | 8577 / 8602 | yield |
| | `pick_next_task` | 6081 | 问调度类要下一个任务 |
| | `context_switch` | 5337 | 换 mm 和寄存器 |
| | `prepare_task_switch` | 5192 | 切换前的簿记 |
| | `finish_task_switch` | 5224 | 切换后放下 prev，包括 lazy TLB 的 `mmdrop` |
| `arch/x86/include/asm/switch_to.h` | `switch_to` | 49 | 换栈和被调用者保存寄存器 |
| `arch/x86/include/asm/mmu_context.h` | `switch_mm_irqs_off` | 172 | 换页表 |
| | `enter_lazy_tlb` | 134 | 标记「页表先留着」 |

---

**导航**：[章节目录](./README.md) · [← 退出、僵尸与回收](./07-exit-reap.md) · [信号与进程 →](./09-signals.md)
