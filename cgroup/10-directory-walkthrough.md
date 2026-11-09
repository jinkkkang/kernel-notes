# 10. 目录走读：怎么读 `/sys/fs/cgroup/`

**结论**：`/sys/fs/cgroup` 不是"一堆配置文件"，而是**内核对象在 kernfs 上的一次投影**。一个目录 = 一个 `struct cgroup`；目录里的 `cgroup.*` 是 core 的接口，`<控制器>.*` 是这个 cgroup 上启用了哪些控制器才长出来的接口，`cgroup.procs` 是"这个人现在住在这里"的唯一权威列表。读目录时按顺序问五个问题：**是不是 v2 → 根上有哪些控制器 → 这层子目录是谁建的 → 沿路径每层还剩哪些控制器 → 目标节点是不是叶子**。

> 源码：`kernel/cgroup/cgroup.c`、`kernel/cgroup/pids.c`、`kernel/sched/core.c`
> 用户文档：`Documentation/admin-guide/cgroup-v2.rst` 目录 **2**（cgroups v2）、**2-1**（Behavior）、**2-6**（cgroup.stat）
> 实测脚本：[demos/02_tree_walk.sh](./demos/02_tree_walk.sh)（只读，sampled on 6.8）

---

## 10.1 第一步：确认这是哪套接口

```text
cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,nsdelegate,memory_recursiveprot)
cgroup2fs
```

`stat -fc %T` 给出 `cgroup2fs`，`mount` 里只有一条 `type cgroup2`，就是**纯 v2 统一层次**。v1 的机器在这一步会看到 `cgroup on /sys/fs/cgroup/cpu type cgroup ...` 这样的多条挂载。判定用错版本，后面所有文件名都会对不上（v1 是 `tasks`、`memory.limit_in_bytes`；v2 是 `cgroup.procs`、`memory.max`）。

kernfs 是"一个目录对应一个内核对象"的虚拟文件系统，cgroup、sysfs、debugfs 都建在它上面，见 [vfs/11-pseudo-fs.md](../vfs/11-pseudo-fs.md)。cgroup 的挂载和根都来自 `cgroup_init()`（`cgroup.c:6100`）：建文件类型表、`cgroup_setup_root(&cgrp_dfl_root, 0)`、再逐个初始化子系统。

---

## 10.2 第二步：根目录上的接口怎么来的

```text
cgroup.controllers     cpuset cpu io memory hugetlb pids rdma misc
cgroup.subtree_control  cpuset cpu io memory pids
```

根目录的文件由两张表拼成：core 的 `cgroup_base_files[]`（`cgroup.c:5224`）加 PSI 的 `cgroup_psi_files[]`（`:5302`）；控制器文件只有在根 `cgroup.subtree_control` 里出现过的控制器才会挂上去。这台机器上 `hugetlb` 有而 `rdma/misc` 没有，就是这个原因。

`cgroup_base_files[]` 中 `cgroup.procs` 这一项（`cgroup.c:5232`）：

```c
	{
		.name = "cgroup.procs",
		.flags = CFTYPE_NS_DELEGATABLE,          /* 可随 cgroup ns 一起委派 */
		.file_offset = offsetof(struct cgroup, procs_file),
		.release = cgroup_procs_release,
		.seq_start = cgroup_procs_start,
		.seq_next = cgroup_procs_next,
		.seq_show = cgroup_procs_show,           /* 读：逐行打印 PID */
		.write = cgroup_procs_write,             /* 写：迁移进程 */
	},
```

读的方向只是遍历 `cg_list` 打印 `task_pid_vnr()`（`:5085`）。**写**才有意义，`__cgroup_procs_write`（`:5159`）做四件事：

```c
static ssize_t __cgroup_procs_write(struct kernfs_open_file *of, char *buf,
				    bool threadgroup)
{
	/* 目的 cgroup 由被写的 kernfs 节点决定 */
	dst_cgrp = cgroup_kn_lock_live(of->kn, false);
	...
	/* 解析 PID，拿到 task；写同一个 PID 到 cgroup.threads 只搬一个线程 */
	task = cgroup_procs_write_start(buf, threadgroup, &threadgroup_locked);
	...
	/* 源 cgroup：这个任务当前所在的组 */
	src_cgrp = task_cgroup_from_root(task, &cgrp_dfl_root);
	...
	/* 权限：要求对源和目的"共同祖先"的 cgroup.procs 也有写权限，防止 fd 继承攻击 */
	saved_cred = override_creds(of->file->f_cred);
	ret = cgroup_attach_permissions(src_cgrp, dst_cgrp,
					of->file->f_path.dentry->d_sb,
					threadgroup, ctx->ns);
	revert_creds(saved_cred);
	...
	ret = cgroup_attach_task(dst_cgrp, task, threadgroup);
```

迁移的完整行为（fork/exit 钩子、`css_set` 复用）见 [04](./04-lifecycle-hooks.md)。这里只要记住：**`echo PID > cgroup.procs` 是唯一的搬迁入口，也是唯一能把进程搬进别人 cgroup 的地方。**

---

## 10.3 第三步：这层子目录是谁建的

v2 里没有"目录类型"字段，但命名约定能告诉你创建者：

| 前缀 | 创建者 | 说明 |
|---|---|---|
| `*.slice` | systemd | 纯分组节点，通常 `cgroup.procs` 为空 |
| `*.service` | systemd | 一个服务单元，是叶子 |
| `*.scope` | systemd | 外部进程组（登录会话、tmux-spawn 等） |
| `*.mount` / `*.socket` / `*.target` … | systemd | 其余单元类型，多数没有进程 |
| `init.scope` | systemd | PID 1 |
| **其它任意名字** | **userspace 直接 `mkdir`** | 例如云安全组件 `aegis`、`aegisMonitor` |

实测：

```text
aegis                        userspace (not systemd)  procs=1    populated=1
aegisMonitor                 userspace (not systemd)  procs=1    populated=1
aegisRtap0                   userspace (not systemd)  procs=0    populated=0
dev-hugepages.mount          systemd                  procs=0    populated=0
init.scope                   systemd                  procs=1    populated=1
system.slice                 systemd                  procs=0    populated=1
user.slice                   systemd                  procs=0    populated=1
```

三点值得注意：

- `aegis*` 不是 systemd 单元，是 AliYunDun 进程自己 `mkdir` 再写 `cgroup.procs` 进来的。**cgroup 不要求必须经由 systemd**。
- `system.slice`、`user.slice` 自己 `procs=0` 但 `populated=1`。`populated` 的含义是"本节点或后代中有活进程"，所以 slice 里没人不代表空——这正是 no-internal-process 规则的直接后果，见 [03](./03-hierarchy-control.md)。
- `aegisMonitor` 只有 1 个进程，`aegisRtap0` 0 个进程却有 37M 内存（首次观测）。后者是正常的：进程退出后，它产生的 page cache 仍记在原 cgroup 上，直到被回收。

`populated` 来自 `cgroup.events`（`cgroup.c:3650`），文档定义是"the cgroup or its descendants contains any live processes"。

---

## 10.4 第四步：沿路径看控制器逐层收窄

`/proc/self/cgroup` 给出一行 `0::/path`，这是**相对当前 cgroup namespace 根**的路径。`proc_cgroup_show`（`cgroup.c:6294`）对每个 hierarchy 打印 `%d:` 加子系统名；v2 只有默认层次一条，所以是 `0::`。

```c
	for_each_root(root) {
		...
		seq_printf(m, "%d:", root->hierarchy_id);
		/* v1 才在这里打印 cgroup,memory 之类的 legacy_name 列表 */
		if (root != &cgrp_dfl_root)
			for_each_subsys(ss, ssid)
				...
		seq_putc(m, ':');

		cgrp = task_cgroup_from_root(tsk, root);

		/*
		 * v1 的僵尸任务显示在根；v2 保留它退出前的路径。
		 * 如果该 cgroup 在僵尸被回收前已经 rmdir，路径后追加 " (deleted)"。
		 */
		if (cgroup_on_dfl(cgrp) || !(tsk->flags & PF_EXITING)) {
			retval = cgroup_path_ns_locked(cgrp, buf, PATH_MAX,
						current->nsproxy->cgroup_ns);
```

注意路径是按 `current->nsproxy->cgroup_ns` 计算出来的；`cgroup_path_ns_locked`（`:2364`）就是 `kernfs_path_from_node(cgrp->kn, root->kn, ...)`，**从 ns 根节点开始截断**。所以在容器里看到的 `/foo` 和外层的 `/kubepods/podX/foo` 是同一个 cgroup，只是起点不同。

把路径逐层拼出来就能看到控制器收窄的过程（实测）：

```text
/sys/fs/cgroup                                               <cpuset cpu io memory hugetlb pids rdma misc>
user.slice                                                   <cpuset cpu io memory pids>
user.slice/user-0.slice                                      <cpu memory pids>
user.slice/user-0.slice/user@0.service                       <cpu memory pids>
user.slice/user-0.slice/user@0.service/tmux-spawn-....scope  <cpu memory pids>
```

规则只有一句：**子目录的 `cgroup.controllers` 等于父目录的 `cgroup.subtree_control`**（`cgroup_calc_subtree_ss_mask`，见 [03](./03-hierarchy-control.md)）。沿路径看每一行的 `subtree_control`，就能解释下一行为什么少了 `cpuset` 和 `io`。少了 `io` 的直接后果是：你的 tmux scope 里**没有 `io.max` / `io.weight` / `io.stat`**（对照实测）：

```text
root                        io.cost.model io.cost.qos io.pressure io.prio.class io.stat
system.slice/docker.service io.max io.pressure io.prio.class io.stat io.weight
user-0.slice（无 io）        io.pressure            <- 只剩 core 的 PSI 文件
.../tmux-spawn-xxx.scope    io.pressure
```

`io.pressure` 是 core 的 `cgroup_psi_files[]`（`cgroup.c:5302`），**不受控制器开关影响**，任何 cgroup 都有——只有 PSI 允许时才出现，见 [08](./08-psi-rstat.md)。所以判断"控制器到底开没开"不能看 `io.pressure` 在不在，要看 `cgroup.controllers` 里有没有 `io`。"为什么我读不到 io.max"这类问题，先沿路径查 `subtree_control`，而不是查控制器是否编译进内核。

---

## 10.5 第五步：判断一个目录是不是叶子

`cgroup.type`（实测为 `domain`）+ `cgroup.subtree_control`（实测为空）+ `populated 1`：这就是一个合法的叶子，可以继续往下建子组，也可以直接放进程。相对地，一个 `subtree_control` 非空的节点就是"分叉点"，不能直接住人。

拿一个真实 systemd 叶子对比：

```text
leaf: system.slice/accounts-daemon.service
  cgroup.controllers:   cpuset cpu io memory pids
  cgroup.type:          domain
  memory.current:       1703936
  memory.max:           max
  cpu.max:              max 100000
  procs:                3987513
```

`memory.max` 显示 `max` 时，`seq_puts_memcg_tunable`（`mm/memcontrol.c:6510`）打印字符串 `max`，内部值是 `PAGE_COUNTER_MAX`，表示不限：

```c
static int seq_puts_memcg_tunable(struct seq_file *m, unsigned long value)
{
	if (value == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)value * PAGE_SIZE);   /* 内部按页计，接口按字节 */
	return 0;
}
```

`cpu.max` 的两列是"配额 周期(us)"（`cpu_period_quota_print`，`kernel/sched/core.c:11444`）；`max 100000` 表示只设置了 100ms 周期、没有配额。

---

## 10.6 dying cgroup：目录消失了，对象还在

```text
cgroup.stat nr_descendants:        125
cgroup.stat nr_dying_descendants:  147
```

`nr_dying_descendants` 比 `nr_descendants` 还大。文档（`cgroup-v2.rst`，`cgroup.stat` 一节）的说法是：cgroup 被用户删除后进入 dying 状态，会停留**一段时间**（取决于系统负载）才被彻底销毁。

代码上分两阶段，`cgroup_destroy_locked`（`cgroup.c:5907`）是第一阶段：

```c
	/* s1. 校验并标记：摘掉用户可见部分，杀掉 css 的 percpu refcnt */
	if (cgroup_is_populated(cgrp))
		return -EBUSY;                  /* 还有人住 -> rmdir 失败 */
	if (css_has_online_children(&cgrp->self))
		return -EBUSY;                  /* 还有活着的孩子 */
	...
	/* 摘目录、给祖先加 dying 计数 */
	spin_lock_irq(&css_set_lock);
	for (tcgrp = parent; tcgrp; tcgrp = cgroup_parent(tcgrp)) {
		tcgrp->nr_descendants--;
		tcgrp->nr_dying_descendants++;
```

计数器在这里就 `++` 了，而真正 `--` 要等到第二阶段 `css_release`（`:5959` 附近），那时所有引用（主要是仍记在这个 memcg 上的页）才算清干净：

```c
		/* cgroup release path：所有引用都放掉之后才走到这里 */
		cgroup_rstat_flush(cgrp);

		spin_lock_irq(&css_set_lock);
		for (tcgrp = cgroup_parent(cgrp); tcgrp;
		     tcgrp = cgroup_parent(tcgrp))
			tcgrp->nr_dying_descendants--;
		spin_unlock_irq(&css_set_lock);
```

所以 `nr_dying_descendants` 一直涨且降不下去，说明**有内核对象长期持有已删除 cgroup 的引用**（常见是 memcg 的页、cgwb 写回、BPF 程序）。这是一个很具体的排障入口，不是"正常现象"四个字能打发掉的。

---

## 10.7 一张图

```text
/sys/fs/cgroup/                      <- root: 只有 cgroup.* + 被授权的控制器文件
├── cgroup.controllers               "内核支持" ⊇
├── cgroup.subtree_control          "已经下发给孩子" ⊆
├── cgroup.procs                     谁直接住在这个 cgroup
├── system.slice/                    systemd 建；subtree_control 非空 -> 不能住人
│   └── docker.service/              叶子
│       └── ...                      （容器运行时会在下面再 mkdir）
├── user.slice/                      systemd 建
│   └── user-0.slice/user@0.service/.../tmux-spawn-xxx.scope   你的 shell
└── aegis/                           userspace mkdir；cpu.max = 9500 100000
```

---

*对照：`kernel/cgroup/cgroup.c`（`cgroup_init` `:6100`、`cgroup_base_files[]` `:5224`、`proc_cgroup_show` `:6294`、`cgroup_destroy_locked` `:5907`）、`kernel/sched/core.c`（`cpu_period_quota_print` `:11444`）、`mm/memcontrol.c`（`seq_puts_memcg_tunable` `:6510`）、`Documentation/admin-guide/cgroup-v2.rst`。*
