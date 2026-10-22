# 3. PID 与命名空间

**结论**：内核持有的身份是 `struct pid`（可跨 namespace 多层编号），不是裸 `pid_t`；容器内外看到不同数字，是因为 `upid.numbers[]` 每层一个号。

## 3.1 `struct upid` / `struct pid`

`include/linux/pid.h:53-75`：

```c
struct upid {
	int nr;                      /* 在该 ns 内的数字 */
	struct pid_namespace *ns;
};

struct pid {
	refcount_t count;
	unsigned int level;          /* 最深 ns 层级 */
	spinlock_t lock;
	struct {
		u64 ino;
		struct rhash_head pidfs_hash;
		struct dentry *stashed;
		struct pidfs_attr *attr; /* pidfs / pidfd */
	};
	struct hlist_head tasks[PIDTYPE_MAX]; /* PID/TGID/PGID/SID */
	struct hlist_head inodes;
	wait_queue_head_t wait_pidfd;        /* pidfd 通知 */
	struct rcu_head rcu;
	struct upid numbers[];               /* 柔性数组：每层一个 upid */
};
```

`enum pid_type`（`include/linux/pid_types.h`）：`PIDTYPE_PID`、`PIDTYPE_TGID`、`PIDTYPE_PGID`、`PIDTYPE_SID`、`PIDTYPE_MAX`。

一个 `task_struct` 通过 `pid_links[type]` 挂到对应 `pid->tasks[type]`。线程组共享同一个 TGID 的 `struct pid`，但每个线程有自己的 `PIDTYPE_PID`。

## 3.2 `struct pid_namespace`

`include/linux/pid_namespace.h:26-50`：

| 成员 | 含义 |
|---|---|
| `idr` | 号分配器 |
| `pid_allocated` | 已分配计数（含 `PIDNS_ADDING`） |
| `child_reaper` | 该 ns 的 init（收尸者） |
| `pid_cachep` | `struct pid` 的 kmem_cache |
| `level` | 嵌套深度，init_pid_ns 为 0 |
| `pid_max` | 本 ns 上限 |
| `parent` | 父 pid ns |
| `user_ns` | 关联的 user ns |
| `ucounts` | 资源计数 |
| `reboot` | 该 ns reboot 时的 group exit code |
| `ns` | `ns_common` |
| `work` | 销毁工作 |

`MAX_PID_NS_LEVEL` = 32。`RESERVED_PIDS` = 300（低号保留）。

## 3.3 分配与查找

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `kernel/pid.c` | `alloc_pid` | 159 | 从当前 ns 向上逐层分配 `nr`，填 `numbers[]`；支持 clone3 `set_tid` |
| | `free_pid` | 110 | 释放 |
| | `free_pids` | 147 | 批量 |
| | `attach_pid` | 392 | task 挂到 `pid->tasks[type]`（需 tasklist_lock） |
| | `detach_pid` / `change_pid` / `transfer_pid` | | 摘链 / 换 pid / 转移 |
| | `find_pid_ns` | 370 | 在指定 ns 查 nr |
| | `find_vpid` | 376 | 在**当前** ns 查 |
| | `find_get_pid` | 530 | 查找并 `get_pid` |
| | `pid_task` | 466 | 从 pid 取某个 type 的 task |
| | `get_task_pid` | 508 | 从 task 取 pid |
| | `pidfd_get_pid` | 594 | 从 pidfd 还原 `struct pid` |

`copy_process` 调用点（`kernel/fork.c:2337`）：

```c
/* 中文：在「将来子进程所属」的 pid_ns_for_children 里分配多层号 */
pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
		args->set_tid_size);
```

## 3.3.1 `alloc_pid` 逐层分配（摘录加注）

`kernel/pid.c:159` 起，逻辑骨架：

```c
struct pid *alloc_pid(struct pid_namespace *ns, pid_t *arg_set_tid,
		      size_t arg_set_tid_size)
{
	/* 中文：set_tid 最多覆盖 ns->level+1 层，供 clone3 指定每层 tid */
	if (arg_set_tid_size > ns->level + 1)
		return ERR_PTR(-EINVAL);

	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);
	pid->level = ns->level;
	/* 初始化 tasks[]、wait_pidfd、pidfs … */

	/* 中文：从当前 ns 走到 init_pid_ns，校验 set_tid / 能力 */
	for (tmp = ns, i = ns->level; i >= 0; i--) {
		pid_max[...] = READ_ONCE(tmp->pid_max);
		if (arg_set_tid_size) { /* 校验 tid 范围与 CAP */ ... }
		tmp = tmp->parent;
	}

	/* 中文：再从当前 ns 向上，每层 idr_alloc 一个 nr，写入 numbers[i] */
	for (tmp = ns, i = ns->level; i >= 0;) {
		if (tid)
			nr = idr_alloc(&tmp->idr, NULL, tid, tid + 1, GFP_ATOMIC);
		else
			nr = idr_alloc_cyclic(...); /* 绕过 RESERVED_PIDS */
		pid->numbers[i].nr = nr;
		pid->numbers[i].ns = tmp;
		tmp = tmp->parent;
		i--;
	}
	/* 挂到各层 pid hash，返回 */
}
```

找任务：用户传入的数字 → `find_vpid(nr)`（当前 ns）或 `find_pid_ns(nr, ns)` → `pid_task(pid, PIDTYPE_PID|TGID|…)`。

## 3.4 为何容器内外 PID 不同

假设宿主机 pid ns level=0，容器 pid ns level=1：

- 容器内 init 的 `numbers[1].nr == 1`，`numbers[0].nr` 可能是宿主机上的 12345。
- 容器内 `getpid()` 走当前 ns → 看到 1。
- 宿主机 `ps` 看同一任务 → 看到 12345。
- `kill` 从哪个 ns 发起，就用哪层的 `nr` 去 `find_pid_ns`。

## 3.5 pidfd（点到为止）

- `CLONE_PIDFD`：`copy_process` 里 `pidfd_prepare`（`fork.c:2362`），把 fd 写回 `args->pidfd`。
- `pidfd_send_signal` / `poll(pidfd)`：基于 `struct pid` 生命周期，避免 PID 复用竞态。
- pidfs：`struct pid` 内嵌 `ino` / `stashed` dentry（`pid.h`）。
- `do_notify_pidfd`：任务退出时唤醒 pidfd waiters（`do_notify_parent` 也会调）。

## 3.6 关键函数表（本节）

见上表；另：`ns_of_pid()`（`pid.h:149`）返回分配该 pid 的最深 ns；`is_child_reaper()` 判断是否为该 ns 的 init。

---

---

**导航**：[章节目录](./README.md) · [← 任务抽象：task_struct](./02-task-struct.md) · [创建路径：fork / clone / clone3 / vfork →](./04-fork-clone.md)
