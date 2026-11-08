# 10. AF_UNIX 收发

**结论**：`AF_UNIX` 的字节和文件描述符都装在 `sk_buff` 里，直接排进对端 `sk_receive_queue`，不经过 `net_device`。流是 `unix_stream_sendmsg`，数据报是 `unix_dgram_sendmsg`。`SCM_RIGHTS` 把 `struct file` 的引用挂在 skb 的 `cb` 上，接收方 `scm_detach_fds` 安装进自己的 fd 表。

> 就绪位 `unix_poll` / `unix_dgram_poll` 仍在 [ipc/08](../ipc/08-unix-and-sysv.md)。本章从 `scm_send` 写到入队。实现在 `net/unix/af_unix.c`。`unix_sock` 在 `include/net/af_unix.h:77`。

---

## 10.1 `unix_sock`

```c
/* include/net/af_unix.h:77。sk 必须是第一个成员。 */
struct unix_sock {
	struct sock		sk;
	struct unix_address	*addr;       /* 绑定的路径或抽象名 */
	struct path		path;
	struct mutex		iolock, bindlock;
	struct sock		*peer;       /* 对端 */
	struct sock		*listener;   /* accept 出来的流记住监听者 */
	struct unix_vertex	*vertex;     /* GC 用的图 */
	spinlock_t		lock;        /* unix_state_lock */
	struct socket_wq	peer_wq;     /* 数据报对端接收队列满时挂在这里 */
	wait_queue_entry_t	peer_wake;
	struct scm_stat		scm_stat;
#if IS_ENABLED(CONFIG_AF_UNIX_OOB)
	struct sk_buff		*oob_skb;
#endif
};
```

`unix_peer(sk)` 就是 `unix_sk(sk)->peer`。`socketpair` 在 `unix_socketpair`（`af_unix.c:1698`，行号与 ipc 章一致）里把两边的 `peer` 互指，状态写成 `TCP_ESTABLISHED`。路径名 `bind` 是 `unix_bind`（`:1357`），哈希细节不在这里展开。

`unix_state_lock` 保护 `peer` 和连接状态。流的发送在拿到对端之后还要看对端接收缓冲。

---

## 10.2 流：按字节切 skb

```text
unix_stream_sendmsg                 net/unix/af_unix.c:2227
  scm_send                          :2239    解析 cmsg，见下节
  wait_for_unix_gc                  传了 fd 时先等 GC
  找到 peer，状态须已连接
  循环：
    按对端 sk_rcvbuf 和本次长度分配 skb
    从 msghdr 拷贝字节
    unix_attach_fds                 只有第一条带 fd 的 skb 挂上（:1821）
    对端 sk_receive_queue 入队
    other->sk_data_ready(other)
  对端缓冲满：在 sk_wq 上睡，unix_write_space 再叫醒
```

`scm_send` 是内联（`include/net/scm.h:90`），有控制消息时进 `__scm_send`（`net/core/scm.c:148`）。

接收：

```text
unix_stream_recvmsg                 af_unix.c:2972
  unix_stream_read_generic          :2736，调用点 :2990
    从 sk_receive_queue 按字节拼给用户
    一个 skb 可以只读一部分，剩下的留在队列里
    读到带 fp 的 skb：scm_recv_unix   调用点 :2940
```

这和 TCP 一样是字节流：一次 `recvmsg` 可以跨多个 skb，也可以只消费一个 skb 的前半。数据报则一次拿整段。

---

## 10.3 数据报

```text
unix_dgram_sendmsg                  af_unix.c:1957
  scm_send                          :1970
  未连接：按 msg_name 找目的 sock
  已连接：用 peer
  unix_dgram_peer_wake 相关：对端接收队列满则先不要报可写
  一个 skb 装下整段消息
  skb_queue_tail(&other->sk_receive_queue)   调用点 :2151
  sk_data_ready

unix_dgram_recvmsg                  :2508
  从 sk_receive_queue 取出整个 skb
  skb_copy_datagram_msg
  scm_recv_unix                     :2499
  skb_free_datagram
```

ipc 章停在 `skb_queue_empty_lockless(&sk->sk_receive_queue)`。入队点就是上面的 `skb_queue_tail`。`unix_dgram_poll` 在对端 `unix_recvq_full_lockless` 时清掉可写，避免发送方空转。

---

## 10.4 `SCM_RIGHTS`

控制消息类型 `SCM_RIGHTS` 是 `0x01`（`include/linux/socket.h`，ipc 章已引用）。

```c
/* include/net/scm.h:29。SCM_MAX_FD 是 253（:16）。 */
struct scm_fp_list {
	short			count;
	short			count_unix;
	short			max;
#ifdef CONFIG_UNIX
	bool			inflight;
	bool			dead;
	struct list_head	vertices;   /* GC 图 */
	struct unix_edge	*edges;
#endif
	struct user_struct	*user;
	struct file		*fp[SCM_MAX_FD];
};

/* :43 */
struct scm_cookie {
	struct pid		*pid;
	struct scm_fp_list	*fp;
	struct scm_creds	creds;
#ifdef CONFIG_SECURITY_NETWORK
	u32			secid;
#endif
};
```

发送：

```text
__scm_send                          net/core/scm.c:148
  cmsg_type == SCM_RIGHTS           :173
    从用户 fd 号 fget 出 struct file，放进 scm->fp
unix_attach_fds                     af_unix.c:1821
  too_many_unix_fds → -ETOOMANYREFS
  UNIXCB(skb).fp = scm_fp_dup(scm->fp)
  unix_prepare_fpl                  让 GC 能看见这些 file
```

接收：

```text
unix_detach_fds                     af_unix.c:1840
  把 UNIXCB(skb).fp 交回 scm，skb 上清空
scm_recv_unix                       include/net/scm.h:210
  __scm_recv_common
  scm_detach_fds                    net/core/scm.c:320
    get_unused_fd_flags + fd_install
    把新的 fd 号写进用户的 cmsg
```

`fd_install` 之后，接收进程的 fd 表多一个指向同一 `struct file` 的引用。发送方关掉自己的 fd 不会关掉接收方这份。文件描述符表的结构在 [vfs/04](../vfs/04-fd-and-open.md)。GC 要处理「skb 上的 fd 又指回另一个 UNIX 套接字」形成的环，`unix_vertex` 和 `wait_for_unix_gc` 为的是这件事，图算法不在这里展开。

[demos/02-unix-scm-fd](./demos/02-unix-scm-fd.c) 用 `socketpair(AF_UNIX, SOCK_STREAM)` 把管道的写端传过去，接收方写入 `Q`，原管道读端读到同一字节。sampled on 6.12.113+：

```text
kernel=6.12.113+
payload=Z passed_fd_works=1 read_back=Q
```

`payload=Z` 是随 fd 一起送的那一个字节。内核路径就是 `unix_stream_sendmsg` → `unix_attach_fds` → `unix_stream_read_generic` → `scm_detach_fds`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
