# 9. 推迟执行怎么选

**结论**：硬中断里只做必须马上做的几件事；要睡觉用中断线程或工作队列，要在硬中断和 NMI 里安全地排队用 `irq_work`，要协议栈轮询用 softirq（NAPI）。

> 工作队列：`kernel/workqueue.c`、`include/linux/workqueue.h`。中断线程：`kernel/irq/manage.c` 的 `irq_thread`（`:1298`）。`irq_work`：`kernel/irq_work.c`、`include/linux/irq_work_types.h`。

---

## 9.1 四种上下文

| | 硬中断 | softirq / tasklet | 中断线程 | 工作队列 | `irq_work` |
|---|---|---|---|---|---|
| 能不能睡眠 | 不能 | 不能 | 能 | 能 | 处理函数默认不能当进程上下文用 |
| 谁拉起 | APIC 向量 | `raise_softirq` | `IRQ_WAKE_THREAD` | `schedule_work` | `irq_work_queue` |
| 延迟 | 最低 | 硬中断一返回就可能跑 | 要等线程被调度 | 再晚一些，走调度器 | 本 CPU 的 IPI 或下一次中断出口 |
| 典型用途 | ack、把活挂上队列 | 网络轮询、定时器 | 驱动里要 `mutex` 的下半部 | 可以失败、可以重试的收尾 | 跟踪、NMI 里不能直接做的记账 |

选的时候先问两个问题：处理函数要不要 `mutex_lock` / `kmalloc(GFP_KERNEL)` / `copy_from_user`？要，就离开硬中断和 softirq。第二问：是不是每来一次中断都要跑，而且次数极高？是，优先 softirq 或 NAPI，而不是每条中断一个工作项。

---

## 9.2 中断线程

`request_threaded_irq(irq, handler, thread_fn, flags, name, dev)`（`manage.c:2145`）。`handler` 在硬中断里跑，返回 `IRQ_WAKE_THREAD` 才唤醒 `thread_fn`。`handler` 传 `NULL` 时核心换成 `irq_default_primary_handler`（`:2186`），它的作用就是唤醒线程。

线程和这条 `irqaction` 绑在一起，`IRQF_ONESHOT` 时线保持 mask，直到线程结束。共享中断要带 `dev_id`。细节在 [06](./06-request-and-flow.md)。

适合：驱动下半部要拿睡眠锁，但又不想自己管一条工作队列。不适合：每秒几十万次的网络包，线程切换会比 softirq 贵。

---

## 9.3 工作队列

`schedule_work`（`include/linux/workqueue.h:626`）把 `work_struct` 丢进 `system_wq`。同一文件里还有这些全局队列（符号在 `kernel/workqueue.c:426` 起创建，`workqueue.c:6705` 分配）：

| 符号 | 行号（声明） | 用途 |
|---|---|---|
| `system_wq` | `workqueue.h:438` | `schedule_work` 用的 per-CPU 队列。注释写着改用 `system_percpu_wq` |
| `system_percpu_wq` | `workqueue.c:428` | 同样是 per-CPU 的 `events` |
| `system_highpri_wq` | `workqueue.h:440` | 高优先级工作项 |
| `system_unbound_wq` | `:442` | 不绑 CPU |
| `system_freezable_wq` | `:444` | 挂起时可以冻住 |
| `system_power_efficient_wq` | `:445` | 省电配置下可以不绑 CPU |

工作项在进程上下文跑，可以睡眠。硬中断里只允许调用 `schedule_work` 这种「把节点挂上链表」的函数，不能在硬中断里执行 `work->func`。

和中断线程的差别：工作队列是共享的，不自动 mask 那条 IRQ。驱动要自己在硬中断里 mask，在工作函数末尾 unmask，否则中断会在工作还没跑完时再次进来。

pool、pwq、worker，以及 `schedule_work` 在 v6.6.160 里实际调用的队列，写在 [init-modules-wq/](../init-modules-wq/README.md)。本节只回答该不该离开硬中断。

---

## 9.4 `irq_work`

有些记账必须从 NMI 或硬中断里排队，又不能等进程上下文。`irq_work` 用一条 IPI（向量 `IRQ_WORK_VECTOR`，`0xf6`，见 [03](./03-apic-and-vectors.md)）在本 CPU 上跑回调。

```c
/* include/linux/irq_work_types.h:8 */
struct irq_work {
	struct __call_single_node	node;   /* 挂进每 CPU 链表的节点 */
	void				(*func)(struct irq_work *);
	struct rcuwait			irqwait; /* 等待这项做完 */
};
```

| 函数 | 行号 | 作用 |
|---|---|---|
| `irq_work_queue` | `kernel/irq_work.c:116` | 排到当前 CPU |
| `irq_work_queue_on` | `:137` | 排到指定 CPU |
| `irq_work_run` | `:259` | 把本 CPU 队列跑掉 |

`func` 里不要睡眠。需要锁时用 `raw_spinlock` 这一档（锁章），因为这项可能在硬中断里跑。perf 采样自己会用 `irq_work`，所以 `irq_work_exit` 这个 tracepoint 禁止被采样事件订阅，否则会套娃（`arch/x86/include/asm/trace/irq_vectors.h:75`）。

---

## 9.5 和 NAPI 的分界

网卡硬中断里常见的做法是关一下自己的中断线，然后 `napi_schedule`（`include/linux/netdevice.h:496`）→ `__napi_schedule`（`net/core/dev.c:6189`）→ `____napi_schedule`（`:4615`）。后者把 `napi` 挂到 `softnet_data.poll_list`，并 `raise_softirq_irqoff(NET_RX_SOFTIRQ)`（`:4649`）。`net_rx_action` 再按权重调用驱动的 `poll`。

若设置了 `NAPI_STATE_THREADED` 且线程已经创建，`____napi_schedule` 改为 `wake_up_process`（`:4629`），不拉 softirq。这是驱动自己的选择，不是 genirq 的中断线程。

本章不展开 `poll` 怎么把包送进协议栈。只需要记住：高速率收包的推迟点是 `NET_RX_SOFTIRQ`，不是 `schedule_work`。

---

## 9.6 一张选择表

| 你要做的事 | 用 |
|---|---|
| 读设备寄存器、清中断原因、把事件记下来 | 硬中断 `handler` |
| 每包都要看，但不能在硬中断里循环太久 | `NET_RX_SOFTIRQ` / NAPI |
| 同一条小任务要串行，又不想要线程 | tasklet（还能用，但不建议新代码依赖它，见 [08](./08-softirq-tasklet.md)） |
| 要睡眠锁或分配可睡眠的内存 | 中断线程，或自己 mask + 工作队列 |
| 从 NMI / 硬中断排队，回调仍然很短 | `irq_work_queue` |
| 和某条 IRQ 的 mask 生命周期绑在一起 | `request_threaded_irq` + `IRQF_ONESHOT` |

---

**导航**：[← softirq](./08-softirq-tasklet.md) · [目录](./README.md) · [定时器 →](./10-timers.md)
