# 3. `rest_init` 与 pid 1

**结论**：`rest_init` 先用 `user_mode_thread(kernel_init)` 造出 pid 1，再用 `kernel_thread(kthreadd)` 造出 pid 2，然后把自己变成 idle。pid 1 等到 kthreadd 就绪后跑 `kernel_init_freeable`（这里才 `workqueue_init`），释放 init 内存，把 `system_state` 设成 `SYSTEM_RUNNING`，最后 `kernel_execve` 进入用户态 init。

> `rest_init`：`init/main.c:664`。`kernel_init`：`:1411`。`kernel_init_freeable`：`:1501`。`user_mode_thread` / `kernel_thread` 的定义在 `kernel/fork.c:2987` 和 `:2968`，标志位差别见 [process-management/05](../process-management/05-kernel-threads.md)。

---

## 3.1 先 pid 1，再 kthreadd

```c
/* init/main.c:664 */
noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS);
	/* ... 把 pid 1 钉在 boot CPU 上，PF_NO_SETAFFINITY ... */

	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES);
	/* ... kthreadd_task = 按 pid 找到的任务 ... */

	system_state = SYSTEM_SCHEDULING;
	complete(&kthreadd_done);
	schedule_preempt_disabled();
	cpu_startup_entry(CPUHP_ONLINE);
}
```

`kthreadd_done` 在 `:662`，`static __initdata DECLARE_COMPLETION(kthreadd_done)`。pid 1 在 `kernel_init` 开头 `wait_for_completion(&kthreadd_done)`（`:1418`）。注释（`:670`）说明顺序：pid 1 必须先拿到 1 这个号；又不能在 kthreadd 存在之前就让它去创建内核线程。

两次创建的参数：

| 调用 | 行 | 入口函数 | 第三个参数 | 标志 |
|------|----|----------|------------|------|
| `user_mode_thread` | 675 | `kernel_init` | （这个函数没有 name 参数） | `CLONE_FS`。定义里不设 `kthread`，所以没有 `PF_KTHREAD` |
| `kernel_thread` | 688 | `kthreadd` | `NULL`（名字） | `CLONE_FS \| CLONE_FILES`。定义里 `kthread = 1` |

`kernel_thread` 的原型是 `pid_t kernel_thread(int (*fn)(void *), void *arg, const char *name, unsigned long flags)`（`fork.c:2968`）。`rest_init` 把名字传成 `NULL`。`kthreadd` 自己在 `kernel/kthread.c` 里 `set_task_comm(tsk, "kthreadd")`，见进程章。

pid 1 被钉在 boot CPU 上（`:681`–`:684`）：`PF_NO_SETAFFINITY`，`set_cpus_allowed_ptr` 只留当前 CPU。注释说任务迁移要等 `sched_init_smp`。

`system_state = SYSTEM_SCHEDULING`（`:700`）之后，`might_sleep` 和 `smp_processor_id` 的检查才算数。注释写明不能更早：`CONFIG_PREEMPTION=y` 时 `kernel_thread` 会在返回前碰到 `might_sleep`。

boot idle（原来的 `start_kernel` 那个任务）接着 `schedule_preempt_disabled`（`:708`），然后 `cpu_startup_entry(CPUHP_ONLINE)`（`:710`），不再返回。

---

## 3.2 `kernel_init`：释放 init 内存，再 exec

```c
/* init/main.c:1411 */
static int __ref kernel_init(void *unused)
{
	wait_for_completion(&kthreadd_done);   /* :1418 */

	kernel_init_freeable();                /* :1420 */
	async_synchronize_full();              /* :1422 等异步 __init 做完 */

	system_state = SYSTEM_FREEING_INITMEM; /* :1424 */
	kprobe_free_init_mem();
	ftrace_free_init_mem();
	kgdb_free_init_mem();
	exit_boot_config();
	free_initmem();                        /* :1429，弱符号默认 :1406 */
	mark_readonly();                       /* :1430 */
	pti_finalize();

	system_state = SYSTEM_RUNNING;         /* :1438 */
	numa_default_policy();
	rcu_end_inkernel_boot();
	do_sysctl_args();

	/* ramdisk_execute_command、execute_command、CONFIG_DEFAULT_INIT，
	   然后 /sbin/init、/etc/init、/bin/init、/bin/sh。见 :1445–:1480 */
}
```

`free_initmem`（`:1406`）是弱符号，默认 `free_initmem_default(POISON_FREE_INITMEM)`。`mark_readonly` 在 `CONFIG_STRICT_KERNEL_RWX` 下会先 `flush_module_init_free_work()`（`:1388`），把模块 init 段的释放工作等完，再 `mark_rodata_ro`。那条工作项是 `schedule_work(&init_free_wq)`，在 `do_init_module` 里排队（[06](./06-module-load.md)）。

`SYSTEM_RUNNING` 在 `include/linux/kernel.h:227`，排在 `SYSTEM_FREEING_INITMEM` 之后。枚举顺序被注释规定不能改，因为有代码用 `<` / `>=` 比较（`:220`）。

用户态文件的尝试顺序：

| 顺序 | 条件 | 行 | 失败时 |
|------|------|----|--------|
| 1 | `ramdisk_execute_command` 非空 | 1445 | 打印错误，继续 |
| 2 | `execute_command` 非空（内核参数 `init=`） | 1459 | `panic`。`init_setup` 在 `:555`，`__setup("init=", init_setup)` 在 `:570` |
| 3 | `CONFIG_DEFAULT_INIT[0] != '\0'` | 1467 | 打印错误，继续 |
| 4 | `/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh` | 1476 | 四个都失败才 `panic`（`:1482`） |

`rdinit=` 由 `rdinit_setup`（`:572`）写进 `ramdisk_execute_command`。`kernel_init_freeable` 末尾如果这个路径 `init_eaccess` 失败，会把它清成 `NULL` 再 `prepare_namespace`（`:1541`）。

---

## 3.3 `kernel_init_freeable`：workqueue 的第二步和第三步

```c
/* init/main.c:1501 */
static noinline void __init kernel_init_freeable(void)
{
	gfp_allowed_mask = __GFP_BITS_MASK;          /* :1504 可以做阻塞分配 */
	set_mems_allowed(node_states[N_MEMORY]);
	rcu_assign_pointer(cad_pid, get_pid(task_pid(current)));

	smp_prepare_cpus(setup_max_cpus);
	workqueue_init();                            /* :1515 */

	init_mm_internals();
	rcu_init_tasks_generic();
	do_pre_smp_initcalls();                      /* :1520 early initcall */
	lockup_detector_init();

	smp_init();
	sched_init_smp();

	workqueue_init_topology();                   /* :1526 */
	padata_init();
	page_alloc_init_late();
	do_basic_setup();                            /* :1530 → do_initcalls */

	kunit_run_all_tests();
	wait_for_initramfs();
	console_on_rootfs();                         /* :1535 打开 /dev/console 三次 dup */
	/* ramdisk 路径不可执行则 prepare_namespace() */
	integrity_load_keys();
}
```

`workqueue_init`（`kernel/workqueue.c:6773`）给每个 online CPU 的 per-cpu pool 和已有的 unbound pool `create_worker`（`:6802`、`:6809`），把 `wq_online` 设为 true（`:6812`），并 `wq_watchdog_init`（`:6813`）。这之前排队的工作项从这里才开始有线程执行。它也会给更早创建、带 `WQ_MEM_RECLAIM` 的队列补 rescuer（`:6793`）。

`workqueue_init_topology`（`workqueue.c:6885`）在 `smp_init` 和 `sched_init_smp` 之后。它按 CPU、SMT、cache、NUMA 填 `wq_pod_types`（`:6890`），再对每条已有 workqueue 调用 `wq_update_pod`（`:6902`）。默认 affinity scope 是 `WQ_AFFN_CACHE`（`workqueue.c:345` 的 `wq_affn_dfl`）。

`do_basic_setup`（`:1306`）的顺序是 `cpuset_init_smp`、`driver_init`、`init_irq_proc`、`do_ctors`、`do_initcalls`。initcall 级别在 [04](./04-initcalls.md)。

`console_on_rootfs`（`:1487`）`filp_open("/dev/console", O_RDWR, 0)`，成功则 `init_dup` 三次，给 pid 1 的 stdin/stdout/stderr。打不开只打警告，不让启动停住。

---

## 3.4 `kernel_execve`

```c
/* init/main.c:1324 */
static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	/* pr_debug 打印 argv_init / envp_init */
	return kernel_execve(init_filename, argv_init, envp_init);
}
```

`kernel_execve` 定义在 `fs/exec.c:2026`。它开头拒绝内核线程：

```c
/* fs/exec.c:2032 */
/* It is non-sense for kernel threads to call execve */
if (WARN_ON_ONCE(current->flags & PF_KTHREAD))
	return -EINVAL;
```

pid 1 走的是 `user_mode_thread`，没有 `PF_KTHREAD`，所以这条 `WARN` 不会因为它是 1 号任务而触发。函数随后 `getname_kernel`、`alloc_bprm`、把 argv/envp 拷进 `linux_binprm`，最后 `bprm_execve`（`:2082`）。成功时这个任务的内核栈按 exec 返回用户态，函数不再回到 `kernel_init` 的下一条。ELF 与 `mm` 的交接见 [process-management/06](../process-management/06-exec.md)。

`try_to_run_init_process`（`:1339`）在 `run_init_process` 返回 `-ENOENT` 时不当成致命错误，换下一个路径；其它 errno 会 `pr_err`。

---

## 3.5 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `init/main.c` | `rest_init` | 664 | pid 1、kthreadd、进入 idle |
| `kernel/fork.c` | `user_mode_thread` | 2987 | pid 1 的创建入口，不设 `kthread` |
| `kernel/fork.c` | `kernel_thread` | 2968 | kthreadd 的创建入口，`kthread = 1` |
| `init/main.c` | `kernel_init` | 1411 | 等 kthreadd，释放 initmem，exec |
| `init/main.c` | `kernel_init_freeable` | 1501 | `workqueue_init`、early initcall、SMP、`do_initcalls` |
| `kernel/workqueue.c` | `workqueue_init` | 6773 | 创建首批 kworker，`wq_online = true` |
| `kernel/workqueue.c` | `workqueue_init_topology` | 6885 | unbound 的 pod |
| `init/main.c` | `run_init_process` | 1324 | 打印并 `kernel_execve` |
| `fs/exec.c` | `kernel_execve` | 2026 | 内核里的 exec，拒绝 `PF_KTHREAD` |
| `init/main.c` | `free_initmem` | 1406 | 弱符号，默认毒化并释放 init 段 |

---

**导航**：[← start_kernel](./02-start-kernel.md) · [目录](./README.md) · [initcall →](./04-initcalls.md)
