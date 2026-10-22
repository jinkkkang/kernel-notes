# 5. 内核线程与特殊任务

**结论**：普通内核线程由 PID 2 的 `kthreadd` 统一创建；idle 用 `fork_idle`；io_uring worker 走 `create_io_thread`，带 `PF_USER_WORKER`。

## 5.1 `kthreadd`

`kernel/kthread.c:787`：

```c
int kthreadd(void *unused)
{
	set_task_comm(tsk, "kthreadd");
	ignore_signals(tsk);
	current->flags |= PF_NOFREEZE;
	cgroup_init_kthreadd();
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&kthread_create_list))
			schedule();                 /* 中文：没活就睡 */
		__set_current_state(TASK_RUNNING);
		spin_lock(&kthread_create_lock);
		while (!list_empty(&kthread_create_list)) {
			create = list_entry(...);
			list_del_init(&create->list);
			spin_unlock(&kthread_create_lock);
			create_kthread(create);     /* 中文：kernel_thread → 新 kthread */
			spin_lock(&kthread_create_lock);
		}
		spin_unlock(&kthread_create_lock);
	}
}
```

用户 API：`kthread_create` / `kthread_create_on_node` / `kthread_create_on_cpu`（`include/linux/kthread.h`）把请求挂进 `kthread_create_list`，再 `wake_up_process(kthreadd_task)`。

`create_kthread`（约 `:451`）内部：`kernel_thread(kthread, create, …)`（`fork.c:2830`），flags 含 `CLONE_VM|CLONE_UNTRACED`。

## 5.2 `kernel_thread` / `fork_idle` / `create_io_thread`

| 函数 | 位置 | 用途 |
|---|---|---|
| `kernel_thread` | `fork.c:2830` | 底层造内核线程（`CLONE_VM\|CLONE_UNTRACED`） |
| `fork_idle` | `fork.c:2680` | 每 CPU idle task（`__init`） |
| `create_io_thread` | `fork.c:2706` | io_uring worker |

`create_io_thread` 标志：

```c
unsigned long flags = CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
		      CLONE_IO|CLONE_VM|CLONE_UNTRACED;
/* args.io_thread = 1; args.user_worker = 1; → 打上 PF_USER_WORKER */
return copy_process(NULL, 0, node, &args);
```

`PF_USER_WORKER`（`sched.h:1810`）：「从用户线程 clone 出来的内核侧 worker」。`is_user_mode_task` 一类判断会把 `PF_KTHREAD|PF_USER_WORKER` 排除出普通用户任务（见 `sched.h:1867` 附近）。

## 5.3 与用户线程的差别（要点）

| | 用户任务 | 内核线程 |
|---|---|---|
| `mm` | 非 NULL | NULL，跑时借 `active_mm` |
| `PF_KTHREAD` | 无 | 有 |
| 创建者 | 某用户父进程 | 通常 `kthreadd`（PID 2） |
| 信号 | 正常 | `kthreadd` 子代常 `ignore_signals` |
| 首次返回 | `ret_from_fork` → 用户态 | `kthread` 包装 → `threadfn` |

## 5.4 关键函数表

| 文件 | 函数 | 作用 |
|---|---|---|
| `kernel/kthread.c` | `kthreadd` / `__kthread_create_on_node` / `kthread_create_on_node` / `create_kthread` | 创建与守护 |
| `kernel/fork.c` | `kernel_thread` / `fork_idle` / `create_io_thread` | 特殊 clone |
| `include/linux/kthread.h` | `kthread_run` 宏等 | 创建并 wake |

---

---

**导航**：[章节目录](./README.md) · [← 创建路径：fork / clone / clone3 / vfork](./04-fork-clone.md) · [地址空间交接：exec →](./06-exec.md)
