# 12. 观测

**结论**：v6.6.160 里能直接读的 VFS 计数在 `/proc/sys/fs/`。dentry、inode、`struct file` 各有自己的 slab 名。跟踪点没有名为 `vfs_*` 的一类；和 VFS 挨着的是 `filemap`、`filelock`、`writeback`，以及 `syscalls:sys_enter_*`。

> 下面带数字的行是在 **6.12.94+** 上读到的（`uname -r` 为 `6.12.94+`）。列的含义按 v6.6.160 的结构体解释。两套内核的默认值和行号不要混用。`/proc/slabinfo` 和 `/sys/kernel/tracing/events/` 这次是 `Permission denied`，不编那里的数字。

---

## 12.1 `/proc/sys/fs/dentry-state`

只读。注册在 `fs/dcache.c:186`，`mode` 是 `0444`，handler 是 `proc_nr_dentry`（`:177`）。打印前它刷新三个计数：

```text
dentry_stat.nr_dentry   = get_nr_dentry()
dentry_stat.nr_unused   = get_nr_dentry_unused()
dentry_stat.nr_negative = get_nr_dentry_negative()
```

`struct dentry_stat_t`（`fs/dcache.c:118`）的顺序就是输出的六列。`maxlen` 是 `6*sizeof(long)`（`:190`）。

| 列 | 字段 | v6.6 里谁写它 |
|---|---|---|
| 1 | `nr_dentry` | `proc_nr_dentry` 用各 CPU 的 `nr_dentry` 求和 |
| 2 | `nr_unused` | 同样，来自 `nr_dentry_unused`。`d_lru_add` 在置 `DCACHE_LRU_LIST` 时加一 |
| 3 | `age_limit` | 静态初值 45（`:134`）。handler 不改它 |
| 4 | `want_pages` | 结构体里的字段，这份初始化没给它赋值 |
| 5 | `nr_negative` | LRU 上的负 dentry，不是全部负项。见 [08](./08-dcache-icache.md) |
| 6 | `dummy` | 注释写明留给以后，handler 不填 |

sampled on 6.12.94+：

```text
131805  128515  45  0  34403  0
```

第三列是 45，和 v6.6 源码里的 `.age_limit = 45` 一致。前两列和第五列是采样当时的计数，不能当成 6.6 机器上的数字。

---

## 12.2 inode 与 file

`/proc/sys/fs/inode-nr` 和 `inode-state` 共用 `struct inodes_stat_t`（`include/uapi/linux/fs.h:103`）：

```c
struct inodes_stat_t {
	long nr_inodes;
	long nr_unused;
	long dummy[5];          /* 凑 sysctl ABI */
};
```

`proc_nr_inodes`（`fs/inode.c:109`）只刷新前两个字段。`inode-nr` 的 `maxlen` 是 `2*sizeof(long)`（`:121`），所以两列。`inode-state` 的 `maxlen` 是 `7*sizeof(long)`（`:128`），所以七列，后五列是 `dummy`。

sampled on 6.12.94+，两次读取之间计数会变：

```text
inode-nr:    97332  0
inode-state: 97331  0  0  0  0  0  0
```

第二列是 `nr_unused`，也就是还在 inode LRU 上的个数。采样当时是 0。

`/proc/sys/fs/file-nr` 打的是整个 `struct files_stat_struct`（`include/uapi/linux/fs.h:97`）：

```c
struct files_stat_struct {
	unsigned long nr_files;       /* proc_nr_files 会刷新 */
	unsigned long nr_free_files;  /* 这个 handler 不写 */
	unsigned long max_files;      /* file-max 单独指这一字段 */
};
```

`proc_nr_files`（`fs/file_table.c:125`）只执行 `files_stat.nr_files = get_nr_files()`。`file-max` 是同一结构的 `max_files`，`mode` 0644（`:141`）。v6.6 的初值由 `files_maxfiles_init`（`:516`）计算：按内存大约留 10% 给 `struct file`，下限 `NR_FILE`（8192，`include/uapi/linux/fs.h:110`）。

sampled on 6.12.94+：

```text
file-nr:   1824  0  9223372036854775807
file-max:  9223372036854775807
```

这个 `file-max` 不是 6.6 公式算出来的量级。只把它当作 6.12 采样机上的读数。`nr_open`（单进程 fd 表能扩到的上限，`fs/file_table.c:150`）采样值是 `1048576`。

---

## 12.3 其它 `/proc/sys/fs` 项

下表只列 v6.6.160 源码里注册过、并且这次在 6.12 上读到了的项。注册点是 6.6 的行号。

| 路径 | 6.6 注册点 | mode | sampled on 6.12.94+ |
|---|---|---|---|
| `fs/overflowuid` | `fs/sysctls.c:13` | 0644 | 未单独读 |
| `fs/overflowgid` | `fs/sysctls.c:21` | 0644 | 未单独读 |
| `fs/protected_symlinks` | `fs/namei.c:1044` | 0644 | `1` |
| `fs/protected_hardlinks` | `fs/namei.c:1053` | 0644 | `1` |
| `fs/protected_fifos` | `fs/namei.c:1062` | 0644 | `1` |
| `fs/protected_regular` | `fs/namei.c:1071` | 0644 | `2` |
| `fs/leases-enable` | `fs/locks.c:99` | 0644 | `1` |
| `fs/lease-break-time` | `fs/locks.c:107`，要 `CONFIG_MMU` | 0644 | `45` |
| `fs/mount-max` | `fs/namespace.c:5128` | 0644 | `100000` |
| `fs/pipe-max-size` | `fs/pipe.c:1478` | 0644 | `1048576` |
| `fs/aio-nr` | `fs/aio.c:229` | 0444 | `0` |
| `fs/aio-max-nr` | `fs/aio.c:236` | 0644。6.6 初值 `0x10000`（`:224`） | `65536` |
| `fs/inotify/max_user_instances` | `fs/notify/inotify/inotify_user.c:63` | 0644 | `128` |
| `fs/inotify/max_user_watches` | `:71` | 0644 | `127865` |
| `fs/inotify/max_queued_events` | `:80` | 0644 | `16384` |
| `fs/fanotify/max_user_groups` | `fs/notify/fanotify/fanotify_user.c:64` | 0644 | `128` |
| `fs/fanotify/max_user_marks` | `:73` | 0644 | `136168` |
| `fs/fanotify/max_queued_events` | `:82` | 0644 | `16384` |
| `fs/dir-notify-enable` | `fs/notify/dnotify/dnotify.c:26` | 0644 | 未单独读 |

`protected_*` 的取值范围：符号链接和硬链接的 `extra2` 是 1（开或关），fifo 和 regular 的 `extra2` 是 2（`namei.c:1051`、`:1069`）。采样里 `protected_regular` 是 2，落在 6.6 允许的 0、1、2 里。

inotify / fanotify 的三个上限是「每个用户能建多少实例 / 多少 watch / 队列能堆多少事件」。超了之后 `inotify_init` 或 `inotify_add_watch` 会失败，本章不展开每个错误码。

`/proc/mounts` 是指向 `/proc/self/mounts` 的符号链接（`fs/proc/root.c:291`）。采样机第一行是 `ext4` 根，第二行是 `proc`。这是 6.12 客户机的挂载，不是 6.6 的示例输出。

---

## 12.4 slab 名

`/proc/slabinfo` 这次没有权限。名字按 v6.6.160 的创建点列出，方便有权限时对：

| slab 名 | 创建点 | 对象 |
|---|---|---|
| `dentry` | `fs/dcache.c:3315`，`KMEM_CACHE_USERCOPY(dentry, ...)`。宏把结构体名变成字符串（`include/linux/slab.h:216`） | `struct dentry`。`d_iname` 那一段允许拷到用户 |
| `inode_cache` | `fs/inode.c:2355` | `struct inode` |
| `filp` | `fs/file_table.c:507` | `struct file` |
| `names_cache` | `fs/dcache.c:3353`，长度 `PATH_MAX` | 路径名字的内核拷贝，`getname` 用 |

dentry 哈希表的名字是 `"Dentry cache"`，inode 哈希表是 `"Inode-cache"`，它们是 `alloc_large_system_hash` 的名字（`dcache.c:3324`、`inode.c:2367`），出现在启动日志里，不是 slab 名。

---

## 12.5 tracepoint：没有 `vfs_*` 这一类

在 v6.6.160 的 `include/trace/events/` 里搜 `TRACE_EVENT`，没有名为 `vfs_open`、`vfs_read`、`vfs_write` 的跟踪点。名字里带 vfs 的是别的子系统，例如 `cachefiles_vfs_error`（`include/trace/events/cachefiles.h:629`），不属于这条路径。

能挂上的、和本章有关的点如下。系统名是头文件里的 `TRACE_SYSTEM`。

**`filemap`**（`include/trace/events/filemap.h`）：

| 事件 | 行号 | 何时 |
|---|---|---|
| `mm_filemap_delete_from_page_cache` | 49 | folio 离开页缓存 |
| `mm_filemap_add_to_page_cache` | 54 | folio 进入页缓存 |
| `filemap_set_wb_err` | 59 | 地址空间记下回写错误 |
| `file_check_and_advance_wb_err` | 84 | 这次打开把错误序号往前拨 |

前两个和 MM 观测章列的是同一对。页缓存内部还有 `mm/filemap.c` 里的调用点，不在这里重列。

**`filelock`**（`include/trace/events/filelock.h`）：

| 事件 | 行号 |
|---|---|
| `locks_get_lock_context` | 39 |
| `posix_lock_inode` | 104 |
| `fcntl_setlk` | 108 |
| `locks_remove_posix` | 112 |
| `flock_lock_inode` | 116 |
| `break_lease_noblock` / `break_lease_block` / `break_lease_unblock` | 157 起 |
| `generic_delete_lease` | 166 |
| `time_out_leases` | 169 |
| `generic_add_lease` | 172 |
| `leases_conflict` | 207 |

**`writeback`**（`include/trace/events/writeback.h`）里和「写完没有」有关的几个：`writeback_queue_io`（`:498`）、`balance_dirty_pages`（`:621`）、`global_dirty_state`（`:530`）、`writeback_pages_written`（`:395`）、`writeback_sb_inodes_requeue`（`:706`）。脏页阈值的含义在 [MM §08](../memory-management/08-page-cache-and-writeback.md)。

**系统调用**：

- `raw_syscalls:sys_enter` 和 `raw_syscalls:sys_exit` 在 `include/trace/events/syscalls.h:18` 和 `:44`，要 `CONFIG_HAVE_SYSCALL_TRACEPOINTS`。参数是系统调用号和寄存器，不带名字。
- `CONFIG_FTRACE_SYSCALLS` 时，每个 `SYSCALL_DEFINE` 还会生成 `syscalls` 系统下的事件。名字由 `"sys_enter"#sname` 拼出来（`include/linux/syscalls.h:147`）。`SYSCALL_DEFINE4(openat, ...)` 的 sname 是 `_openat`，事件名是 `sys_enter_openat`。同类还有 `sys_enter_read`、`sys_enter_write`、`sys_enter_close`、`sys_enter_mount`。系统名是 `"syscalls"`（`kernel/trace/trace_syscalls.c:497`）。

这次环境读不到 `/sys/kernel/tracing/events/`，所以上面只核对了源码，没有在机器上 `ls` 出事件目录。打开跟踪的命令放在 [13](./13-perf-analysis.md)，标 RECIPE。

kprobe 可以挂在真实函数上，例如 `vfs_read`（`fs/read_write.c:450`）、`vfs_write`（`:564`）、`vfs_open`（`fs/open.c:1062`）、`do_filp_open`（`fs/namei.c:3875`）、`vfs_fsync`（`fs/sync.c:200`）。这些是函数，不是 tracepoint。

---

## 12.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/dcache.c` | `proc_nr_dentry` | 177 | 刷新 dentry 三列再打印 |
| `fs/dcache.c` | `dentry_stat` 初值 | 133 | `age_limit = 45` |
| `fs/inode.c` | `proc_nr_inodes` | 109 | 刷新 inode 两列 |
| `fs/file_table.c` | `proc_nr_files` | 125 | 刷新 `nr_files` |
| `fs/file_table.c` | `files_maxfiles_init` | 516 | 6.6 的 `file-max` 初值算法 |
| `include/trace/events/filemap.h` | `TRACE_SYSTEM filemap` | 3 | 页缓存进出 |
| `include/trace/events/filelock.h` | `TRACE_SYSTEM filelock` | 8 | 锁和租约 |
| `include/trace/events/syscalls.h` | `sys_enter` | 18 | 原始系统调用入口 |
| `kernel/trace/trace_syscalls.c` | 系统名 `"syscalls"` | 497 | `sys_enter_openat` 这一类 |

---

**导航**：[← 伪文件系统](./11-pseudo-fs.md) · [目录](./README.md) · [性能配方 →](./13-perf-analysis.md)
