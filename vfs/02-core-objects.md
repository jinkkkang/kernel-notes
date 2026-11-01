# 2. 核心对象

**结论**：一次打开要同时有四样东西：`super_block`（这套文件系统）、`dentry`（路径上的名字）、`inode`（文件本体）、`struct file`（这次打开）。具体行为不写在 VFS 里，写在四张函数表上：`super_operations`、`inode_operations`、`dentry_operations`、`file_operations`。

> 定义：`include/linux/fs.h`、`include/linux/dcache.h`、`include/linux/path.h`。字段顺序与 v6.6.160 一致，`#ifdef` 也按源码留下。没编进内核的字段在那份二进制里不存在。

C 新手先看两对「嵌进去」和「指过去」：

- `inode` **包含** `struct address_space i_data`。`inode->i_mapping` 是指针，`inode_init_always` 把它设成 `&inode->i_data`（`fs/inode.c:221`）。有的文件系统会改这个指针，让多个 inode 共用另一份地址空间。
- `dentry->d_inode` **指向** `inode`。多个 `dentry` 可以指向同一个 `inode`（硬链接）。负 dentry 的 `d_inode` 是 `NULL`，类型位是 `DCACHE_MISS_TYPE`。

页缓存那份 `address_space` 的完整字段在 [MM §8.1](../memory-management/08-page-cache-and-writeback.md)（`include/linux/fs.h:472`）。本章不重抄。

---

## 2.1 关系

```text
super_block                      一个已挂载的文件系统实例
  ├─ s_root -> dentry            根目录的名字
  ├─ s_op   -> super_operations
  └─ s_mounts                    挂在这套超级块上的 struct mount 链表

dentry                           目录项，缓存「父目录 + 名字」
  ├─ d_parent -> dentry
  ├─ d_inode  -> inode           NULL 表示负项：这个名字确定不存在
  ├─ d_op     -> dentry_operations
  └─ d_hash                      挂进全局 dentry 哈希桶

inode                            文件本体
  ├─ i_sb     -> super_block
  ├─ i_op     -> inode_operations
  ├─ i_fop    -> file_operations  新打开时拷到 file->f_op
  ├─ i_mapping -> address_space   页缓存。初值是 &i_data
  └─ i_data   address_space       嵌在 inode 里的那一份

struct file                      一次打开
  ├─ f_path   { vfsmount, dentry }
  ├─ f_inode  -> inode            从 dentry 缓存下来的指针
  ├─ f_op     -> file_operations
  └─ f_count  引用计数。到 0 才进 __fput
```

`struct path`（`include/linux/path.h:8`）就是挂载点加 dentry，不是字符串：

```c
/* include/linux/path.h:8 */
struct path {
	struct vfsmount *mnt;     /* 当前路径落在哪个挂载上 */
	struct dentry *dentry;    /* 这个挂载里的名字 */
} __randomize_layout;
```

`path_get`（`fs/namei.c:552`）对这两边各加一次引用：`mntget` 加挂载，`dget` 加 dentry。`path_put`（`:565`）成对放下。

---

## 2.2 `qstr` 与 `dentry`

名字查找不拿整条路径去做字符串比较。每个分量先算成 `qstr`：哈希和长度叠在一个 `u64` 里，名字指针另放。

```c
/* include/linux/dcache.h:49。小端上 hash 在低 32 位，见 HASH_LEN_DECLARE :35 */
struct qstr {
	union {
		struct {
			u32 hash;          /* 小端布局。大端是 len 在前，见 dcache.h:38 */
			u32 len;
		};
		u64 hash_len;          /* 一次比较哈希和长度 */
	};
	const unsigned char *name;
};
```

```c
/* include/linux/dcache.h:82 */
struct dentry {
	/* --- RCU 查找会摸到的前半 --- */
	unsigned int d_flags;              /* DCACHE_*，由 d_lock 保护 */
	seqcount_spinlock_t d_seq;         /* 每 dentry 一把顺序锁，RCU 走查它有没有变 */
	struct hlist_bl_node d_hash;       /* 挂进 dentry_hashtable 的桶 */
	struct dentry *d_parent;           /* 父目录。根的 parent 是自己，IS_ROOT :31 */
	struct qstr d_name;
	struct inode *d_inode;             /* 这个名字对应的 inode。NULL 是负项 */

	/*
	 * 短名字直接放这里，不必再分配。
	 * 64 位上 DNAME_INLINE_LEN 是 32（dcache.h:71）。
	 */
	unsigned char d_iname[DNAME_INLINE_LEN];

	/* --- 引用计数走查还会摸到的后半 --- */
	struct lockref d_lockref;          /* 自旋锁和引用计数叠在一个字里 */
	const struct dentry_operations *d_op;
	struct super_block *d_sb;          /* 这条 dentry 属于哪个超级块 */
	unsigned long d_time;              /* 给 d_revalidate 用的时间戳 */
	void *d_fsdata;                    /* 文件系统私有 */

	union {
		struct list_head d_lru;    /* 在超级块的 s_dentry_lru 上 */
		wait_queue_head_t *d_wait; /* 正在 in-lookup 时，别人在这等 */
	};
	struct list_head d_child;          /* 挂在父目录的 d_subdirs 上 */
	struct list_head d_subdirs;        /* 自己的孩子 */

	/*
	 * 这三项共用一块内存，同一时刻只用一个。
	 * 活着的正项用 d_alias 挂到 inode->i_dentry；
	 * 正在查找用 d_in_lookup_hash；
	 * 释放走 RCU 回调用 d_rcu。
	 */
	union {
		struct hlist_node d_alias;
		struct hlist_bl_node d_in_lookup_hash;
		struct rcu_head d_rcu;
	} d_u;
} __randomize_layout;
```

`d_lock` 是宏，展开成 `d_lockref.lock`（`dcache.h:80`）。引用计数在 `d_lockref.count`。

`d_is_negative`（`dcache.h:433`）看的是类型位，不是只看 `d_inode == NULL`：它调用 `d_is_miss`（`:388`），要求 `(d_flags & DCACHE_ENTRY_TYPE) == DCACHE_MISS_TYPE`。`DCACHE_MISS_TYPE` 是 0（`:199`）。注释写明还没把 whiteout 算进去。

后面章节会用到的 `d_flags` 位（`dcache.h:154` 起）：

| 宏 | 值 | 含义 |
|---|---|---|
| `DCACHE_OP_HASH` | `0x00000001` | 查找前要调 `d_op->d_hash` |
| `DCACHE_OP_COMPARE` | `0x00000002` | 比较名字要调 `d_op->d_compare` |
| `DCACHE_OP_REVALIDATE` | `0x00000004` | 命中后要调 `d_op->d_revalidate` |
| `DCACHE_OP_DELETE` | `0x00000008` | `dput` 时问文件系统该不该留 |
| `DCACHE_REFERENCED` | `0x00000040` | 最近用过，shrink 时转一圈再丢 |
| `DCACHE_DONTCACHE` | `0x00000080` | 最后一次 `dput` 时清掉 |
| `DCACHE_SHRINK_LIST` | `0x00000400` | 已经在收缩链表上 |
| `DCACHE_MOUNTED` | `0x00010000` | 这里有挂载 |
| `DCACHE_NEED_AUTOMOUNT` | `0x00020000` | 走到这里要自动挂载 |
| `DCACHE_MANAGE_TRANSIT` | `0x00040000` | 经过时调 `d_manage` |
| `DCACHE_LRU_LIST` | `0x00080000` | 在 LRU 上 |
| `DCACHE_MISS_TYPE` | `0x00000000` | 负项 |
| `DCACHE_DIRECTORY_TYPE` | `0x00200000` | 目录 |
| `DCACHE_REGULAR_TYPE` | `0x00400000` | 普通文件 |
| `DCACHE_SYMLINK_TYPE` | `0x00600000` | 符号链接 |
| `DCACHE_ENTRY_TYPE` | `0x00700000` | 上面几种类型的掩码 |
| `DCACHE_PAR_LOOKUP` | `0x10000000` | 父目录以共享锁在查找 |
| `DCACHE_NORCU` | `0x40000000` | 释放不等 RCU 宽限期 |

`DCACHE_MANAGED_DENTRY`（`:193`）是 `DCACHE_MOUNTED | DCACHE_NEED_AUTOMOUNT | DCACHE_MANAGE_TRANSIT`。路径查找看到它就去跨挂载，见 [03](./03-path-lookup.md)。

```c
/* include/linux/dcache.h:128 */
struct dentry_operations {
	int (*d_revalidate)(struct dentry *, unsigned int);       /* 缓存还算不算数 */
	int (*d_weak_revalidate)(struct dentry *, unsigned int);  /* 跳转之后的弱校验 */
	int (*d_hash)(const struct dentry *, struct qstr *);      /* 文件系统自定义哈希 */
	int (*d_compare)(const struct dentry *,
			unsigned int, const char *, const struct qstr *);
	int (*d_delete)(const struct dentry *);   /* 返回非 0：别进缓存，立刻丢掉 */
	int (*d_init)(struct dentry *);
	void (*d_release)(struct dentry *);       /* 引用归零、即将释放 */
	void (*d_prune)(struct dentry *);         /* 从缓存摘掉时 */
	void (*d_iput)(struct dentry *, struct inode *); /* 默认是 iput */
	char *(*d_dname)(struct dentry *, char *, int);  /* 伪文件现场生成名字 */
	struct vfsmount *(*d_automount)(struct path *);
	int (*d_manage)(const struct path *, bool);      /* 经过这个目录时拦截 */
	struct dentry *(*d_real)(struct dentry *, const struct inode *); /* 层叠 FS 找下层 */
} ____cacheline_aligned;
```

`d_real` 是层叠文件系统用的钩子。overlayfs 的 copy-up、whiteout 不在本章展开。

---

## 2.3 `inode`

源码注释要求把路径查找和 `stat` 常读的字段放在结构体前部（`fs.h:637`）。

```c
/* include/linux/fs.h:641 */
struct inode {
	umode_t			i_mode;        /* 类型和权限，S_IFREG / S_IFDIR 在高位 */
	unsigned short		i_opflags;     /* IOP_* 快路径位，见下表 */
	kuid_t			i_uid;
	struct list_head	i_lru;         /* 超级块 inode LRU */
	kgid_t			i_gid;
	unsigned int		i_flags;       /* S_SYNC、S_NOATIME、S_IMMUTABLE 等 */

#ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl	*i_acl;
	struct posix_acl	*i_default_acl;
#endif

	const struct inode_operations	*i_op;
	struct super_block	*i_sb;
	struct address_space	*i_mapping;    /* 页缓存。初值 &i_data */

#ifdef CONFIG_SECURITY
	void			*i_security;
#endif

	/* --- stat 数据。路径查找不读这一段 --- */
	unsigned long		i_ino;
	/*
	 * 文件系统只能读 i_nlink。改链接数用 set_nlink / inc_nlink 等。
	 * 两个成员是同一块内存：对外的名字是 const，对内用 __i_nlink 写。
	 */
	union {
		const unsigned int i_nlink;
		unsigned int __i_nlink;
	};
	dev_t			i_rdev;        /* 设备号。普通文件用不到 */
	loff_t			i_size;
	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	__i_ctime;     /* 用 inode_*_ctime 访问 */
	spinlock_t		i_lock;        /* 保护 i_blocks、i_bytes，有时还有 i_size */
	unsigned short		i_bytes;
	u8			i_blkbits;     /* 块大小是 1 << i_blkbits */
	u8			i_write_hint;
	blkcnt_t		i_blocks;

#ifdef __NEED_I_SIZE_ORDERED
	seqcount_t		i_size_seqcount; /* 32 位 SMP 上把 i_size 读一致 */
#endif

	/* --- 杂项 --- */
	unsigned long		i_state;       /* I_NEW / I_FREEING / I_DIRTY_* */
	struct rw_semaphore	i_rwsem;       /* 就是过去的 i_mutex */

	unsigned long		dirtied_when;  /* 第一次变脏的 jiffies */
	unsigned long		dirtied_time_when;

	struct hlist_node	i_hash;        /* inode 哈希：超级块 + i_ino */
	struct list_head	i_io_list;     /* 回写域上的 IO 链表 */
#ifdef CONFIG_CGROUP_WRITEBACK
	struct bdi_writeback	*i_wb;

	/* 外来脏页检测，见 wbc_detach_inode() */
	int			i_wb_frn_winner;
	u16			i_wb_frn_avg_time;
	u16			i_wb_frn_history;
#endif
	struct list_head	i_sb_list;     /* 挂在 super_block->s_inodes */
	struct list_head	i_wb_list;     /* 回写链表 */
	union {
		struct hlist_head	i_dentry; /* 指向这个 inode 的全部 dentry */
		struct rcu_head		i_rcu;    /* 释放时复用这块 */
	};
	atomic64_t		i_version;
	atomic64_t		i_sequence;    /* futex 用 */
	atomic_t		i_count;       /* inode 引用。iput 减到 0 才考虑回收 */
	atomic_t		i_dio_count;   /* 正在进行的 Direct IO */
	atomic_t		i_writecount;  /* 以写打开的次数。负数表示禁止再写 */
#if defined(CONFIG_IMA) || defined(CONFIG_FILE_LOCKING)
	atomic_t		i_readcount;   /* 只读打开的 struct file 数 */
#endif
	union {
		const struct file_operations	*i_fop; /* 打开时拷进 file->f_op */
		void (*free_inode)(struct inode *);     /* 释放路径复用这个槽 */
	};
	struct file_lock_context	*i_flctx; /* flock / POSIX / lease 三条链表 */
	struct address_space	i_data;    /* 嵌进去的页缓存。完整字段见 MM §8.1 */
	struct list_head	i_devices;
	union {
		struct pipe_inode_info	*i_pipe;
		struct cdev		*i_cdev;
		char			*i_link;   /* 快符号链接：目标直接放这里 */
		unsigned		i_dir_seq;
	};

	__u32			i_generation;

#ifdef CONFIG_FSNOTIFY
	__u32			i_fsnotify_mask; /* 这个 inode 关心的事件并集 */
	struct fsnotify_mark_connector __rcu	*i_fsnotify_marks;
#endif

#ifdef CONFIG_FS_ENCRYPTION
	struct fscrypt_info	*i_crypt_info;
#endif

#ifdef CONFIG_FS_VERITY
	struct fsverity_info	*i_verity_info;
#endif

	void			*i_private;    /* 文件系统或设备的私有指针 */
} __randomize_layout;
```

`i_opflags`（`fs.h:628`）让权限检查少做一次间接调用：

| 宏 | 值 | 含义 |
|---|---|---|
| `IOP_FASTPERM` | `0x0001` | 可以用通用权限检查，不必调 `i_op->permission` |
| `IOP_LOOKUP` | `0x0002` | 目录实现了 `lookup` |
| `IOP_NOFOLLOW` | `0x0004` | 已经看过，没有 `get_link`，不当符号链接（`dcache.c:1989`） |
| `IOP_XATTR` | `0x0008` | 超级块有 xattr |
| `IOP_DEFAULT_READLINK` | `0x0010` | 用默认 `readlink` |

`i_state` 里和生命周期、脏有关的位（`fs.h:2274` 起）。注释（`:2191`）说明这些位由 `i_lock` 保护：

| 宏 | 值 | 含义 |
|---|---|---|
| `I_DIRTY_SYNC` | `1 << 0` | inode 元数据脏。`fdatasync` 不一定写它 |
| `I_DIRTY_DATASYNC` | `1 << 1` | 影响数据读取的元数据脏 |
| `I_DIRTY_PAGES` | `1 << 2` | 有脏页 |
| `I_NEW` | `1 << 3` | 刚分配，还没初始化完 |
| `I_WILL_FREE` | `1 << 4` | 准备释放，先把脏数据写出去 |
| `I_FREEING` | `1 << 5` | 正在释放 |
| `I_CLEAR` | `1 << 6` | `evict_inode` 已做完 |
| `I_SYNC` | `1 << 7` | 正在写回这个 inode |
| `I_REFERENCED` | `1 << 8` | LRU 上最近被用过，再给一轮 |
| `I_DIRTY_TIME` | `1 << 11` | 只有时间戳脏 |
| `I_DONTCACHE` | `1 << 16` | 引用归零就丢掉，不进 LRU |
| `I_DIRTY` | sync \| datasync \| pages | 需要写回的脏 |
| `I_DIRTY_ALL` | `I_DIRTY` \| `I_DIRTY_TIME` | 加上时间戳 |

`i_rwsem` 的加锁子类在 `enum inode_i_mutex_lock_class`（`fs.h:792`）：`I_MUTEX_NORMAL`、`I_MUTEX_PARENT`、`I_MUTEX_CHILD`、`I_MUTEX_XATTR`、`I_MUTEX_NONDIR2`、`I_MUTEX_PARENT2`。锁的实现见 [locks-and-ds/06](../locks-and-ds/06-sleepable-locks.md)。

---

## 2.4 `struct file`

一次 `open` 成功得到一个 `struct file`。进程里的整数 fd 只是 `files_struct` 里指向它的槽，见 [04](./04-fd-and-open.md)。

源码把 `f_lock`、`f_count`、`f_pos` 和只读的 `f_path` / `f_inode` / `f_op` 分成不同的缓存行（`fs.h:989`）。

```c
/* include/linux/fs.h:994 */
struct file {
	/*
	 * 三成员共用一块内存。
	 * 文件还活着时用 f_iocb_flags（O_APPEND / O_DIRECT 等折进 IOCB_*）。
	 * 最后一次 fput 延迟释放时改用 f_llist 或 f_rcuhead。
	 */
	union {
		struct llist_node	f_llist;
		struct rcu_head		f_rcuhead;
		unsigned int		f_iocb_flags;
	};

	spinlock_t		f_lock;        /* 保护 f_ep、f_flags。不能在硬中断里拿 */
	fmode_t			f_mode;        /* FMODE_READ / FMODE_WRITE / FMODE_OPENED … */
	atomic_long_t		f_count;       /* 引用。get_file 加，fput 减到 0 进 __fput */
	struct mutex		f_pos_lock;    /* 多线程同时读 f_pos 时串行 */
	loff_t			f_pos;         /* 当前偏移。O_PATH / 流式文件另说 */
	unsigned int		f_flags;       /* open 的 O_* */
	struct fown_struct	f_owner;       /* SIGIO 发给谁 */
	const struct cred	*f_cred;       /* 打开时的凭证 */
	struct file_ra_state	f_ra;          /* 这个打开上的预读窗口 */
	struct path		f_path;        /* 挂载 + dentry */
	struct inode		*f_inode;      /* 缓存的 inode，等于 f_path 上那个 */
	const struct file_operations	*f_op;

	u64			f_version;
#ifdef CONFIG_SECURITY
	void			*f_security;
#endif
	void			*private_data; /* 驱动 / 文件系统私有 */

#ifdef CONFIG_EPOLL
	struct hlist_head	*f_ep;         /* epoll 钩子 */
#endif
	struct address_space	*f_mapping;    /* 通常等于 f_inode->i_mapping */
	errseq_t		f_wb_err;      /* 这个打开已经看过的回写错误 */
	errseq_t		f_sb_err;      /* syncfs 用的超级块错误序号 */
} __randomize_layout
  __attribute__((aligned(4)));
```

`get_file`（`fs.h:1042`）做 `atomic_long_inc(&f->f_count)`。`file_count`（`:1048`）是读这个计数。减到 0 的路径在 [04](./04-fd-and-open.md)。

`do_dentry_open`（`fs/open.c:873`）里和模式有关的几步：`f_inode = inode`，`f_mapping = inode->i_mapping`（`:881`）；`O_PATH` 把 `f_op` 换成空表并只留 `FMODE_PATH | FMODE_OPENED`（`:886`）；否则 `f_op = fops_get(inode->i_fop)`（`:910`），再调 `f_op->open`（`:929`）。有 `read` 或 `read_iter` 才置 `FMODE_CAN_READ`（`:934`）。`a_ops->direct_IO` 非空才置 `FMODE_CAN_ODIRECT`（`:942`）。打开结束清掉 `O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC`（`:945`），因为这些只在 open 当次有意义。

后面会用到的 `f_mode` 位（`fs.h:111` 起）：

| 宏 | 值 | 含义 |
|---|---|---|
| `FMODE_READ` | `0x1` | 允许读 |
| `FMODE_WRITE` | `0x2` | 允许写 |
| `FMODE_LSEEK` | `0x4` | 允许 seek |
| `FMODE_PATH` | `0x4000` | `O_PATH`。几乎不能读写 |
| `FMODE_ATOMIC_POS` | `0x8000` | 普通文件和目录的偏移要原子更新 |
| `FMODE_WRITER` | `0x10000` | 已经 `get_write_access` |
| `FMODE_CAN_READ` | `0x20000` | `f_op` 有 `read` 或 `read_iter` |
| `FMODE_CAN_WRITE` | `0x40000` | `f_op` 有 `write` 或 `write_iter` |
| `FMODE_OPENED` | `0x80000` | `do_dentry_open` 成功 |
| `FMODE_CREATED` | `0x100000` | 这次 open 创建了文件 |
| `FMODE_STREAM` | `0x200000` | 没有 `f_pos`，例如某些管道 |
| `FMODE_CAN_ODIRECT` | `0x400000` | 允许 `O_DIRECT` |
| `FMODE_NOWAIT` | `0x8000000` | 这个文件的 IO 可以返回 `-EAGAIN`（`fs.h:173` 的注释） |

`FMODE_CAN_ODIRECT` 的值是 `0x400000`（`fs.h:161`）。和上面 `FMODE_CREATED` 不是同一位。

---

## 2.5 `super_block`

```c
/* include/linux/fs.h:1190 */
struct super_block {
	struct list_head	s_list;            /* 全局超级块链表。必须是第一个字段 */
	dev_t			s_dev;             /* 查找用的设备号，不是 kdev_t */
	unsigned char		s_blocksize_bits;
	unsigned long		s_blocksize;
	loff_t			s_maxbytes;        /* 这个文件系统允许的最大文件 */
	struct file_system_type	*s_type;
	const struct super_operations	*s_op;
	const struct dquot_operations	*dq_op;
	const struct quotactl_ops	*s_qcop;
	const struct export_operations *s_export_op;
	unsigned long		s_flags;           /* SB_RDONLY 等，挂载时定 */
	unsigned long		s_iflags;          /* 内部 SB_I_* */
	unsigned long		s_magic;
	struct dentry		*s_root;           /* 根 dentry */
	struct rw_semaphore	s_umount;
	int			s_count;
	atomic_t		s_active;          /* 激活引用。到 0 才真正拆超级块 */
#ifdef CONFIG_SECURITY
	void			*s_security;
#endif
	const struct xattr_handler **s_xattr;
#ifdef CONFIG_FS_ENCRYPTION
	const struct fscrypt_operations	*s_cop;
	struct fscrypt_keyring	*s_master_keys;
#endif
#ifdef CONFIG_FS_VERITY
	const struct fsverity_operations *s_vop;
#endif
#if IS_ENABLED(CONFIG_UNICODE)
	struct unicode_map *s_encoding;
	__u16 s_encoding_flags;
#endif
	struct hlist_bl_head	s_roots;           /* NFS 的备用根 */
	struct list_head	s_mounts;          /* 挂载实例。文件系统不要自己用 */
	struct block_device	*s_bdev;           /* 磁盘文件系统才有 */
	struct bdev_handle	*s_bdev_handle;
	struct backing_dev_info *s_bdi;            /* 回写设备 */
	struct mtd_info		*s_mtd;
	struct hlist_node	s_instances;       /* 挂在 file_system_type->fs_supers */
	unsigned int		s_quota_types;
	struct quota_info	s_dquot;

	struct sb_writers	s_writers;         /* 冻住超级块时挡住新的写 */

	/*
	 * 这几项放一起：经常读、很少改。
	 */
	void			*s_fs_info;        /* 文件系统私有，tmpfs 放 shmem_sb_info */
	u32			s_time_gran;       /* c/m/atime 的纳秒粒度 */
	time64_t		s_time_min;
	time64_t		s_time_max;
#ifdef CONFIG_FSNOTIFY
	__u32			s_fsnotify_mask;
	struct fsnotify_mark_connector __rcu	*s_fsnotify_marks;
#endif

	char			s_id[32];          /* 给日志看的名字 */
	uuid_t			s_uuid;
	u8			s_uuid_len;

	unsigned int		s_max_links;
	struct mutex		s_vfs_rename_mutex; /* 只给 VFS 的 rename 用 */
	const char		*s_subtype;        /* /proc/mounts 里 type.subtype */

	const struct dentry_operations *s_d_op; /* 新建 dentry 的默认 d_op */

	struct shrinker		s_shrink;          /* 每超级块一个 shrinker，见 08 */

	atomic_long_t		s_remove_count;    /* nlink 已是 0 但仍被引用的 inode 数 */
	atomic_long_t		s_fsnotify_connectors;
	int			s_readonly_remount; /* 正在改只读状态 */
	errseq_t		s_wb_err;          /* syncfs 报告的回写错误 */
	struct workqueue_struct *s_dio_done_wq;  /* Direct IO 完成从中断上下文挪回来 */
	struct hlist_head	s_pins;

	/*
	 * 解释 uid/gid、配额、设备节点的用户命名空间。
	 */
	struct user_namespace	*s_user_ns;

	struct list_lru		s_dentry_lru;      /* 这个超级块的 dentry LRU */
	struct list_lru		s_inode_lru;
	struct rcu_head		rcu;
	struct work_struct	destroy_work;

	struct mutex		s_sync_lock;       /* sync 串行 */
	int			s_stack_depth;     /* 在文件系统栈里的深度 */

	spinlock_t		s_inode_list_lock ____cacheline_aligned_in_smp;
	struct list_head	s_inodes;          /* 全部 inode */

	spinlock_t		s_inode_wblist_lock;
	struct list_head	s_inodes_wb;       /* 正在回写的 inode */
} __randomize_layout;
```

`alloc_super` 把 `s_shrink.scan_objects` 设成 `super_cache_scan`，`count_objects` 设成 `super_cache_count`（`fs/super.c:387`）。回收怎么进到这里见 [08](./08-dcache-icache.md)，`shrink_slab` 本身见 [MM §10.1](../memory-management/10-01-overview.md)。

---

## 2.6 四张函数表

函数指针可以是 `NULL`。VFS 在调用前自己判断，例如 `vfs_read` 先看 `f_op->read`，没有再看 `read_iter`（`fs/read_write.c:467`）。

```c
/* include/linux/fs.h:1915 */
struct file_operations {
	struct module *owner;
	loff_t (*llseek) (struct file *, loff_t, int);
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter) (struct kiocb *, struct iov_iter *);   /* 现在的读入口 */
	ssize_t (*write_iter) (struct kiocb *, struct iov_iter *);
	int (*iopoll)(struct kiocb *kiocb, struct io_comp_batch *,
			unsigned int flags);                         /* 轮询完成，块层用 */
	int (*iterate_shared) (struct file *, struct dir_context *); /* 读目录 */
	__poll_t (*poll) (struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	int (*mmap) (struct file *, struct vm_area_struct *);
	unsigned long mmap_supported_flags;
	int (*open) (struct inode *, struct file *);
	int (*flush) (struct file *, fl_owner_t id);                 /* close 时、fput 之前 */
	int (*release) (struct inode *, struct file *);              /* 最后一次 fput */
	int (*fsync) (struct file *, loff_t, loff_t, int datasync);
	int (*fasync) (int, struct file *, int);
	int (*lock) (struct file *, int, struct file_lock *);        /* POSIX 字节范围锁 */
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	int (*check_flags)(int);
	int (*flock) (struct file *, int, struct file_lock *);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	void (*splice_eof)(struct file *file);
	int (*setlease)(struct file *, int, struct file_lock **, void **);
	long (*fallocate)(struct file *file, int mode, loff_t offset, loff_t len);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
#ifndef CONFIG_MMU
	unsigned (*mmap_capabilities)(struct file *);
#endif
	ssize_t (*copy_file_range)(struct file *, loff_t, struct file *,
			loff_t, size_t, unsigned int);
	loff_t (*remap_file_range)(struct file *file_in, loff_t pos_in,
				   struct file *file_out, loff_t pos_out,
				   loff_t len, unsigned int remap_flags);
	int (*fadvise)(struct file *, loff_t, loff_t, int);
	int (*uring_cmd)(struct io_uring_cmd *ioucmd, unsigned int issue_flags);
	int (*uring_cmd_iopoll)(struct io_uring_cmd *, struct io_comp_batch *,
				unsigned int poll_flags);
} __randomize_layout;
```

`uring_cmd` 是 `io_uring` 的文件系统命令入口。提交环和完成队列不在本章，留给以后的 block-io 章。

```c
/* include/linux/fs.h:1967 */
struct inode_operations {
	struct dentry * (*lookup) (struct inode *, struct dentry *, unsigned int);
	const char * (*get_link) (struct dentry *, struct inode *, struct delayed_call *);
	int (*permission) (struct mnt_idmap *, struct inode *, int);
	struct posix_acl * (*get_inode_acl)(struct inode *, int, bool);

	int (*readlink) (struct dentry *, char __user *, int);

	int (*create) (struct mnt_idmap *, struct inode *, struct dentry *,
		       umode_t, bool);
	int (*link) (struct dentry *, struct inode *, struct dentry *);
	int (*unlink) (struct inode *, struct dentry *);
	int (*symlink) (struct mnt_idmap *, struct inode *, struct dentry *,
			const char *);
	int (*mkdir) (struct mnt_idmap *, struct inode *, struct dentry *,
		      umode_t);
	int (*rmdir) (struct inode *, struct dentry *);
	int (*mknod) (struct mnt_idmap *, struct inode *, struct dentry *,
		      umode_t, dev_t);
	int (*rename) (struct mnt_idmap *, struct inode *, struct dentry *,
			struct inode *, struct dentry *, unsigned int);
	int (*setattr) (struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr) (struct mnt_idmap *, const struct path *,
			struct kstat *, u32, unsigned int);
	ssize_t (*listxattr) (struct dentry *, char *, size_t);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start,
		      u64 len);
	int (*update_time)(struct inode *, int);
	int (*atomic_open)(struct inode *, struct dentry *,
			   struct file *, unsigned open_flag,
			   umode_t create_mode);          /* 查找和打开合成一步 */
	int (*tmpfile) (struct mnt_idmap *, struct inode *,
			struct file *, umode_t);
	struct posix_acl *(*get_acl)(struct mnt_idmap *, struct dentry *, int);
	int (*set_acl)(struct mnt_idmap *, struct dentry *,
		       struct posix_acl *, int);
	int (*fileattr_set)(struct mnt_idmap *idmap,
			    struct dentry *dentry, struct fileattr *fa);
	int (*fileattr_get)(struct dentry *dentry, struct fileattr *fa);
	struct offset_ctx *(*get_offset_ctx)(struct inode *inode);
} ____cacheline_aligned;
```

`lookup` 的调用方是慢路径 `__lookup_slow`（`fs/namei.c:1707`）：缓存没有这个名字时，分配一个新 dentry，再让文件系统去磁盘或自己的索引里找。

```c
/* include/linux/fs.h:2058 */
struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb); /* 不填则用普通 inode 缓存 */
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);

	void (*dirty_inode) (struct inode *, int flags);
	int (*write_inode) (struct inode *, struct writeback_control *wbc);
	int (*drop_inode) (struct inode *);    /* 不填则 generic_drop_inode：nlink 0 或已摘哈希就丢 */
	void (*evict_inode) (struct inode *);  /* 真正清页缓存、释放文件系统私有数据 */
	void (*put_super) (struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*freeze_super) (struct super_block *, enum freeze_holder who);
	int (*freeze_fs) (struct super_block *);
	int (*thaw_super) (struct super_block *, enum freeze_holder who);
	int (*unfreeze_fs) (struct super_block *);
	int (*statfs) (struct dentry *, struct kstatfs *);
	int (*remount_fs) (struct super_block *, int *, char *);
	void (*umount_begin) (struct super_block *);

	int (*show_options)(struct seq_file *, struct dentry *); /* /proc/mounts 的选项 */
	int (*show_devname)(struct seq_file *, struct dentry *);
	int (*show_path)(struct seq_file *, struct dentry *);
	int (*show_stats)(struct seq_file *, struct dentry *);
#ifdef CONFIG_QUOTA
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);
	struct dquot __rcu **(*get_dquots)(struct inode *);
#endif
	long (*nr_cached_objects)(struct super_block *, struct shrink_control *);
	long (*free_cached_objects)(struct super_block *, struct shrink_control *);
	void (*shutdown)(struct super_block *sb);
};
```

`generic_drop_inode`（`fs.h:2911`）返回真的条件是 `i_nlink == 0` 或 inode 已经不在哈希上。`iput_final`（`fs/inode.c:1777`）在 `s_op->drop_inode` 为空时用它。

谁填这些表、VFS 在哪调用，对照如下。这张表不是结构体布局。

| 表 | 谁填 | VFS 在哪用 |
|---|---|---|
| `super_operations` | 文件系统的 `fill_super` / `get_tree` | `alloc_inode`、`evict_inode`、`sync_fs`、`statfs` |
| `inode_operations` | 建 inode 时赋给 `i_op` | `lookup`、`create`、`getattr`、`get_link` |
| `file_operations` | 建 inode 时赋给 `i_fop`；打开时拷到 `f_op` | `read_iter`、`write_iter`、`mmap`、`fsync`、`release` |
| `dentry_operations` | `s_d_op` 或文件系统对单个 dentry 设置 | `d_revalidate`、`d_hash`、`d_compare`、`d_delete` |
| `address_space_operations` | 文件系统赋给 `i_mapping->a_ops` | 页缓存读页、标脏、Direct IO。字段在 MM §8.1 |

`a_ops` 不在这四张表里，但读写一定会碰到。它的 `read_folio`、`dirty_folio`、`direct_IO`、`writepages` 定义在 `include/linux/fs.h:404`。内存侧怎么用它们，见 [MM §08](../memory-management/08-page-cache-and-writeback.md)。

---

## 2.7 `file_system_type`

注册一种文件系统，不是注册一个已挂上的实例。实例是 `super_block`。

```c
/* include/linux/fs.h:2367 */
struct file_system_type {
	const char *name;                  /* "tmpfs"、"proc"、"sysfs" */
	int fs_flags;                      /* FS_REQUIRES_DEV、FS_USERNS_MOUNT 等，紧挨着定义 */
	int (*init_fs_context)(struct fs_context *); /* 新挂载 API 的入口 */
	const struct fs_parameter_spec *parameters;
	struct dentry *(*mount) (struct file_system_type *, int,
		       const char *, void *);       /* 旧 mount 回调。新代码走 init_fs_context */
	void (*kill_sb) (struct super_block *);
	struct module *owner;
	struct file_system_type *next;     /* 已注册类型的链表 */
	struct hlist_head fs_supers;       /* 这个类型下的全部超级块 */

	/* 下面是 lockdep 的钥匙，让不同文件系统的锁在报告里能分开 */
	struct lock_class_key s_lock_key;
	struct lock_class_key s_umount_key;
	struct lock_class_key s_vfs_rename_key;
	struct lock_class_key s_writers_key[SB_FREEZE_LEVELS];
	struct lock_class_key i_lock_key;
	struct lock_class_key i_mutex_key;
	struct lock_class_key invalidate_lock_key;
	struct lock_class_key i_mutex_dir_key;
};
```

和 `fs_flags` 写在一起的位（`fs.h:2370`）：`FS_REQUIRES_DEV` = 1，`FS_BINARY_MOUNTDATA` = 2，`FS_HAS_SUBTYPE` = 4，`FS_USERNS_MOUNT` = 8，`FS_DISALLOW_NOTIFY_PERM` = 16，`FS_ALLOW_IDMAP` = 32，`FS_RENAME_DOES_D_MOVE` = 32768。

tmpfs 的类型名是 `"tmpfs"`（`mm/shmem.c:4609`），`init_fs_context` 是 `shmem_init_fs_context`。proc 的名字是 `"proc"`（`fs/proc/root.c:278`）。

---

## 2.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/path.h` | `struct path` | 8 | 挂载指针 + dentry 指针 |
| `include/linux/dcache.h` | `struct dentry` | 82 | 名字缓存 |
| `include/linux/dcache.h` | `d_is_negative` | 433 | 类型位是 `DCACHE_MISS_TYPE` |
| `include/linux/fs.h` | `struct inode` | 641 | 文件本体 |
| `include/linux/fs.h` | `struct file` | 994 | 一次打开 |
| `include/linux/fs.h` | `struct super_block` | 1190 | 一个文件系统实例 |
| `include/linux/fs.h` | `struct file_operations` | 1915 | 打开之后的操作 |
| `include/linux/fs.h` | `struct inode_operations` | 1967 | 目录项和属性操作 |
| `include/linux/fs.h` | `struct super_operations` | 2058 | 超级块生命周期 |
| `fs/inode.c` | `inode_init_always` | 156 | `i_count = 1`，`i_mapping = &i_data` |
| `fs/open.c` | `do_dentry_open` | 873 | 把 `i_fop` 装进 `f_op` 并调用 `open` |
| `fs/open.c` | `vfs_open` | 1062 | `f_path = *path`，再 `do_dentry_open` |
| `include/linux/fs.h` | `generic_drop_inode` | 2911 | 链接数为 0 或已摘哈希则丢 inode |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [路径查找 →](./03-path-lookup.md)
