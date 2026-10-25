# 3. PID 与命名空间

**结论**：内核认的身份是 `struct pid`，不是一个裸的 `pid_t`。同一任务在每一层 pid 命名空间里各有一个数字，放在 `numbers[]` 里。容器内外看到的号不同，就是因为看的层不同。

## 3.1 `struct upid` / `struct pid`

`enum pid_type` 在 `include/linux/pid.h:9`：

| 枚举 | 含义 |
|---|---|
| `PIDTYPE_PID` | 这一个线程 |
| `PIDTYPE_TGID` | 线程组（用户态的进程） |
| `PIDTYPE_PGID` | 进程组 |
| `PIDTYPE_SID` | 会话 |
| `PIDTYPE_MAX` | 上面几种的个数，用来做数组长度 |

```c
/* include/linux/pid.h:54 */
struct upid {
	int nr;				/* 在这一层 ns 里看到的数字 */
	struct pid_namespace *ns;	/* 这个数字属于哪个 pid ns */
};

/* :59 */
struct pid {
	refcount_t count;			/* 引用计数 */
	unsigned int level;			/* 最深的那一层。0 是初始 ns */
	spinlock_t lock;			/* 保护 proc 的 inode 链 */
	/* 四种 pid_type 各一条链表：哪些 task 正在用这个号 */
	struct hlist_head tasks[PIDTYPE_MAX];
	struct hlist_head inodes;		/* /proc 里的 inode，退出时用来冲 dcache */
	/* pidfd 的 poll 堵在这里。任务退出时 do_notify_pidfd 会 wake_up_all */
	wait_queue_head_t wait_pidfd;
	struct rcu_head rcu;			/* free_pid 经 call_rcu 再 put_pid */
	struct upid numbers[];			/* 柔性数组：每一层一个 upid */
};
```

一个 `task_struct` 用 `pid_links[type]` 挂到对应的 `pid->tasks[type]`。每个线程有自己的 `PIDTYPE_PID`。线程组共用组长的 `PIDTYPE_TGID`，那个指针放在 `signal->pids[PIDTYPE_TGID]`，不放在每个线程自己的 `thread_pid` 上。

## 3.2 `struct pid_namespace`

`include/linux/pid_namespace.h:26`。号的上限不是这个结构里的字段：全局变量 `pid_max` 在 `kernel/pid.c:62`。

```c
struct pid_namespace {
	struct idr idr;				/* 这一层的号分配器 */
	struct rcu_head rcu;			/* 销毁时延迟释放 */
	unsigned int pid_allocated;		/* 已分配个数，最高位是 PIDNS_ADDING */
	struct task_struct *child_reaper;	/* 这一层的 init，负责收尸 */
	struct kmem_cache *pid_cachep;		/* 这一层用的 struct pid 缓存 */
	unsigned int level;			/* 嵌套深度。init_pid_ns 是 0 */
	struct pid_namespace *parent;		/* 外面一层。初始 ns 的 parent 是 NULL */
#ifdef CONFIG_BSD_PROCESS_ACCT
	struct fs_pin *bacct;			/* BSD 进程记账钉在哪个挂载上 */
#endif
	struct user_namespace *user_ns;		/* 拥有这个 pid ns 的 user ns */
	struct ucounts *ucounts;		/* 用户命名空间上的资源计数 */
	int reboot;				/* 这个 ns 里 reboot 时用的组退出码 */
	struct ns_common ns;			/* 所有命名空间共有的那一块（含 inode 号） */
#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
	int memfd_noexec_scope;			/* 这一层的 vm.memfd_noexec */
#endif
} __randomize_layout;
```

`MAX_PID_NS_LEVEL` 是 32（`pid_namespace.h:15`）。`PIDNS_ADDING` 是 `1U << 31`（`:48`），表示这个 ns 还允许分配新号。`RESERVED_PIDS` 是 300，定义在 `kernel/pid.c:64`：分配器绕了一圈之后，新号从 300 起，把小数字留给 init 一类的低号。

`pid_max` 的默认值来自 `PID_MAX_DEFAULT`（`include/linux/threads.h:28`）：小内核是 `0x1000`，否则是 `0x8000`。`pid_idr_init`（`kernel/pid.c:648`）再按 CPU 个数把它抬高，但不超过 `pid_max_max`。

## 3.3 分配与查找

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `kernel/pid.c` | `put_pid` | 108 | 引用减一，到 0 就释放缓存并放下 pid ns |
| | `free_pid` | 129 | 从每一层 idr 摘掉号，再 `call_rcu` |
| | `alloc_pid` | 162 | 从当前 ns 走到初始 ns，每一层分配一个 `nr` |
| | `disable_pid_allocation` | 305 | 清掉 `PIDNS_ADDING`，这个 ns 不再发新号 |
| | `find_pid_ns` | 312 | 在指定 ns 里按数字找 |
| | `find_vpid` | 318 | 在**当前** ns 里找 |
| | `attach_pid` | 334 | 把 task 挂上 `pid->tasks[type]`。调用者要拿着 `tasklist_lock` 的写锁 |
| | `detach_pid` | 359 | 摘下来 |
| | `change_pid` | 364 | 换成另一个 `struct pid` |
| | `exchange_tids` | 371 | 两个任务交换线程号。`de_thread` 用它 |
| | `transfer_pid` | 391 | 把 TGID / PGID / SID 从旧任务转到新任务 |
| | `pid_task` | 399 | 从 `struct pid` 取出某个 type 上的 task |
| | `find_task_by_pid_ns` | 416 | 数字加 ns，直接拿 task |
| | `find_task_by_vpid` | 423 | 当前 ns 里的数字 |
| | `find_get_task_by_vpid` | 428 | 找到并加引用 |
| | `get_task_pid` | 441 | 从 task 取出 `struct pid` 并加引用 |
| | `get_pid_task` | 451 | 从 pid 取出 task 并加引用 |
| | `find_get_pid` | 463 | `find_vpid` 再 `get_pid` |
| | `pid_nr_ns` | 475 | 这个 pid 在指定 ns 里的数字 |
| | `pid_vnr` | 489 | 在当前 ns 里的数字 |
| | `__task_pid_nr_ns` | 495 | 从 task 看出去的 PID / TGID / PGID / SID |
| | `find_ge_pid` | 521 | idr 里不小于 nr 的下一个 |
| | `pidfd_get_pid` | 527 | 从 pidfd 这个 fd 拿回 `struct pid` |
| | `pidfd_get_task` | 563 | 再从里面拿线程组组长 |
| | `pidfd_create` | 598 | 给一个已经存在的 pid 做 pidfd |
| `kernel/fork.c` | `__pidfd_prepare` | 2141 | 分配一个还没安装的 pidfd 文件 |
| | `pidfd_prepare` | 2190 | 同上，但先确认这个 pid 是线程组组长 |
| `include/linux/pid.h` | `ns_of_pid` | 152 | 分配这个 pid 的最深那层 ns |
| | `is_child_reaper` | 166 | 最深层的号是不是 1 |

`copy_process` 在 `kernel/fork.c:2527` 调用它（外面的判断在 `:2526`）。idle 线程传入的是 `init_struct_pid`，不走这条分配：

```c
if (pid != &init_struct_pid) {
	/* 在「子进程将来所属」的 pid ns 里分配。每一层一个号 */
	pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
			args->set_tid_size);
	if (IS_ERR(pid)) {
		retval = PTR_ERR(pid);
		goto bad_fork_cleanup_thread;
	}
}
```

### `alloc_pid` 怎么逐层发号

`kernel/pid.c:162`。`set_tid` 是 `clone3` 用来指定每一层 tid 的数组，长度不能超过 `ns->level + 1`。

分配循环（`:190` 起）从当前 ns 走到初始 ns。指定了 tid 时，号必须落在 `1 .. pid_max-1`，而且除了 1 以外还要求这一层已经有 `child_reaper`；调用者还得在这一层的 user ns 里有 checkpoint/restore 能力。没指定就用 `idr_alloc_cyclic`。游标已经超过 `RESERVED_PIDS` 时，新号从 300 起，避免把低号再次发出去。

```c
for (i = ns->level; i >= 0; i--) {
	int tid = 0;

	if (set_tid_size) {
		tid = set_tid[ns->level - i];
		/* pid_max 是 kernel/pid.c 里的全局变量 */
		if (tid < 1 || tid >= pid_max)
			goto out_free;
		if (tid != 1 && !tmp->child_reaper)
			goto out_free;
		if (!checkpoint_restore_ns_capable(tmp->user_ns))
			goto out_free;
		set_tid_size--;
	}
	/* ... 拿 pidmap_lock ... */
	if (tid) {
		nr = idr_alloc(&tmp->idr, NULL, tid, tid + 1, GFP_ATOMIC);
		if (nr == -ENOSPC)
			nr = -EEXIST;	/* 这个号已经有人 */
	} else {
		int pid_min = 1;
		if (idr_get_cursor(&tmp->idr) > RESERVED_PIDS)
			pid_min = RESERVED_PIDS;
		/* 先放一个 NULL，免得 find_pid_ns 看见半初始化的 pid */
		nr = idr_alloc_cyclic(&tmp->idr, NULL, pid_min,
				      pid_max, GFP_ATOMIC);
	}
	pid->numbers[i].nr = nr;
	pid->numbers[i].ns = tmp;
	tmp = tmp->parent;
}
```

循环之后才把 `struct pid` 填完整（引用计数、`tasks[]`、`wait_pidfd`、`inodes`），再用 `idr_replace` 把指针公开给 `find_pid_ns`，同时给每一层的 `pid_allocated` 加一（`:271`）。失败路径在 `:284` 之后把已经占住的号还回去。

用户态拿着一个数字来找任务：当前 ns 用 `find_vpid(nr)`，指定 ns 用 `find_pid_ns(nr, ns)`，再 `pid_task(pid, PIDTYPE_PID)` 或 `PIDTYPE_TGID`。

## 3.4 为什么容器内外 PID 不同

宿主机 pid ns 的 `level` 是 0，容器里面那层是 1 时：

- 容器 init 的 `numbers[1].nr == 1`，`numbers[0].nr` 可能是宿主机上的 12345。
- 容器里 `getpid()` 看的是当前 ns，所以是 1。
- 宿主机 `ps` 看同一任务，走的是 level 0 的号，所以是 12345。
- `kill` 从哪一层发起，就用那一层的 `nr` 去 `find_pid_ns`。

`/proc/pid/status` 的 `NSpid` 把从「读这个文件的人」所在的层一直到任务最深层的号都打出来（`fs/proc/array.c:212`）。只有一列，说明从这个视角看只有一层。

## 3.5 pidfd

pidfd 是一个匿名 inode 文件，名字固定写成 `[pidfd]`。文件的私有数据就是 `struct pid`，文件自己 `get_pid` 抓住它。

- `CLONE_PIDFD`：`copy_process` 在 `:2542` 调用 `__pidfd_prepare`（`fork.c:2141`）。这个帮手用 `anon_inode_getfile("[pidfd]", &pidfd_fops, pid, ...)` 做出文件，`get_pid` 让文件抓住 `struct pid`，再把 fd 数字写回 `args->pidfd`。文件要等任务对世界可见之后才 `fd_install`（`:2731`）。
- `pidfd_prepare`（`:2190`）多检查一步：这个 pid 必须已经挂着线程组组长。`CLONE_PIDFD` 那时任务还没挂上，所以创建路径用的是不检查的 `__pidfd_prepare`。
- `pidfd_send_signal`（`kernel/signal.c:3917`）和 `poll`：`pidfd_poll`（`fork.c:2087`）堵在 `pid->wait_pidfd` 上，等 `thread_group_exited` 为真，也就是整个线程组都退出。号被新进程复用时，那是另一份 `struct pid`，旧文件不会认错人。
- `do_notify_pidfd`（`signal.c:2069`）在 `do_notify_parent` 里被调用，对 `pid->wait_pidfd` 做 `wake_up_all`。

`CLONE_PIDFD` 不能和 `CLONE_THREAD` 一起用（`fork.c:2316`）。`pidfd_get_task` 的注释也写了：pidfd 目前只代表线程组组长。

## 3.6 和创建路径的衔接

号分配好之后，`copy_process` 在 `:2636` 拿下 `tasklist_lock`。组长的 `attach_pid` 从 `:2707` 开始，这条线程自己的 `PIDTYPE_PID` 在 `:2722`。在这之前 `find_vpid` 找不到这个新任务。

---

**导航**：[章节目录](./README.md) · [← 任务抽象：task_struct](./02-task-struct.md) · [创建路径：fork / clone / clone3 / vfork →](./04-fork-clone.md)
