# 9. 观测

**结论**：epoll 的限额在 `/proc/sys/fs/epoll/max_user_watches`，每个 epfd 的监视项在 `/proc/<pid>/fdinfo/<epfd>`；管道容量看 `F_GETPIPE_SZ` 和 `fs/pipe-*` 三个 sysctl。`pipefifo_fops` 没有 `show_fdinfo`，管道 fdinfo 只有 proc 的通用行。

> 打印函数：`ep_show_fdinfo`（`fs/eventpoll.c:954`）。`seq_show`（`fs/proc/fd.c:23`）先打通用的 pos/flags/mnt_id/ino，再在 `:69` 调用 `f_op->show_fdinfo`。
> 本机 `uname -r` 为 **6.12.113+**。下面的数字都是这次采样，结构以 v6.6.160 为准。

---

## 9.1 epoll 限额

```text
$ cat /proc/sys/fs/epoll/max_user_watches
3647020
```

sampled on 6.12.113+。v6.6.160 的初值公式在 `eventpoll_init`（`fs/eventpoll.c:2452`）：低端内存页数的百分之四换成字节，再除以 `sizeof(epitem) + sizeof(eppoll_entry)`。sysctl 表在 `:319`，名字 `max_user_watches`，模式 `0644`。把这个数改成 0 之后，新的 `EPOLL_CTL_ADD` 会在 `ep_insert` 里得到 `-ENOSPC`。已有的监视项不会被拆掉。

计数记在创建 epfd 的 `user_struct->epoll_watches`（`include/linux/sched/user.h:17`），按用户累计，不按 epfd 累计。

---

## 9.2 epfd 的 fdinfo

`ep_show_fdinfo` 持 `ep->mtx`，按红黑树顺序每项打一行：

```text
tfd: %8d events: %8x data: %16llx  pos:%lli ino:%lx sdev:%x
```

`tfd` 是注册时的描述符号，`events` 是内核保存的兴趣掩码（含 `ADD` 时或上去的 `EPOLLERR | EPOLLHUP`），`data` 是用户的 `epoll_event.data`，`pos` / `ino` / `sdev` 来自目标 `struct file`。

[demos/02-pipe-wakeup.c](./demos/02-pipe-wakeup.c) 在同一进程里把管道读端以 `EPOLLIN | EPOLLET` 加入 epoll 之后读了两份 fdinfo。sampled on 6.12.113+：

```text
--- epfd fdinfo ---
pos:	0
flags:	02000002
mnt_id:	16
ino:	1042
tfd:        3 events: 80000019 data:                3  pos:0 ino:2669 sdev:e
--- pipe_r fdinfo ---
pos:	0
flags:	02000000
mnt_id:	15
ino:	9833
F_GETPIPE_SZ=65536
```

`events` 的 `80000019` 是这次注册的掩码：

| 位 | 宏 | 值 |
|----|----|----|
| 0 | `EPOLLIN` | `0x1` |
| 3 | `EPOLLERR` | `0x8` |
| 4 | `EPOLLHUP` | `0x10` |
| 31 | `EPOLLET` | `0x80000000` |

用户只写了 `EPOLLIN | EPOLLET`。`EPOLLERR | EPOLLHUP` 是 `do_epoll_ctl` 在 `ADD` 时或上去的（`eventpoll.c:2223`）。`data` 是程序放进 `epoll_event.data.fd` 的读端号，这次是 3。`tfd` 同一号。

头上的 `flags: 02000002` 是 epfd 自己的打开旗标，对应 `do_epoll_create` 使用的 `O_RDWR | O_CLOEXEC`。`ino: 1042` 是这次采样里匿名 inode 的号。`anon_inode_getfile` 创建的文件共用一个 inode（`fs/anon_inodes.c:152`），所以不同 epfd 的头上 `ino` 可以相同；目标文件的 inode 在 `tfd` 那一行，这次管道是 `2669`。管道号和挂载号每次创建都会变，上表只解释这一次输出里稳定的掩码。

管道读端的 fdinfo 只有 `pos` / `flags` / `mnt_id` / `ino`。`flags: 02000000` 是 `O_CLOEXEC`（`O_RDONLY` 的低位是 0）。`pipefifo_fops`（`fs/pipe.c:1227`）没有 `show_fdinfo`，容量不在这里。`F_GETPIPE_SZ` 返回 65536，即默认 16 个槽乘本机页大小 4096。

eventfd 的 fdinfo 在 `eventfd_show_fdinfo`（`fs/eventfd.c:292`）：`eventfd-count`、`eventfd-id`、`eventfd-semaphore`。signalfd 打 `sigmask`（`signalfd.c:232`）。timerfd 打 `clockid`、`ticks`、`settime flags`、`it_value`、`it_interval`（`timerfd.c:321`）。

---

## 9.3 pipe 的 sysctl

```text
$ cat /proc/sys/fs/pipe-max-size
1048576
$ cat /proc/sys/fs/pipe-user-pages-soft
16384
$ cat /proc/sys/fs/pipe-user-pages-hard
0
```

sampled on 6.12.113+。v6.6.160 里 `pipe_max_size` 的初值就是 1048576（`fs/pipe.c:54`），硬限制初值 0（不设，`:59`），软限制初值是 `PIPE_DEF_BUFFERS * INR_OPEN_CUR`（`:60`）。三个 ctl 表项在 `:1476`。非特权用户把单根管道抬到 `pipe-max-size` 以上会被 `pipe_set_size` 拒绝；记账超过硬限制时 `alloc_pipe_info` 直接失败，超过软限制时新管道缩到 2 个槽。

---

## 9.4 跟踪点

本环境没有 `perf`、`bpftrace`、`strace`，`/sys/kernel/tracing/events/syscalls/` 读不到。下面是配方，没有采样数字。

```text
# RECIPE
perf trace -e epoll_wait,epoll_ctl,poll,select,read,write -p <pid>
bpftrace -e 'kprobe:ep_poll_callback { @[kstack] = count(); }'
```

`ep_poll_callback` 在每次目标唤醒时进入，高连接数下计数本身会成为开销。先用系统调用跟踪点看 `epoll_wait` 的返回次数，再决定要不要碰这个 kprobe。`epoll_wait` 返回 0 只说明超时，返回正数才是交付了事件；ET 漏读时返回次数会比 LT 少，和 [05](./05-epoll-wait-and-modes.md) 的用户态计数是同一现象。
