# 3. socket 与 sock

**结论**：`struct socket` 是系统调用看见的对象，挂在 `struct file` 上，操作表是 `proto_ops`；`struct sock` 是协议看见的对象，收发队列和内存记账都在它上面，操作表是 `proto`。INET 套接字把 `sock` 嵌在 `inet_sock` 的第一个成员里。

> `socket` 在 `include/linux/net.h:117`。`sock_common` / `sock` 在 `include/net/sock.h:163` / `:360`。`proto` 在 `:1275`，`proto_ops` 在 `net.h:161`。创建在 `net/socket.c` 的 `__sock_create`（`:1500`）。

等待队列的唤醒函数不在这里展开，词汇在 [locks-and-ds/08](../locks-and-ds/08-wait-queue-and-completion.md)。`poll` 怎样挂进 epoll，在 [ipc/02](../ipc/02-poll-select.md)。

---

## 3.1 从 `socket()` 到 fd

```text
__sys_socket                 net/socket.c:1710
  __sock_create              :1500
    sock_alloc               :629     分配 socket，并拿到一个 inode
    按 family 调 net_proto_family->create
      inet_create            net/ipv4/af_inet.c:251
        sk_alloc             net/core/sock.c:2151   从 proto->slab 拿 sock
        sock_init_data       :3553
  sock_map_fd                net/socket.c:485     装进 struct file，返回 fd
```

`sock_map_fd` 之后，VFS 的 `read`/`write`/`poll` 经 `socket_file_ops` 转回 `proto_ops`。fd 表的安装细节在 [vfs/04](../vfs/04-fd-and-open.md)。

```c
/* include/linux/net.h:117 */
struct socket {
	socket_state		state;  /* SS_UNCONNECTED / SS_CONNECTED 等 */
	short			type;   /* SOCK_STREAM / SOCK_DGRAM */
	unsigned long		flags;  /* SOCK_NOSPACE 等 */
	struct file		*file;
	struct sock		*sk;
	const struct proto_ops	*ops; /* IPV6_ADDRFORM 或 MPTCP 时可能换 */
	struct socket_wq	wq;   /* 嵌在 socket 里的等待队列 */
};
```

`socket_wq`（`net.h:99` 附近）的第一个成员必须是 `wait_queue_head_t wait`。`sock_poll_wait` 把 epoll 的条目挂在 `sock->wq.wait` 上。

`proto_ops`（`:161`）是面向 fd 的函数表，顺序如下。带 `CONFIG_COMPAT` 的 `compat_ioctl` 只有打开兼容层时才在。

| 成员 | 作用 |
|------|------|
| `family` / `owner` | 协议族和模块 |
| `release` / `bind` / `connect` / `socketpair` / `accept` | 生命周期和连接 |
| `getname` | `getsockname` / `getpeername` |
| `poll` | 就绪。TCP 是 `tcp_poll`，UNIX 流是 `unix_poll` |
| `ioctl` / `gettstamp` / `listen` / `shutdown` | 控制 |
| `setsockopt` / `getsockopt` | 选项 |
| `sendmsg` / `recvmsg` | 数据 |
| `mmap` / `splice_read` / `splice_eof` | 和 VFS 的交接 |
| `read_sock` / `sendmsg_locked` 等 | 注释写明调用时已经拿着 sock 锁 |

IPv4 流的表是 `inet_stream_ops`（`net/ipv4/af_inet.c:1051`）：`.poll = tcp_poll`，`.sendmsg` 最终进 `tcp_sendmsg`。数据报是 `inet_dgram_ops`（`:1085`）。

---

## 3.2 `sock_common`

`sock`、`inet_timewait_sock`、`request_sock` 共用这个头，查找哈希只看前一段。64 位且打开 IPv6 时，注释要求 `sk_refcnt` 落在偏移 128（`:205`）。

```c
/* include/net/sock.h:163 */
struct sock_common {
	union {
		__addrpair	skc_addrpair;
		struct {
			__be32	skc_daddr;      /* 对端 IPv4 */
			__be32	skc_rcv_saddr;  /* 本端绑定地址 */
		};
	};
	union {
		unsigned int	skc_hash;
		__u16		skc_u16hashes[2];
	};
	union {
		__portpair	skc_portpair;
		struct {
			__be16	skc_dport;
			__u16	skc_num;        /* 本端端口，主机序 */
		};
	};
	unsigned short		skc_family;
	volatile unsigned char	skc_state;      /* TCP_* ，见 tcp_states.h */
	unsigned char		skc_reuse:4;
	unsigned char		skc_reuseport:1;
	unsigned char		skc_ipv6only:1;
	unsigned char		skc_net_refcnt:1;
	int			skc_bound_dev_if;
	union {
		struct hlist_node	skc_bind_node;
		struct hlist_node	skc_portaddr_node;
	};
	struct proto		*skc_prot;
	possible_net_t		skc_net;

#if IS_ENABLED(CONFIG_IPV6)
	struct in6_addr		skc_v6_daddr;
	struct in6_addr		skc_v6_rcv_saddr;
#endif
	atomic64_t		skc_cookie;

	union {
		unsigned long	skc_flags;
		struct sock	*skc_listener;              /* request_sock */
		struct inet_timewait_death_row *skc_tw_dr; /* timewait */
	};
	/* sock_copy 不复制 dontcopy_begin 到 dontcopy_end */
	union {
		struct hlist_node	skc_node;
		struct hlist_nulls_node	skc_nulls_node;
	};
	unsigned short		skc_tx_queue_mapping;
#ifdef CONFIG_SOCK_RX_QUEUE_MAPPING
	unsigned short		skc_rx_queue_mapping;
#endif
	union {
		int		skc_incoming_cpu;
		u32		skc_rcv_wnd;
		u32		skc_tw_rcv_nxt;
	};
	refcount_t		skc_refcnt;
	union {
		u32		skc_rxhash;
		u32		skc_window_clamp;
		u32		skc_tw_snd_nxt;
	};
};
```

`struct sock` 开头用宏把 `sk_daddr`、`sk_num`、`sk_state`、`sk_prot`、`sk_net` 这些名字映射到 `__sk_common` 里的字段（`sock.h:366` 起）。读代码时 `sk->sk_state` 就是 `skc_state`。

TCP 状态枚举在 `include/net/tcp_states.h:12`，从 1 开始：`TCP_ESTABLISHED`、`TCP_SYN_SENT`、`TCP_SYN_RECV`、`TCP_FIN_WAIT1`、`TCP_FIN_WAIT2`、`TCP_TIME_WAIT`、`TCP_CLOSE`、`TCP_CLOSE_WAIT`、`TCP_LAST_ACK`、`TCP_LISTEN`、`TCP_CLOSING`、`TCP_NEW_SYN_RECV`。`AF_UNIX` 流也借用其中几个名字。

---

## 3.3 `struct sock`

字段顺序按 `:360`。条件编译块只在对应配置打开时出现。

```c
struct sock {
	struct sock_common	__sk_common; /* 必须是第一个成员 */

	/* --- 早解复用 --- */
	struct dst_entry __rcu	*sk_rx_dst;
	int			sk_rx_dst_ifindex;
	u32			sk_rx_dst_cookie;

	socket_lock_t		sk_lock;          /* 用户上下文与 backlog 的锁 */
	atomic_t		sk_drops;
	int			sk_rcvlowat;
	struct sk_buff_head	sk_error_queue;
	struct sk_buff_head	sk_receive_queue; /* 已排好、等 recvmsg 的 skb */
	struct {
		atomic_t	rmem_alloc;       /* 填 64 位上的洞，逻辑上是接收记账 */
		int		len;
		struct sk_buff	*head;
		struct sk_buff	*tail;
	} sk_backlog;                             /* 进程正拿着锁时，软中断把 skb 暂放这里 */
#define sk_rmem_alloc sk_backlog.rmem_alloc

	int			sk_forward_alloc;
	u32			sk_reserved_mem;
#ifdef CONFIG_NET_RX_BUSY_POLL
	unsigned int		sk_ll_usec;
	unsigned int		sk_napi_id;       /* 最近一次收包的 NAPI，busy poll 用 */
#endif
	int			sk_rcvbuf;
	int			sk_disconnects;
	struct sk_filter __rcu	*sk_filter;
	union {
		struct socket_wq __rcu	*sk_wq;   /* 指向 socket->wq */
		struct socket_wq	*sk_wq_raw;
	};
#ifdef CONFIG_XFRM
	struct xfrm_policy __rcu *sk_policy[2];
#endif
	struct dst_entry __rcu	*sk_dst_cache;
	atomic_t		sk_omem_alloc;
	int			sk_sndbuf;

	/* --- 发送热线 --- */
	int			sk_wmem_queued;
	refcount_t		sk_wmem_alloc;
	unsigned long		sk_tsq_flags;     /* TCP Small Queues */
	union {
		struct sk_buff	*sk_send_head;
		struct rb_root	tcp_rtx_queue;    /* TCP 重传红黑树占同一位置 */
	};
	struct sk_buff_head	sk_write_queue;
	__s32			sk_peek_off;
	int			sk_write_pending;
	__u32			sk_dst_pending_confirm;
	u32			sk_pacing_status; /* SK_PACING_NONE / NEEDED / FQ */
	long			sk_sndtimeo;
	union {
		struct timer_list sk_timer;
		struct timer_list mptcp_retransmit_timer;
	};
	__u32			sk_priority;
	__u32			sk_mark;
	unsigned long		sk_pacing_rate;
	unsigned long		sk_max_pacing_rate;
	struct page_frag	sk_frag;
	netdev_features_t	sk_route_caps;    /* 路由带回来的 NETIF_F_TSO 等 */
	int			sk_gso_type;
	unsigned int		sk_gso_max_size;
	gfp_t			sk_allocation;
	__u32			sk_txhash;

	u8			sk_gso_disabled : 1,
				sk_kern_sock : 1,
				sk_no_check_tx : 1,
				sk_no_check_rx : 1,
				sk_userlocks : 4;     /* 用户改过 SO_SNDBUF / SO_RCVBUF */
	u8			sk_pacing_shift;
	u16			sk_type;
	u16			sk_protocol;
	u16			sk_gso_max_segs;
	unsigned long		sk_lingertime;
	struct proto		*sk_prot_creator;
	rwlock_t		sk_callback_lock;
	int			sk_err, sk_err_soft;
	u32			sk_ack_backlog;
	u32			sk_max_ack_backlog;
	kuid_t			sk_uid;
	u8			sk_txrehash;
#ifdef CONFIG_NET_RX_BUSY_POLL
	u8			sk_prefer_busy_poll;
	u16			sk_busy_poll_budget;
#endif
	spinlock_t		sk_peer_lock;
	int			sk_bind_phc;
	struct pid		*sk_peer_pid;
	const struct cred	*sk_peer_cred;
	long			sk_rcvtimeo;
	ktime_t			sk_stamp;
#if BITS_PER_LONG==32
	seqlock_t		sk_stamp_seq;
#endif
	atomic_t		sk_tskey;
	atomic_t		sk_zckey;
	u32			sk_tsflags;
	u8			sk_shutdown;      /* RCV_SHUTDOWN / SEND_SHUTDOWN */
	u8			sk_clockid;
	u8			sk_txtime_deadline_mode : 1,
				sk_txtime_report_errors : 1,
				sk_txtime_unused : 6;
	bool			sk_use_task_frag;
	struct socket		*sk_socket;
	void			*sk_user_data;
#ifdef CONFIG_SECURITY
	void			*sk_security;
#endif
	struct sock_cgroup_data	sk_cgrp_data; /* cgroup 标记，控制器见 cgroup/ */
	struct mem_cgroup	*sk_memcg;
	void			(*sk_state_change)(struct sock *sk);
	void			(*sk_data_ready)(struct sock *sk);
	void			(*sk_write_space)(struct sock *sk);
	void			(*sk_error_report)(struct sock *sk);
	int			(*sk_backlog_rcv)(struct sock *sk, struct sk_buff *skb);
#ifdef CONFIG_SOCK_VALIDATE_XMIT
	struct sk_buff *	(*sk_validate_xmit_skb)(struct sock *sk,
							struct net_device *dev,
							struct sk_buff *skb);
#endif
	void			(*sk_destruct)(struct sock *sk);
	struct sock_reuseport __rcu	*sk_reuseport_cb;
#ifdef CONFIG_BPF_SYSCALL
	struct bpf_local_storage __rcu	*sk_bpf_storage;
#endif
	struct rcu_head		sk_rcu;
	netns_tracker		ns_tracker;
	struct hlist_node	sk_bind2_node;
#if IS_ENABLED(CONFIG_PROVE_LOCKING) && IS_ENABLED(CONFIG_MODULES)
	struct module		*sk_owner;
#endif
};
```

接收有两条队列。软中断可以把 skb 直接放进 `sk_receive_queue`，并 `sk_data_ready` 叫醒等待者。若用户进程正拿着 `sk_lock`，软中断改走 `sk_backlog`，等进程放锁时再由 `sk_backlog_rcv` 排空。TCP 的 `backlog_rcv` 是 `tcp_v4_do_rcv`（`tcp_prot` 在 `net/ipv4/tcp_ipv4.c:3299`）。

发送侧 `sk_wmem_alloc` 是已经交给网络栈、还没析构的字节。流套接字能不能再写，看 `sk_stream_memory_free`（`sock.h:1422`）：`sk_wmem_queued >= sk_sndbuf` 就失败，否则再问 `proto->stream_memory_free`。不够就在套接字等待队列上睡觉。`__sk_mem_raise_allocated`（`net/core/sock.c:3104`）同时看该协议的 `sysctl_mem`。

`sk_pacing` 的取值在 `:560`：`SK_PACING_NONE`、`SK_PACING_NEEDED`、`SK_PACING_FQ`。FQ 的实际限速在 qdisc，不在这张结构里展开。

---

## 3.4 `proto`：协议自己的表

`proto`（`:1275`）比 `proto_ops` 更靠近 skb。数据路径上要认的指针：

```c
struct proto {
	int  (*sendmsg)(struct sock *sk, struct msghdr *msg, size_t len);
	int  (*recvmsg)(struct sock *sk, struct msghdr *msg, size_t len,
			int flags, int *addr_len);
	int  (*backlog_rcv)(struct sock *sk, struct sk_buff *skb);
	int  (*hash)(struct sock *sk);
	void (*unhash)(struct sock *sk);
	int  (*get_port)(struct sock *sk, unsigned short snum);
	bool (*stream_memory_free)(const struct sock *sk, int wake);
	bool (*sock_is_readable)(struct sock *sk);
	void (*enter_memory_pressure)(struct sock *sk);
	atomic_long_t *memory_allocated;
	struct percpu_counter *sockets_allocated;
	unsigned long *memory_pressure;
	long *sysctl_mem;
	int  *sysctl_wmem;
	int  *sysctl_rmem;
	struct kmem_cache *slab;
	unsigned int obj_size;          /* sk_alloc 的对象大小 */
	struct request_sock_ops *rsk_prot;
	struct timewait_sock_ops *twsk_prot;
	union {
		struct inet_hashinfo *hashinfo;
		struct udp_table *udp_table;
		struct raw_hashinfo *raw_hash;
		struct smc_hashinfo *smc_hash;
	} h;
	char name[32];                  /* "TCP" / "UDP"，/proc/net/protocols */
	/* 其余 close/connect/setsockopt/owner 等按 sock.h:1275 的顺序排在前后 */
};
```

`tcp_prot` 在 `tcp_ipv4.c:3280`，`udp_prot` 在 `udp.c:2984`。`obj_size` 决定 `sk_alloc` 拿的是 `tcp_sock` 还是 `udp_sock`：对象比 `struct sock` 大，多出来的字段由协议解释。INET 的嵌套在下一节。

---

## 3.5 `inet_sock` 套在 `sock` 外面

```c
/* include/net/inet_sock.h:209
 * sk 必须是第一个成员，打开 IPv6 时 pinet6 是第二个。
 */
struct inet_sock {
	struct sock		sk;
#if IS_ENABLED(CONFIG_IPV6)
	struct ipv6_pinfo	*pinet6;
#endif
#define inet_daddr		sk.__sk_common.skc_daddr
#define inet_rcv_saddr		sk.__sk_common.skc_rcv_saddr
#define inet_dport		sk.__sk_common.skc_dport
#define inet_num		sk.__sk_common.skc_num
	unsigned long		inet_flags;
	__be32			inet_saddr;     /* 实际用来发送的源地址 */
	__s16			uc_ttl;
	__be16			inet_sport;
	struct ip_options_rcu __rcu	*inet_opt;
	atomic_t		inet_id;
	__u8			tos;
	__u8			min_ttl;
	__u8			mc_ttl;
	__u8			pmtudisc;
	__u8			rcv_tos;
	__u8			convert_csum;
	int			uc_index;
	int			mc_index;
	__be32			mc_addr;
	struct { __u16 lo, hi; } local_port_range;
	struct ip_mc_socklist __rcu	*mc_list;
	struct inet_cork_full	cork;   /* UDP/raw cork 时攒 IP 头 */
};
```

TCP 再在 `inet_connection_sock` 里嵌一层 `inet_sock`（`include/net/inet_connection_sock.h:88`），然后 `tcp_sock` 把 `inet_connection_sock` 放在第一个成员（`include/linux/tcp.h:177`）。所以 `tcp_sk(sk)` 是向下转，`sk` 指针本身始终指向这块内存的开头。TCP 字段在 [08](./08-tcp.md)。

`sendmsg` / `recvmsg` 的系统调用入口是 `__sys_sendmsg`（`net/socket.c:2664`）和 `__sys_recvmsg`（`:2872`），两者都进 `sock_sendmsg`（`:756`）或 `sock_recvmsg`（`:1065`），再调用 `ops->sendmsg` / `ops->recvmsg`。从这里起各协议分叉。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
