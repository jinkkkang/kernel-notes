# 7. 退出、僵尸与回收

**结论**：`do_exit` 先拆资源再 `exit_notify` 变成 `EXIT_ZOMBIE`；真正释放 `task_struct` 要等父进程 `wait`（或 autoreap / 父忽略 `SIGCHLD`）走到 `release_task`。

## 7.1 入口

| 入口 | 位置 | 行为 |
|---|---|---|
| `sys_exit` | `exit.c:1094` | 只退当前线程 → `do_exit` |
| `sys_exit_group` | `:1138` | 整组退 → `do_group_exit` → 可能 `zap_other_threads` |
| 信号致命 / `BUG` 等 | | 最终也进 `do_exit` |

`do_group_exit`（`exit.c:1104`）在需要时调用 `zap_other_threads`（定义于 `kernel/signal.c:1371`，调用点 `exit.c:1124`）。

## 7.2 `do_exit` 释放顺序（`exit.c:906`）

摘录关键序（带中文注释意图）：

```c
void __noreturn do_exit(long code)
{
	synchronize_group_exit(tsk, code);
	exit_signals(tsk);           /* 中文：设 PF_EXITING，不再接普通信号 */
	group_dead = atomic_dec_and_test(&tsk->signal->live);
	/* group_dead：本线程组最后一个活线程 */
	tsk->exit_code = code;
	trace_sched_process_exit(tsk, group_dead);  /* :957 已核对 */

	exit_mm();                   /* 中文：甩掉 mm / 处理 mm_release、CLEARTID futex */
	exit_sem(tsk);
	exit_shm(tsk);
	exit_files(tsk);
	exit_fs(tsk);
	exit_nsproxy_namespaces(tsk);
	exit_task_work(tsk);
	exit_thread(tsk);
	cgroup_task_exit(tsk);

	exit_notify(tsk, group_dead); /* 中文：变僵尸、通知父、孤儿过继 */
	/* ... 更多收尾 ... */
	/* 最终 schedule 掉，不再返回 */
}
```

`trace_sched_process_exit`：**已核实**在 `kernel/exit.c:957`，定义见 `include/trace/events/sched.h:335`（带 `group_dead` 参数）。

## 7.3 `exit_notify` / 僵尸 / 过继

`exit_notify`（`exit.c:748`）伪序：

```c
static void exit_notify(struct task_struct *tsk, int group_dead)
{
	forget_original_parent(tsk, &dead);
	/* 中文：把自己的 children 过继给 reaper，收集已可 release 的到 dead 列表 */

	if (group_dead)
		kill_orphaned_pgrp(...); /* 中文：孤儿进程组的 SIGHUP/SIGCONT 语义 */

	/* do_notify_parent：发 SIGCHLD；若父 IGN/SA_NOCLDWAIT → autoreap */
	if (thread_group_leader(tsk) && !autoreap)
		tsk->exit_state = EXIT_ZOMBIE;
	else
		tsk->exit_state = EXIT_DEAD;

	/* 唤醒父在 wait_chldexit 上的等待 */
	/* 对 dead 列表逐个 release_task */
}
```

`find_child_reaper`（`:608`）：优先同 pid ns 的 `child_reaper`；若父树里有 `has_child_subreaper`，则找最近的 `is_child_subreaper`（systemd 用户服务常用）。

`signal_struct` 相关字段：`is_child_subreaper` / `has_child_subreaper` / `autoreap`（`sched/signal.h:133-135`）。


## 7.4 wait 路径

```
wait4 / waitid / waitpid
  → do_wait / __do_wait          # exit.c:1721 / 1674
      → do_wait_thread / do_wait_pid / ptrace_do_wait
          → wait_task_zombie     # :1184
              → release_task     # :248
```

`release_task`：从 pid hash / 任务链表摘掉，`__exit_signal` 汇总统计到父，最终 `put_task_struct` 释放。

## 7.5 孤儿与僵尸残留

- 父先退：子过继给 reaper（容器里是容器 init，不是宿主机 PID 1）
- 父不 `wait` 且未忽略 `SIGCHLD`：子一直 **Z**
- 线程组：非 leader 线程退出可较快释放；leader 常留僵尸直到整组收尸完毕（历史兼容）

## 7.6 关键函数表

| 文件 | 函数 | 作用 |
|---|---|---|
| `kernel/exit.c` | `do_exit` / `do_group_exit` | 退出 |
| | `exit_mm` / `exit_files` / `exit_fs` / … | 拆资源 |
| | `exit_notify` / `forget_original_parent` / `find_child_reaper` | 通知与过继 |
| | `do_wait` / `wait_task_zombie` / `release_task` | 回收 |
| | `zap_other_threads` | 组退出杀同伴 |
| `kernel/signal.c` | `do_notify_parent` | `SIGCHLD` + autoreap 判断 |

---

---

**导航**：[章节目录](./README.md) · [← 地址空间交接：exec](./06-exec.md) · [上下文切换（进程管理视角） →](./08-context-switch.md)
