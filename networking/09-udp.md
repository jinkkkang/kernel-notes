# 9. UDP

**结论**：UDP 没有连接状态机。`udp_sendmsg` 每次现查路由、做 skb，经 `udp_send_skb` 进入 IP；`udp_recvmsg` 从 `sk_receive_queue` 取出整段数据报。套接字挂在 `udp_table` 的两张哈希上，键是本端端口，以及本端端口加地址。

> `udp_sendmsg` 在 `net/ipv4/udp.c:1115`，`udp_recvmsg` 在 `:1877`。`udp_table` 在 `include/net/udp.h:73`。协议登记 `udp_protocol.handler = udp_rcv`（`net/ipv4/af_inet.c:1762`）。

---

## 9.1 哈希表

```c
/* include/net/udp.h:59 */
struct udp_hslot {
	struct hlist_head	head;
	int			count;
	spinlock_t		lock;
};

/* :73 */
struct udp_table {
	struct udp_hslot	*hash;    /* 本端端口 */
	struct udp_hslot	*hash2;   /* 本端端口 + 本端地址 */
	unsigned int		mask;     /* 槽数减一 */
	unsigned int		log;
};
```

`udp_hashslot`（`:81`）用 `udp_hashfn(net, num, mask)`。`hash2` 在查已连接套接字或指定了目的地址时更准。每份 `struct net` 有自己的 `ipv4.udp_table`，和 TCP 的哈希一样随 netns 分开。

`__udp4_lib_lookup`（`udp.c:528`）在收包路径上按四元组找 `sock`。早解复用是 `udp_v4_early_demux`（`:2622`），在路由之前就试图把 skb 的 `sk` 填上。

---

## 9.2 发送

```text
udp_sendmsg                         net/ipv4/udp.c:1115
  已 connect：用 inet 里记住的目的地址
  否则从 msg_name 取 sockaddr_in
  路由：ip_route_output_flow，得到 rtable
  cork 或 MSG_MORE：先攒在 inet->cork
  否则：
    ip_make_skb                     :1321
    udp_send_skb                    :963，调用点 :1326
      填 UDP 头，按 ip_summed 处理校验和
      ip_send_skb → ip_output → __dev_queue_xmit
```

`udp_send_skb` 的定义在 `:963`。没有重传队列，skb 交给 IP 之后套接字不再留副本。发送缓冲仍然记在 `sk_wmem_alloc` 上，析构函数是 `sock_wfree`。veth 用这个析构判断「像本地 UDP」来决定要不要做 GRO（`veth.c:338` 附近的注释）。

校验和可以是 0（IPv4 表示不使用）。`sk_no_check_tx` 来自 `SO_NO_CHECK`。

---

## 9.3 接收

```text
udp_rcv
  __udp4_lib_rcv                    udp.c:2448
    __udp4_lib_lookup
    校验和
    udp_queue_rcv_skb               :2268
      __udp_queue_rcv_skb           :2113
        接收缓冲够：skb 进 sk_receive_queue，sk_data_ready
        不够：sk_drops++，丢掉

udp_recvmsg                         :1877
  MSG_ERRQUEUE：ip_recv_error
  __skb_recv_udp                    从 sk_receive_queue 取一整个 skb
  拷给用户，长度是这个数据报，不是字节流
  释放 skb
```

一个 `recvmsg` 对应一个数据报。用户缓冲短于数据报时，多出来的字节丢掉，`MSG_TRUNC` 让返回值仍是原始长度。没有序号，也没有 `out_of_order_queue`。

INET 的 `inet_recvmsg`（`net/ipv4/af_inet.c:872`）在调用 `udp_recvmsg` 之前执行 `sock_rps_record_flow`（`:880`）。已建立状态才会把当前 CPU 写进 RFS 全局表。表的结构在 [04](./04-netdev-and-napi.md)，用法在 [15](./15-mq-rps-xps-affinity.md)。

---

## 9.4 本机套接字计数

[demos/01-sock-stats](./demos/01-sock-stats.c) 在 sampled on 6.12.113+ 上先读 `/proc/net/sockstat`，再监听一个 TCP 端口、连上一条回路、绑定一个 UDP 套接字。当次：

```text
kernel=6.12.113+
tcp_listen_port=53309 udp_port=55613
before TCP: inuse 9  UDP: inuse 0
after  TCP: inuse 12 UDP: inuse 1
ss 对这条回路：cubic cwnd:10 rto:204 rtt:0.02/0.01
skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0)
```

`inuse` 增加 3 对应监听套接字、已连接的客户端和 accept 出来的服务端。UDP `inuse` 从 0 到 1。`ss -m` 的 `skmem` 对应 `sk_rmem_alloc`、`sk_rcvbuf`、`sk_wmem_alloc`、`sk_sndbuf` 这一组，字段在 [03](./03-socket-and-sock.md)。端口号和 `rtt` 每次运行都会变。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
