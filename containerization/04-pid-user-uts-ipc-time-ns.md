# 4. pid、user、uts、ipc、time 命名空间

**结论**：pid 与 time 的 `unshare` 只改「孩子将进入的层」，调用者自己的层要等下一次 fork（pid）或 fork/exec（time）。uts 和 ipc 立刻换成新对象。user ns 立刻换 `cred`，但 uid/gid 映射要等父命名空间往 `uid_map`/`gid_map` 写一次；没写映射之前，这个 ns 里的 kuid 对不上任何全局 id。容器里「我是 root」是映射表上的一行，是 `cap_capable` 沿 user ns 父链认的能力。

> pid 的号怎么分配见 [process-management/03](../process-management/03-pid-namespaces.md)。本节只写容器进入规则，以及 user ns 映射。

---

## 4.1 pid：先改指针，再靠孩子进去

`copy_pid_ns`（`kernel/pid_namespace.c:145`）：

```c
if (!(flags & CLONE_NEWPID))
	return get_pid_ns(old_ns);
if (task_active_pid_ns(current) != old_ns)
	return ERR_PTR(-EINVAL);
return create_pid_namespace(user_ns, old_ns);
```

`create_pid_namespace` 在 `:72`。新层的 `parent` 是调用时的 `pid_ns_for_children`。`task_active_pid_ns(current) != old_ns` 这条把「再往里嵌」限制在当前活动层上：不能从外面跳过自己所在的层去给孩子加更深的层。

`unshare(CLONE_NEWPID)` 之后，`/proc/self/ns/pid` 仍是旧 inode，`pid_for_children` 变成新 inode。下一次 fork 的孩子在新层里拿号，`alloc_pid` 从 `nsproxy->pid_ns_for_children` 往外每一层发一个 `nr`（进程章 03）。新层的 1 号是 `child_reaper`：这个 ns 里最后一个 init 退出时，`zap_pid_ns_processes`（`pid_namespace.c:169`）给还活着的任务发 `SIGKILL` 并 `kernel_wait4` 收尸。

`setns` 进 pid ns 同样只改 `pid_ns_for_children`（安装函数在 pidns 的 `ops->install`）。调用者的 `getpid()` 不变。

层级上限 `MAX_PID_NS_LEVEL` 是 32，见进程章。user ns 对 pid ns 的拥有关系记在 `pid_namespace->user_ns`。

---

## 4.2 user ns：对象

```c
/* include/linux/user_namespace.h:17 */
struct uid_gid_extent {
	u32 first;         /* 本 ns 里看到的起始 id */
	u32 lower_first;   /* 写入完成后是内核全局 id 空间里的起始 */
	u32 count;
};

/* :23 。注释写明 64 字节，一条缓存行 */
struct uid_gid_map {
	u32 nr_extents;
	union {
		struct uid_gid_extent extent[UID_GID_MAP_MAX_BASE_EXTENTS];
		struct {
			struct uid_gid_extent *forward; /* 按本 ns id 排序，超过 5 段时用 */
			struct uid_gid_extent *reverse; /* 按 lower_first 排序 */
		};
	};
};
```

`UID_GID_MAP_MAX_BASE_EXTENTS` 是 5（`user_namespace.h:14`），`UID_GID_MAP_MAX_EXTENTS` 是 340（`:15`）。不超过 5 段时区间嵌在结构里；超过则 `sort_idmaps` 做两份排序数组，查找走 `bsearch`（`map_id_up_max`，`user_namespace.c:362`）。

```c
/* include/linux/user_namespace.h:68 */
struct user_namespace {
	struct uid_gid_map	uid_map;
	struct uid_gid_map	gid_map;
	struct uid_gid_map	projid_map;
	struct user_namespace	*parent;
	int			level;
	kuid_t			owner;          /* 创建者的 euid，父 ns 映射后的全局 id */
	kgid_t			group;
	struct ns_common	ns;
	unsigned long		flags;          /* USERNS_SETGROUPS_ALLOWED 等 */
	/* 创建这个 ns 时，创建者的有效能力里有没有 CAP_SETFCAP */
	bool			parent_could_setfcap;

#ifdef CONFIG_KEYS
	struct list_head	keyring_name_list;
	struct key		*user_keyring_register;
	struct rw_semaphore	keyring_sem;
#endif
#ifdef CONFIG_PERSISTENT_KEYRINGS
	struct key		*persistent_keyring_register;
#endif
	struct work_struct	work;           /* free_user_ns */
#ifdef CONFIG_SYSCTL
	struct ctl_table_set	set;
	struct ctl_table_header *sysctls;
#endif
	struct ucounts		*ucounts;
	long ucount_max[UCOUNT_COUNTS];
	long rlimit_max[UCOUNT_RLIMIT_COUNTS];
} __randomize_layout;
```

`ucount_type`（`:40`）给每种命名空间一个计数：`UCOUNT_USER_NAMESPACES`、`UCOUNT_PID_NAMESPACES`、`UCOUNT_UTS_NAMESPACES`、`UCOUNT_IPC_NAMESPACES`、`UCOUNT_NET_NAMESPACES`、`UCOUNT_MNT_NAMESPACES`、`UCOUNT_CGROUP_NAMESPACES`、`UCOUNT_TIME_NAMESPACES`，后面还有 inotify/fanotify。超过 `ucount_max` 时 `inc_ucount` 失败，调用方得到 `-ENOSPC`。

`create_user_ns`（`user_namespace.c:82`）的门闩：

- `parent_ns->level > 32` → `-ENOSPC`（`:91`）。新层的 `level` 是 `parent + 1`（`:135`）。
- `current_chrooted()` → `-EPERM`（`:105`）。已经 chroot 的进程不能再往里建 user ns。
- 创建者的 euid/egid 在父 ns 里没有映射 → `-EPERM`（`:113`）。
- `security_create_user_ns`（`security/security.c:3583`）让 LSM 再否一次。

通过之后 `set_cred_user_ns`（`:43`）把新 cred 的 `cap_permitted`、`cap_effective`、`cap_bset` 设成 `CAP_FULL_SET`，`cap_inheritable` 和 `cap_ambient` 清空，`user_ns` 指向新 ns。这些能力只在新 ns 里有效。`parent_could_setfcap` 记下创建瞬间有效集里有没有 `CAP_SETFCAP`（`:126`）。

---

## 4.3 uid/gid 映射：文件格式与一次写入

`/proc/<pid>/uid_map` 的写入口是 `proc_uid_map_write`（`user_namespace.c:1108`），gid 是 `proc_gid_map_write`（`:1125`），projid 是 `proc_projid_map_write`（`:1142`）。三者都进 `map_write`（`:920`）。proc 的文件操作表在 `fs/proc/base.c:3132`，目录项在 `:3370`。

行格式由 `map_write` 的解析循环决定（`:992`）：一行三个无符号十进制数，空格分开，行尾不能有多余字段。

```text
first  lower_first  count
```

`first` 是这个 user ns 里的起始 id，`lower_first` 是父 user ns 里的起始 id，`count` 是长度。区间不能绕回（和不能小于等于起点，`:1019`），区间之间不能重叠（`mappings_overlap`）。一次写必须从文件偏移 0 开始，长度小于 `PAGE_SIZE`（`:935`）。`map->nr_extents != 0` 时直接 `-EPERM`（`:968`）：一张图只能成功写一次。

写进结构体之前，`map_write` 把每一段的 `lower_first` 用父 ns 的映射 `map_id_range_down` 折成内核全局 id（`:1060`）。所以内存里的 `uid_gid_extent.lower_first` 是 kuid 空间的数。`/proc` 里读回来的第二列仍是父 ns 视角的 id，读路径用 `from_kuid` 折回去。

权限在 `new_idmap_permitted`（`:1160`）：

1. 对 uid 图，若某一段的 `lower_first == 0`（映射里包含父 ns 的 uid 0），`verify_root_map`（`:878`）要求：写自己的图时 `parent_could_setfcap` 为真；写别人的图时，打开这个文件的 cred 在父 ns 里有 `CAP_SETFCAP`。否则拒绝。非特权进程创建的 user ns 通常没有 `CAP_SETFCAP`，因此不能把父 ns 的 0 映进自己的图。
2. 单段且 `count == 1`，写入者是这个 ns 的 `owner`，并且映的就是写入者自己的 euid（gid 则还要求 `setgroups` 已经是 deny）：直接允许（`:1172`）。这就是非特权 `unshare --user` 把容器内 0 映到自己的宿主 uid 的那条路。本机样例在 [11](./11-observability.md)。
3. 其余情况要在父 ns 里同时满足 `ns_capable` 和 `file_ns_capable` 的 `CAP_SETUID` 或 `CAP_SETGID`（`:1195`）。
4. `map_write` 在调用 `new_idmap_permitted` 之前，还要求对目标 ns 有 `CAP_SYS_ADMIN`（`:974`），`cap_setid` 合法时才查。单段映射自己的那条路发生在 `new_idmap_permitted` 内部，仍然先过了这一道 `CAP_SYS_ADMIN`：新 user ns 里 `set_cred_user_ns` 已经给了全能力，所以创建者写自己的图时这道是过的。

`make_kuid`（`:408`）是 `KUIDT_INIT(map_id_down(&ns->uid_map, uid))`。没有映射时得到的值让 `uid_valid()` 为假。`from_kuid`（`:427`）反向；没有映射时返回 `(uid_t)-1`。`from_kuid_munged`（`:452`）把这种情况变成 overflow uid，给需要「总能印出一个数」的路径用。

查找：`map_id_range_down`（`:312`）在 `nr_extents <= 5` 时线性扫 `extent[]`，否则走排序后的 `forward`。向下映射的公式是 `id - extent->first + extent->lower_first`（`:325`）。向上是 `map_id_up`（`:375`），比的是 `lower_first`。

### `setgroups`

`/proc/<pid>/setgroups` 的写在 `proc_setgroups_write`（`:1213`）。只接受 `allow` 或 `deny`。

- 写成 `deny`：清 `USERNS_SETGROUPS_ALLOWED`。gid_map 已经写过（`nr_extents != 0`）则 `-EPERM`（`:1264`）。
- 写成 `allow`：标志已经被清掉就不能再设回来（`:1258`）。

非特权的单 gid 映射要求标志已经清掉（`new_idmap_permitted` `:1181`）。所以顺序是先 `deny`，再写 `gid_map`。写了 `deny` 之后，这个 ns 里不能再 `setgroups` 把父 ns 的附加组带进来。`proc_setgroups_show`（`:1202`）印 `allow` 或 `deny`。

初始 user ns 的 `uid_map` 是一整段恒等映射。父指针为空的 ns 拒绝写入（`proc_uid_map_write` `:1115` 返回 `-EPERM`）。

---

## 4.4 能力沿 user ns 父链

`cap_capable`（`security/commoncap.c:66`）从目标 ns 往 parent 走：

- `ns == cred->user_ns`：看 `cap_effective` 里有没有这一位。
- `ns->level <= cred->user_ns->level`：已经走到不比 cred 更深的层，`-EPERM`。
- 目标 ns 的 parent 就是 cred 的 user ns，并且 `ns->owner` 等于 cred 的 euid：直接通过。创建者在父 ns 里对子 ns 拥有全部能力。
- 否则 `ns = ns->parent` 继续。父 ns 里有的能力，对所有子 ns 也有。

`ns_capable`（`kernel/capability.c:381`）是带审计的入口，`capable`（`:434`）对 `init_user_ns` 调用它。容器里的 root 能改自己的 hostname、建自己的 mnt ns，是因为检查发生在容器的 user ns 上，而创建者的 cred 就在这个 ns 里并且有效集是满的。同一位能力对宿主机的 `init_user_ns` 仍然是 `-EPERM`。

`struct cred` 里和这有关的字段（`include/linux/cred.h:110`，整结构到 `:146`）：

```c
struct cred {
	atomic_long_t	usage;
	kuid_t		uid;             /* real */
	kgid_t		gid;
	kuid_t		suid;
	kgid_t		sgid;
	kuid_t		euid;
	kgid_t		egid;
	kuid_t		fsuid;           /* VFS 用 */
	kgid_t		fsgid;
	unsigned	securebits;
	kernel_cap_t	cap_inheritable;
	kernel_cap_t	cap_permitted;
	kernel_cap_t	cap_effective;
	kernel_cap_t	cap_bset;        /* bounding set */
	kernel_cap_t	cap_ambient;
#ifdef CONFIG_KEYS
	unsigned char	jit_keyring;
	struct key	*session_keyring;
	struct key	*process_keyring;
	struct key	*thread_keyring;
	struct key	*request_key_auth;
#endif
#ifdef CONFIG_SECURITY
	void		*security;       /* LSM */
#endif
	struct user_struct *user;
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct group_info *group_info;
	union {
		int non_rcu;
		struct rcu_head rcu;
	};
} __randomize_layout;
```

`kernel_cap_t` 在 6.6 是 `typedef struct { u64 val; } kernel_cap_t`（`include/linux/capability.h:24`）。`CAP_LAST_CAP` 是 `CAP_CHECKPOINT_RESTORE`，值 40（`include/uapi/linux/capability.h:419`）。容器最后留下哪些位，见 [10](./10-seccomp-caps-lsm.md)。

---

## 4.5 uts

```c
/* include/linux/utsname.h:24 */
struct uts_namespace {
	struct new_utsname name;          /* sysname、nodename、release、version、machine、domainname */
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct ns_common ns;
} __randomize_layout;
```

`copy_utsname` 在 `CLONE_NEWUTS` 时做一份新的，名字从旧 ns 拷来。随后 `sethostname` / `setdomainname` 只改这份。容器的 hostname 就是写 `uts_namespace->name`。`utsname()`（`utsname.h:80`）返回当前任务这份。`/proc/<pid>/ns/uts` 的 ops 在 `kernel/utsname.c:160`。

---

## 4.6 ipc

```c
/* include/linux/ipc_namespace.h:31 */
struct ipc_namespace {
	struct ipc_ids	ids[3];            /* sem、msg、shm 三套 idr */

	int		sem_ctls[4];
	int		used_sems;

	unsigned int	msg_ctlmax;
	unsigned int	msg_ctlmnb;
	unsigned int	msg_ctlmni;
	struct percpu_counter percpu_msg_bytes;
	struct percpu_counter percpu_msg_hdrs;

	size_t		shm_ctlmax;
	size_t		shm_ctlall;
	unsigned long	shm_tot;
	int		shm_ctlmni;
	int		shm_rmid_forced;   /* IPC_RMID 是否对所有 shm 强制 */

	struct notifier_block ipcns_nb;

	struct vfsmount	*mq_mnt;           /* mqueuefs 的 kern_mount */
	unsigned int    mq_queues_count;
	unsigned int    mq_queues_max;     /* 初始 DFLT_QUEUESMAX，256 */
	unsigned int    mq_msg_max;
	unsigned int    mq_msgsize_max;
	unsigned int    mq_msg_default;
	unsigned int    mq_msgsize_default;

	struct ctl_table_set	mq_set;
	struct ctl_table_header	*mq_sysctls;
	struct ctl_table_set	ipc_set;
	struct ctl_table_header	*ipc_sysctls;

	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct llist_node mnt_llist;
	struct ns_common ns;
} __randomize_layout;
```

`CLONE_NEWIPC` 得到一套空的 SysV 信号量、消息队列、共享内存，以及自己的 POSIX mqueue 挂载。旧 ns 里的对象不再可达，所以 `unshare` 带上 `CLONE_NEWIPC` 时会 `exit_shm`（`kernel/fork.c:3461`），并在 `do_sysvsem` 时 `exit_sem`（`:3455`）。`CLONE_NEWIPC` 与 `CLONE_SYSVSEM` 不能一起用（`nsproxy.c:175`）。ops 在 `ipc/namespace.c:253`。

---

## 4.7 time

```c
/* include/linux/time_namespace.h:14 */
struct timens_offsets {
	struct timespec64 monotonic;   /* CLOCK_MONOTONIC 的偏移 */
	struct timespec64 boottime;    /* CLOCK_BOOTTIME 的偏移 */
};

/* :19 */
struct time_namespace {
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	struct ns_common	ns;
	struct timens_offsets	offsets;
	struct page		*vvar_page;       /* VDSO 读偏移用的页 */
	bool			frozen_offsets;   /* 有任务加入后不许再改偏移 */
} __randomize_layout;
```

`CLOCK_REALTIME` 不在偏移里。容器里 `clock_gettime(CLOCK_MONOTONIC)` 会加上 `offsets.monotonic`（`timens_add_monotonic`，`time_namespace.h:65`）。偏移从 `/proc/<pid>/timens_offsets` 写，写入口 `timens_offsets_write`（`fs/proc/base.c:1618`）调用 `proc_timens_set_offset`。文件登记在 `base.c:3302`。

和 pid 一样，`unshare(CLONE_NEWTIME)` 改的是 `time_ns_for_children`。`setns` 提交时若带了 `CLONE_NEWTIME`，`commit_nsset` 调 `timens_commit`（`nsproxy.c:538`）。`frozen_offsets` 一旦置位，再写偏移会被拒绝。runc 在 stage-0 里写这个文件（`update_timens_offsets`，见 [09](./09-runc-create-path.md)），因为孩子进入之后偏移就冻住了。

---

## 4.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `kernel/pid_namespace.c` | `copy_pid_ns` | 145 | 只产生给孩子的新层 |
| `kernel/pid_namespace.c` | `zap_pid_ns_processes` | 169 | 层内 init 退出时清掉剩余任务 |
| `kernel/user_namespace.c` | `create_user_ns` | 82 | 建 user ns，拒绝 chroot 过的调用者 |
| `kernel/user_namespace.c` | `set_cred_user_ns` | 43 | 新 ns 内能力置满 |
| `kernel/user_namespace.c` | `map_write` | 920 | 解析并一次写入映射 |
| `kernel/user_namespace.c` | `map_id_range_down` | 312 | 本 ns id → 全局 id |
| `kernel/user_namespace.c` | `make_kuid` / `from_kuid` | 408 / 427 | 映射入口 |
| `kernel/user_namespace.c` | `new_idmap_permitted` | 1160 | 单 id 自映射，或父 ns 的 `CAP_SETUID` |
| `kernel/user_namespace.c` | `verify_root_map` | 878 | 映进父 ns uid 0 要 `CAP_SETFCAP` |
| `kernel/user_namespace.c` | `proc_setgroups_write` | 1213 | `allow` / `deny`，deny 不可逆 |
| `security/commoncap.c` | `cap_capable` | 66 | 沿 user ns 父链查能力 |
| `include/linux/utsname.h` | `struct uts_namespace` | 24 | 主机名那一份 |
| `include/linux/ipc_namespace.h` | `struct ipc_namespace` | 31 | SysV 与 POSIX mqueue |
| `include/linux/time_namespace.h` | `struct time_namespace` | 19 | monotonic / boottime 偏移 |
| `fs/proc/base.c` | `timens_offsets_write` | 1618 | 写偏移 |

---

**导航**：[← mount ns](./03-mount-ns-pivot-root.md) · [目录](./README.md) · [net ns →](./05-net-ns.md)
