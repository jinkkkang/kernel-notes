# 8. 上下文切换（进程管理视角）

**结论**：本章只关心「何时进调度器、切换时 mm/寄存器怎么换」；**谁被选中**（EEVDF 等）留给调度大章。

## 8.1 谁调用 `__schedule`

`__schedule(int sched_mode)` 在 `kernel/sched/core.c:7111`（`static`）。常见入口：

| 场景 | 路径 |
|---|---|
| 主动阻塞 | `schedule()`（`:7366`）← `wait_event` / 锁睡眠 / 阻塞 IO |
| 抢占 | 返回用户态或 `preempt_enable` 时发现 `TIF_NEED_RESCHED` → `preempt_schedule_*` → `__schedule` |
| yield | `sched_yield` → `schedule` |
| 退出 | `do_exit` 末尾 `schedule`（永不返回） |

## 8.2 `context_switch`（`:5474`）

```c
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	prepare_task_switch(rq, prev, next);
	arch_start_context_switch(prev);

	if (!next->mm) {                    /* 中文：切入内核线程 */
		enter_lazy_tlb(prev->active_mm, next);
		next->active_mm = prev->active_mm;  /* 借用，不切页表 */
		if (prev->mm)
			mmgrab_lazy_tlb(prev->active_mm);
		else
			prev->active_mm = NULL;
	} else {                            /* 中文：切入用户任务 */
		switch_mm_irqs_off(prev->active_mm, next->mm, next); /* 切页表 */
		if (!prev->mm) {
			rq->prev_mm = prev->active_mm; /* 稍后 mmdrop_lazy_tlb */
			prev->active_mm = NULL;
		}
	}

	/* 中文：真正切换栈与寄存器；之后的代码在 next 上下文里「返回」 */
	switch_to(prev, next, prev);
	barrier();
	return finish_task_switch(prev);
}
```

四种 mm 情况（源码注释原文逻辑）：

| prev → next | mm 动作 |
|---|---|
| kernel → kernel | lazy + 转移 `active_mm` |
| user → kernel | lazy + `mmgrab_lazy_tlb` |
| kernel → user | `switch_mm` + 稍后 `mmdrop_lazy_tlb` |
| user → user | `switch_mm`（同 mm 则优化） |

## 8.3 边界：本章停在入口

- `__schedule` 内部 `pick_next_task` → 各 `sched_class`：**不在本章展开**。
- 下一章从 EEVDF / `sched_entity` / `vruntime`（或 v7.3 对应的 deadline 虚拟时间）讲起。

## 8.4 关键函数表

| 文件 | 函数 | 作用 |
|---|---|---|
| `kernel/sched/core.c` | `__schedule` / `schedule` | 调度入口 |
| | `context_switch` | mm + 寄存器切换 |
| | `prepare_task_switch` / `finish_task_switch` | 切换前后簿记 |
| arch | `switch_to` / `switch_mm_irqs_off` / `enter_lazy_tlb` | 架构实现 |

---

---

**导航**：[章节目录](./README.md) · [← 退出、僵尸与回收](./07-exit-reap.md) · [信号与进程 →](./09-signals.md)
