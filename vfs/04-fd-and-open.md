# 4. 文件描述符与 open / close

**结论**：fd 是 `files_struct` 里的下标，槽里放的是 `struct file *`。`open` 先占一个空槽，路径查找成功后再把指针填进去；`close` 把槽清空并 `fput`，引用到 0 才跑 `release`、`dput` 和 `mntput`。

> 表在 `include/linux/fdtable.h`。占槽在 `fs/file.c` 的 `alloc_fd`（`:487`），安装在 `fd_install`（`:590`），最后释放在 `fs/file_table.c` 的 `__fput`（`:392`）。

C 新手区分三层引用：

- `fdtable->fd[n]` 这个槽持有 `struct file` 的一次引用。`close` 清槽时减掉。
- `dup`、`fork` 共享文件表、把 `struct file` 传进内核，都会再 `get_file` 一次。
- `struct file` 自己还持有 `dentry` 和 `vfsmount` 的引用。这些要等 `__fput` 才放。

`fork` 时 `CLONE_FILES` 让两个任务共用同一个 `files_struct`：`copy_files` 只把 `files_struct.count` 加一（`kernel/fork.c:1797`）。不共享则把表复制一份，每个打开的 `struct file` 引用加一。`CLONE_*` 的进程语义见 [process-management/04](../process-management/04-fork-clone.md)。

---

## 4.1 `fdtable` 与 `files_struct`

```c
/* include/linux/fdtable.h:26 */
struct fdtable {
	unsigned int max_fds;            /* fd 数组当前长度 */
	struct file __rcu **fd;          /* 下标就是 fd。RCU 保护指针替换 */
	unsigned long *close_on_exec;    /* 每位一个 fd，exec 时关掉 */
	unsigned long *open_fds;         /* 这一位为 1 表示槽已占用 */
	unsigned long *full_fds_bits;    /* 加速「这一长字全满了」 */
	struct rcu_head rcu;
};
```

```c
/* include/linux/fdtable.h:48。NR_OPEN_DEFAULT 等于 BITS_PER_LONG（:24） */
struct files_struct {
	/* --- 读多写少的前半 --- */
	atomic_t count;                  /* 几个任务共享这张表 */
	bool resize_in_progress;         /* 正在把 fd 数组换大 */
	wait_queue_head_t resize_wait;

	struct fdtable __rcu *fdt;       /* 当前表。扩容后指向新分配的 fdtable */
	struct fdtable fdtab;            /* 嵌在里面的初版表，小进程不用另分配 */

	/* --- 会写的后半，SMP 上单独一条缓存行 --- */
	spinlock_t file_lock ____cacheline_aligned_in_smp;
	unsigned int next_fd;            /* 下次分配从这里开始找洞 */
	unsigned long close_on_exec_init[1];
	unsigned long open_fds_init[1];
	unsigned long full_fds_bits_init[1];
	struct file __rcu *fd_array[NR_OPEN_DEFAULT]; /* 初版 fd 槽，就地放着 */
};
```

刚创建时 `fdt` 指向里面的 `fdtab`，`fdtab.fd` 指向 `fd_array`。fd 用超 `NR_OPEN_DEFAULT` 之后，`expand_files`（`fs/file.c:221`）另分配一张更大的 `fdtable`，用 RCU 把 `files->fdt` 换过去。旧表等宽限期再释放。

读槽用 `files_lookup_fd_raw`（`fdtable.h:82`）：fd 小于 `max_fds` 时 `array_index_nospec` 后再 `rcu_dereference_raw`。拿着 `file_lock` 的版本是 `files_lookup_fd_locked`（`:93`），RCU 读锁下的版本是 `files_lookup_fd_rcu`（`:100`）。

两个上限不要混：

- `RLIMIT_NOFILE`：这个进程还能用的最大 fd 加一。`get_unused_fd_flags` 把它传给 `alloc_fd` 的 `end`（`fs/file.c:550`）。
- `/proc/sys/fs/nr_open`：单张表允许扩到多大。对应 `sysctl_nr_open`（`fs/file_table.c:150`）。
- `/proc/sys/fs/file-max`：全机 `struct file` 个数上限，`files_stat.max_files`（`:142`）。

---

## 4.2 `open` 怎么把 fd 交出去

```text
openat                              fs/open.c:1448
  force_o_largefile 时或上 O_LARGEFILE
  do_sys_open                       :1434
    build_open_how(flags, mode)
    do_sys_openat2                  :1406

do_sys_openat2
  build_open_flags                  把 O_* / RESOLVE_* 收成 struct open_flags
  getname                           把用户路径拷进内核
  fd = get_unused_fd_flags          fs/file.c:550
    alloc_fd(0, rlimit(RLIMIT_NOFILE), flags)    :487
  do_filp_open                      fs/namei.c:3875    见 03
    失败：put_unused_fd             把刚占的位清掉
    成功：fd_install(fd, file)      fs/file.c:590
  putname
```

`alloc_fd`（`:487`）拿着 `files->file_lock`：

```text
从 max(start, files->next_fd) 起 find_next_fd
fd >= end：-EMFILE                 碰到 RLIMIT_NOFILE
expand_files 失败：把错误返回
expand 中途睡过：回到 repeat       表可能已经变了
next_fd = fd + 1
__set_open_fd
O_CLOEXEC：__set_close_on_exec，否则清掉这一位
```

这一步只把「占用位」设上，`fd[fd]` 仍是 `NULL`。源码里有一段 `BUG` 风格的检查：槽里不该已经有指针（`:532`）。

`fd_install`（`:590`）把指针填进去，并消耗掉调用者持有的那次 `struct file` 引用：

```text
rcu_read_lock_sched
正在扩容：
  改拿 file_lock，rcu_assign_pointer(fdt->fd[fd], file)
否则：
  smp_rmb 配对 expand_fdtable 的 smp_wmb
  rcu_assign_pointer(fdt->fd[fd], file)
```

扩容和安装之间用这段内存屏障，避免读到新表指针却看见旧的空槽内容。

`openat2`（`fs/open.c:1456`）不走 `do_sys_open`，自己从用户拷 `struct open_how`，再进同一个 `do_sys_openat2`。`usize` 小于 `OPEN_HOW_SIZE_VER0` 返回 `-EINVAL`，大于 `PAGE_SIZE` 返回 `-E2BIG`（`:1465`）。

---

## 4.3 `struct file` 的引用

`do_dentry_open`（`fs/open.c:873`）在 `vfs_open` 已经把 `f_path` 设好之后：

```text
path_get(&f->f_path)               再拿一份 dentry 和 vfsmount，和 path 上那份分开算
f_inode = inode
f_mapping = inode->i_mapping
f_wb_err / f_sb_err 采样当前错误序号
O_PATH：
  f_mode = FMODE_PATH | FMODE_OPENED
  f_op = 空的 file_operations
  返回
只读打开：i_readcount_inc
写打开且不是特殊文件：
  get_write_access(inode)          i_writecount
  __mnt_want_write
  f_mode |= FMODE_WRITER
普通文件或目录：f_mode |= FMODE_ATOMIC_POS
f_op = fops_get(inode->i_fop)      模块引用也加一
security_file_open
break_lease                        写打开会打断 lease，见 09
f_op->open(inode, file)            文件系统自己的打开
f_mode |= FMODE_OPENED
按有没有 read/write/llseek 设 FMODE_CAN_*
a_ops->direct_IO 存在则 FMODE_CAN_ODIRECT
清掉 O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC
f_iocb_flags = iocb_flags(f)
file_ra_state_init
O_DIRECT 但没有 FMODE_CAN_ODIRECT：-EINVAL
fsnotify_open
```

`fops_get`（`include/linux/fs.h:2430`）在 `f_op->owner` 是模块时 `try_module_get`。失败就当没有这张表。

`alloc_empty_file` 在 `path_openat` 开头就把 `f_count` 设成 1。之后 `fd_install` 把这仅有的一次引用交给 fd 表。查找失败时 `path_openat` 对这份 file 做 `fput`（`namei.c:3865`）。

`get_file`（`fs.h:1042`）是 `atomic_long_inc(&f->f_count)`。`mmap` 成功路径会再拿一次，因为 VMA 也引用这份 file（`mm/mmap.c:2797` 的 `get_file`）。

---

## 4.4 `close` 与 `__fput`

`close` 系统调用（`fs/open.c:1559`）不走 `filp_close` 那条给内核用的封装，它自己做：

```text
close                               fs/open.c:1559
  file = close_fd_get_file(fd)      清槽，把 struct file 交回来。空槽返回 -EBADF
  filp_flush(file, current->files)
  __fput_sync(file)                 引用到 0 就当场 __fput，不推迟
```

`filp_flush`（`fs/open.c:1523`）会 `dnotify_flush` 和 `locks_remove_posix`（`:1537`）。`O_PATH` 跳过这两步。

内核里更常见的是 `filp_close`（`:1543`）：`filp_flush` 然后 `fput`。`fput`（`fs/file_table.c:467`）把 `f_count` 减到 0 之后：

```text
当前不是中断、也不是内核线程：
  task_work_add(____fput, TWA_RESUME)    回到用户态之前跑
  成功就返回
否则：
  挂上 delayed_fput_list
  schedule_delayed_work(delayed_fput_work, 1)
```

`____fput`（`:444`）只是从 `callback_head` 里取出 `struct file` 再调 `__fput`。`close` 系统调用用 `__fput_sync`（`:496`），因为马上要回用户态，注释写明不必再推迟。

`__fput`（`:392`）在 `FMODE_OPENED` 置位时按这个顺序收尾：

```text
fsnotify_close
eventpoll_release                  注释要求它是清理链的第一个
locks_remove_file                  摘掉还挂在这个 file 上的锁
ima_file_free
FASYNC：f_op->fasync(-1, file, 0)
f_op->release(inode, file)         文件系统的释放
字符设备：cdev_put
fops_put                           放下模块引用
put_pid(f_owner.pid)
put_file_access                    还写计数 / 读计数
dput(dentry)
FMODE_NEED_UNMOUNT：dissolve_on_fput
mntput(mnt)
file_free                          对象回到 filp 的 slab
```

没有 `FMODE_OPENED` 的 file（打开失败）直接 `file_free`（`:399`）。

`dput` 可能把 dentry 放进 LRU，也可能连 inode 一起丢。那一步在 [08](./08-dcache-icache.md)。

---

## 4.5 读 fd 时的 `fdget`

`read` / `write` 不把 fd 表锁拿全程。`ksys_read`（`fs/read_write.c:602`）用 `fdget_pos`：拿到 `struct file` 的一次引用，需要时再锁 `f_pos_lock`。读完把新的偏移写回 `f_pos`，再 `fdput_pos`。

`FMODE_ATOMIC_POS` 的文件在多个任务共享同一个 `struct file` 时，偏移更新要串行。`do_dentry_open` 对普通文件和目录置这个位（`fs/open.c:908`），注释指向 POSIX.1-2008。

管道等带 `FMODE_STREAM` 的文件没有共享偏移。`file_ppos`（`fs/read_write.c:597`）对它们返回 `NULL`，`vfs_read` 就不会去改 `f_pos`。

---

## 4.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/fdtable.h` | `struct fdtable` | 26 | fd 数组和两张位图 |
| `include/linux/fdtable.h` | `struct files_struct` | 48 | 进程的打开文件表 |
| `include/linux/fdtable.h` | `files_lookup_fd_raw` | 82 | 按下标取 `struct file *` |
| `fs/file.c` | `expand_files` | 221 | fd 不够时换一张更大的表 |
| `fs/file.c` | `alloc_fd` | 487 | 占一个空位，先不填指针 |
| `fs/file.c` | `get_unused_fd_flags` | 550 | 上限是 `RLIMIT_NOFILE` |
| `fs/file.c` | `put_unused_fd` | 564 | open 失败时把位还回去 |
| `fs/file.c` | `fd_install` | 590 | 把 `struct file` 填进槽 |
| `fs/file.c` | `close_fd` | 642 | 清槽再 `filp_close` |
| `fs/open.c` | `do_sys_openat2` | 1406 | 占槽、查找、安装 |
| `fs/open.c` | `openat` | 1448 | 系统调用 |
| `fs/open.c` | `openat2` | 1456 | 带 `open_how` 的系统调用 |
| `fs/open.c` | `do_dentry_open` | 873 | 装 `f_op`、调用 `open` |
| `fs/open.c` | `vfs_open` | 1062 | 设置 `f_path` 后打开 |
| `fs/open.c` | `filp_close` | 1543 | flush 然后 `fput` |
| `fs/open.c` | `close` | 1559 | 清槽后 `__fput_sync` |
| `fs/file_table.c` | `__fput` | 392 | 引用到 0 的真正析构 |
| `fs/file_table.c` | `fput` | 467 | 减引用，最后一次推迟到 task_work |
| `fs/file_table.c` | `__fput_sync` | 496 | `close` 系统调用用的同步版本 |
| `include/linux/fs.h` | `get_file` | 1042 | `f_count` 加一 |

---

**导航**：[← 路径查找](./03-path-lookup.md) · [目录](./README.md) · [读写 →](./05-read-write.md)
