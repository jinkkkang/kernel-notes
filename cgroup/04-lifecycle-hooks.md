# 4. 与进程生命周期的挂钩

**结论**：fork 时 cgroup 要在子任务「曝光」前定好 `css_set`（可 `CLONE_INTO_CGROUP` 直接进目标组）；exit 时先跑各控制器 `exit`，真正从 css_set 摘链在 `cgroup_task_dead`。进程章讲 `copy_process` / `do_exit`，本章钉 **cgroup 插销钉在哪**。

> 衔接：[process-management/04-fork-clone.md](../process-management/04-fork-clone.md)、[07-exit-reap.md](../process-management/07-exit-reap.md)  
> 主文件：`kernel/cgroup/cgroup.c`、`kernel/fork.c`、`kernel/exit.c`、`kernel/cgroup/namespace.c`

---

## 4.1 fork 路径

`kernel/fork.c` 在 `copy_process` 里（约 `:2426`、`:2582`）：

```text
copy_process
  → cgroup_can_fork(child, args)     # fork.c:2426
       → cgroup_css_set_fork(kargs)  # cgroup.c:6863
       → 各 ss->can_fork（如 pids）
  … 失败则 cgroup_cancel_fork
  → … 其它复制 …
  → cgroup_post_fork(child, args)    # fork.c:2582
       → css_set_move_task 挂到 cset
       → 各 ss->fork
       → CLONE_NEWCGROUP 时更新 ns->root_cset
```

### `cgroup_css_set_fork`（`:6863`）

- 默认：子进程继承父的 `css_set`（`get_css_set`）。
- 若 `CLONE_INTO_CGROUP`（`include/uapi/linux/sched.h:38`，`1ULL<<33`）：
  - 从 `kargs->cgroup` 文件描述符解析目标 cgroup；
  - 做写权限与 `cgroup_attach_permissions`；
  - `find_css_set(旧, 目标)` 得到新 cset；
  - 持 `cgroup_mutex` + `cgroup_threadgroup_rwsem`（与普通路径锁阶略有不同）。

### `cgroup_can_fork` / `cgroup_cancel_fork` / `cgroup_post_fork`

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_css_set_fork` | `cgroup.c:6863` | 准备子进程 css_set |
| `cgroup_can_fork` | `:6997` | 调 css_set_fork + `ss->can_fork` |
| `cgroup_cancel_fork` | `:7036` | fork 失败回滚 |
| `cgroup_post_fork` | `:7057` | 挂链、`ss->fork`、处理 FREEZE/kill_seq、`CLONE_NEWCGROUP` |

`cgroup_post_fork` 要点（`:7057` 起）：

- 把 child 链进 `cset->tasks`（`css_set_move_task`）；
- 若目标组处于 freeze，给 child 打 `JOBCTL_TRAP_FREEZE`；
- 若 `kill_seq` 变化（组正在被 kill），补 `SIGKILL`；
- `CLONE_NEWCGROUP`：让新 ns 的 `root_cset` 指向子进程实际 cset。

---

## 4.2 exit / 回收路径

v7.3 命名是 **`cgroup_task_*`**（不是老资料里的 `cgroup_exit` 三件套原名）：

| 符号 | 位置 | 谁调用 | 作用 |
|---|---|---|---|
| `cgroup_task_exit` | `cgroup.c:7153` | `kernel/exit.c:990` | 调各 `ss->exit` |
| `cgroup_task_dead` | `cgroup.c`（RT 下经 irq_work） | 切换收尾路径 | `do_cgroup_task_dead`：从 css_set **摘链** |
| `cgroup_task_release` | `:7241` | `exit.c:262` | 各 `ss->release` |
| `cgroup_task_free` | `:7251` | 释放 task 时 | 放掉 css_set 引用等 |

`cgroup_task_exit` 本身很瘦：只遍历 `have_exit_callback` 调 `ss->exit`。真正 `css_set_move_task(..., NULL)` 在 `do_cgroup_task_dead`（`:7174` 一带）。

**迁移**：用户态写 `cgroup.procs` → `cgroup_attach_task`（见 [03](./03-hierarchy-control.md)）；控制器用 `can_attach`/`attach` 更新自己的记账（如 pids 换组、cpuset 改亲和）。

---

## 4.3 cgroup namespace

`include/linux/cgroup_namespace.h:7`：

```c
struct cgroup_namespace {
	struct ns_common	ns;           /* 中文：通用 ns 头 */
	struct user_namespace	*user_ns;   /* 中文：关联 user ns */
	struct ucounts		*ucounts;   /* 中文：数量限制记账 */
	struct css_set          *root_cset; /* 中文：本 ns 内路径解析的「根」cset */
};
```

| 对比项 | pid namespace | cgroup namespace |
|---|---|---|
| 标志 | `CLONE_NEWPID` | `CLONE_NEWCGROUP`（`0x02000000`） |
| 主要效果 | PID 号从 1 重编 | **路径视图**从 `root_cset` 起算；不创造新的资源树 |
| 实现 | `kernel/pid.c` 等 | `kernel/cgroup/namespace.c`：`copy_cgroup_ns`（`:48`）、`free_cgroup_ns`（`:36`） |

`copy_cgroup_ns`：无 `CLONE_NEWCGROUP` 则共享旧 ns；有则要求 `CAP_SYS_ADMIN`，把 **当前任务的 css_set** 抓成新 ns 的 `root_cset`。

`setns` 安装：`cgroupns_install`（同文件）。`nsproxy->cgroup_ns` 指向它。

---

## 4.4 与进程章对照表

| 进程章概念 | 本章落点 |
|---|---|
| `kernel_clone` / `copy_process` | `cgroup_can_fork` → `cgroup_post_fork` |
| `CLONE_INTO_CGROUP` / `clone3` 的 cgroup fd | `cgroup_css_set_fork` |
| `CLONE_NEWCGROUP` | `copy_cgroup_ns` + `cgroup_post_fork` 更新 `root_cset` |
| `do_exit` | `cgroup_task_exit` / `cgroup_task_dead` / `cgroup_task_release` |
| 写 `/proc` 看不见 cgroup 限额 | 限额在 `/sys/fs/cgroup`；`/proc/<pid>/cgroup` 只报**路径** |

---

*对照：`kernel/cgroup/cgroup.c`、`namespace.c`、`kernel/fork.c`、`kernel/exit.c`。*
