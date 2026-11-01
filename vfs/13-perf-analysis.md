# 13. 性能配方

**结论**：路径查找慢，先看是不是 RCU-walk 频繁掉成 REF-walk，以及 dcache 命中后还在不在 `lookup` 里。`fsync` 慢要分开「VFS 已经进了 `f_op->fsync`」和「块层还在写」。tmpfs 的 `fsync` 是 `noop_fsync`，延迟不该来自磁盘。Direct IO 和缓冲 IO 在 `generic_file_read_iter` 里按 `IOCB_DIRECT` 分叉。

> 下面的命令都是 **RECIPE**。这次环境读 `/sys/kernel/tracing` 是 `Permission denied`，没有跑 perf / bpftrace / ftrace，也没有输出可以贴。函数和事件名按 v6.6.160 核对过，见 [12](./12-observability.md)。

采样用的机器是 6.12.94+。在 6.6 上跑同样的事件名之前，先确认那份内核编进了 `CONFIG_FTRACE_SYSCALLS` 和对应的跟踪点。

---

## 13.1 路径查找热在哪

一次 open 的用户态时间大多落在 `link_path_walk` → `lookup_fast`。RCU-walk 失败会整条重走，`do_filp_open`（`fs/namei.c:3875`）里 `-ECHILD` 就再调一次不带 `LOOKUP_RCU` 的 `path_openat`。

RECIPE，perf 看 open 的内核栈：

```bash
perf record -e syscalls:sys_enter_openat -e syscalls:sys_exit_openat -a -- sleep 5
perf script | head
```

`sys_enter_openat` 来自 `SYSCALL_DEFINE4(openat)` 的元数据（`include/linux/syscalls.h:147`），系统名是 `syscalls`。没有 `CONFIG_FTRACE_SYSCALLS` 时这个事件不存在，可以改用 `raw_syscalls:sys_enter`，再按调用号过滤。调用号跟架构走，这里不写死数字。

RECIPE，看 RCU 失败后的第二次查找有没有堆在 `lookup_slow`：

```bash
perf probe -x /usr/lib/debug/boot/vmlinux --add do_filp_open
perf record -e probe:do_filp_open -a -- sleep 5
```

`do_filp_open` 不是 tracepoint。动态探针要符号。发行版内核常常得指向带调试信息的 vmlinux，路径因系统而异。

更直接的是对 `lookup_slow` 和 `try_to_unlazy` 计数。两者都是 `fs/namei.c` 里的 `static` 函数（`lookup_slow` 在 `:1717`，`try_to_unlazy` 在 `:779`），不一定出现在 kallsyms 里。看不到符号时，对非 static 的 `do_filp_open` 采样调用栈，看栈里有没有 `__lookup_slow` 的调用者 `walk_component`。

热点的常见来源，按代码而不是按某次测量：

| 现象 | 代码上的原因 |
|---|---|
| `lookup_fast` 经常返回空 | dcache 没有这截名字，`walk_component` 进 `lookup_slow`（`namei.c:2014`） |
| 大量 `-ECHILD` 重走 | `d_seq` 对不上、权限检查要睡眠、或 `d_revalidate` 要求离开 RCU |
| `i_op->lookup` 占时间 | 慢路径真的去问文件系统。tmpfs 的 `simple_lookup` 很短；磁盘目录会读块 |
| 符号链接深 | `total_link_count` 每跟一次加一，到 `MAXSYMLINKS`（40）返回 `-ELOOP` |

负 dentry 命中也走 `lookup_fast`，不会进 `lookup`。`/proc/sys/fs/dentry-state` 第五列只数 LRU 上的负项，见 [12](./12-observability.md)。它不是命中率。

---

## 13.2 dcache 命中

RECIPE，用 bpftrace 数 `lookup_fast` 返回空的次数做不到，如果这个 static 函数没有符号。可以数非 static 的 `__d_lookup`（`fs/dcache.c:2465`）被调用的次数，它只在 REF-walk 里用。RCU-walk 用的是 static 的 `__d_lookup_rcu`（`:2356`）。

RECIPE：

```bash
bpftrace -e 'kprobe:__d_lookup { @[comm] = count(); }'
```

这只说明 REF-walk 的哈希查找有多少，不包含仍在 RCU 里就命中的那部分。把这个数当成「全部 dcache 查找」会偏小。

页缓存命中是另一件事。文件已经打开之后的 `read` 看的是 `address_space`，跟踪点是 `filemap:mm_filemap_add_to_page_cache`（`include/trace/events/filemap.h:54`）。RECIPE：

```bash
perf record -e filemap:mm_filemap_add_to_page_cache -a -- sleep 5
```

这个事件是「folio 被放进页缓存」，不是「read 在缓存里找到了」。命中不会发它。未命中才会分配 folio 并打这个点。缺页侧的 `pgmajfault` 在 [MM §07](../memory-management/07-page-fault.md)。

---

## 13.3 `fsync` 延迟

RECIPE，把一次 fsync 从进系统调用量到出系统调用：

```bash
perf record -e syscalls:sys_enter_fsync -e syscalls:sys_exit_fsync -a -- sleep 10
```

内核里的链是 `fsync` → `do_fsync` → `vfs_fsync` → `f_op->fsync`（`fs/sync.c:218`、`:206`、`:200`、`:180`）。时间如果耗在 `vfs_fsync` 返回之前，就在具体文件系统的 `fsync` 里，常常是在等回写。回写跟踪点 `writeback:balance_dirty_pages`、`writeback:writeback_pages_written` 见 [12](./12-observability.md)。块设备队列不在本章。

tmpfs 的 `f_op->fsync` 是 `noop_fsync`（`fs/libfs.c:1561`），函数体只有 `return 0`。对 tmpfs 上的 fd 做 fsync，延迟不该出现在写盘上。若观测到 tmpfs 的 fsync 很慢，先确认这个 fd 的挂载是不是 tmpfs（`/proc/self/mountinfo`），而不是路径看起来像 `/tmp`、实际是别的文件系统。

RECIPE，ftrace 只看 `vfs_fsync` 的耗时：

```bash
cd /sys/kernel/tracing
echo function_graph > current_tracer
echo vfs_fsync > set_ftrace_filter
echo 1 > tracing_on
# 让目标进程做一次 fsync
echo 0 > tracing_on
```

`function_graph` 要 `CONFIG_FUNCTION_GRAPH_TRACER`。没有这个配置时命令会失败，不要把失败输出当成时延。

---

## 13.4 Direct IO 和缓冲 IO

分叉在 `generic_file_read_iter`（`mm/filemap.c:2836`）：`ki_flags & IOCB_DIRECT` 为真则 `a_ops->direct_IO`，否则 `filemap_read`。写的分叉在 `__generic_file_write_iter`（`:4107`）。

能用这个分叉的文件系统，`read_iter` 得是 `generic_file_read_iter` 或自己再调它。tmpfs 的 `read_iter` 是 `shmem_file_read_iter`，不看 `IOCB_DIRECT`，见 [10](./10-tmpfs-walkthrough.md)。拿 tmpfs 对比 DIO 和缓冲 IO 得不到磁盘那条差异。

RECIPE，对支持 `generic_file_read_iter` 的文件分别跑 `O_DIRECT` 和普通读，看 `filemap:mm_filemap_add_to_page_cache` 还增不增：

```bash
perf stat -e filemap:mm_filemap_add_to_page_cache -- dd if=/path/to/file of=/dev/null bs=1M count=64 iflag=direct
perf stat -e filemap:mm_filemap_add_to_page_cache -- dd if=/path/to/file of=/dev/null bs=1M count=64
```

缓冲读在缓存是空的时候会把 folio 放进页缓存，这个事件会涨。Direct IO 的目的就是不走那条拷贝；短读回退到 `filemap_read` 时（`filemap.c:2863` 附近的条件）仍可能涨。把两次 `perf stat` 的差值解释成「DIO 完全不碰页缓存」之前，先看读的长度是不是以 EOF 或文件系统的短读结束。

写方向上，缓冲写还会进 `balance_dirty_pages`。那是内存章的限速，不是 VFS 多出来的一层锁。事件名 `writeback:balance_dirty_pages`（`include/trace/events/writeback.h:621`）。

---

## 13.5 不该用来下结论的数字

| 读到的东西 | 它不是 |
|---|---|
| `dentry-state` 第一列很大 | 命中率。它是 dentry 对象个数 |
| `dentry-state` 第五列 | 全部负项。它只数 LRU 上的负项 |
| `file-nr` 第一列 | 全机 fd 个数。它是 `struct file` 个数，一个 file 可以对应多个 fd（`dup`） |
| `inode-state` 第二列为 0 | inode 泄漏。它是「正在 LRU 上、引用已经可以回收」的个数。忙的 inode 不在这列 |
| tmpfs 上 `fsync` 的耗时 | 磁盘延迟 |

`file-max` 在 6.12 采样机上是一个很大的数，和 v6.6 `files_maxfiles_init` 的 10% 内存算法不是同一个读数，见 [12](./12-observability.md)。不要用那个采样值去判断 6.6 内核是不是把上限设满了。

---

## 13.6 关键符号

| 文件 | 符号 | 行号 | 和配方的关系 |
|---|---|---|---|
| `fs/namei.c` | `do_filp_open` | 3875 | RCU 失败再 REF-walk |
| `fs/namei.c` | `lookup_slow` | 1717 | static，不一定能 kprobe |
| `fs/namei.c` | `try_to_unlazy` | 779 | static |
| `fs/dcache.c` | `__d_lookup` | 2465 | REF-walk 的哈希查找，非 static |
| `mm/filemap.c` | `generic_file_read_iter` | 2828 | `IOCB_DIRECT` 分叉 |
| `mm/filemap.c` | `filemap_read` | 2655 | 缓冲读 |
| `fs/sync.c` | `vfs_fsync` | 200 | fsync 的 VFS 入口 |
| `fs/libfs.c` | `noop_fsync` | 1561 | tmpfs |
| `include/trace/events/filemap.h` | `mm_filemap_add_to_page_cache` | 54 | folio 进入页缓存 |
| `include/linux/syscalls.h` | `sys_enter` 事件名拼接 | 147 | `syscalls:sys_enter_openat` |

---

**导航**：[← 观测](./12-observability.md) · [目录](./README.md)
