# VFS 大章

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照 `/tmp/linux` 的该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.94+**（见 [12](./12-observability.md)）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：系统调用先落到 VFS 的 `dentry` / `inode` / `file`，再由具体文件系统的函数表决定读页缓存还是自己处理；页缓存和回写的内存侧已经写在内存章，这里只接到那些入口。

面向 **C 语言新手**：`struct` 里写 `struct inode *d_inode` 是指针，对象在别处；写 `struct address_space i_data` 是把那块结构嵌在 `inode` 里面。函数表（`file_operations` 这类）是一组函数指针，文件系统注册时填上自己的函数，VFS 只按名字去调。

## 阅读顺序

```text
01 总览
  → 02 四个核心对象和四张 ops 表
  → 03 路径查找（RCU-walk / REF-walk）
  → 04 文件描述符与 open/close
  → 05 read/write 与页缓存交接
  → 06 mmap 与 filemap_fault
  → 07 挂载、命名空间、传播
  → 08 dcache / icache 与 shrinker
  → 09 锁与 inotify / fanotify
  → 10 用 tmpfs 把 open→read→write→fsync 走一遍
  → 11 procfs / sysfs / debugfs
  → 12 观测 → 13 性能配方
```

先认清「名字」和「对象」是两回事：`dentry` 是路径上的名字，`inode` 是文件本身。硬链接是多个 `dentry` 指同一个 `inode`。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 系统调用到 VFS、具体 FS、页缓存、块层的总图 | 中 |
| [02-core-objects.md](./02-core-objects.md) | `super_block` / `inode` / `dentry` / `file` 与四张 ops | **厚** |
| [03-path-lookup.md](./03-path-lookup.md) | `path_openat`、`link_path_walk`、RCU-walk、dcache、挂载点、符号链接 | **厚** |
| [04-fd-and-open.md](./04-fd-and-open.md) | `files_struct`、`fdtable`、`struct file` 引用计数、open/close | **厚** |
| [05-read-write.md](./05-read-write.md) | `read_iter` / `write_iter`、`kiocb`、`iov_iter`、缓冲 IO、Direct IO | **厚** |
| [06-mmap-and-file.md](./06-mmap-and-file.md) | `f_op->mmap`、`vm_operations_struct`、`filemap_fault` | **厚** |
| [07-mount-and-ns.md](./07-mount-and-ns.md) | `vfsmount` / `mount`、新挂载 API、shared/slave/private/unbindable | **厚** |
| [08-dcache-icache.md](./08-dcache-icache.md) | dentry/inode LRU、shrinker、负 dentry | **厚** |
| [09-locks-and-notify.md](./09-locks-and-notify.md) | flock、POSIX 锁、lease、inotify、fanotify | **厚** |
| [10-tmpfs-walkthrough.md](./10-tmpfs-walkthrough.md) | tmpfs 上 open→read→write→fsync 的一条具体路径 | **厚** |
| [11-pseudo-fs.md](./11-pseudo-fs.md) | procfs、sysfs/kernfs、debugfs，`/proc/<pid>` 怎么生成 | **厚** |
| [12-observability.md](./12-observability.md) | `/proc/sys/fs/*`、slab 名、6.6 里实际存在的 tracepoint | **厚** |
| [13-perf-analysis.md](./13-perf-analysis.md) | 路径查找、dcache、fsync、DIO 与缓冲 IO 的配方 | **厚** |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `inode->i_mapping` 指向的 `address_space`，以及 `read_iter` 何时把活交给页缓存 | 页缓存 folio、脏页限速、回写线程：[memory-management/08-page-cache-and-writeback.md](../memory-management/08-page-cache-and-writeback.md) |
| `filemap_fault` 从 VFS 缺页入口走到 `read_folio` | 缺页分发、`do_fault`、userfaultfd：[memory-management/07-page-fault.md](../memory-management/07-page-fault.md) |
| 每个超级块的 `s_shrink` 怎么被 `shrink_slab` 调到 | 水位、kswapd、`shrink_slab` 本身：[memory-management/10-01-overview.md](../memory-management/10-01-overview.md) |
| shmem/tmpfs 的 VFS 函数表和 open/read/write/fsync | shmem 换出、`shmem_inode_info`、缺页分配：[memory-management/08-page-cache-and-writeback.md](../memory-management/08-page-cache-and-writeback.md) §8.4 |
| `task_struct->files` 这张表怎么用 | `task_struct` 本身：[process-management/02-task-struct.md](../process-management/02-task-struct.md) |
| 路径查找里的 RCU 读锁 | RCU 原语：[locks-and-ds/07-rcu.md](../locks-and-ds/07-rcu.md) |
| `io_uring` 在 `file_operations` 上有 `uring_cmd` 指针 | 提交、完成、固定缓冲留给以后的 block-io 章，本章不展开 |
| FUSE 请求队列、overlayfs 的 copy-up 与 whiteout | [containerization/](../containerization/README.md)。本章只在 `dentry_operations.d_real` 上点到这个钩子存在 |

块层的 `bio` / `request` 不在本章推导。磁盘文件系统的 `a_ops->read_folio` 最终会提交 bio；tmpfs 没有这一步。

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/fs.h` | `inode`、`file`、`super_block`、三张 ops、`kiocb` |
| `include/linux/dcache.h` | `dentry`、`dentry_operations`、`DCACHE_*` |
| `include/linux/fdtable.h` | `files_struct`、`fdtable` |
| `include/linux/mount.h`、`fs/mount.h` | `vfsmount`、`struct mount`、`mnt_namespace` |
| `include/linux/fs_context.h` | 新挂载 API 的 `fs_context` |
| `fs/namei.c` | 路径查找与 `do_filp_open` |
| `fs/open.c`、`fs/file.c`、`fs/file_table.c` | open、fd 表、`fput` |
| `fs/read_write.c`、`mm/filemap.c` | `vfs_read` / `vfs_write`、页缓存读写、`filemap_fault` |
| `fs/namespace.c`、`fs/fsopen.c`、`fs/pnode.c` | mount、fsopen/fsmount、传播 |
| `fs/dcache.c`、`fs/inode.c`、`fs/super.c` | dcache、icache、每超级块 shrinker |
| `fs/locks.c`、`fs/notify/` | 文件锁、inotify、fanotify |
| `mm/shmem.c` | tmpfs |
| `fs/proc/`、`fs/kernfs/`、`fs/sysfs/`、`fs/debugfs/` | 伪文件系统 |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.94+）：[12](./12-observability.md) 里 `/proc/sys/fs/` 下能读到的计数。`/proc/slabinfo` 与 `/sys/kernel/tracing/events/` 当时 `Permission denied`，不编数字。
- **RECIPE**：[13](./13-perf-analysis.md) 的 perf、bpftrace、ftrace。本环境没有跟踪权限，步骤只作配方。

## 未覆盖

- ext4 / xfs / btrfs 的目录项、日志、分配组
- FUSE 请求队列、overlayfs 的 whiteout 与 copy-up（见 [containerization/](../containerization/README.md)）
- `io_uring` 的提交环与固定文件表
- NFS/9p 的网络查找
- `fanotify` 权限事件在 LSM 里的完整裁决

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
