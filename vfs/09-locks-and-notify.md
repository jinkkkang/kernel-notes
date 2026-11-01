# 9. 文件锁与通知

**结论**：v6.6.160 里字节范围锁走 `fcntl` 的 `F_GETLK` / `F_SETLK` / `F_SETLKW`，整文件建议锁走 `flock(2)`，租约走 `F_SETLEASE`。目录变化通知有三套系统调用：老的 `F_NOTIFY`（dnotify）、`inotify_*`、`fanotify_init` / `fanotify_mark`。三套都在这份源码里。

> 锁的对象在 `include/linux/filelock.h`。实现在 `fs/locks.c`。inotify 在 `fs/notify/inotify/inotify_user.c`，fanotify 在 `fs/notify/fanotify/fanotify_user.c`。

C 新手先分清「锁住哪一段」和「谁会收到事件」。锁挂在 `inode->i_flctx` 上，挡住别的打开或别的 `fcntl`。通知挂在 `inode->i_fsnotify_marks` 上，操作发生之后往一个 fd 里塞事件，默认不挡住那个操作。fanotify 的权限类事件是例外，那一类会等用户态回答。

---

## 9.1 锁放在哪

```c
/* include/linux/filelock.h:149 */
struct file_lock_context {
	spinlock_t		flc_lock;
	struct list_head	flc_flock;   /* flock(2) */
	struct list_head	flc_posix;   /* fcntl 字节范围锁 */
	struct list_head	flc_lease;   /* F_SETLEASE / F_GETLEASE */
};
```

`inode->i_flctx` 一开始是 `NULL`。第一次加锁才分配。分配点可以看 tracepoint `locks_get_lock_context`（`include/trace/events/filelock.h:39`）。

`struct file_lock`（`filelock.h:107`）前半是一个匿名 union：一边是 `struct file_lock_core c`，另一边把同样的字段展开成 `fl_blocker`、`fl_list`、`fl_owner`、`fl_flags`、`fl_type`、`fl_pid` 这些老名字。后面才是范围和租约专用字段：

```c
struct file_lock {
	union {
		struct file_lock_core c;
		struct { /* fl_blocker、fl_list、fl_owner、fl_type、fl_pid … */ };
	};
	loff_t fl_start;
	loff_t fl_end;
	struct fasync_struct *fl_fasync; /* 租约被打断时发信号 */
	unsigned long fl_break_time;
	unsigned long fl_downgrade_time;
	const struct file_lock_operations *fl_ops;
	const struct lock_manager_operations *fl_lmops;
	union { /* nfs / nfs4 / afs / ceph 的私有 */ } fl_u;
};
```

`fl_type` 用的是 `F_RDLCK`、`F_WRLCK`、`F_UNLCK`。`lock_is_unlock` / `lock_is_read` / `lock_is_write` 在 `filelock.h:170` 起。

三条链表互不解释对方的锁。`flock` 看整文件，POSIX 锁看字节区间，lease 看的是「还有没有别的打开」。

---

## 9.2 `flock(2)`

系统调用在 `fs/locks.c:2069`。

```text
flock(fd, cmd)
  cmd 带 LOCK_MAND：打一条 pr_warn_once，直接返回 0     :2083
    注释：强制锁早就不能挡住 open/read/write，请求被忽略
  flock_translate_cmd(cmd & ~LOCK_NB)
  fdget
  不是解锁、又没有读或写模式：失败
  flock_make_lock
  security_file_lock
  没有 LOCK_NB：fl_flags |= FL_SLEEP
  f_op->flock 非空：交给文件系统
  否则 locks_lock_file_wait
```

`LOCK_NB` 表示不要睡。文件系统没有自己的 `flock` 时，VFS 把锁放进 `flc_flock`。冲突检测在 `flock_lock_inode`（`:1022`）。tracepoint 名是 `flock_lock_inode`（`filelock.h:116`）。

`flock` 的所有者是这份 `struct file`，不是进程号。`dup` 只是多一个 fd，槽里仍是同一个指针。不带 `CLONE_FILES` 的 `fork` 会新建 `files_struct`，但 `dup_fd` 对每个已经打开的 file 做 `get_file`（`fs/file.c:384`），父子仍共享同一份 `struct file`，因此共享这把 flock。

---

## 9.3 POSIX 字节范围锁

`fcntl`（`fs/fcntl.c:436`）里和锁有关的命令：

| 命令 | 处理 | 行号 |
|---|---|---|
| `F_GETLK` | `fcntl_getlk` | `fcntl.c:343`，实现 `locks.c:2217` |
| `F_SETLK` | `fcntl_setlk` | `fcntl.c:356`，实现 `locks.c:2343` |
| `F_SETLKW` | 同上，允许睡眠 | `fcntl.c:357` |
| `F_SETLEASE` | `fcntl_setlease` | `fcntl.c:399`，实现 `locks.c:1999` |
| `F_GETLEASE` | `fcntl_getlease` | `fcntl.c:396` |
| `F_OFD_GETLK` / `F_OFD_SETLK` / `F_OFD_SETLKW` | 和 POSIX 锁同一套函数，所有者是这份打开 | `fcntl.c:341` 起 |
| `F_NOTIFY` | `fcntl_dirnotify` | `fcntl.c:402`，这是 dnotify |

32 位兼容还有 `F_GETLK64` / `F_SETLK64` / `F_SETLKW64`（`fcntl.c:481` 起，以及 `locks.c:2423` 的 `fcntl_getlk64`）。

`fcntl_setlk` 把用户态的 `struct flock` 收成内核的 `file_lock`，再 `vfs_lock_file`（`locks.c:2291`）。默认实现进 `posix_lock_inode`（`:1094`）。等待版本是 `posix_lock_inode_wait`（`:1360`）。

POSIX 锁按字节闭区间 `[fl_start, fl_end]` 冲突：读锁和读锁可以重叠，写锁和任何锁冲突。所有者是 `fl_owner`，通常是 `files_struct`，所以同一个进程里不同 fd 打开同一文件，锁是通的；`CLONE_FILES` 的线程也通。`close` 时 `filp_flush` 调用 `locks_remove_posix`（`fs/open.c:1537`）。

`F_OFD_SETLK` 这一类 open file description 锁也走 POSIX 链表，但所有者是 `struct file` 而不是进程。标志位是 `FL_OFDLCK`，tracepoint 的 `show_fl_flags` 里有这个名字（`filelock.h:30`）。本章不把 OFD 锁和传统 POSIX 锁的每一条差异展开，只标明它们共用 `flc_posix` 和 `posix_lock_inode`。

---

## 9.4 lease

`fcntl_setlease`（`locks.c:1999`）在 `leases_enable` 为真时调用 `generic_setlease`（`:1870`）。租约挂在 `flc_lease`。

写打开会在 `do_dentry_open` 里 `break_lease`（`fs/open.c:920`）。打断可以是非阻塞或阻塞，对应 tracepoint `break_lease_noblock` 和 `break_lease_block`（`filelock.h:157`）。超时由 `lease_break_time` 控制，单位在 sysctl 里，见 [12](./12-observability.md)。`/proc/sys/fs/leases-enable` 关掉之后，新的 `F_SETLEASE` 会失败；已经加上的租约不在本章跟踪每个回收步骤。

`generic_add_lease` 和 `leases_conflict` 都有同名 tracepoint（`filelock.h:172`、`:207`）。

---

## 9.5 inotify

v6.6.160 的系统调用：

| 系统调用 | 位置 |
|---|---|
| `inotify_init` | `fs/notify/inotify/inotify_user.c:713`，等价于 flags = 0 |
| `inotify_init1` | `:708` |
| `inotify_add_watch` | `:718` |
| `inotify_rm_watch` | `:782` |

`inotify_init1` 的 flags 用 `IN_CLOEXEC` 和 `IN_NONBLOCK`，这两个就是 `O_CLOEXEC` 和 `O_NONBLOCK`（`include/uapi/linux/inotify.h:72`）。`do_inotify_init`（`inotify_user.c:683`）建一个 `fsnotify_group`，再用 `anon_inode_getfd("inotify", ...)` 把它变成 fd（`:700`）。

`inotify_add_watch` 的 mask 是 `IN_*` 事件位（`inotify.h:30` 起）：`IN_ACCESS`、`IN_MODIFY`、`IN_ATTRIB`、`IN_CLOSE_WRITE`、`IN_CLOSE_NOWRITE`、`IN_OPEN`、`IN_MOVED_FROM`、`IN_MOVED_TO`、`IN_CREATE`、`IN_DELETE`、`IN_DELETE_SELF`、`IN_MOVE_SELF`，再加上队列溢出 `IN_Q_OVERFLOW`、忽略 `IN_IGNORED`、卸载 `IN_UNMOUNT`。创建 watch 时还能或上 `IN_ONLYDIR`、`IN_DONT_FOLLOW`、`IN_EXCL_UNLINK`、`IN_MASK_CREATE`、`IN_MASK_ADD`、`IN_ONESHOT`。

事件从被监视 inode 的 `i_fsnotify_marks` 进来，写进这个 group 的队列。队列长度受 `/proc/sys/fs/inotify/max_queued_events` 限制。用户对 inotify fd 做 `read`，读出的是一串 `struct inotify_event`。

---

## 9.6 fanotify

v6.6.160 有两个系统调用，没有第三个：

| 系统调用 | 位置 |
|---|---|
| `fanotify_init` | `fs/notify/fanotify/fanotify_user.c:1409` |
| `fanotify_mark` | `:1909` |

`fanotify_init` 的 flags 在 `include/uapi/linux/fanotify.h`：`FAN_CLOEXEC`、`FAN_NONBLOCK`、通知类 `FAN_CLASS_NOTIF` / `FAN_CLASS_CONTENT` / `FAN_CLASS_PRE_CONTENT`、`FAN_UNLIMITED_QUEUE`、`FAN_UNLIMITED_MARKS`、`FAN_ENABLE_AUDIT`，以及报告格式 `FAN_REPORT_PIDFD`、`FAN_REPORT_TID`、`FAN_REPORT_FID`、`FAN_REPORT_DIR_FID`、`FAN_REPORT_NAME`、`FAN_REPORT_TARGET_FID`、`FAN_REPORT_FD_ERROR`。

事件位包括 `FAN_ACCESS`、`FAN_MODIFY`、`FAN_ATTRIB`、`FAN_CLOSE_WRITE`、`FAN_CLOSE_NOWRITE`、`FAN_OPEN`、`FAN_OPEN_EXEC`、`FAN_CREATE`、`FAN_DELETE`、`FAN_DELETE_SELF`、`FAN_MOVE_SELF`、`FAN_MOVED_FROM`、`FAN_MOVED_TO`、`FAN_RENAME`、`FAN_ONDIR`、`FAN_EVENT_ON_CHILD`，以及权限事件 `FAN_OPEN_PERM`、`FAN_ACCESS_PERM`、`FAN_OPEN_EXEC_PERM`。`FAN_FS_ERROR` 也在（`fanotify.h:23`）。

权限类要 `FAN_CLASS_CONTENT` 或 `FAN_CLASS_PRE_CONTENT`。文件系统类型若带 `FS_DISALLOW_NOTIFY_PERM`（`include/linux/fs.h:2374`，值 16），fanotify 权限事件在这个超级块上不允许。proc 的 `proc_fs_type` 就带了这个标志（`fs/proc/root.c:282`）。

`fanotify_mark` 可以标 inode、挂载或文件系统。mark 的旗标和 `FAN_MARK_ADD` / `FAN_MARK_REMOVE` 在同一个 uapi 头里。读事件得到 `struct fanotify_event_metadata`（`fanotify.h:131`）。带 `FAN_REPORT_FID` 时后面还跟着 `fanotify_event_info_fid`。

和 inotify 的差别，用这张表就够：

| | inotify | fanotify |
|---|---|---|
| 监视什么 | 一个 inode 路径 | inode、挂载或整个文件系统 |
| 权限事件 | 没有 | `FAN_*_PERM`，用户态不回答就堵住操作 |
| 事件里的身份 | wd 整数 | 可以是 fd，或 fid |
| 系统调用 | init / init1 / add_watch / rm_watch | init / mark |

---

## 9.7 dnotify

`fcntl(fd, F_NOTIFY, arg)` 仍在（`fs/fcntl.c:402`）。实现是 `fcntl_dirnotify`，对象在 `fs/notify/dnotify/dnotify.c`。它只能监视目录，事件通过信号交付，不通过专门的 fd。`/proc/sys/fs/dir-notify-enable` 可以关掉它（`dnotify.c:24`）。新代码用 inotify 或 fanotify。本章确认这套调用在 6.6 还在，不展开信号编号。

---

## 9.8 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/filelock.h` | `struct file_lock_context` | 149 | 三条链表 |
| `include/linux/filelock.h` | `struct file_lock` | 107 | 一把锁或一份租约 |
| `fs/locks.c` | `flock` | 2069 | 系统调用 |
| `fs/locks.c` | `flock_lock_inode` | 1022 | flock 冲突 |
| `fs/locks.c` | `posix_lock_inode` | 1094 | 字节范围冲突与合并 |
| `fs/locks.c` | `fcntl_setlk` | 2343 | `F_SETLK` / `F_SETLKW` |
| `fs/locks.c` | `vfs_lock_file` | 2291 | 可被文件系统的 `f_op->lock` 截走 |
| `fs/locks.c` | `generic_setlease` | 1870 | 加租约 |
| `fs/locks.c` | `fcntl_setlease` | 1999 | `F_SETLEASE` |
| `fs/fcntl.c` | `fcntl` | 436 | 命令分发 |
| `fs/notify/inotify/inotify_user.c` | `inotify_init` | 713 | flags 为 0 |
| `fs/notify/inotify/inotify_user.c` | `inotify_init1` | 708 | |
| `fs/notify/inotify/inotify_user.c` | `inotify_add_watch` | 718 | |
| `fs/notify/inotify/inotify_user.c` | `inotify_rm_watch` | 782 | |
| `fs/notify/fanotify/fanotify_user.c` | `fanotify_init` | 1409 | |
| `fs/notify/fanotify/fanotify_user.c` | `fanotify_mark` | 1909 | |
| `include/trace/events/filelock.h` | `locks_get_lock_context` | 39 | 分配 `i_flctx` |
| `include/linux/fsnotify_backend.h` | `struct fsnotify_group` | 185 | inotify 和 fanotify 共用的组 |

---

**导航**：[← dcache](./08-dcache-icache.md) · [目录](./README.md) · [tmpfs 走读 →](./10-tmpfs-walkthrough.md)
