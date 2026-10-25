# 5. 内核线程与特殊任务

**结论**：普通内核线程由 2 号任务 `kthreadd` 统一创建。每颗 CPU 的 idle 用 `fork_idle`。用户态 init 和 usermodehelper 用 `user_mode_thread`，不打 `PF_KTHREAD`。io_uring 的 worker 走 `create_io_thread`，同时带 `PF_IO_WORKER` 和 `PF_USER_WORKER`。

## 5.1 `kthreadd`

`kernel/kthread.c:738`：

```c
int kthreadd(void *unused)
{
	struct task_struct *tsk = current;

	set_task_comm(tsk, "kthreadd");
	ignore_signals(tsk);		/* 信号全部忽略 */
	set_cpus_allowed_ptr(tsk, housekeeping_cpumask(HK_TYPE_KTHREAD));
	set_mems_allowed(node_states[N_MEMORY]);

	current->flags |= PF_NOFREEZE;
	cgroup_init_kthreadd();

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&kthread_create_list))
			schedule();			/* 没活就睡 */
		__set_current_state(TASK_RUNNING);

		spin_lock(&kthread_create_lock);
		while (!list_empty(&kthread_create_list)) {
			struct kthread_create_info *create;

			create = list_entry(kthread_create_list.next,
					    struct kthread_create_info, list);
			list_del_init(&create->list);
			spin_unlock(&kthread_create_lock);

			create_kthread(create);		/* 真正去 kernel_thread */

			spin_lock(&kthread_create_lock);
		}
		spin_unlock(&kthread_create_lock);
	}
}
```

用户（内核里的其他代码）调用 `kthread_create` / `kthread_create_on_node` / `kthread_create_on_cpu`（`include/linux/kthread.h:27`、`:11`、`:31`）。它们把请求挂进 `kthread_create_list`，再 `wake_up_process(kthreadd_task)`。`kthread_run`（`:51`）是「创建再唤醒」的宏。

`create_kthread`（`kthread.c:403`）的调用是：

```c
pid = kernel_thread(kthread, create, create->full_name,
		    CLONE_FS | CLONE_FILES | SIGCHLD);
```

`kernel_thread`（`fork.c:2968`）自己再补上 `CLONE_VM | CLONE_UNTRACED`，并设 `args.kthread = 1`。所以生出来的任务有 `PF_KTHREAD`，也不会向 ptracer 报这次创建。包装函数 `kthread` 再去跑用户请求的 `threadfn`。

## 5.2 四条创建入口

| 函数 | 位置 | 谁用它 | 标志上的差别 |
|---|---|---|---|
| `kernel_thread` | `fork.c:2968` | `kthreadd`，以及别的内核代码 | `kthread = 1`，强制 `CLONE_VM \| CLONE_UNTRACED`，打上 `PF_KTHREAD` |
| `user_mode_thread` | `fork.c:2987` | `kernel_init`（`init/main.c:675`）和 usermodehelper（`kernel/umh.c:133`） | 同样强制 `CLONE_VM \| CLONE_UNTRACED`，但**不**设 `kthread`，所以没有 `PF_KTHREAD` |
| `fork_idle` | `fork.c:2827` | 每颗 CPU 的 idle，只在初始化时 | 直接 `copy_process(&init_struct_pid, ...)`，不分配新 pid，再 `init_idle` |
| `create_io_thread` | `fork.c:2853` | io_uring worker | 见下面。返回的任务还没唤醒，调用者自己 `wake_up_new_task` |

`create_io_thread` 的标志分成两截写：

```c
unsigned long flags = CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_IO;
struct kernel_clone_args args = {
	.flags		= ((lower_32_bits(flags) | CLONE_VM |
			    CLONE_UNTRACED) & ~CSIGNAL),
	.exit_signal	= (lower_32_bits(flags) & CSIGNAL),
	.fn		= fn,
	.fn_arg		= arg,
	.io_thread	= 1,		/* → PF_IO_WORKER */
	.user_worker	= 1,		/* → PF_USER_WORKER */
};
return copy_process(NULL, 0, node, &args);
```

合在一起就是：和调用者同一个线程组、共享 mm / fs / files / sighand / io_context，不报 ptrace，不把低 8 位当成退出信号。`copy_process`（`:2345`）看到 `user_worker` 就打 `PF_USER_WORKER`，并把屏蔽设成「只留 `SIGKILL` 和 `SIGSTOP`」。

`PF_USER_WORKER` 的定义在 `sched.h:1761`：从用户线程 clone 出来的内核侧 worker。`is_user_task()`（`:1819`）把 `PF_KTHREAD` 和 `PF_USER_WORKER` 都排除在普通用户任务之外。

## 5.3 和用户线程差在哪

| | 用户任务 | 内核线程（`PF_KTHREAD`） | user worker |
|---|---|---|---|
| `mm` | 非 NULL | NULL。跑的时候借 `active_mm` | 和创建它的用户线程共享（`CLONE_VM`） |
| `PF_KTHREAD` | 无 | 有 | 无。有的是 `PF_USER_WORKER`，io_uring 再加 `PF_IO_WORKER` |
| 谁创建 | 某个用户父进程 | 通常是 `kthreadd` | `create_io_thread` 的调用者 |
| 信号 | 正常投递 | `kthreadd` 的子代常被 `ignore_signals` | 只留致命信号和 `SIGSTOP` |
| 第一次被调度 | `ret_from_fork` 回到用户态，`ax == 0` | `kthread` 包装再进 `threadfn` | 帧里的 `fn` 先在内核跑 |

`user_mode_thread` 造出来的任务第一次也进 `fn`。`ret_from_fork`（`arch/x86/kernel/process.c:151`）允许这个 `fn` 调用 `kernel_execve()` 成功后再返回，然后走 `syscall_exit_to_user_mode`，于是 PID 1 能从内核进入用户态的 init。

## 5.4 关键函数

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `kernel/kthread.c` | `kthreadd` | 738 | PID 2，循环创建 |
| | `create_kthread` | 403 | 从队列取出请求，调用 `kernel_thread` |
| | `__kthread_create_on_node` | 428 | 把请求挂进 `kthread_create_list` 并唤醒 kthreadd |
| `include/linux/kthread.h` | `kthread_create` | 27 | 宏，节点用 `NUMA_NO_NODE` |
| | `kthread_create_on_cpu` | 31 | 绑到指定 CPU |
| | `kthread_run` | 51 | 创建后 `wake_up_process` |
| `kernel/fork.c` | `kernel_thread` | 2968 | 打 `PF_KTHREAD` 的底层入口 |
| | `user_mode_thread` | 2987 | init / usermodehelper |
| | `fork_idle` | 2827 | 每 CPU idle |
| | `create_io_thread` | 2853 | io_uring worker |
| `arch/x86/kernel/process.c` | `ret_from_fork` | 145 | 新任务第一次运行的入口 |

---

**导航**：[章节目录](./README.md) · [← 创建路径：fork / clone / clone3 / vfork](./04-fork-clone.md) · [地址空间交接：exec →](./06-exec.md)
