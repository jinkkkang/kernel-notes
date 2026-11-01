# 8. softirq 与 tasklet

**结论**：硬中断出口若发现 `local_softirq_pending()` 非 0，就在关中断的窗口里调用 `invoke_softirq`；非 RT 内核多数时候直接跑 `__do_softirq`，时间或次数超限才唤醒每 CPU 的 `ksoftirqd`。tasklet 在 v6.6.160 里仍是 `TASKLET_SOFTIRQ` / `HI_SOFTIRQ` 上的一条链表。

> 实现：`kernel/softirq.c`。向量枚举：`include/linux/interrupt.h:548`。`local_bh_disable`：`include/linux/bottom_half.h:18`。

---

## 8.1 十条 softirq

枚举顺序就是优先级：数字小的先跑。`ffs` 从低位找，所以 `HI_SOFTIRQ` 最先。

| 值 | 名字 | `softirq_to_name[]`（`softirq.c:63`） | 谁注册（举例） |
|---|---|---|---|
| 0 | `HI_SOFTIRQ` | `HI` | `tasklet_hi_action` |
| 1 | `TIMER_SOFTIRQ` | `TIMER` | `run_timer_softirq`（`kernel/time/timer.c:2312`） |
| 2 | `NET_TX_SOFTIRQ` | `NET_TX` | 网络发送 |
| 3 | `NET_RX_SOFTIRQ` | `NET_RX` | `net_rx_action`（`net/core/dev.c:6880`，注册 `:11957`） |
| 4 | `BLOCK_SOFTIRQ` | `BLOCK` | 块层 |
| 5 | `IRQ_POLL_SOFTIRQ` | `IRQ_POLL` | `irq_poll` |
| 6 | `TASKLET_SOFTIRQ` | `TASKLET` | `tasklet_action` |
| 7 | `SCHED_SOFTIRQ` | `SCHED` | 调度负载均衡等 |
| 8 | `HRTIMER_SOFTIRQ` | `HRTIMER` | `hrtimer_run_softirq`（`kernel/time/hrtimer.c:2312`） |
| 9 | `RCU_SOFTIRQ` | `RCU` | 注释要求 RCU 保持最后（`interrupt.h:559`） |

`NR_SOFTIRQS` 是 10。名字数组和枚举必须一起改，注释写在 `interrupt.h:579`。

热插拔时可以留在即将下线 CPU 上的掩码是 `SOFTIRQ_HOTPLUG_SAFE_MASK`（`interrupt.h:575`）：`TIMER`、`IRQ_POLL`、`HRTIMER`、`RCU`。其余要被迁走或在下线前排空。

---

## 8.2 谁把位置上

```c
/* kernel/softirq.c:711 */
void raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}
```

已经关中断时用 `raise_softirq_irqoff`（`:694`）。真正置位的是 `__raise_softirq_irqoff`（`:720`）：`or_softirq_pending(1UL << nr)`，并打 `trace_softirq_raise`。`raise_softirq_irqoff` 在此之外多一步：当前既不在硬中断也不在 softirq 里（`!in_interrupt()`）时 `wakeup_softirqd()`（`:707`）。网络收包路径在 `____napi_schedule` 里若当前不在 `net_rx_action` 中，就 `raise_softirq_irqoff(NET_RX_SOFTIRQ)`（`net/core/dev.c:4649`）。那时通常已经在硬中断里，所以只置位，等 `irq_exit_rcu` 再跑。

`open_softirq`（`softirq.c:727`）在启动时把函数指针写进 `softirq_vec[nr]`。运行中不要再增加新的 softirq 号；头文件注释（`interrupt.h:542`）建议用 tasklet 或线程。

---

## 8.3 `__do_softirq` 的循环

`__do_softirq`（`:610`）只是 `handle_softirqs(false)`。`ksoftirqd` 走 `handle_softirqs(true)`（`run_ksoftirqd`，`:942`）。

```c
/* kernel/softirq.c:535 起，删去统计和 trace 后的骨架 */
pending = local_softirq_pending();
softirq_handle_begin();          /* 抢占计数加上 SOFTIRQ_OFFSET */
set_softirq_pending(0);          /* 先清掉，处理中新来的留到下一轮 */
local_irq_enable();              /* softirq 处理函数里中断是开的 */
while ((softirq_bit = ffs(pending))) {
    vec_nr = ...;
    trace_softirq_entry(vec_nr);
    h->action(h);                /* 这一条的处理函数 */
    trace_softirq_exit(vec_nr);
    pending >>= softirq_bit;
}
local_irq_disable();
pending = local_softirq_pending();
if (pending) {
    if (time_before(jiffies, end) && !need_resched() && --max_restart)
        goto restart;
    wakeup_softirqd();           /* 否则交给 ksoftirqd */
}
```

两个上限（`:499`）：

```c
#define MAX_SOFTIRQ_TIME  msecs_to_jiffies(2)
#define MAX_SOFTIRQ_RESTART 10
```

要么时间超过约 2ms（以 jiffies 计，所以精度是一个 tick），要么重启超过 10 次，要么 `need_resched()`，就停下来唤醒 `ksoftirqd`。`ksoftirqd` 自己再跑时传入 `ksirqd == true`，非 RT 下会多做一次 `rcu_softirq_qs()`（`:590`）。

处理函数里中断是打开的，所以 softirq 可以被新的硬中断打断。硬中断里可以再 `raise_softirq`，新位置进下一轮 `pending`。softirq 不能睡眠：`in_softirq()` 为真时调度器不允许这条任务让出。

---

## 8.4 从硬中断出口进来

非 RT 的 `invoke_softirq`（`softirq.c:443`，包在 `:320` 的 `#else /* CONFIG_PREEMPT_RT */` 里）：

| 条件 | 动作 |
|---|---|
| 没有强制中断线程，或这颗 CPU 的 `ksoftirqd` 还没创建 | `__do_softirq()`，或者在没有 `CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK` 时 `do_softirq_own_stack()` |
| `force_irqthreads()` 为真且 `ksoftirqd` 已存在 | 只 `wakeup_softirqd()` |

x86-64 的中断出口栈情况见 [02](./02-x86-entry.md)：硬中断已经在中断栈上时，`CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK` 允许 softirq 就地跑。

RT 的 `invoke_softirq`（`:294`）不在硬中断里跑处理函数，只唤醒 `ksoftirqd`。`do_softirq_post_smp_call_flush`（`:310`）也只在 RT 下有实际检查。

`ksoftirqd` 由 `spawn_ksoftirqd`（`:995`）在每颗 CPU 上拉起。`ksoftirqd_should_run`（`:937`）看本 CPU 是否还有 pending。

---

## 8.5 `local_bh_disable`

关 bottom half 不是关硬中断。它给抢占计数加上 `SOFTIRQ_DISABLE_OFFSET`，让 `invoke_softirq` / `__do_softirq` 发现 softirq 被挡住。

```c
/* include/linux/bottom_half.h:18 */
static inline void local_bh_disable(void)
{
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}
```

没开 `CONFIG_PREEMPT_RT` 也没开 `CONFIG_TRACE_IRQFLAGS` 时，`__local_bh_disable_ip` 就是 `preempt_count_add` 加一个编译屏障（`bottom_half.h:11`）。RT 或需要 trace 时走 `kernel/softirq.c:155` 的外部函数。

`local_bh_enable`（`:32`）减掉同样的计数。减到可以跑 softirq 且 pending 非 0 时，`__local_bh_enable_ip`（非 RT 在 `softirq.c:383`）会当场 `__do_softirq`。所以「在 `local_bh_disable` 里 `raise_softirq`」是合法的：处理函数推迟到 enable。

`spin_lock_bh` 在锁之外再做一次同样的计数（锁那一章）。不要在已经 `local_bh_disable` 的段里睡眠。

---

## 8.6 tasklet

头文件写明这个 API 已不推荐，建议改用中断线程（`interrupt.h:619` 的注释）。v6.6.160 的结构体和调度函数都还在。

```c
/* include/linux/interrupt.h:642 */
struct tasklet_struct {
	struct tasklet_struct *next;     /* 每 CPU 链表 */
	unsigned long state;             /* TASKLET_STATE_SCHED / RUN */
	atomic_t count;                  /* 非 0 表示 disable，暂不跑 */
	bool use_callback;               /* 真：用 callback；假：用旧的 func(data) */
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
};
```

`tasklet_schedule` 最终到 `__tasklet_schedule`（`softirq.c:759`）→ `__tasklet_schedule_common`（`:743`），把节点挂到本 CPU 链表并 `raise_softirq_irqoff(TASKLET_SOFTIRQ)`。`tasklet_hi_schedule` 用 `HI_SOFTIRQ`（`:766`）。

`tasklet_action_common`（`:787`）在 softirq 里摘链表。同一条 tasklet 不会在两颗 CPU 上同时跑：`TASKLET_STATE_RUN` 配 `tasklet_trylock`（`interrupt.h:691`，要 SMP 或 RT）。正在别的 CPU 上跑时，这次调度会留到以后。

`count` 初值 0 是使能（`DECLARE_TASKLET`，`:655`），初值 1 是禁用（`DECLARE_TASKLET_DISABLED`，`:662`）。旧接口 `DECLARE_TASKLET_OLD`（`:672`）填 `func` 而不是 `callback`。

---

## 8.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `kernel/softirq.c` | `raise_softirq` | 711 | 置 pending 位 |
| | `handle_softirqs` | 535 | 按位跑处理函数，有时间和次数上限 |
| | `__do_softirq` | 610 | `handle_softirqs(false)` |
| | `invoke_softirq` | 443（非 RT）/ 294（RT） | 硬中断出口的入口 |
| | `wakeup_softirqd` | 74 | 唤醒本 CPU 的 `ksoftirqd` |
| | `run_ksoftirqd` | 942 | ksoftirqd 线程函数 |
| | `open_softirq` | 727 | 注册某条 softirq 的函数 |
| | `__tasklet_schedule` | 759 | 挂 tasklet 并拉起 `TASKLET_SOFTIRQ` |
| `include/linux/bottom_half.h` | `local_bh_disable` | 18 | 挡住 softirq |
| `net/core/dev.c` | `____napi_schedule` | 4615 | 把 NAPI 挂上 `poll_list` 并可能拉 `NET_RX_SOFTIRQ` |

---

**导航**：[← 分配失败](./07-irq-alloc-failure.md) · [目录](./README.md) · [怎么推迟 →](./09-deferred-work-choice.md)
