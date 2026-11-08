# 10. 性能与坑

**结论**：epoll 省下的是「每次调用按 nfds 重挂等待队列」；fd 很少或兴趣集合每次都变时，这点省不下多少。边缘触发没读完、多个 epfd 监视同一个 listen fd、以及管道上的一字节一唤醒，是三个独立的坑。

> 行为以 v6.6.160 的 `fs/eventpoll.c`、`fs/select.c`、`fs/pipe.c` 为准。
> 本节没有延迟或 CPU 占用的实测数字。本环境没有 `perf` / `bpftrace`。标了 RECIPE 的命令没有跑过。

---

## 10.1 什么时候 epoll 未必更合适

select / poll 每次系统调用的成本随 `nfds` 涨：拷贝用户集合，对每个 fd `vfs_poll`，再在返回前 `poll_freewait`。epoll 把挂 `eppoll_entry` 的成本放在 `EPOLL_CTL_ADD`，`epoll_wait` 的扫描量随当前 `rdllist` 的长度，而不是随历史上加过的 fd 数。

兴趣集合很小，而且下一次调用就换成另一批 fd 时，`ADD` + `DEL` 的红黑树插入和 slab 分配比一次 `poll` 扫几个 fd 更重。集合稳定在几千个连接上时，省掉的是每一轮对全部连接调用 `f_op->poll` 并重新 `add_wait_queue`。具体交叉点取决于 `f_op->poll` 本身贵不贵（套接字要看接收队列，管道只看 `head`/`tail`），这里不给一个普遍的 fd 个数门槛。

`epoll_ctl` 和 `epoll_wait` 都要拿 `ep->mtx`。一个 epfd 上并发的 `ctl` 和 `wait` 在这把 mutex 上排队。多线程共享一个 epfd 时，睡眠侧的 `wq` 已经是独占唤醒（见 [05](./05-epoll-wait-and-modes.md)），锁的排队是另一件事。

---

## 10.2 ET 漏事件

`ep_send_events` 对 `EPOLLET` 的项交付后不挂回 `rdllist`（`eventpoll.c:1717`）。管道里还剩字节、eventfd 的 `count` 还大于 0，都不会再产生一次 `epoll_wait` 返回，除非目标文件再执行一次 `wake_up*`。

[05](./05-epoll-wait-and-modes.md) 的用户态计数是这个规则的直接结果：4 字节只读 1 字节时，LT 返回 4 次，ET 返回 1 次，剩下 3 字节要靠循环读到 `EAGAIN`。把 `EAGAIN` 当成错误拆掉连接，会把正常的「这次已经读完」误判成失败。非阻塞 fd 上 `EAGAIN` 是 ET 循环的结束条件。

`EPOLLONESHOT` 连兴趣掩码一起清掉。漏的不是「没读完」，而是「没 `EPOLL_CTL_MOD`」。数据还在，第二次 `epoll_wait` 也返回 0，直到 `MOD` 让 `ep_modify` 复查。

---

## 10.3 惊群与 `EPOLLEXCLUSIVE`

两层唤醒，不要混成一层：

1. 目标文件的等待队列。非独占的 `eppoll_entry` 会全部被 `wake_up` 调到。多个 epfd `ADD` 同一个 listen fd 时，一次到达可以让每个 epfd 都醒。`EPOLLEXCLUSIVE` 把这些 `eppoll_entry` 标成独占，`ep_poll_callback` 返回 1 时消耗 `nr_exclusive`（通常为 1），后面的独占 epoll 这次不再被调用。允许的事件位和「不能加在 epoll 目标上、不能 `MOD` 新加」见 [04](./04-epoll-ctl-and-callback.md)。
2. 单个 `eventpoll.wq`。`ep_poll` 用 `__add_wait_queue_exclusive` 把 `epoll_wait` 挂上，回调里的 `wake_up(&ep->wq)` 只叫一个线程。这一层不需要 `EPOLLEXCLUSIVE`。

`accept` 本身若也有线程堵在 listen fd 的等待队列上，那些等待项多半是非独占或另一次独占名额，`EPOLLEXCLUSIVE` 管不到它们。要收的是「多个 epoll 实例同时 `accept`」时，给每个 epfd 的 listen fd 加上这一位，并让 `accept` 走非阻塞，`EAGAIN` 就回到 `epoll_wait`。

---

## 10.4 管道：小写一次，醒一次

`pipe_write` 在末尾对 `rd_wait` 做 `wake_up_interruptible_sync_poll`（`fs/pipe.c:599`）。`poll_usage` 一旦被 `pipe_poll` 置上，即使写入前管道里已经有数据，这次写入仍会唤醒。epoll 监视读端之后，每次 `write` 系统调用至少对应一次 `ep_poll_callback`。

把逻辑上的一条消息拆成很多次 1 字节 `write`，就会得到同样多次 `epoll_wait` 返回（LT），或同样多次边缘（ET，若每次写都发生在读者已经排空之后）。`F_SETPIPE_SZ` 加大的是槽数，不合并系统调用。合并短写是用户态的事；内核只在同一次 `pipe_write` 里把尾部字节并进上一槽（`:463` 一带，槽带 `PIPE_BUF_FLAG_CAN_MERGE` 且页内还有空间）。

默认 16 个槽、本机 65536 字节（[09](./09-observability.md)）对一次几十字节的写入不会写满，写端不会睡在 `wr_wait` 上。写端睡眠出现在读者不取、槽被占满之后。那时每次读出空间都会 `wake_up_interruptible_sync_poll(&wr_wait, EPOLLOUT | EPOLLWRNORM)`（`pipe_read` `:390`），写端再被独占地放进一个。

---

## 10.5 RECIPE

下面两条没有在本机跑过。不要把 [05](./05-epoll-wait-and-modes.md) 的命中次数读成耗时。

```text
# RECIPE：系统调用次数，不包含 ep_poll_callback 的开销
perf stat -e syscalls:sys_enter_epoll_wait,syscalls:sys_enter_epoll_ctl,\
syscalls:sys_enter_read,syscalls:sys_enter_write -- ./ipc/demos/01-epoll-lt-vs-et

# RECIPE：回调是不是被每次 write 打中。高频率时本身很贵
bpftrace -e 'kprobe:ep_poll_callback { @ = count(); }' -c ./ipc/demos/02-pipe-wakeup
```

自写环回（eventfd 或 pipe 的写端、读端放在同一 epoll 里，测 `epoll_wait` 返回到下一次 `write` 的间隔）也是 RECIPE。间隔里包含调度和 `WF_SYNC` 是否把读者留在同一 CPU，本环境没有把这段时间量出来。
