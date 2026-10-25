# 3. 层次与控制接口

**结论**：v2 树上，父节点用 `cgroup.subtree_control` **授权**子树能启用哪些控制器；子节点再在自己的 `subtree_control` 里向下传。另两条硬规矩：**top-down**（孩子不能启用父未授权的控制器）、**no internal process**（非 root、又不是 threaded 的节点，一旦向下开了控制器，本节点就不能再挂进程）。

> 实现：`kernel/cgroup/cgroup.c`  
> 用户文档：`Documentation/admin-guide/cgroup-v2.rst` 目录 **2-4**（Controlling Controllers）、**2-5**（Delegation）

---

## 3.1 你天天碰的文件

core 文件表是 `cgroup_base_files[]`（`cgroup.c:5224`）。压力文件另在 `cgroup_psi_files[]`（`:5302`），包在 `CONFIG_PSI` 里。

| 文件 | 读写 | 含义 | 备注 |
|---|---|---|---|
| `cgroup.type` | 读写 | 读出 `domain` / `domain threaded` / `domain invalid` / `threaded` | 非 root；写入只接受字符串 `threaded`（`:3544`） |
| `cgroup.procs` | 读写 | 线程组 leader 的 PID；**写入 PID** = 把该进程迁入 | `CFTYPE_NS_DELEGATABLE` |
| `cgroup.threads` | 读写 | 单个线程的 TID | 同上；threaded 模式用 |
| `cgroup.controllers` | 只读 | 本节点**可用**的控制器 | 子节点上通常来自父的 `subtree_control` |
| `cgroup.subtree_control` | 读写 | `+cpu +memory` / `-io`：允许或收回**子 cgroup** 启用这些控制器 | 可委托 |
| `cgroup.events` | 只读 | `populated`、`frozen` 两行（`:3650`） | 非 root |
| `cgroup.max.descendants` | 读写 | 后代个数上限 | 对应 `cgroup->max_descendants` |
| `cgroup.max.depth` | 读写 | 向下最多再嵌套几层 | 对应 `cgroup->max_depth` |
| `cgroup.stat` | 只读 | `nr_descendants`、`nr_dying_descendants`（`:3656`） | |
| `cgroup.freeze` | 读写 | 写 `1` 冻、`0` 解冻 | 非 root；实现见 [07](./07-io-pids-misc.md) |
| `cgroup.kill` | 只写 | 写 `1`：给整棵活着的子树里的进程发 `SIGKILL`，并让 `kill_seq++` | 非 root；threaded 节点返回 `-EOPNOTSUPP` |
| `cpu.stat` | 只读 | `usage_usec` / `user_usec` / `system_usec`，控制器打开后再加节流计数 | core 文件，不依赖先启用 cpu 控制器 |
| `cpu.stat.local` | 只读 | 本节点自己的节流时间（不带孩子） | `CONFIG_CGROUP_SCHED` 时才有内容 |
| `io.pressure` / `memory.pressure` / `cpu.pressure` | 读，可写触发器 | PSI，见 [08](./08-psi-rstat.md) | `CONFIG_PSI` |
| `irq.pressure` | 同上 | IRQ 压力，只有 `full` | 还要 `CONFIG_IRQ_TIME_ACCOUNTING` |
| `cgroup.pressure` | 读写 | **一个数字 0/1**，不是 some/full 文本；1 表示本节点 PSI 文件可见 | `CONFIG_PSI` |

本机 root 曾见：`cgroup.controllers` = `cpuset cpu io memory hugetlb pids`（sampled on 6.12，见 [09](./09-observability.md)）。

v2 挂载选项（`cgroup.c:1941`）和 `cgroup_root->flags` 的对应：

| 选项 | 标志 |
|---|---|
| `nsdelegate` | `CGRP_ROOT_NS_DELEGATE` |
| `favordynmods` | `CGRP_ROOT_FAVOR_DYNMODS` |
| `memory_localevents` | `CGRP_ROOT_MEMORY_LOCAL_EVENTS` |
| `memory_recursiveprot` | `CGRP_ROOT_MEMORY_RECURSIVE_PROT` |

---

## 3.2 Top-down 约束

父的 `subtree_control` 没 `+memory`，孩子就**不能**自己变出 memory 控制器接口。

生效掩码：`cgroup_calc_subtree_ss_mask()`（`cgroup.c:1581`）。它先并上隐式控制器 `cgrp_dfl_implicit_ss_mask`，再按每个子系统的 `depends_on` 把依赖一并打开，最后和「这棵树上实际有的子系统」取交集。类型是 `u16`。

写入：`cgroup_subtree_control_write`（`:3371`）。展示：`cgroup_subtree_control_show`（`:2998`）。

写入前还有一道门 `cgroup_vet_subtree_control_enable`（`:3330`）。`:3364` 明确写着：节点上还有任务就不能给孩子开控制器，避免孩子和本节点的任务抢资源。

---

## 3.3 No internal process 约束

大意：在默认层次上，非 root、又不是 threaded / thread root 的 cgroup，如果 `subtree_control != 0`（要给孩子开控制器），**本节点不能再容纳进程**。迁移前的检查是 `cgroup_migrate_vet_dst`（`:2648`）：

```c
	/* v1 没有这条限制 */
	if (!cgroup_on_dfl(dst_cgrp))
		return 0;

	if (!cgroup_is_valid_domain(dst_cgrp->dom_cgrp))
		return -EOPNOTSUPP;

	/* 已经是或可以变成 thread root，或者已经 threaded，就不管 */
	if (cgroup_can_be_thread_root(dst_cgrp) || cgroup_is_threaded(dst_cgrp))
		return 0;

	/* no internal process：目标还有向下的控制器，就拒绝 */
	if (dst_cgrp->subtree_control)
		return -EBUSY;
```

函数注释（`:2643`）写明：除了 mixable、（可能的）thread root 和 threaded cgroup，目标上有任务时 `subtree_control` 必须是 0，免得孩子和任务抢。root 是 mixable，所以 root 可以既挂进程又给孩子开控制器。

**为什么**：避免「父节点既跑业务又给孩子限流」时，资源归属和委派模型变糊。systemd 常见布局：`system.slice` / `user.slice` / 具体 `*.service` 叶子。

---

## 3.4 创建 / 迁移 / 委托

### 创建

```text
mkdir /sys/fs/cgroup/myjob
  → kernfs → cgroup_mkdir()                 # cgroup.c:5749
       → 名字里不能有换行（否则 /proc/<pid>/cgroup 无法解析）
       → cgroup_kn_lock_live                  # 锁住还活着的父节点
       → cgroup_check_hierarchy_limits        # max_descendants / max_depth
       → cgroup_create                        # 分配 struct cgroup、挂 kernfs
       → css_populate_dir(&cgrp->self)        # 摆上 cgroup.* 文件
       → cgroup_apply_control_enable          # 按父的掩码准备各 css
       → kernfs_activate
```

### 迁移（写 procs）

```text
echo <pid> > .../cgroup.procs
  → cgroup_procs_write (:5206)               # threadgroup = true
  → __cgroup_procs_write (:5159)
       → cgroup_kn_lock_live                  # 锁目标目录
       → cgroup_procs_write_start (:2900)     # 解析 pid，按需拿线程组锁
       → cgroup_attach_permissions (:5138)
            → cgroup_procs_write_permission (:5107)
            → cgroup_migrate_vet_dst (:2648)  # 含 no-internal-process
       → cgroup_attach_task (:2868)
            → cgroup_migrate_add_src          # 记下每个源 css_set
            → cgroup_migrate_prepare_dst (:2771)
                 → find_css_set               # 旧 cset + 目标 cgroup → 新 cset
            → cgroup_migrate (:2838)
                 → cgroup_migrate_execute (:2541)
                      → ss->can_attach
                      → css_set_move_task     # 真正换 cset
                      → cgroup_freezer_migrate_task
                      → ss->attach
       → cgroup_procs_write_finish (:2957)
```

写 `cgroup.threads` 走同一条 `__cgroup_procs_write`，只是 `threadgroup = false`（`:5217`）。

`cgroup_attach_task` 的函数体（`:2868`）就是上面这条链，没有更隐藏的一步：

```c
int cgroup_attach_task(struct cgroup *dst_cgrp, struct task_struct *leader,
		       bool threadgroup)
{
	DEFINE_CGROUP_MGCTX(mgctx);
	struct task_struct *task;
	int ret = 0;

	/* 在 css_set_lock 下把源 cset 登记进迁移上下文 */
	spin_lock_irq(&css_set_lock);
	rcu_read_lock();
	task = leader;
	do {
		cgroup_migrate_add_src(task_css_set(task), dst_cgrp, &mgctx);
		if (!threadgroup)
			break;
	} while_each_thread(leader, task);
	rcu_read_unlock();
	spin_unlock_irq(&css_set_lock);

	ret = cgroup_migrate_prepare_dst(&mgctx);
	if (!ret)
		ret = cgroup_migrate(leader, threadgroup, &mgctx);

	cgroup_migrate_finish(&mgctx);

	if (!ret)
		TRACE_CGROUP_PATH(attach_task, dst_cgrp, leader, threadgroup);

	return ret;
}
```

调用时必须已经拿着 `cgroup_mutex`；迁整个线程组时还要拿着 `cgroup_threadgroup_rwsem`。

### 委托（delegation）

把某子树目录的写权限交给非特权用户。源码里带 `CFTYPE_NS_DELEGATABLE` 的文件只有这些：

| 文件 | 位置 |
|---|---|
| `cgroup.procs` | `cgroup.c:5233` |
| `cgroup.threads` | `:5243` |
| `cgroup.subtree_control` | `:5256` |
| `memory.oom.group` | `mm/memcontrol.c:6902` |
| `memory.reclaim` | `mm/memcontrol.c:6908` |

`/sys/kernel/cgroup/delegate` 把这张名单列出来（`delegate_show`，`cgroup.c:7082`）。

挂上 `nsdelegate` 之后，cgroup namespace 的根成了委托边界：namespace 里面不能改边界外的控制器文件。权限检查在 `cgroup_attach_permissions` → `cgroup_procs_write_permission`（`:5107`）。容器常用 **cgroup namespace** + 委托目录，使容器内只能在自己的子树里搬家。语义以 `cgroup-v2.rst` 2-5 为准。

---

## 3.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_mkdir` | `cgroup.c:5749` | 创建子 cgroup |
| `cgroup_create` | `:5609`（`static`） | 分配节点、挂 kernfs |
| `cgroup_check_hierarchy_limits` | `:5726` | 检查 `max_descendants` / `max_depth` |
| `cgroup_apply_control_enable` | `:3192` | 按掩码创建或显示 css |
| `cgroup_subtree_control_write` | `:3371` | 写 `subtree_control` |
| `cgroup_subtree_control_show` | `:2998` | 读 `subtree_control` |
| `cgroup_calc_subtree_ss_mask` | `:1581` | 算有效子系统掩码（含依赖） |
| `cgroup_procs_write` | `:5206` | 写 `cgroup.procs` |
| `cgroup_threads_write` | `:5217` | 写 `cgroup.threads` |
| `__cgroup_procs_write` | `:5159` | 两条写路径的公共实现 |
| `cgroup_procs_write_start` | `:2900` | 解析 PID、按需锁线程组 |
| `cgroup_procs_write_finish` | `:2957` | 放任务引用、解线程组锁 |
| `cgroup_procs_write_permission` | `:5107` | 写权限与 namespace 边界 |
| `cgroup_attach_permissions` | `:5138` | 权限 + `cgroup_migrate_vet_dst` |
| `cgroup_migrate_vet_dst` | `:2648` | 目标能不能收任务 |
| `cgroup_attach_task` | `:2868` | 执行迁移 |
| `cgroup_migrate_prepare_dst` | `:2771` | 准备目标 css_set |
| `cgroup_migrate` | `:2838` | 收集任务并调用执行器 |
| `cgroup_migrate_execute` | `:2541` | `can_attach` / 换 cset / `attach` |
| `cgroup_migrate_add_src` | `:2724` | 登记源 css_set |
| `cgroup_migrate_finish` | `:2679` | 丢掉迁移上下文里的引用 |
| `cgroup_kn_lock_live` | `:1655` | 对 kernfs 节点取「还活着」的 cgroup 并加锁 |
| `cgroup_kn_unlock` | `:1623` | 配对解锁 |
| `cgroup_kill` / `__cgroup_kill` | `:3994` / `:3968` | `cgroup.kill` 的下推与发信号 |
| `cgroup_freeze_write` | `:3943` | `cgroup.freeze` 的写入口 |

`cgroup_create` 是 `static`，定义在 `:5609`，由 `cgroup_mkdir` 在 `:5767` 调用。

---

## 3.6 操作口诀

1. 先在父上 `echo +cpu +memory > cgroup.subtree_control`  
2. 再 `mkdir` 孩子——孩子才会出现对应 `cpu.*` / `memory.*`  
3. 把进程写进**叶子**的 `cgroup.procs`  
4. 改限额写控制器文件（`memory.max`、`cpu.max` …），不是写 `subtree_control`

可选 shell 示意见 [demos/](./demos/)。那些命令是 RECIPE，没有在本笔记里重新执行。

---

*对照：`kernel/cgroup/cgroup.c`。*
