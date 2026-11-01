# 7. 挂载与命名空间

**结论**：用户看到的目录树是一棵 `struct mount`。每个 `mount` 里嵌着一个 `vfsmount`，指向某个 `super_block` 的一棵 dentry 子树。旧的 `mount(2)` 和新的 `fsopen` / `fsconfig` / `fsmount` 最后都建成 `fs_context`，再把新的 `mount` 接到父挂载上。传播类型决定这次挂载要不要复制到同组的其它挂载。

> `vfsmount` 在 `include/linux/mount.h:70`。`struct mount` 和 `mnt_namespace` 在 `fs/mount.h`。系统调用在 `fs/namespace.c` 与 `fs/fsopen.c`。

C 新手先分清三个词。`file_system_type` 是「tmpfs 这种格式」。`super_block` 是「已经创建出来的那一个实例」。`struct mount` 是「这个实例出现在某棵目录树的某个点上」。同一个超级块可以有多个 `mount`（bind mount）。

---

## 7.1 `vfsmount` 嵌在 `struct mount` 里

```c
/* include/linux/mount.h:70 */
struct vfsmount {
	struct dentry *mnt_root;     /* 这次挂载暴露出来的根，不一定是 sb->s_root */
	struct super_block *mnt_sb;
	int mnt_flags;               /* MNT_READONLY、MNT_NOSUID、MNT_SHARED … */
	struct mnt_idmap *mnt_idmap; /* 挂载级的 uid/gid 映射 */
} __randomize_layout;
```

路径查找拿着的是 `struct path` 里的 `vfsmount *`。挂载关系（父、孩子、传播组）不在 `vfsmount` 里，在包着它的 `struct mount` 里。`real_mount`（`fs/mount.h:84`）用 `container_of(mnt, struct mount, mnt)` 从内嵌的 `vfsmount` 找回外层。

```c
/* fs/mount.h:39 */
struct mount {
	struct hlist_node mnt_hash;          /* 挂载哈希，lookup_mnt 用 */
	struct mount *mnt_parent;            /* 挂在谁上面。根的 parent 是自己 */
	struct dentry *mnt_mountpoint;       /* 父挂载上的那个挂载点 dentry */
	struct vfsmount mnt;                 /* 嵌在里面，不是指针 */
	union {
		struct rcu_head mnt_rcu;
		struct llist_node mnt_llist;
	};
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;    /* 每 CPU 的引用计数和写者计数 */
#else
	int mnt_count;
	int mnt_writers;
#endif
	struct list_head mnt_mounts;         /* 孩子链表的头 */
	struct list_head mnt_child;          /* 自己挂在父的 mnt_mounts 上 */
	struct list_head mnt_instance;       /* 挂在 super_block->s_mounts */
	const char *mnt_devname;             /* 显示名，例如 /dev/vdc */
	struct list_head mnt_list;           /* 命名空间里的全部挂载 */
	struct list_head mnt_expire;
	struct list_head mnt_share;          /* 共享组：环形链表 */
	struct list_head mnt_slave_list;     /* 我的从挂载 */
	struct list_head mnt_slave;          /* 我作为从挂载，挂在 master 的链表上 */
	struct mount *mnt_master;            /* 非空即 slave，见 IS_MNT_SLAVE */
	struct mnt_namespace *mnt_ns;
	struct mountpoint *mnt_mp;           /* 挂载点对象，多个 mount 可共用 */
	union {
		struct hlist_node mnt_mp_list;
		struct hlist_node mnt_umount;
	};
	struct list_head mnt_umounting;
#ifdef CONFIG_FSNOTIFY
	struct fsnotify_mark_connector __rcu *mnt_fsnotify_marks;
	__u32 mnt_fsnotify_mask;
#endif
	int mnt_id;
	int mnt_group_id;                    /* 共享组 id。0 表示不在共享组 */
	int mnt_expiry_mark;
	struct hlist_head mnt_pins;
	struct hlist_head mnt_stuck_children;
} __randomize_layout;
```

```c
/* fs/mount.h:8 */
struct mnt_namespace {
	struct ns_common	ns;
	struct mount		*root;
	struct list_head	list;      /* 这个命名空间里的 mount */
	spinlock_t		ns_lock;
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	u64			seq;       /* 用来识破挂载循环。匿名 ns 的 seq 是 0 */
	wait_queue_head_t	poll;
	u64			event;
	unsigned int		mounts;    /* 挂载个数 */
	unsigned int		pending_mounts;
} __randomize_layout;
```

`is_anon_ns`（`fs/mount.h:144`）看 `ns->seq == 0`。`alloc_mnt_ns` 在 `anon` 为真时不给 `seq` 赋值（`fs/namespace.c:3837`），`kzalloc` 留下的 0 就是匿名。`fsmount` 用 `alloc_mnt_ns(..., true)`（`:4122`）把新挂载放进这个命名空间，再由 `move_mount` 接进目标命名空间。

`mnt_has_parent`（`:89`）是 `mnt != mnt->mnt_parent`。命名空间的根挂载父指针指向自己。

和传播有关的 `mnt_flags`（`include/linux/mount.h:37`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `MNT_SHARED` | `0x1000` | 共享组的一员 |
| `MNT_UNBINDABLE` | `0x2000` | 不能被 bind 复制 |
| `MNT_SHARED_MASK` | `MNT_UNBINDABLE` | `set_mnt_shared` 时先清掉的位 |

`IS_MNT_SHARED`（`fs/pnode.h:13`）是 `mnt_flags & MNT_SHARED`。`IS_MNT_SLAVE`（`:14`）是 `mnt_master` 非空。slave 不靠一个独立的 flag 位，靠指针。

`set_mnt_shared`（`fs/pnode.h:33`）先清 `MNT_SHARED_MASK`，再或上 `MNT_SHARED`。

---

## 7.2 旧的 `mount(2)`

```text
mount                                 fs/namespace.c:3966
  把类型、设备、选项拷进内核
  do_mount                              :3771
    user_path_at 找到挂载点
    path_mount                            :3692
```

`path_mount` 先剥掉历史魔数 `MS_MGC_VAL`（`:3698`），做 LSM 和 `may_mount` 检查，再把 `MS_NOSUID` / `MS_NODEV` / `MS_NOEXEC` / `MS_RDONLY` 等拆成 `mnt_flags` 和 `sb_flags`。然后按标志分发（`:3756`）：

```text
MS_REMOUNT 且 MS_BIND：do_reconfigure_mnt
MS_REMOUNT：do_remount
MS_BIND：do_loopback                    绑定挂载。MS_REC 则复制整棵子树
MS_SHARED / MS_PRIVATE / MS_SLAVE / MS_UNBINDABLE：
  do_change_type                        :2594
MS_MOVE：do_move_mount_old
其余：do_new_mount                      :3365    真正的新文件系统
```

`do_new_mount` 即使从旧系统调用进来，也建 `fs_context`：

```text
get_fs_type(fstype)
fs_context_for_mount
fc->oldapi = true                       :3400    告诉文件系统这是 mount(2)
vfs_parse_fs_string("source", ...)
parse_monolithic_mount_data             整段选项字符串
mount_capable
vfs_get_tree                            文件系统的 get_tree 填超级块
do_new_mount_fc                         做成 struct mount 并接上
put_fs_context
```

传播标志的用户态值（`include/uapi/linux/mount.h:31`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `MS_REC` | 16384 | 连同子挂载一起改 |
| `MS_UNBINDABLE` | `1 << 17` | 改成不可绑定 |
| `MS_PRIVATE` | `1 << 18` | 改成私有 |
| `MS_SLAVE` | `1 << 19` | 改成从挂载 |
| `MS_SHARED` | `1 << 20` | 改成共享 |

`flags_to_propagation_type`（`fs/namespace.c:2578`）丢掉 `MS_REC` 和 `MS_SILENT` 之后，要求剩下的恰好是这四者之一，并且是 2 的幂。两个传播位同时设会返回 0，`do_change_type` 接着返回 `-EINVAL`。

---

## 7.3 新挂载 API

四个系统调用把「建超级块」和「接到目录树上」拆开。

```text
fsopen(name, flags)                   fs/fsopen.c:121
  flags 只能有 FSOPEN_CLOEXEC（uapi mount.h:83），否则 -EINVAL
  get_fs_type
  fs_context_for_mount
  fc->phase = FS_CONTEXT_CREATE_PARAMS
  把 fs_context 装进一个文件，返回 fs_fd

fsconfig(fs_fd, cmd, ...)             fs/fsopen.c:358
  FSCONFIG_SET_FLAG / SET_STRING / SET_BINARY / SET_PATH / SET_FD
  FSCONFIG_CMD_CREATE                 调用 vfs_get_tree
  FSCONFIG_CMD_CREATE_EXCL            不许复用已有超级块
  FSCONFIG_CMD_RECONFIGURE            改已挂上的超级块

fsmount(fs_fd, flags, attr_flags)     fs/namespace.c:4034
  要求超级块已经建好
  新建一个 mount，放进匿名 mnt_namespace
  返回一个「还没接到可见目录树」的挂载 fd

move_mount(...)                       fs/namespace.c:4168
  把 fsmount 得到的挂载接到目标路径
```

`fspick`（`fs/fsopen.c:164`）对已经挂上的树取一个 `fs_context`，用来 `FSCONFIG_CMD_RECONFIGURE`。`open_tree`（`fs/namespace.c:2745`）把现有挂载变成一个 fd，可以再 `move_mount`。`mount_setattr`（`:4753`）改已挂上的属性，传播标志用 `MOUNT_SETATTR_PROPAGATION_FLAGS`（`:4007`），仍是那四个 `MS_*`。

`fs_context`（`include/linux/fs_context.h:90`）是这次挂载请求的草稿纸：

```c
struct fs_context {
	const struct fs_context_operations *ops;
	struct mutex		uapi_mutex;
	struct file_system_type	*fs_type;
	void			*fs_private;   /* 文件系统自己的解析结果 */
	void			*sget_key;
	struct dentry		*root;         /* get_tree 成功后的根 */
	struct user_namespace	*user_ns;
	struct net		*net_ns;
	const struct cred	*cred;
	struct p_log		log;           /* 挂载错误信息，给用户读 */
	const char		*source;
	void			*security;
	void			*s_fs_info;
	unsigned int		sb_flags;
	unsigned int		sb_flags_mask;
	unsigned int		s_iflags;
	enum fs_context_purpose	purpose:8;
	enum fs_context_phase	phase:8;
	bool			need_free:1;
	bool			global:1;      /* 进 init_user_ns */
	bool			oldapi:1;      /* 来自 mount(2) */
	bool			exclusive:1;   /* CREATE_EXCL：不许复用超级块 */
};
```

`fs_context_operations`（`:115`）里文件系统要实现的是 `parse_param`、`get_tree`，可选 `parse_monolithic` 和 `reconfigure`。`vfs_get_tree`（声明在 `fs_context.h:142`）调用 `ops->get_tree`。tmpfs 的 `get_tree` 是 `shmem_get_tree`，里面是 `get_tree_nodev(fc, shmem_fill_super)`（`mm/shmem.c:4399`）。

`FSCONFIG_*` 的数值（`include/uapi/linux/mount.h:97`）：`SET_FLAG` = 0，`SET_STRING` = 1，`SET_BINARY` = 2，`SET_PATH` = 3，`SET_PATH_EMPTY` = 4，`SET_FD` = 5，`CMD_CREATE` = 6，`CMD_RECONFIGURE` = 7，`CMD_CREATE_EXCL` = 8。

---

## 7.4 四种传播

`do_change_type`（`fs/namespace.c:2594`）在命名空间锁里对这个挂载（`MS_REC` 时还有子挂载）调用 `change_mnt_propagation`（`fs/pnode.c:122`）。

```text
type == MS_SHARED：
  set_mnt_shared                     置 MNT_SHARED。do_change_type 还会 invent_group_ids
type 是另外三种：
  do_make_slave                      挂到原来的共享组下面，变成从
  type != MS_SLAVE：
    摘掉 mnt_slave / mnt_master      不再是任何人的从
    MS_UNBINDABLE：置 MNT_UNBINDABLE
    否则（MS_PRIVATE）：清掉 MNT_UNBINDABLE
```

四种结果：

| 类型 | 挂载别人时 | 别人在同组挂载时 |
|---|---|---|
| shared | 事件传给同组，也接收同组的事件 | `mnt_share` 环上的对等体一起得到一份 |
| slave | 接收 master 传来的挂载，自己的挂载不回传 | 只在 `mnt_slave_list` 上跟随 master |
| private | 不传也不收 | `mnt_master` 为空，也没有 `MNT_SHARED` |
| unbindable | 不参与传播，并且 `do_loopback` 拒绝复制它 | `MNT_UNBINDABLE`。`__do_loopback` 见到它直接 `-EINVAL`（`namespace.c:2634`） |

新挂载接上父挂载时，如果父挂载是 shared，`attach_recursive_mnt`（`fs/namespace.c:2318`）会调用 `propagate_mnt`（`fs/pnode.c:287`），把同一份挂载复制到共享组里该看到它的那些挂载上。`propagation_next`（`fs/pnode.c:149`）顺着「先走自己的 slave，再走同组下一个」遍历。

`/proc/self/mountinfo` 的可选字段里能看到 `shared:`、`master:`、`unbindable`。生成这些行的是 proc 的挂载迭代，本章不逐列拆格式。`mount-max`（`/proc/sys/fs/mount-max`）限制一个命名空间里的挂载数，sysctl 在 `fs/namespace.c:5126`。

---

## 7.5 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/mount.h` | `struct vfsmount` | 70 | 根 dentry、超级块、挂载标志 |
| `fs/mount.h` | `struct mount` | 39 | 父子、传播链表、命名空间 |
| `fs/mount.h` | `struct mnt_namespace` | 8 | 一棵挂载树的命名空间 |
| `fs/mount.h` | `real_mount` | 84 | `container_of` 找回 `struct mount` |
| `fs/pnode.h` | `IS_MNT_SHARED` / `IS_MNT_SLAVE` | 13 / 14 | 共享看标志，从属看 `mnt_master` |
| `fs/namespace.c` | `mount` | 3966 | 旧系统调用 |
| `fs/namespace.c` | `path_mount` | 3692 | 按 `MS_*` 分发 |
| `fs/namespace.c` | `do_new_mount` | 3365 | 旧调用也建 `fs_context` |
| `fs/namespace.c` | `do_change_type` | 2594 | 改传播类型 |
| `fs/namespace.c` | `flags_to_propagation_type` | 2578 | 只接受单独一个传播位 |
| `fs/namespace.c` | `fsmount` | 4034 | 从已建好的超级块做一个挂载 fd |
| `fs/namespace.c` | `move_mount` | 4168 | 把挂载接到目标 |
| `fs/namespace.c` | `open_tree` | 2745 | 现有挂载变成 fd |
| `fs/fsopen.c` | `fsopen` | 121 | 按名字打开一个 `fs_context` |
| `fs/fsopen.c` | `fsconfig` | 358 | 填参数或 `CMD_CREATE` |
| `fs/fsopen.c` | `fspick` | 164 | 对已挂载树取 context |
| `fs/pnode.c` | `change_mnt_propagation` | 122 | shared / slave / private / unbindable |
| `fs/pnode.c` | `propagate_mnt` | 287 | 把新挂载复制到同组 |
| `include/linux/fs_context.h` | `struct fs_context` | 90 | 一次挂载请求 |
| `include/uapi/linux/mount.h` | `FSCONFIG_CMD_CREATE` | 103 | 值 6 |

---

**导航**：[← mmap](./06-mmap-and-file.md) · [目录](./README.md) · [dcache →](./08-dcache-icache.md)
