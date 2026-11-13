# 2. `start_kernel`

**结论**：`start_kernel` 在开中断之前把调度器和 workqueue 的数据结构建好，开中断之后把控制台、凭证、cgroup 这些能睡觉的初始化做完，最后 `arch_call_rest_init` 进入 `rest_init`，自己不再返回。

> 定义：`init/main.c:853`，`asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector`。`system_state` 的枚举在 `include/linux/kernel.h:223`，进入本函数时仍是初值 `SYSTEM_BOOTING`。改成 `SYSTEM_SCHEDULING` 发生在 `rest_init`（[03](./03-rest-init-and-pid1.md)）。

---

## 2.1 阶段

行号是 `init/main.c` 里的调用点。函数本体在别的文件，本章不展开它们的内部。

| 阶段 | 调用 | 行 | 此时中断 |
|------|------|----|----------|
| 给 idle 栈盖魔数，记下 boot CPU | `set_task_stack_end_magic`、`smp_setup_processor_id` | 858–859 | 尚未执行本函数里的 `local_irq_disable` |
| 早期 cgroup，然后关中断 | `cgroup_init_early`、`local_irq_disable` | 863–865 | 关掉。`early_boot_irqs_disabled = true`（`:866`） |
| 架构、命令行、内存、ftrace | `setup_arch` … `mm_core_init`、`ftrace_init`、`early_trace_init` | 872–916 | 仍关 |
| 调度器，然后 workqueue 早期 | `sched_init`、`housekeeping_init`、`workqueue_init_early` | 923、935、942 | 仍关。`:925` 还 `WARN` 一次：若中断已经被打开就再关掉 |
| RCU、中断控制器、定时器、softirq | `rcu_init`、`early_irq_init`、`init_IRQ`、`init_timers`、`hrtimers_init`、`softirq_init`、`time_init` | 944–963 | 仍关 |
| 打开中断 | `local_irq_enable` | 978 | 打开。`early_boot_irqs_disabled = false`（`:977`） |
| 控制台、锁自测、fork 缓存、命名空间、cgroup | `console_init`、`fork_init`、`proc_root_init`、`cgroup_init` 等 | 987–1047 | 开 |
| 交给 `rest_init` | `arch_call_rest_init` | 1051 | 开。注释写 "we're now alive" |

`maple_tree_init`（`:929`）夹在 `radix_tree_init` 和 `housekeeping_init` 之间。maple tree 的用法在内存章，这里只记下它早于 workqueue。

---

## 2.2 调度先于 workqueue，workqueue 先于中断

`sched_init` 之前的注释说明为什么它必须在定时器中断之前：

```c
/* init/main.c:918 */
/*
 * Set up the scheduler prior starting any interrupts (such as the
 * timer interrupt). Full topology setup happens at smp_init()
 * time - but meanwhile we still have a functioning scheduler.
 */
sched_init();
```

完整拓扑在 pid 1 的 `kernel_init_freeable` 里 `smp_init` / `sched_init_smp` 才做（[03](./03-rest-init-and-pid1.md)）。此刻只有 boot CPU 上的调度器。

紧接着先 `housekeeping_init`，再 `workqueue_init_early`。注释把两件事的顺序说死了：

```c
/* init/main.c:931 */
/*
 * Set up housekeeping before setting up workqueues to allow the unbound
 * workqueue to take non-housekeeping into account.
 */
housekeeping_init();

/*
 * Allow workqueue creation and work item queueing/cancelling
 * early.  Work item execution depends on kthreads and starts after
 * workqueue_init().
 */
workqueue_init_early();
```

`workqueue_init_early` 的定义在 `kernel/workqueue.c:6645`。它分配 `system_percpu_wq` 等全局队列（`:6705` 起，名单在 [10](./10-cmwq-queue-and-flags.md)）。此函数返回之后可以 `alloc_workqueue` 和 `queue_work`，队列上还没有执行工作项的 kworker。kworker 在 `workqueue_init`（`workqueue.c:6773`），由 pid 1 调用。

---

## 2.3 开中断

开中断前最后一道检查在 `:975`：`WARN(!irqs_disabled(), "Interrupts were enabled early\n")`。然后：

```c
/* init/main.c:977 */
early_boot_irqs_disabled = false;
local_irq_enable();
```

这之后才是 `console_init`（`:987`）、`locking_selftest`（`:999`）。锁自测的注释写它需要中断已经打开，因为它要自测硬 / 软中断开关的锁反转。softirq 的向量和 `ksoftirqd` 的创建不在这一节；softirq 机制见 [interrupts/08](../interrupts/08-softirq-tasklet.md)。`softirq_init`（`:961`）只是这条时间线上的一个调用点。

`boot_init_stack_canary`（`:970`）在开中断之前，注释要求它用上已经初始化完的 rng（`:965` 的 `random_init` 在 `timekeeping_init` 之后）。

---

## 2.4 开中断之后、`rest_init` 之前

这一段都在进程上下文、中断已开，所以可以做会睡觉的分配。和后面几章有关的调用：

| 调用 | 行 | 和后文的关系 |
|------|----|--------------|
| `calibrate_delay` | 1016 | `loops_per_jiffy`。workqueue 用它估算 CPU-intensive 阈值（`workqueue.c:6754`，在 `workqueue_init` 里） |
| `pid_idr_init` | 1020 | 后面 `rest_init` 才能分出 pid 1、pid 2 |
| `fork_init` | 1028 | `kernel_thread` / `user_mode_thread` 用的缓存 |
| `proc_root_init` | 1039 | `/proc`。`/proc/modules` 本身是 `proc_modules_init` 这个 initcall 注册的，见 [11](./11-observability.md) |
| `cgroup_init` | 1042 | 早期那次是 `:863` 的 `cgroup_init_early` |
| `arch_call_rest_init` | 1051 | 弱符号默认实现 `:804`，里面只有 `rest_init()` |

```c
/* init/main.c:804 */
void __init __weak __noreturn arch_call_rest_init(void)
{
	rest_init();
}
```

`start_kernel` 在 `:1051` 之后还有一段给旧编译器挡尾调用的空代码（`:1057`），函数属性已经是 `__noreturn`。正常路径从 `rest_init` 进 idle，不会回到这里。

---

## 2.5 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `init/main.c` | `start_kernel` | 853 | 本文件的总入口 |
| `init/main.c` | `arch_call_rest_init` | 804 | 弱符号，默认调用 `rest_init` |
| `kernel/workqueue.c` | `workqueue_init_early` | 6645 | 分配系统 workqueue，尚无 kworker |
| `init/main.c` | `parse_early_param` | 739 | `early_param` 登记的选项。`start_kernel:887` 调用 |
| `include/linux/kernel.h` | `system_state` | 223 | `SYSTEM_BOOTING` 起的有序枚举 |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [rest_init 与 pid 1 →](./03-rest-init-and-pid1.md)
