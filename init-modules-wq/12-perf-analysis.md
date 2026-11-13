# 12. 性能与配方

**结论**：模块装不上先看 `finit_module` 的返回值和 dmesg 里的 `pr_err`；工作项堆在 `inactive_works` 上是 `max_active` 把它们挡住了；回收路径用的队列要有 `WQ_MEM_RECLAIM`，否则没有 rescuer；worker 池的 `worklist` 头 30 秒不动，watchdog 会打 `BUG: workqueue lockup`。下面每条都是 RECIPE，没有实测栏。

> 机制在 [06](./06-module-load.md)、[09](./09-cmwq-objects.md)、[10](./10-cmwq-queue-and-flags.md)。backlog 里和 demo 对应的 ID 是 `IMW-KMOD-WQ`。不要在生产机上加载教学模块，也不要为了看 watchdog 故意把工作函数写成死循环。

---

## 12.1 模块加载失败

现象：`insmod` / `modprobe` 非 0，`/proc/modules` 里没有这行，或者有一行停在 `Loading`。

看什么：返回值对应 `load_module` 的失败标签（`kernel/module/main.c:2997` 起）。常见的几类：

| 返回 | 源码位置 | 含义 |
|------|----------|------|
| `-EPERM` | `may_init_module` `:2642` | 没有 `CAP_SYS_MODULE`，或 `modules_disabled` 已置位 |
| `-EINVAL` | `finit_module` `:3222` | flags 带了三个合法比特以外的位 |
| `-ENOENT` | `delete_module` 路径；加载侧名字对不上是另一条 | 卸的时候 `find_module` 失败（`:722`） |
| init 的负返回 | `do_init_module` `:2545` | `mod->init` 失败，状态改 `GOING` 并 `free_module` |
| 正数 | `:2548` | init 没遵守 0 / 负 errno。模块仍然 `LIVE`，同时 `pr_warn` 和 `dump_stack` |

dmesg 里能对上的格式串（不是一次采样）：

- 专有许可：`"%s: module license '%s' taints kernel.\n"`（`main.c:1593`）
- `.gnu.linkonce.this_module` 大小和运行内核不匹配（`:1881`）
- init 返回正数的那条 `pr_warn`（`:2549`）

`module:module_load` 在 `do_init_module` 之前就发出（`:2993`）。看到 load 事件却没有 `Live`，说明 init 失败或还停在 `COMING`。

改什么：先确认头文件和 `uname -r` 是同一份构建。`sizeof(struct module)` 对不上时不要试图用忽略 vermagic 的标志硬装。`initcall_debug` 可以给 `mod->init` 加上耗时两行（[11](./11-observability.md)），它不替代返回值。

**RECIPE-M1**

目的：分清「没权装」和「init 返回了错误」。
命令：在测试机上按 [demos/01-run.sh](./demos/01-run.sh) 加载 demo 01。先不设 `KN_DEMO_LOAD`，确认脚本自己退出且没有 `insmod`。再在具备 `CAP_SYS_MODULE` 的测试机上设 `KN_DEMO_LOAD=1`。
期望看什么：成功时 `/proc/modules` 有 `kn_demo_wq` 且状态 `Live`；dmesg 里 init 行和 work 行都有本次的 `msg`。失败时保留 errno，不要把脚本改成忽略错误。
实测（待补）：未跑。状态 **待真机验证**。

---

## 12.2 工作项积压

现象：调用 `queue_work` 返回 true，但是 `work->func` 迟迟不跑；或者同一条队列上的函数突然串行。

看什么：`__queue_work` 在 `pwq->nr_active >= pwq->max_active` 时把工作放进 `pwq->inactive_works`，并带上 `WORK_STRUCT_INACTIVE`（`workqueue.c:1826`）。这些项不在 `pool->worklist` 上，`worker_thread` 不会取它们。`trace_workqueue_activate_work` 要等它们被挪进 worklist 才有（`:1821`）。

`max_active` 为 0 的创建请求会变成 `WQ_DFL_ACTIVE`（256）。传 1 且带 `WQ_UNBOUND` 会或上 `__WQ_ORDERED`（`:4769`），整条队列同时只跑一个。demo 05（还没写）打算用这个上限把积压打出来。

`WQ_SYSFS` 的 `max_active` 文件读的是 `saved_max_active`（`:6039`），可以在测试机上临时改小再改回。改之前把原值读下来。

改什么：工作函数里不要再同步等待同一条队列上的另一项（`flush_work` 自己）。unbound 队列如果只是想并行，把 `max_active` 留 0，不要传 1。per-cpu 队列的并行度还受池子里的 worker 数限制，那是 `nr_running` 的并发管理，不是 `max_active`。

**RECIPE-W1**

目的：确认一项工作是在跑，还是在 `inactive_works` 里。
命令：对目标 `work->func` 打开 `workqueue:workqueue_queue_work`、`workqueue:workqueue_activate_work`、`workqueue:workqueue_execute_start`，过滤函数指针。看完把 enable 写回 0。
期望看什么：排队之后很快有 activate 和 execute_start。只有 queue、没有 activate，就是被 `max_active` 挡住了。
实测（待补）：未跑。

---

## 12.3 rescuer

现象：内存回收时整机卡住，工作队列上还有未完成的项，但是没有新的 kworker 被创建出来。

看什么：rescuer 只存在于 `WQ_MEM_RECLAIM`（`init_rescuer`，`:4727`）。线程函数是 `rescuer_thread`（`:2884`），nice 是 `RESCUER_NICE_LEVEL` 即 `MIN_NICE`（`:110`、`:2890`）。名字是 `kworker/R-<队列名>`（`:4738`）。

池子在 `MAYDAY_INITIAL_TIMEOUT`（约 10ms，至少两个 tick，`:98`）里没能再造出 worker，`pool_mayday_timeout`（`:2432`）对 worklist 上的项 `send_mayday`（`:2408`）。`send_mayday` 把 pwq 挂上 `wq->maydays` 并 `wake_up_process(wq->rescuer->task)`（`:2427`）。没有 rescuer 的队列在 `send_mayday` 开头直接 return（`:2415`）。之后每 `MAYDAY_INTERVAL`（`HZ / 10`，`:101`）再叫一次。

rescuer 每轮最多处理 `RESCUER_BATCH`（16，`:104`）项，然后如果还有别的 pwq 在求救就先换下一个（`:2938`）。

`check_flush_dependency`（`:3004`）在非 cancel 的 flush 上检查：目标队列没有 `WQ_MEM_RECLAIM`，而当前任务在回收内存，或者当前任务自己跑在一条也没有这个标志的队列上。两条警告格式是 `PF_MEMALLOC task %d(%s) is flushing !WQ_MEM_RECLAIM ...`（`:3018`）和 `WQ_MEM_RECLAIM %s:%ps is flushing !WQ_MEM_RECLAIM ...`（`:3022`）。

改什么：回收路径、写回路径会调用的队列，创建时带 `WQ_MEM_RECLAIM`。不要在这种队列的工作函数里 `flush` 一条普通队列。demo 01 故意不带这个标志，它不是回收路径上的例子。

**RECIPE-W2**

目的：在测试机上确认一条自建的 `WQ_MEM_RECLAIM` 队列出现 `kworker/R-` 线程。
命令：另写一个只在测试机加载的模块，`alloc_workqueue(..., WQ_MEM_RECLAIM | WQ_UNBOUND, 0)`，`ps` 里找 `kworker/R-<名字>`，然后 `destroy_workqueue`。不要拿系统队列做实验。
期望看什么：线程在 `alloc_workqueue` 返回之后、`destroy` 返回之前存在。`wq_online` 还是 false 的早期启动路径会把 rescuer 推迟到 `workqueue_init`（`:6793`），模块加载时 `wq_online` 已经是 true，rescuer 在 `alloc_workqueue` 里就建好（`:4810`）。
实测（待补）：未跑。不要为了这条去触发真实的 OOM。

---

## 12.4 watchdog

现象：dmesg 里出现 `BUG: workqueue lockup`，后面跟着 pool 编号和卡住的秒数。

看什么：`CONFIG_WQ_WATCHDOG` 下 `wq_watchdog_thresh` 初值 30（`workqueue.c:6406`）。定时器回调 `wq_watchdog_timer_fn`（`:6472`）把阈值乘上 `HZ`。池的 `worklist` 非空，并且 `max(pool->watchdog_ts, touched)` 距离现在超过阈值，就：

```c
/* kernel/workqueue.c:6518 */
pr_emerg("BUG: workqueue lockup - pool");
pr_cont_pool_info(pool);
pr_cont(" stuck for %us!\n",
	jiffies_to_msecs(now - pool_ts) / 1000);
```

然后 `show_all_workqueues()`（`:6530`）。per-cpu 池还会 `show_cpu_pools_hogs`（`:6445`），把 `busy_hash` 里 `task_is_running` 的 worker 用 `sched_show_task` 打出来。注释（`:6390`）列举的原因：缺 `WQ_MEM_RECLAIM`、非法的 flush 依赖、一项一直处于 RUNNING 的并发管理工作。虚拟机被宿主机暂停时也会看起来像停顿，回调里先 `kvm_check_and_clear_guest_paused`（`:6497`）。

`worklist` 从空变为非空时 `__queue_work` 更新 `pool->watchdog_ts = jiffies`（`:1819`）。阈值 0 表示关掉（`:6481` 直接 return）。运行时参数是 `workqueue.watchdog_thresh`，sysfs 见 [11](./11-observability.md)。`wq_watchdog_init` 在 `workqueue_init` 末尾（`:6813`），定时器是 `TIMER_DEFERRABLE`（`:6598`）。

改什么：先看 `sched_show_task` 打出来的那个 worker 停在哪个函数，而不是先把阈值调大。阈值调大只推迟警告。工作函数里不要拿一把回收路径也要的锁然后睡觉。

**RECIPE-W3**

目的：确认这台机器的阈值是多少，而不是假设 30。
命令（只读）：

```text
cat /sys/module/workqueue/parameters/watchdog_thresh
```

文件不存在就记下 `CONFIG_WQ_WATCHDOG` 没编进来，把 ID 标成 blocked，不要编一个 30。
期望看什么：一个整数。默认源码是 30，发行版可能改过。
实测（待补）：未读。

---

## 12.5 和 demo 的对应

| 配方 | demo | 状态 |
|------|------|------|
| RECIPE-M1 加载与 dmesg | [demos/01-kmod-wq.c](./demos/01-kmod-wq.c) | **待真机验证**。脚本默认不 `insmod` |
| 启动期 `initcall_debug` | demos README 里的 02，还没有脚本 | 未写 |
| 两个模块的导出符号依赖 | demos README 里的 03 | 未写 |
| `delayed_work` 的 cancel 与 flush | demos README 里的 04 | 未写 |
| 把 `max_active` 压小看积压 | demos README 里的 05，对应 RECIPE-W1 | 未写 |

RECIPE-W2、RECIPE-W3 没有对应的 demo 源文件。W2 不要和 demo 01 混在一个模块里：demo 01 不带 `WQ_MEM_RECLAIM`。

---

## 12.6 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `kernel/module/main.c` | `may_init_module` | 2640 | `-EPERM` |
| `kernel/module/main.c` | `do_init_module` | 2515 | init 返回值怎么处理 |
| `kernel/workqueue.c` | `__queue_work` | 1726 | `max_active` 把工作放进 inactive |
| `kernel/workqueue.c` | `init_rescuer` | 4722 | 仅 `WQ_MEM_RECLAIM` |
| `kernel/workqueue.c` | `send_mayday` | 2408 | 把 pwq 交给 rescuer |
| `kernel/workqueue.c` | `check_flush_dependency` | 3004 | flush 依赖警告 |
| `kernel/workqueue.c` | `wq_watchdog_timer_fn` | 6472 | 默认 30 秒的停顿检测 |
| `kernel/workqueue.c` | `wq_watchdog_thresh` | 6406 | 初值 30，`CONFIG_WQ_WATCHDOG` |

---

**导航**：[← 观测](./11-observability.md) · [目录](./README.md)
