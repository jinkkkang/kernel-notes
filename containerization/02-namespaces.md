# 2. 命名空间、nsproxy、unshare 与 setns

**结论**：v6.6.160 能单独拆开的命名空间是八个：mnt、uts、ipc、user、pid、net、cgroup、time。除了 user ns，当前任务的指针都收在 `struct nsproxy` 里。pid 和 time 还各有一份「子进程将用」的指针，调用者自己的层不跟着 `unshare` 立刻变。`/proc/<pid>/ns/` 里的符号链接是 nsfs 上的 inode，`setns` 拿这个 fd 安装。

> `struct nsproxy` 在 `include/linux/nsproxy.h:31`。复制与 `setns` 在 `kernel/nsproxy.c`。`/proc` 目录项在 `fs/proc/namespaces.c:15`。

进程章已经把 `nsproxy` 嵌在 `task_struct` 里讲过（[02](../process-management/02-task-struct.md)）。这里把它当成容器的开关板：哪些指针在一次 `unshare` 里换掉，换的顺序是什么。

---

## 2.1 `ns_common` 与 `nsproxy`

每个具体命名空间结构的末尾附近都嵌一块 `ns_common`（`include/linux/ns_common.h:9`）。`/proc/<pid>/ns/*` 的 inode 号就是 `ns.inum`。

```c
/* include/linux/ns_common.h:9 */
struct ns_common {
	atomic_long_t stashed;                 /* nsfs dentry 的缓存 */
	const struct proc_ns_operations *ops;  /* 名字、CLONE_* 类型、get/install */
	unsigned int inum;                     /* proc_alloc_inum 分的号，nsfs 的 i_ino */
	refcount_t count;                      /* 这个命名空间自己的引用 */
};
```

`proc_ns_operations`（`include/linux/proc_ns.h:16`）的 `type` 就是对应的 `CLONE_NEW*`。`setns` 用它核对用户传入的 flags（`nsproxy.c:558`）。

```c
/* include/linux/nsproxy.h:31 */
struct nsproxy {
	refcount_t count;                          /* 多少个任务拿着这份 nsproxy */
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children; /* 以后 fork 的子进程用的 pid ns */
	struct net *net_ns;
	struct time_namespace *time_ns;            /* 本任务看到的 time ns */
	struct time_namespace *time_ns_for_children;
	struct cgroup_namespace *cgroup_ns;
};
```

头文件 `:19` 写明：pid ns 的例外是用 `task_active_pid_ns` 看自己，这里的指针只给孩子。time ns 做成了两个字段：`create_new_namespaces` 把 `time_ns` 直接 `get_time_ns` 旧的（`nsproxy.c:122`），`time_ns_for_children` 才按 `CLONE_NEWTIME` 复制（`:116`）。没有 `CLONE_VM` 时，`copy_namespaces` 再调 `timens_on_fork`（`:183`），让新任务在 fork 路径上进入孩子那一层。`exec_task_namespaces`（`:259`）在两份指针不一致时给当前任务换一份，这是 exec 把 time ns 补上的地方。

user ns 不在这张表里。它在 `cred->user_ns`。`copy_namespaces` 的快速路径掩码（`:157`）也没有 `CLONE_NEWUSER`。

`init_nsproxy`（`nsproxy.c:32`）把 uts/pid/net/cgroup/time 指到各 `init_*`。`mnt_ns` 初始是 `NULL`，挂载树起来之后再填。

---

## 2.2 八个 `CLONE_NEW*`

数值在 `include/uapi/linux/sched.h`，进程章 [01](../process-management/01-overview.md) 有全表。和命名空间有关的是这些：

| 标志 | 值 | 放在哪 | 复制函数 |
|---|---|---|---|
| `CLONE_NEWNS` | `0x00020000` | `nsproxy->mnt_ns` | `copy_mnt_ns`（`fs/namespace.c:3849`） |
| `CLONE_NEWCGROUP` | `0x02000000` | `nsproxy->cgroup_ns` | `copy_cgroup_ns`（`kernel/cgroup/namespace.c:50`） |
| `CLONE_NEWUTS` | `0x04000000` | `nsproxy->uts_ns` | `copy_utsname` |
| `CLONE_NEWIPC` | `0x08000000` | `nsproxy->ipc_ns` | `copy_ipcs` |
| `CLONE_NEWUSER` | `0x10000000` | `cred->user_ns` | `create_user_ns`（`kernel/user_namespace.c:82`） |
| `CLONE_NEWPID` | `0x20000000` | `pid_ns_for_children` | `copy_pid_ns`（`kernel/pid_namespace.c:145`） |
| `CLONE_NEWNET` | `0x40000000` | `nsproxy->net_ns` | `copy_net_ns`（`net/core/net_namespace.c:495`） |
| `CLONE_NEWTIME` | `0x00000080` | `time_ns_for_children` | `copy_time_ns` |

`CLONE_NEWTIME` 的值和 `CSIGNAL`（flags 低 8 位）重叠。旧的 `clone` 把退出信号塞在低 8 位，所以这个标志只给 `unshare` 和 `clone3`。`copy_process` 的检查从 `kernel/fork.c:2269` 起：`CLONE_NEWNS` 与 `CLONE_FS` 同时出现返回 `-EINVAL`（`:2269`），`CLONE_NEWUSER` 与 `CLONE_FS` 同样（`:2272`）。`CLONE_NEWIPC` 与 `CLONE_SYSVSEM` 同时出现，`copy_namespaces` 返回 `-EINVAL`（`nsproxy.c:175`）。

新建任何带在掩码里的 ns，都要在目标 user ns 里有 `CAP_SYS_ADMIN`，否则 `copy_namespaces` 返回 `-EPERM`（`:165`）。

---

## 2.3 `create_new_namespaces` 的顺序

`kernel/nsproxy.c:67`。失败按相反顺序放引用。顺序本身就是所有权顺序：后面的 ns 记录前面已经建好的 user ns（调用者传入的 `user_ns`）。

```text
create_nsproxy                         :52     kmem_cache，引用计数 1
copy_mnt_ns                            :78
copy_utsname                           :84
copy_ipcs                              :90
copy_pid_ns                            :96     写入 pid_ns_for_children
copy_cgroup_ns                         :103
copy_net_ns                            :110
copy_time_ns                           :116    写入 time_ns_for_children
get_time_ns(旧 time_ns)                :122    本任务的 time_ns 先沿用旧的
```

`copy_namespaces`（`:151`）在标志全空、并且（共享 mm，或者 time 的两份指针已经相同）时，只 `get_nsproxy` 返回 0。有任何一个 `CLONE_NEW*`（user 除外，见上）就走 `create_new_namespaces`，成功后 `tsk->nsproxy = new_ns`（`:186`）。

`free_nsproxy`（`:190`）按 mnt、uts、ipc、pid、time、cgroup、net 的顺序放下。cgroup 和 net 的 `put` 没有判空：初始 `init_nsproxy` 在对应配置打开时总会填上。

---

## 2.4 `unshare`

`ksys_unshare`（`kernel/fork.c:3394`），系统调用 `sys_unshare` 在 `:3516`。它改的是当前任务。开头先把连带标志补上（`:3407`）：

- `CLONE_NEWUSER` 附带 `CLONE_THREAD | CLONE_FS`
- `CLONE_NEWNS` 附带 `CLONE_FS`
- `CLONE_VM` 附带 `CLONE_SIGHAND`，`CLONE_SIGHAND` 再附带 `CLONE_THREAD`

然后：

```text
check_unshare_flags
unshare_fs / unshare_fd
unshare_userns                         :3442   可能做出一份还没 commit 的 cred
unshare_nsproxy_namespaces             :3444   用 new_cred->user_ns 当新 ns 的属主
switch_task_namespaces                 :3468   先换 nsproxy
commit_creds                           :3491   再安装 user ns
perf_event_namespaces                  :3497
```

`unshare_nsproxy_namespaces`（`nsproxy.c:213`）自己再查一次 `CAP_SYS_ADMIN`。user ns 先建好、cred 后提交：这段窗口里新的 mnt/net 已经记着新 user ns 的指针，能力要等 `commit_creds` 才变成新 ns 里的全能力（`set_cred_user_ns`，`user_namespace.c:43`）。runc 因此把 user ns 单独 `unshare`，等父进程写完 uid_map 再 `unshare` 其余的，见 [09](./09-runc-create-path.md)。

---

## 2.5 `setns`

`SYSCALL_DEFINE2(setns)` 在 `nsproxy.c:546`。fd 有两种：

- nsfs 文件（`proc_ns_file`）：从 inode 取出 `ns_common`。用户若传了 flags，必须等于 `ns->ops->type`，否则 `-EINVAL`（`:558`）。flags 随即被改写成这个 type。
- pidfd：`check_setns_flags`（`:276`）要求 flags 非空，且只含八个 `CLONE_NEW*`。然后 `validate_nsset`（`:375`）去目标任务上抓对应的 ns。

安装分三步，注释写明这是 `unshare` 的逆操作，全部成功才到「不能回头」的 `commit_nsset`（`:369`）：

```text
prepare_nsset          :331   先按 flags=0 复制一份 nsproxy（全部共享旧的）
validate_ns            :363   调每个 ns 的 ops->install，把要加入的 ns 填进这份副本
commit_nsset           :512   提交 cred、fs 的 root/pwd、ipc 的 exit_sem、timens_commit
                              最后 switch_task_namespaces
```

`validate_nsset` 的安装顺序（`:431` 起）：user、mnt、uts、ipc、pid、cgroup、net、time。user 在最前，因为后面的 `install` 用新 user ns 做权限检查。

`commit_nsset` 里，只有「flags 含 `CLONE_NEWNS` 并且还含别的」时才把临时 `fs_struct` 的 root/pwd 写回当前任务（`:526`）。单独 `setns` 进一个 mnt ns 时，`prepare_nsset` 直接借用当前的 `fs`（`:347`），`install` 就地改它。

`ptrace_may_access(..., PTRACE_MODE_READ_REALCREDS)` 挡在 pidfd 路径上（`:392`）。nsfs fd 路径不抓目标任务，权限在各个 `ops->install` 里。

---

## 2.6 `/proc/<pid>/ns/`

`fs/proc/base.c:3286` 把 `"ns"` 登记成目录。目录内容来自 `ns_entries[]`（`fs/proc/namespaces.c:15`），配置打开时的顺序是：

```text
net, uts, ipc, pid, pid_for_children, user, mnt, cgroup, time, time_for_children
```

`proc_ns_get_link`（`:42`）调用 `ns_get_path`（`fs/nsfs.c:137`），得到 nsfs 上的 dentry。`nsfs_show_path`（`fs/nsfs.c:252`）印成 `名字:[inode]`，所以 `readlink` 看到的是 `mnt:[4026532350]` 这种字符串。`stat -L` 拿到的 inode 就是这个数字；不对链接做跟随的 `stat` 拿到的是 proc 自己的符号链接 inode。实测见 [11](./11-observability.md)。

nsfs 的魔数是 `NSFS_MAGIC`（`include/uapi/linux/magic.h:94`），值 `0x6e736673`。`stat -f` 的类型名是 `nsfs`。

`pid_for_children` 读的是 `nsproxy->pid_ns_for_children`，`pid` 读的是任务自己的活动 pid ns。两者的 inode 可以不同：`unshare(CLONE_NEWPID)` 之后、下一次 fork 之前，自己仍在旧层，孩子的指针已经指向新层。time 与 `time_for_children` 同一规则。

---

## 2.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/ns_common.h` | `struct ns_common` | 9 | inum、ops、引用计数 |
| `include/linux/nsproxy.h` | `struct nsproxy` | 31 | 除 user ns 以外的当前指针 |
| `include/linux/proc_ns.h` | `struct proc_ns_operations` | 16 | `type` 等于 `CLONE_NEW*` |
| `kernel/nsproxy.c` | `create_new_namespaces` | 67 | 按固定顺序复制 |
| `kernel/nsproxy.c` | `copy_namespaces` | 151 | fork 路径 |
| `kernel/nsproxy.c` | `unshare_nsproxy_namespaces` | 213 | `unshare` 路径 |
| `kernel/nsproxy.c` | `switch_task_namespaces` | 239 | `task_lock` 下换指针 |
| `kernel/nsproxy.c` | `commit_nsset` | 512 | `setns` 的提交点 |
| `kernel/nsproxy.c` | `setns` | 546 | 系统调用 |
| `kernel/fork.c` | `ksys_unshare` | 3394 | 先 user ns 的 cred，再 nsproxy，再 `commit_creds` |
| `fs/proc/namespaces.c` | `ns_entries` | 15 | `/proc/<pid>/ns` 的成员 |
| `fs/nsfs.c` | `ns_get_path` | 137 | 从任务和 ops 得到 nsfs 路径 |
| `fs/nsfs.c` | `nsfs_show_path` | 252 | `name:[ino]` |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [mount ns 与 pivot_root →](./03-mount-ns-pivot-root.md)
