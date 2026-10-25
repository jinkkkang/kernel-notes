# 4. 与进程生命周期的挂钩

**结论**：fork 时 cgroup 要在子任务「曝光」前定好 `css_set`（可 `CLONE_INTO_CGROUP` 直接进目标组）；exit 时 `cgroup_exit` 先把任务从 `css_set->tasks` 挪到 `dying_tasks`，并调用各控制器的 `exit`。真正摘掉 `cg_list`、调用 `ss->release` 在 `cgroup_release`；放下 `css_set` 引用在 `cgroup_free`。进程章讲 `copy_process` / `do_exit`，本章钉 **cgroup 插销钉在哪**。

> 衔接：[process-management/04-fork-clone.md](../process-management/04-fork-clone.md)、[07-exit-reap.md](../process-management/07-exit-reap.md)  
> 主文件：`kernel/cgroup/cgroup.c`、`kernel/fork.c`、`kernel/exit.c`、`kernel/cgroup/namespace.c`

---

## 4.1 fork 路径

`kernel/fork.c` 的 `copy_process` 里，和 cgroup 有关的调用点是这几处：

```text
copy_process
  → cgroup_fork(child)                    # fork.c:2439
       # 先把 child->cgroups 指到 init_css_set，cg_list 清空
       # 这只是占位，真正的 cset 还没定
  … 复制地址空间、文件、信号 …
  → cgroup_can_fork(child, args)          # fork.c:2606
       → cgroup_css_set_fork(kargs)       # cgroup.c:6435
       → 各 ss->can_fork（如 pids）
       # 失败则 cgroup_css_set_put_fork，fork 走 bad_fork_put_pidfd
  → sched_cgroup_fork(child, args)        # fork.c:2619；实现在 kernel/sched/core.c:4804
       # cgroup 已经钉住之后，把 child 放上正确的 task_group / 运行队列
  … 挂进任务表，对外可见 …
  → cgroup_post_fork(child, args)         # fork.c:2736
       → css_set_move_task 挂到 cset->tasks
       → 各 ss->fork
       → CLONE_NEWCGROUP 时改写 ns->root_cset
  # 若 can_fork 之后失败：cgroup_cancel_fork  # fork.c:2751
```

`sched_cgroup_fork` 不属于 cgroup core，但插在 `cgroup_can_fork` 和 `cgroup_post_fork` 中间：`CONFIG_CGROUP_SCHED` 时用 `kargs->cset->subsys[cpu_cgrp_id]` 取出 `task_group`，写进 `p->sched_task_group`（`core.c:4816`）。

### `cgroup_fork`（`cgroup.c:6374`）

只有两行：`RCU_INIT_POINTER(child->cgroups, &init_css_set)`，以及 `INIT_LIST_HEAD(&child->cg_list)`。此时孩子还没有自己的 css_set。

### `cgroup_css_set_fork`（`:6435`）

- 默认（没有 `CLONE_INTO_CGROUP`）：`get_css_set` 父进程当前的 css_set，放进 `kargs->cset`。同时记下 `dfl_cgrp->kill_seq`，留给 `cgroup_post_fork` 判断组是不是正在被 kill。
- 若 `CLONE_INTO_CGROUP`（`include/uapi/linux/sched.h:38`，值 `0x200000000ULL`）：
  - 先 `cgroup_lock()`，再 `cgroup_threadgroup_change_begin()`（普通路径只拿线程组读锁，这条路径锁序不同）；
  - 用 `kargs->cgroup` 里的文件描述符 `fget_raw`，再 `cgroup_get_from_file` 得到目标 cgroup（必须是 cgroup2 目录，不能是 `cgroup.procs` 那个文件）；
  - `cgroup_may_write` 检查调用者对目标目录可写；
  - `cgroup_attach_permissions` 做和写 `cgroup.procs` 同一套权限与 no-internal-process 检查；
  - `find_css_set(父 cset, 目标)` 得到孩子要用的新 cset，记到 `kargs->cset`，目标 cgroup 记到 `kargs->cgrp`。

### `cgroup_can_fork` / `cgroup_cancel_fork` / `cgroup_post_fork`

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_fork` | `cgroup.c:6374` | 把孩子暂时指到 `init_css_set` |
| `cgroup_css_set_fork` | `:6435` | 准备孩子真正的 css_set |
| `cgroup_css_set_put_fork` | `:6542` | 放掉准备过程中拿的 cset / cgroup 引用，并解线程组锁 |
| `cgroup_can_fork` | `:6575` | 调 `cgroup_css_set_fork`，再按 `have_canfork_callback` 调 `ss->can_fork` |
| `cgroup_cancel_fork` | `:6614` | `can_fork` 已经成功、但 fork 后面失败时：`ss->cancel_fork` + `cgroup_css_set_put_fork` |
| `cgroup_post_fork` | `:6635` | 挂链、`ss->fork`、处理 FREEZE / `kill_seq`、`CLONE_NEWCGROUP` |
| `sched_cgroup_fork` | `kernel/sched/core.c:4804` | 把孩子放上 cpu 控制器对应的 `task_group` |

`cgroup_post_fork`（`:6635`）做的事：

- `cset->nr_tasks++`，`css_set_move_task(child, NULL, cset, false)` 把孩子链进 `cset->tasks`（`child->pid` 为 0 的 init 任务例外，只放掉引用）；
- 非内核线程、且目标组 `CGRP_FREEZE` 置位时，给孩子打上 `JOBCTL_TRAP_FREEZE`（`include/linux/sched/jobctl.h:34`，位 23）；
- 若 `kargs->kill_seq` 和组上现在的 `kill_seq` 不一致，说明准备 cset 期间有人写了 `cgroup.kill`，补一个 `SIGKILL`；
- 按 `have_fork_callback` 调 `ss->fork`；
- `CLONE_NEWCGROUP`：把新 namespace 的 `root_cset` 换成孩子实际所在的 cset；
- 最后 `cgroup_css_set_put_fork`，解开 `cgroup_css_set_fork` 拿着的锁和临时引用。

---

## 4.2 exit / 回收路径

三条入口的名字是 `cgroup_exit`、`cgroup_release`、`cgroup_free`。

| 符号 | 位置 | 谁调用 | 作用 |
|---|---|---|---|
| `cgroup_exit` | `cgroup.c:6731` | `kernel/exit.c:894`（`do_exit` 后段） | 从 `tasks` 挪到 `dying_tasks`，`nr_tasks--`，再调 `ss->exit` |
| `cgroup_release` | `cgroup.c:6761` | `kernel/exit.c:257`（`release_task`） | 调 `ss->release`，再从 `dying_tasks` 摘掉 `cg_list` |
| `cgroup_free` | `cgroup.c:6776` | `kernel/fork.c:982`（`__put_task_struct`） | `put_css_set`，放下 css_set 引用 |

`cgroup_exit`（`:6731`）在 `css_set_lock` 下：

```text
css_set_move_task(tsk, cset, NULL, false)   # 离开 tasks / mg_tasks
list_add_tail(&tsk->cg_list, &cset->dying_tasks)
cset->nr_tasks--
若是 deadline 任务：dec_dl_tasks_cs
若 cgroup 处于 CGRP_FREEZE：cgroup_update_frozen
解锁后：按 have_exit_callback 调 ss->exit
```

`cgroup_release` 不再碰引用计数，只通知控制器「任务的结构要拆了」（pids 在这里 uncharge），然后 `list_del_init(&task->cg_list)`。

`cgroup_free` 只有 `put_css_set(task_css_set(task))`。任务结构最后消失时，css_set 才可能被释放。

**迁移**：用户态写 `cgroup.procs` → `cgroup_attach_task`（见 [03](./03-hierarchy-control.md)）；控制器用 `can_attach` / `attach` 更新自己的记账（如 pids 换组、cpuset 改亲和）。

---

## 4.3 cgroup namespace

结构体在 `include/linux/cgroup.h:773`：

```c
struct cgroup_namespace {
	struct ns_common	ns;		/* 通用 ns 头：inum、ops、引用计数 */
	struct user_namespace	*user_ns;	/* 拥有这个 ns 的 user namespace */
	struct ucounts		*ucounts;	/* UCOUNT_CGROUP_NAMESPACES 记账 */
	struct css_set		*root_cset;	/* 本 ns 里路径从哪个 cset 起算 */
};
```

`nsproxy->cgroup_ns`（`include/linux/nsproxy.h:40`）指向它。初始值是 `init_cgroup_ns`（`cgroup.c:234`），`root_cset` 为 `&init_css_set`。

| 对比项 | pid namespace | cgroup namespace |
|---|---|---|
| 标志 | `CLONE_NEWPID`（`include/uapi/linux/sched.h:32`，`0x20000000`） | `CLONE_NEWCGROUP`（同文件 `:28`，`0x02000000`） |
| 主要效果 | PID 号从 1 重编 | **路径视图**从 `root_cset` 起算；不创造新的资源树 |
| 实现 | `kernel/pid.c` 等 | `kernel/cgroup/namespace.c` |

`copy_cgroup_ns`（`namespace.c:50`）：

- 没有 `CLONE_NEWCGROUP`：`get_cgroup_ns(old_ns)`，父子共享；
- 有该标志：要求对 `user_ns` 有 `CAP_SYS_ADMIN`，`inc_cgroup_namespaces` 记账，在 `css_set_lock` 下把**当前任务的 css_set** 抓成新 ns 的 `root_cset`。

`free_cgroup_ns`（`:40`）按相反顺序放掉 `root_cset`、ucount、user ns 和 inum。

`setns` 安装：`cgroupns_install`（`:98`）。调用者和目标 ns 的 user ns 都要 `CAP_SYS_ADMIN`。换的是 `nsproxy->cgroup_ns`，不移动任务本身的 cgroup。

`cgroup_post_fork` 里若带了 `CLONE_NEWCGROUP`，还会把这个新 ns 的 `root_cset` 从「创建者当时的 cset」改成「孩子最终的 cset」（两者在 `CLONE_INTO_CGROUP` 时可能不同）。

---

## 4.4 与进程章对照表

| 进程章概念 | 本章落点 |
|---|---|
| `kernel_clone` / `copy_process` | `cgroup_fork` → `cgroup_can_fork` → `cgroup_post_fork` |
| `CLONE_INTO_CGROUP` / `clone3` 的 cgroup fd | `cgroup_css_set_fork` |
| `CLONE_NEWCGROUP` | `copy_cgroup_ns` + `cgroup_post_fork` 更新 `root_cset` |
| `do_exit` | `cgroup_exit`（`exit.c:894`） |
| `release_task` | `cgroup_release`（`exit.c:257`） |
| `__put_task_struct` | `cgroup_free`（`fork.c:982`） |
| 写 `/proc` 看不见 cgroup 限额 | 限额在 `/sys/fs/cgroup`；`/proc/<pid>/cgroup` 只报**路径** |

---

*对照：`kernel/cgroup/cgroup.c`、`namespace.c`、`kernel/fork.c`、`kernel/exit.c`。*
