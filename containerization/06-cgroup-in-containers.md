# 6. 容器里的 cgroup

**结论**：cgroup 把容器进程挂到一棵已经存在的树上，并让控制器按这棵子树记账。容器运行时要么在进程可见之前用 `clone3` 的 `CLONE_INTO_CGROUP` 指定目标组，要么等进程起来后写 `cgroup.procs`。`CLONE_NEWCGROUP` 只改路径从哪一段算起，不新建限额。对象模型、cpu/memory/io 控制器、PSI 都在 [cgroup/](../cgroup/README.md)，这里只写容器创建时用到的挂钩。

> 内核侧以 [cgroup/04](../cgroup/04-lifecycle-hooks.md) 和 [cgroup/03](../cgroup/03-hierarchy-control.md) 为准。行号下面再标一次，避免两章各说各的。

---

## 6.1 两条挂法

**写 `cgroup.procs`**。父进程 `mkdir` 出子 cgroup，再把容器 init 的 pid 写进去。写路径是 `cgroup_procs_write`（`kernel/cgroup/cgroup.c:5206`）→ `__cgroup_procs_write`（`:5159`）→ `cgroup_attach_task`（`:2868`）。权限和 no-internal-process 检查在 `cgroup_attach_permissions`（`:5138`）。这是 runc v1.5.2 创建路径上 `manager.Apply` 走的路，见 [09](./09-runc-create-path.md)：Apply 发生在 nsexec 继续 clone 之前，后面的 stage-1/stage-2 从已经在目标组里的父任务 fork，所以孩子生下来就在这组。

**`CLONE_INTO_CGROUP`**。值 `0x200000000ULL`（`include/uapi/linux/sched.h:38`），只有 `clone3` 能带。`clone_args.cgroup` 是目标 cgroup 目录的 fd。`cgroup_css_set_fork`（`cgroup.c:6435`）在孩子对外可见之前 `find_css_set`，`cgroup_post_fork`（`:6635`）再把孩子链上这个 cset。完整步骤、和 `cgroup.procs` 同一套权限检查，见 cgroup 章 04。runc 的 **exec 进已有容器**（`setnsProcess`）在 cgroup v2 上走这条：`prepareCgroupFD`（`libcontainer/process_linux.go:372`）打开目录 fd，设 `SysProcAttr.UseCgroupFD`。创建容器的 init 进程在 v1.5.2 里仍用 Apply，不用这条。

两条路最后都是同一个 `css_set`。差别只在「任务第一次曝光时已经在目标组」还是「先出现在父组，再迁移」。迁移窗口里如果父组限额更松，子进程能在窗口里多花资源。`CLONE_INTO_CGROUP` 把窗口消掉。

---

## 6.2 委托

容器运行时通常自己留在父 cgroup，把子目录的写权限交给容器内的管理者（systemd、kubelet 的容器侧，或 rootless 的用户）。v2 里带 `CFTYPE_NS_DELEGATABLE` 的文件，cgroup 章 03 已经列全：`cgroup.procs`、`cgroup.threads`、`cgroup.subtree_control`，以及 memory 的 `memory.oom.group`、`memory.reclaim`。`/sys/kernel/cgroup/delegate` 由 `delegate_show`（`cgroup.c:7082`）印出这张名单。

挂载选项 `nsdelegate` 对应 `CGRP_ROOT_NS_DELEGATE`（cgroup 章 03 的表）。打开之后，cgroup namespace 的根是委托边界：ns 里面的进程不能改边界外的控制器文件。检查在 `cgroup_procs_write_permission`（`cgroup.c:5107`）。本机 cgroup2 的挂载选项里有 `nsdelegate`，见 [11](./11-observability.md)。

no-internal-process 约束不变：非 root、非 threaded 的节点一旦 `subtree_control` 非空，就不能再挂进程（`cgroup_migrate_vet_dst`，`:2648`）。容器的叶子组才能放进程；中间节点只向下授权控制器。

---

## 6.3 cgroup namespace

```c
/* include/linux/cgroup.h:773 ，cgroup 章 04 同一结构 */
struct cgroup_namespace {
	struct ns_common	ns;
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	struct css_set		*root_cset; /* 本 ns 里路径从哪个 cset 起算 */
};
```

`copy_cgroup_ns`（`kernel/cgroup/namespace.c:50`）在 `CLONE_NEWCGROUP` 时把当前任务的 css_set 抓成 `root_cset`。`cgroup_post_fork` 若同时有 `CLONE_INTO_CGROUP`，会把这个 `root_cset` 改成孩子最终的 cset（两者可以不同）。`/proc/self/cgroup` 从 `root_cset` 往下印，容器里看到的路径以自己的组为 `0::/`，宿主机仍看到完整路径。

`setns` 的 `cgroupns_install`（`kernel/cgroup/namespace.c:98`）换的是 `nsproxy->cgroup_ns`。任务仍留在原来的 cgroup，`/proc/self/cgroup` 的路径起点改成新的 `root_cset`。

v1 的多棵树和 v2 的统一层次不要混在一次容器配置里当同一套语义。runc 在 v2 统一模式和 v1 之间走不同的 Apply。混用时的现象见 [12](./12-perf-and-pitfalls.md)。

---

## 6.4 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `kernel/cgroup/cgroup.c` | `cgroup_css_set_fork` | 6435 | `CLONE_INTO_CGROUP` 准备 cset |
| `kernel/cgroup/cgroup.c` | `cgroup_post_fork` | 6635 | 挂链，并在 `CLONE_NEWCGROUP` 时改 `root_cset` |
| `kernel/cgroup/cgroup.c` | `cgroup_procs_write` | 5206 | 写 pid 迁移 |
| `kernel/cgroup/cgroup.c` | `cgroup_attach_task` | 2868 | 迁移执行 |
| `kernel/cgroup/cgroup.c` | `cgroup_procs_write_permission` | 5107 | 含 nsdelegate 边界 |
| `kernel/cgroup/cgroup.c` | `delegate_show` | 7082 | `/sys/kernel/cgroup/delegate` |
| `kernel/cgroup/namespace.c` | `copy_cgroup_ns` | 50 | 新建 cgroup ns |
| `kernel/cgroup/namespace.c` | `cgroupns_install` | 98 | `setns` |
| `include/uapi/linux/sched.h` | `CLONE_INTO_CGROUP` | 38 | `0x200000000ULL` |

---

**导航**：[← net ns](./05-net-ns.md) · [目录](./README.md) · [overlayfs →](./07-overlayfs.md)
