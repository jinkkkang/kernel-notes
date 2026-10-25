# 7. 退出、僵尸与回收

**结论**：`do_exit` 先拆资源，再在 `exit_notify` 里变成 `EXIT_ZOMBIE`。真正释放 `task_struct` 要等父进程 `wait`，或者父进程忽略 `SIGCHLD` / 带了 `SA_NOCLDWAIT`，走到 `release_task`。非组长线程退出时会直接变成 `EXIT_DEAD`，不等待。

## 7.1 入口

| 入口 | 位置 | 行为 |
|---|---|---|
| `sys_exit` | `kernel/exit.c:997` | 只退当前线程，调用 `do_exit` |
| `sys_exit_group` | `:1041` | 整组退出，调用 `do_group_exit` |
| `do_group_exit` | `:1007` | 置 `SIGNAL_GROUP_EXIT`，`zap_other_threads`（`kernel/signal.c:1383`，调用点 `exit.c:1027`），再 `do_exit` |
| 致命信号 | `get_signal` → `do_group_exit` | 默认动作是终止时走这里。`PF_USER_WORKER` 不在这里被 `do_group_exit`，它自己收尾 |
| oops 等 | `make_task_dead`（`exit.c:936`） | 最后也进 `do_exit` |

`do_group_exit` 如果看见 `signal->group_exec_task`，退出码改成 0（`:1013`）。这是 exec 正在等同伴退出的时候，别的线程不要用自己的退出码覆盖组退出码。

## 7.2 `do_exit`（`exit.c:818`）

```c
void __noreturn do_exit(long code)
{
	struct task_struct *tsk = current;
	int group_dead;

	synchronize_group_exit(tsk, code); /* quick_threads--；最后一个则置 SIGNAL_GROUP_EXIT */
	/* 中间还有 kcov、coredump、ptrace、io_uring 的收尾 */
	exit_signals(tsk);  /* sets PF_EXITING。signal.c:3010 */

	/* sync mm's RSS info before statistics gathering */
	if (tsk->mm)
		sync_mm_rss(tsk->mm);
	group_dead = atomic_dec_and_test(&tsk->signal->live);
	/* group_dead 为真：这是线程组里最后一个活着的线程。
	 * 真的话会取消 itimer、记下 RSS 高水位；全局 init 则 panic。 */
	tsk->exit_code = code;

	exit_mm();			/* :543。甩掉 mm，处理 clear_child_tid 的 futex */
	if (group_dead)
		acct_process();
	trace_sched_process_exit(tsk);	/* :881。只有一个参数 tsk */

	exit_sem(tsk);
	exit_shm(tsk);
	exit_files(tsk);
	exit_fs(tsk);
	if (group_dead)
		disassociate_ctty(1);	/* 最后一个线程才跟控制终端分手 */
	exit_task_namespaces(tsk);	/* 放下 nsproxy */
	exit_task_work(tsk);
	exit_thread(tsk);
	sched_autogroup_exit_task(tsk);
	cgroup_exit(tsk);

	exit_notify(tsk, group_dead);	/* 变僵尸、通知父进程、孤儿过继 */
	/* 中间还释放 io_context、splice 管道、检查没拿锁 */
	do_task_dead();			/* sched/core.c:6730。schedule 之后不再回来 */
}
```

`trace_sched_process_exit` 的事件在 `include/trace/events/sched.h:333`，字段是 `comm`、`pid`、`prio`。`group_dead` 留在 `do_exit` 里，用来判断这是不是组里最后一条线程。

`exit_mm` 里会处理 `clear_child_tid`：这就是 `CLONE_CHILD_CLEARTID` 让 pthread join 醒来的地方。

## 7.3 `exit_notify`、僵尸、过继

`exit_notify`（`exit.c:736`）：

```c
static void exit_notify(struct task_struct *tsk, int group_dead)
{
	bool autoreap;
	struct task_struct *p, *n;
	LIST_HEAD(dead);

	write_lock_irq(&tasklist_lock);
	forget_original_parent(tsk, &dead);
	/* 把自己的子进程过继走，已经可以释放的放进 dead */

	if (group_dead)
		kill_orphaned_pgrp(tsk->group_leader, NULL);
		/* 孤儿进程组：停止的作业要收到 SIGHUP 和 SIGCONT */

	tsk->exit_state = EXIT_ZOMBIE;
	if (unlikely(tsk->ptrace)) {
		/* 被 ptrace 时，组长且组已空且没被过继，才用自己的 exit_signal；
		 * 否则用 SIGCHLD，让调试器看到这次退出。 */
		int sig = thread_group_leader(tsk) &&
				thread_group_empty(tsk) &&
				!ptrace_reparented(tsk) ?
			tsk->exit_signal : SIGCHLD;
		autoreap = do_notify_parent(tsk, sig);
	} else if (thread_group_leader(tsk)) {
		autoreap = thread_group_empty(tsk) &&
			do_notify_parent(tsk, tsk->exit_signal);
	} else {
		autoreap = true;	/* 非组长：不留僵尸 */
	}

	if (autoreap) {
		tsk->exit_state = EXIT_DEAD;
		list_add(&tsk->ptrace_entry, &dead);
	}

	/* mt-exec, de_thread() is waiting for group leader */
	if (unlikely(tsk->signal->notify_count < 0))
		wake_up_process(tsk->signal->group_exec_task);
	write_unlock_irq(&tasklist_lock);

	list_for_each_entry_safe(p, n, &dead, ptrace_entry) {
		list_del_init(&p->ptrace_entry);
		release_task(p);
	}
}
```

`autoreap` 是 `do_notify_parent`（`kernel/signal.c:2085`）的返回值，也是 `exit_notify` 里的局部变量。父进程把 `SIGCHLD` 设成 `SIG_IGN`，或者挂了 `SA_NOCLDWAIT`，函数返回真，子进程不留成持久僵尸。

过继分两步，都在 `forget_original_parent`（`:697`）里：

1. `find_child_reaper`（`:592`）：这一层 pid ns 的 `child_reaper`。如果正在退出的就是它，就改成同组还活着的线程；同组也没有，就 `zap_pid_ns_processes`，把这个命名空间清掉。
2. `find_new_reaper`（`:630`）：优先同组还活着的线程；否则若 `has_child_subreaper`，沿 `real_parent` 找最近的 `is_child_subreaper`（服务管理器用 `prctl` 标自己）；再不行就用上面的 `child_reaper`。容器里这个 reaper 是容器的 init，不是宿主机的 PID 1。

这两个标志在 `signal_struct` 里是位域（`include/linux/sched/signal.h:132`）：`is_child_subreaper`、`has_child_subreaper`。子进程在 `copy_process` 里继承 `has_child_subreaper`（`fork.c:2703`）。

## 7.4 wait 路径

```
sys_wait4 / sys_waitid / sys_waitpid     exit.c:1812 / :1720 / :1831
  do_wait                                :1593
    do_wait_thread / ptrace_do_wait / do_wait_pid
                                         :1503 / :1517 / :1567
      wait_consider_task                 :1391
        wait_task_zombie                 :1107
          release_task                   :245
```

`release_task` 拿 `tasklist_lock`，`__exit_signal`（`:143`）把这个线程的 utime/stime 等加进 `signal_struct` 的累计字段，从 pid 哈希和任务链表摘掉。如果这是组长以外的最后一条线程，而组长已经是僵尸，还会再通知组长的父亲，必要时把组长也释放掉。最后 `put_task_struct_rcu_user`（`:235`）等 RCU 宽限期再 `put_task_struct`。

## 7.5 孤儿和僵尸会留多久

- 父进程先退：子进程过继给 `find_new_reaper` 找到的人。在容器里那是容器 init。
- 父进程不 `wait`，也没有忽略 `SIGCHLD`：组长一直是 **Z**。
- 非组长线程：`exit_notify` 里 `autoreap` 直接为真，很快 `release_task`。组长常常留到整组都被收完，这样 `wait` 还能看到原来的进程号。

## 7.6 关键函数

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `kernel/exit.c` | `do_exit` | 818 | 当前线程退出，不返回 |
| | `do_group_exit` | 1007 | 整组退出 |
| | `make_task_dead` | 936 | oops 等灾难路径 |
| | `sys_exit` / `sys_exit_group` | 997 / 1041 | 系统调用 |
| | `synchronize_group_exit` | 802 | 最后一个线程置 `SIGNAL_GROUP_EXIT` |
| | `exit_mm` | 543 | 放下 mm，唤醒 join 用的 futex |
| `fs/file.c` | `exit_files` | 444 | 放下文件表 |
| `fs/fs_struct.c` | `exit_fs` | 96 | 放下 `fs_struct` |
| `kernel/nsproxy.c` | `exit_task_namespaces` | 254 | 放下 nsproxy |
| `ipc/sem.c` | `exit_sem` | 2335 | System V 信号量 |
| `ipc/shm.c` | `exit_shm` | 444 | System V 共享内存 |
| `include/linux/task_work.h` | `exit_task_work` | 41 | 跑完挂着的 task_work |
| `arch/x86/kernel/process.c` | `exit_thread` | 121 | x86 上的线程退出 |
| `kernel/exit.c` | `exit_notify` | 736 | 变僵尸、通知、过继 |
| | `forget_original_parent` | 697 | 过继子进程 |
| | `find_child_reaper` | 592 | 找这层 pid ns 的 init |
| | `find_new_reaper` | 630 | 同组线程、subreaper，或 init |
| | `do_wait` | 1593 | wait 系统调用的公共体 |
| | `wait_consider_task` | 1391 | 看这一个子进程该不该收 |
| | `wait_task_zombie` | 1107 | 收僵尸 |
| | `release_task` | 245 | 从 pid 哈希摘掉并释放 |
| | `__exit_signal` | 143 | 把计数加进 `signal_struct` |
| `kernel/signal.c` | `zap_other_threads` | 1383 | 组退出时杀掉同伴 |
| | `do_notify_parent` | 2085 | `SIGCHLD`，并决定要不要自收尸 |
| | `exit_signals` | 3010 | 置 `PF_EXITING` |
| `kernel/sched/core.c` | `do_task_dead` | 6730 | 最后一次 `schedule`，不再返回 |

---

**导航**：[章节目录](./README.md) · [← 地址空间交接：exec](./06-exec.md) · [上下文切换（进程管理视角） →](./08-context-switch.md)
