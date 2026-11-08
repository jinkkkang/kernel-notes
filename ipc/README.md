# 本地 IPC 与多路复用

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.113+**（`uname -r`，4 核）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：本地 IPC 和多路复用共用「文件 → `f_op->poll` → wait queue → 唤醒」；epoll 把监视状态留在内核，高并发靠 `epitem` 红黑树加就绪链表，`epoll_wait` 只扫已经就绪的项。

面向 **C 语言新手**：`epoll` 自己也是一个文件描述符。`epoll_ctl` 往这个 fd 里登记别的 fd，`epoll_wait` 等的是「这个 epoll fd 上有就绪事件」，不是去轮询那一排目标 fd。

## 阅读顺序

```text
01 总览与选型
  → 02 select / poll 与 poll_table
  → 03 eventpoll / epitem / eppoll_entry
  → 04 epoll_ctl 与 ep_poll_callback
  → 05 epoll_wait 与 LT / ET / ONESHOT / EXCLUSIVE
  → 06 pipe 的创建和读写唤醒
  → 07 eventfd / signalfd / timerfd
  → 08 AF_UNIX 就绪与 SysV / POSIX mqueue（薄）
  → 09 观测 → 10 性能与坑
```

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 等许多 fd、选型表、就绪总图 | 中偏薄 |
| [02-poll-select.md](./02-poll-select.md) | `poll_table`、`do_select`、`do_sys_poll` | 中 |
| [03-epoll-objects.md](./03-epoll-objects.md) | `eventpoll` / `epitem` / `eppoll_entry` / `epoll_filefd` | **厚** |
| [04-epoll-ctl-and-callback.md](./04-epoll-ctl-and-callback.md) | `ep_insert`、`ep_poll_callback`、嵌套深度 | **厚** |
| [05-epoll-wait-and-modes.md](./05-epoll-wait-and-modes.md) | `ep_poll`、`ep_send_events`、四种行为位 | **厚** |
| [06-pipe.md](./06-pipe.md) | `pipe2`、`pipe_read` / `pipe_write` 的 sync poll 唤醒 | 中 |
| [07-eventfd-signalfd-timerfd.md](./07-eventfd-signalfd-timerfd.md) | 计数、信号、定时器三个 fd | 中 |
| [08-unix-and-sysv.md](./08-unix-and-sysv.md) | `unix_poll` 就绪；SysV 三板斧；POSIX mqueue | 中偏薄 |
| [09-observability.md](./09-observability.md) | `max_user_watches`、fdinfo、pipe sysctl | 中 |
| [10-perf-analysis.md](./10-perf-analysis.md) | 少量 fd、漏读、惊群、小写多醒 | 中 |
| [demos/](./demos/) | 三个用户态程序 | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `poll_wait` 把自定义 `func` 挂进 `wait_queue_head` | 等待队列词汇、`default_wake_function`、`WF_SYNC`：[locks-and-ds/08-wait-queue-and-completion.md](../locks-and-ds/08-wait-queue-and-completion.md)。`ep_poll_callback` 是那一节所说的自定义唤醒 |
| 用户态锁的 `FUTEX_WAIT` / `FUTEX_WAKE` | [locks-and-ds/09-futex.md](../locks-and-ds/09-futex.md)。本章选型表只留一行 |
| `pipe_buffer` / `pipe_inode_info` 全字段，splice 怎样把页放进槽 | [vfs/05b-splice-and-zerocopy.md](../vfs/05b-splice-and-zerocopy.md) |
| inotify / fanotify / dnotify | [vfs/09-locks-and-notify.md](../vfs/09-locks-and-notify.md) |
| 信号挂在 `pending` / `shared_pending`，`dequeue_signal` 怎么取 | [process-management/09-signals.md](../process-management/09-signals.md)。signalfd 只写 fd 侧 |
| `EPOLL_URING_WAKE` 这一位怎么被 io_uring 的 poll 武装认出来 | [block-io/12-io-uring.md](../block-io/12-io-uring.md)。本章只点名 |
| `AF_UNIX` 的 `unix_poll` 报可读可写 | `sk_buff`、发送队列、`unix_stream_sendmsg` 的字节账留给以后的 networking 章。NAPI busy poll 只点到 `eventpoll.napi_id` |
| SysV 对象放在 `ipc_namespace` | 命名空间本身：[containerization/04-pid-user-uts-ipc-time-ns.md](../containerization/04-pid-user-uts-ipc-time-ns.md) |
| `epoll_pwait` 在等待期间换信号掩码 | `-ERESTART*` 与 `restart_block`：[syscalls/08-restart.md](../syscalls/08-restart.md) |

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/poll.h`、`fs/select.c` | `poll_table`、select / poll |
| `fs/eventpoll.c`、`include/uapi/linux/eventpoll.h`、`include/linux/eventpoll.h` | epoll 对象与三个系统调用 |
| `include/linux/pipe_fs_i.h`、`fs/pipe.c` | pipe。结构体字段注解在 vfs/05b |
| `fs/eventfd.c`、`fs/signalfd.c`、`fs/timerfd.c` | eventfd / signalfd / timerfd |
| `net/unix/af_unix.c` | `AF_UNIX` 的 `unix_poll`（薄） |
| `ipc/util.c`、`ipc/msg.c`、`ipc/sem.c`、`ipc/shm.c`、`ipc/mqueue.c` | SysV 与 POSIX mqueue（薄） |
| `include/linux/wait.h`、`kernel/sched/wait.c` | 唤醒扫描。词汇在 locks-and-ds/08 |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.113+）：[demos/](./demos/) 三个程序的 stdout 贴在 [05](./05-epoll-wait-and-modes.md)、[06](./06-pipe.md)、[07](./07-eventfd-signalfd-timerfd.md) 和 [09](./09-observability.md)。同一次开机读到的 `/proc/sys/fs/epoll/max_user_watches`、`pipe-max-size`、`pipe-user-pages-*` 以及 `F_GETPIPE_SZ` 贴在 [09](./09-observability.md)。`getconf PAGE_SIZE` 为 4096。
- **RECIPE**：[10](./10-perf-analysis.md) 的 `perf` / `bpftrace`。本环境没有 `perf`、`bpftrace`、`strace`，`/sys/kernel/tracing/events/syscalls/` 读不到。不编延迟数字。

## 未覆盖

- `sk_buff`、TCP 就绪、NAPI busy poll 的预算和 `napi_busy_loop` 循环
- `IORING_OP_POLL_ADD` 的请求对象
- `watch_queue` / `KEYCTL_WATCH_*`。pipe 上的 `O_NOTIFICATION_PIPE` 只点名
- SysV 每条命令的兼容层、endian、完整权限判例
- 用户态库（libevent 等）

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
