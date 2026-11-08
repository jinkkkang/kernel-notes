# ipc demos

对照内核 **v6.6.160** 的 epoll 交付、pipe 唤醒和 eventfd。用户态只用 libc。

| 文件 | 验证点 |
|------|--------|
| [01-epoll-lt-vs-et.c](./01-epoll-lt-vs-et.c) | 同一根管道上 LT 按字节反复返回，ET 只返回一次，ONESHOT 在 `MOD` 之前不再返回 |
| [02-pipe-wakeup.c](./02-pipe-wakeup.c) | 写 1 字节后读端 `EPOLLET` 得到 `EPOLLIN`；顺带打 fdinfo 和 `F_GETPIPE_SZ` |
| [03-eventfd-cross-thread.c](./03-eventfd-cross-thread.c) | 另一个线程 `write` 8 字节计数，等待线程从 `epoll_wait` 读到 1 |

编译与运行：

```bash
cd ipc/demos
make
./01-epoll-lt-vs-et
./02-pipe-wakeup
./03-eventfd-cross-thread
```

下面是写笔记时在本机跑的 stdout。主机内核 **6.12.113+**，`gcc -Wall -Wextra -O2`。行为按 v6.6.160 的 `ep_send_events` / `pipe_write` / `eventfd_write` 解释，数字是 sampled on 6.12。

`01-epoll-lt-vs-et`：

```text
kernel=6.12.113+
lt_hits=4
et_hits=1 et_unread_after_first=3
oneshot_first=1 oneshot_second=0 oneshot_after_mod=1
```

`02-pipe-wakeup`：

```text
kernel=6.12.113+
before_write hits=0 events=0x0
after_write hits=1 events=0x1
read_byte=Z
after_read hits=0 events=0x0
F_GETPIPE_SZ=65536
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
```

`mnt_id`、`ino`、`tfd` 行里的目标 inode 随当次打开变化。`events: 80000019` 是 `EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLET`，解释在 [09](../09-observability.md)。

`03-eventfd-cross-thread`：

```text
kernel=6.12.113+
waiter_events=0x1 count=1
```

没有 `perf` / `bpftrace` 的配方在 [10](../10-perf-analysis.md)，标了 RECIPE，本环境没跑。
