# 3. 路径查找

**结论**：`open` 的路径查找在 `path_openat` 里先做 RCU-walk：不拿 dentry 引用，靠 `d_seq` 发现并发修改；任何一步对不上就返回 `-ECHILD`，`do_filp_open` 再用不带 `LOOKUP_RCU` 的 REF-walk 重走。最后一个分量才决定打开、创建或跟随符号链接。

> 主路径：`fs/namei.c`。`do_filp_open` 在 `:3875`，`path_openat` 在 `:3836`，`link_path_walk` 在 `:2260`。

C 新手把路径想成一串名字，不是一串 inode。`/tmp/a/b` 先定位起点（根或 `dirfd`），再对 `tmp`、`a`、`b` 各做一次「在父 dentry 的哈希桶里找孩子」。找到的是 `dentry`。`dentry->d_inode` 才是文件。

---

## 3.1 查找用的栈：`nameidata`

```c
/* fs/namei.c:572。EMBEDDED_LEVELS 是 2 */
#define EMBEDDED_LEVELS 2
struct nameidata {
	struct path	path;          /* 当前走到的挂载 + dentry */
	struct qstr	last;          /* 正在处理的这一截名字 */
	struct path	root;          /* 这次查找的根。chroot / openat2 的 RESOLVE_IN_ROOT 会改它 */
	struct inode	*inode;        /* path.dentry->d_inode 的缓存 */
	unsigned int	flags, state;  /* flags 是 LOOKUP_*；state 是 ND_* */
	unsigned	seq, next_seq, m_seq, r_seq;
	/* seq：当前 dentry 的 d_seq。m_seq：mount_lock。r_seq：rename_lock */
	int		last_type;     /* LAST_NORM / LAST_DOT / LAST_DOTDOT / LAST_ROOT */
	unsigned	depth;         /* 已经压栈、还没走完的符号链接层数 */
	int		total_link_count; /* 这次查找跟过的符号链接总数 */
	struct saved {
		struct path link;
		struct delayed_call done; /* get_link 要推迟释放的缓冲 */
		const char *name;         /* 跟随链接之前，原路径还剩的部分 */
		unsigned seq;
	} *stack, internal[EMBEDDED_LEVELS];
	struct filename	*name;     /* 用户传进来的路径，内核拷贝 */
	struct nameidata *saved;  /* 嵌套查找时指向上一层 nameidata */
	unsigned	root_seq;
	int		dfd;
	vfsuid_t	dir_vfsuid;    /* 最后一个目录的 uid，创建文件时用 */
	umode_t		dir_mode;
} __randomize_layout;
```

`ND_ROOT_PRESET` = 1，`ND_ROOT_GRABBED` = 2，`ND_JUMPED` = 4（`namei.c:597`）。`ND_JUMPED` 表示走过 `..`、挂载点或符号链接，弱 revalidate 会用到。

`MAXSYMLINKS` 是 40（`include/linux/namei.h:13`）。`reserve_stack` 在 `total_link_count` 达到 40 时返回 `-ELOOP`（`namei.c:1742`）。前两层符号链接用 `internal[]`，更深才 `kmalloc`（`nd_alloc_stack`，`:637`）。RCU 模式下这次分配用 `GFP_ATOMIC`。

`LOOKUP_*` 里和本章有关的位（`include/linux/namei.h:16` 起）：

| 宏 | 值 | 含义 |
|---|---|---|
| `LOOKUP_FOLLOW` | `0x0001` | 最后一截如果是符号链接也要跟 |
| `LOOKUP_DIRECTORY` | `0x0002` | 结果必须是目录 |
| `LOOKUP_AUTOMOUNT` | `0x0004` | 终点也做自动挂载 |
| `LOOKUP_PARENT` | `0x0010` | 停在父目录。`link_path_walk` 自己会加上，走完再清 |
| `LOOKUP_REVAL` | `0x0020` | `d_revalidate` 不要信缓存 |
| `LOOKUP_RCU` | `0x0040` | RCU-walk |
| `LOOKUP_MOUNTPOINT` | `0x0080` | 终点跨挂载 |
| `LOOKUP_OPEN` | `0x0100` | 这次是 open |
| `LOOKUP_CREATE` | `0x0200` | 可能创建 |
| `LOOKUP_EXCL` | `0x0400` | 独占创建 |
| `LOOKUP_RENAME_TARGET` | `0x0800` | rename 的目标 |
| `LOOKUP_EMPTY` | `0x4000` | 允许空路径 |
| `LOOKUP_DOWN` | `0x8000` | 起点就跨挂载 |
| `LOOKUP_NO_SYMLINKS` | `0x010000` | 不许穿过符号链接 |
| `LOOKUP_NO_MAGICLINKS` | `0x020000` | 不许 `nd_jump_link` |
| `LOOKUP_NO_XDEV` | `0x040000` | 不许跨挂载 |
| `LOOKUP_BENEATH` | `0x080000` | 不许走出起点 |
| `LOOKUP_IN_ROOT` | `0x100000` | 把 dirfd 当根 |
| `LOOKUP_CACHED` | `0x200000` | 只查缓存，不允许慢路径 |

`LOOKUP_IS_SCOPED`（`:43`）是 `LOOKUP_BENEATH | LOOKUP_IN_ROOT`。`openat2` 的 `RESOLVE_*` 会翻译成这些位。翻译函数在 `build_open_flags`，本章只追到 `do_filp_open` 拿到的 `op->lookup_flags`。

---

## 3.2 调用链

```text
do_filp_open                         fs/namei.c:3875
  set_nameidata                      :615     current->nameidata = &nd
  path_openat(flags | LOOKUP_RCU)    :3883
    返回 -ECHILD：
      path_openat(flags)             :3885    去掉 LOOKUP_RCU，REF-walk
    返回 -ESTALE：
      path_openat(flags|LOOKUP_REVAL):3887
  restore_nameidata                  :3888

path_openat                          :3836
  alloc_empty_file                   先拿到一个还没打开的 struct file
  O_TMPFILE：do_tmpfile              :3801
  O_PATH：do_o_path                  :3824    只做 path_lookupat + vfs_open
  普通打开：
    s = path_init                    :2363    定起点，RCU 模式在这里 rcu_read_lock
    while 没有错误，且 link_path_walk 返回 0：
        s = open_last_lookups        :3567    非 NULL 表示最后一个分量是还要跟的符号链接
    do_open                          :3651
    terminate_walk                   :686     RCU 模式 leave_rcu；否则 path_put
```

`path_openat` 的循环（`:3851`）是：

```text
s = path_init(nd, flags);
while (!(error = link_path_walk(s, nd)) &&
       (s = open_last_lookups(nd, file, op)) != NULL)
    ;
if (!error)
    error = do_open(nd, file, op);
terminate_walk(nd);
```

`link_path_walk` 把除最后一截以外的分量都走完，最后一截留在 `nd->last`。`open_last_lookups` 处理这最后一截：普通查找、创建，或者发现符号链接后把剩余路径交回来，外层 `while` 再走。

`do_open`（`:3651`）在文件还没 `FMODE_OPENED` 时调用 `vfs_open`（`:3695`）。`vfs_open` 把 `path` 抄进 `file->f_path`，再进 `do_dentry_open`（`fs/open.c:1062`）。

---

## 3.3 `link_path_walk`：一截一截走

`link_path_walk`（`:2260`）先跳过开头的 `/`。路径空了就直接返回 0，当前 `nd->path` 就是结果。

对每一截：

```text
may_lookup                         :1729    父目录要有执行权限。RCU 下权限检查带 MAY_NOT_BLOCK
hash_name(父 dentry, name)        :2236 或带 word-at-a-time 的版本
  "." / ".." 记成 LAST_DOT / LAST_DOTDOT
父 dentry 带 DCACHE_OP_HASH：
  d_op->d_hash                     :2304    文件系统可以改这一截的哈希
把这一截放进 nd->last
后面还有 '/'：
  walk_component(nd, WALK_MORE)    :1998
这一截是末尾：
  返回 0，留给 open_last_lookups
walk_component 返回非空字符串：
  这是符号链接正文，把它当成新的 name 继续 for 循环
```

`walk_component`（`:1998`）：

```text
last_type 不是 LAST_NORM：
  handle_dots                      :1960    "." 什么都不做；".." 走 follow_dotdot(_rcu)
否则：
  dentry = lookup_fast             :1631
  返回 NULL：lookup_slow           :1717    缓存没有，问文件系统
  step_into                        :1848    跨挂载，或发现符号链接
```

`step_into` 先 `handle_mounts`（`:1541`）。不是符号链接，或者调用方要求不跟，就把 `nd->path` 换成新的 dentry，返回 `NULL`。是符号链接并且该跟，就 `pick_link`（`:1768`），返回链接正文给 `link_path_walk` 继续扫。

---

## 3.4 RCU-walk 和 REF-walk

**RCU-walk**（`nd->flags & LOOKUP_RCU`）：

- `path_init` 调用 `rcu_read_lock`（`namei.c:2375`）。
- 不 `dget`。dentry 可能正在被别人 `dput`，只要还在 RCU 宽限期内，指针还能读。
- 每跨一个 dentry，用 `d_seq` 做顺序锁检查。`read_seqcount_retry` 失败说明查找过程中这个 dentry 被改过，返回 `-ECHILD`。
- 权限、revalidate、`get_link` 都不许睡眠。要睡眠就先「变正」：`try_to_unlazy`（`:779`）给当前 path 补上真正的引用，清掉 `LOOKUP_RCU`，`rcu_read_unlock`。补不上就仍然 `-ECHILD`。
- 哈希查找用 `__d_lookup_rcu`（`fs/dcache.c:2356`），它在 RCU 读锁下走桶，不拿 dentry 引用。

**REF-walk**（没有 `LOOKUP_RCU`）：

- 每个走到的 dentry 都 `dget`，离开时 `dput`。
- 慢路径可以拿 `inode->i_rwsem`、分配 dentry、调用 `i_op->lookup`，这些都会睡眠。
- 哈希查找用 `__d_lookup`（`fs/dcache.c:2465`）。它找到后会把引用加上。对外的 `d_lookup`（`:2435`）再加 `rename_lock`，避免和 rename 交错。

`do_filp_open` 把两次尝试串起来（`:3883`）：

```text
path_openat(..., flags | LOOKUP_RCU)
  结果是 -ECHILD：
    path_openat(..., flags)          同一条路径，改用引用计数
  结果是 -ESTALE：
    path_openat(..., flags | LOOKUP_REVAL)
```

`-ECHILD` 在这里的意思是「RCU 走查失败，请重试」，不是用户能看到的最终错误。用户看到的是第二次的结果。`LOOKUP_CACHED` 要求必须在 RCU 模式（`path_init`，`:2369`），退不回 REF-walk 时返回 `-EAGAIN`。

`try_to_unlazy`（`:769` 的注释）只尝试把 `nd->path` 和 `nd->root` 变成持有引用。失败之后到 `terminate_walk` 之前，不要再碰这份 `nameidata`。`try_to_unlazy_next`（`:815`）是「下一个 dentry 已经找到了」的版本。

---

## 3.5 dcache 哈希

全局桶数组是 `dentry_hashtable`（`fs/dcache.c:101`），桶头类型是 `hlist_bl_head`：一把位锁加一条 hlist。哈希函数：

```c
/* fs/dcache.c:103 */
static inline struct hlist_bl_head *d_hash(unsigned int hash)
{
	return dentry_hashtable + (hash >> d_hash_shift);
}
```

`d_hash_shift` 在 `dcache_init` 里由 `alloc_large_system_hash` 算出来（`:3323` 起，早启动的路径在 `:3295`）。名字的哈希本身来自 `hash_name` / `full_name_hash`（`fs/namei.c`，是否按机器字比较取决于 `CONFIG_DCACHE_WORD_ACCESS`）。盐是父 dentry 指针，所以同名文件在不同目录里落进不同的桶。

桶里还要父指针和名字都对上。`__d_lookup_rcu` 的参数就是父 dentry、`qstr`、以及用来回填的 seq（`dcache.c:2356`）。

另一张小表 `in_lookup_hashtable` 只有 `1 << 10` 个桶（`dcache.c:108`）。它不存已经查完的名字，只存「正在问文件系统」的 dentry。`d_alloc_parallel`（`:2643`）让两个并发的慢查找共用同一个 in-lookup dentry，避免同一个名字建两次。查完调用 `d_lookup_done`。

慢路径 `__lookup_slow`（`namei.c:1680`）：

```text
目录已死：-ENOENT
d_alloc_parallel
  已经有人查完（不再是 in-lookup）：d_revalidate，失败就作废重来
  仍是 in-lookup：
    inode->i_op->lookup          文件系统把 inode 填进这个 dentry，或返回另一个
    d_lookup_done
```

`lookup_slow`（`:1717`）在调用前后加 `inode_lock_shared`。创建路径 `lookup_open` 会把锁升级成独占，见 [04](./04-fd-and-open.md) 只接到 `open_last_lookups`，不重复锁的细节。

负 dentry 也会进这张哈希。下次查找命中它，就知道「这个名字不存在」，不必再叫文件系统。类型位和回收见 [08](./08-dcache-icache.md)。

---

## 3.6 越过挂载点

`step_into` 调用 `handle_mounts`（`namei.c:1541`）。RCU 下先试 `__follow_mount_rcu`。失败就 `try_to_unlazy_next`，再走会睡眠的 `traverse_mounts`（`:1445`）。

`traverse_mounts` 的快路径：dentry 没有 `DCACHE_MANAGED_DENTRY` 就停。负项在这里返回 `-ENOENT`（`:1453`，用 `d_flags_negative`）。

慢路径 `__traverse_mounts`（`:1388`）按标志循环：

```text
DCACHE_MANAGE_TRANSIT：
  LOOKUP_NO_XDEV 则 -EXDEV
  否则 d_op->d_manage
DCACHE_MOUNTED：
  lookup_mnt(path)                 fs/namespace.c:720
    当前命名空间里有挂载：
      换成 mounted->mnt_root，继续看新的根上还有没有叠挂载
DCACHE_NEED_AUTOMOUNT：
  follow_automount                 文件系统的 d_automount
```

`lookup_mnt` 在挂载哈希里找「这个 `vfsmount` + 这个 dentry」上的孩子。`struct mount` 和 `vfsmount` 的差别见 [07](./07-mount-and-ns.md)。

`".."` 走到挂载根时要退回父挂载。RCU 版本是 `follow_dotdot_rcu`（`namei.c:1888`）：如果当前 dentry 就是 `mnt->mnt_root`，用 `choose_mountpoint_rcu` 找到挂载点，再拿挂载点的父 dentry。`LOOKUP_NO_XDEV` 在这里返回 `-ECHILD`（`:1900`），让 REF-walk 给出确定的 `-EXDEV`。`LOOKUP_BENEATH` 下 `..` 已经在根上则失败（`:1922`）。

---

## 3.7 符号链接

`pick_link`（`:1768`）做这几件事：

```text
reserve_stack                    总数到 MAXSYMLINKS（40）就 -ELOOP
LOOKUP_NO_SYMLINKS 或挂载带 MNT_NOSYMFOLLOW：-ELOOP
安全检查 security_inode_follow_link
目标字符串：
  先读 inode->i_link             快符号链接，正文放在 inode 里
  为空则 i_op->get_link          RCU 下先传 NULL dentry；返回 -ECHILD 再 unlazy 重试
```

链接正文交回 `link_path_walk`。正文以 `/` 开头时，`pick_link` 调用 `nd_jump_root`（`:1826`）再跳过多余的斜杠。相对链接接着当前目录走。`link_path_walk` 用 `nd->stack[].name` 记住原路径还没走完的部分（`:2348`）。

`WALK_TRAILING` 表示这是最后一截。`step_into` 里，最后一截的符号链接只有 `LOOKUP_FOLLOW` 置位才跟（`:1858`）。`build_open_flags` 把 `lookup_flags` 从 0 起算（`fs/open.c:1226`），没有 `O_NOFOLLOW` 就或上 `LOOKUP_FOLLOW`（`:1325`）。`O_CREAT|O_EXCL` 会先把 `O_NOFOLLOW` 或上（`:1319`），独占创建不跟随最后一截。末尾带 `/` 时 `open_last_lookups` 还会再或上 `LOOKUP_FOLLOW | LOOKUP_DIRECTORY`（`namei.c:3586`）。

---

## 3.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/namei.h` | `MAXSYMLINKS` | 13 | 40。再跟就 `-ELOOP` |
| `fs/namei.c` | `struct nameidata` | 573 | 一次查找的全部状态 |
| `fs/namei.c` | `path_init` | 2363 | 定起点，RCU 模式加读锁 |
| `fs/namei.c` | `link_path_walk` | 2260 | 走完除最后一截外的全部分量 |
| `fs/namei.c` | `walk_component` | 1998 | 一截：点、快查找或慢查找 |
| `fs/namei.c` | `lookup_fast` | 1631 | RCU 用 `__d_lookup_rcu`，否则 `__d_lookup` |
| `fs/namei.c` | `lookup_slow` | 1717 | 共享锁下调用 `i_op->lookup` |
| `fs/namei.c` | `step_into` | 1848 | 跨挂载或捡起符号链接 |
| `fs/namei.c` | `handle_mounts` | 1541 | RCU 失败再 `traverse_mounts` |
| `fs/namei.c` | `__traverse_mounts` | 1388 | 挂载、自动挂载、`d_manage` |
| `fs/namei.c` | `pick_link` | 1768 | 取符号链接正文 |
| `fs/namei.c` | `try_to_unlazy` | 779 | RCU-walk 改成持有引用 |
| `fs/namei.c` | `open_last_lookups` | 3567 | 最后一截：打开、创建或再跟链接 |
| `fs/namei.c` | `path_openat` | 3836 | 分配 `struct file` 并走完 |
| `fs/namei.c` | `do_filp_open` | 3875 | RCU，失败再 REF，再 `LOOKUP_REVAL` |
| `fs/dcache.c` | `d_hash` | 103 | `hash >> d_hash_shift` 选桶 |
| `fs/dcache.c` | `__d_lookup_rcu` | 2356 | RCU 下的哈希查找 |
| `fs/dcache.c` | `__d_lookup` | 2465 | 找到后持有引用 |
| `fs/dcache.c` | `d_lookup` | 2435 | 再拿 `rename_lock` |
| `fs/dcache.c` | `d_alloc_parallel` | 2643 | 慢路径共用 in-lookup dentry |
| `fs/namespace.c` | `lookup_mnt` | 720 | 这个 dentry 上有没有挂载 |

---

**导航**：[← 对象](./02-core-objects.md) · [目录](./README.md) · [fd 与 open →](./04-fd-and-open.md)
