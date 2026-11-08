# 8. TCP

**结论**：一条 TCP 连接是 `struct tcp_sock`。发送把字节放进 `sk_write_queue`，`tcp_write_xmit` 按拥塞窗口切 skb；接收在 `TCP_ESTABLISHED` 走 `tcp_rcv_established`，序号对得上就进 `sk_receive_queue`。拥塞算法通过 `icsk_ca_ops` 挂上，6.6 里 cubic 的 `cong_avoid` 是 `cubictcp_cong_avoid`。

> `tcp_sock` 在 `include/linux/tcp.h:177`。它的第一个成员是 `inet_connection_sock`（`include/net/inet_connection_sock.h:88`），再往下才是 `inet_sock` 和 `sock`，见 [03](./03-socket-and-sock.md)。状态枚举在 `include/net/tcp_states.h`。

---

## 8.1 状态

`sk->sk_state` 的取值见 [03](./03-socket-and-sock.md)。数据路径上分三岔，都在 `tcp_v4_do_rcv`（`net/ipv4/tcp_ipv4.c:1720`）之前由 `tcp_v4_rcv`（`:2005`）先查表：

| `sk_state` | 去向 |
|------------|------|
| `TCP_ESTABLISHED` | `tcp_rcv_established`（`tcp_input.c:6046`） |
| `TCP_LISTEN` | `tcp_v4_cookie_check` / `tcp_rcv_state_process`，可能生出子套接字 |
| `TCP_TIME_WAIT` | `tcp_v4_rcv` 里 `do_time_wait` |
| `TCP_NEW_SYN_RECV` | 半连接 `request_sock`，`tcp_check_req` 之后可能变成子套接字 |
| 其余（`SYN_SENT`、`FIN_WAIT*`、`CLOSE_WAIT` 等） | `tcp_rcv_state_process`（`:6664`） |

`tcp_v4_rcv` 先丢掉 `pkt_type != PACKET_HOST` 的包（`:2018`），拉出 TCP 头，`skb_checksum_init` 失败走 `csum_error`。查找是 `__inet_lookup_skb`（`:2047`），哈希表在 `net->ipv4.tcp_death_row.hashinfo`。

---

## 8.2 `inet_connection_sock` 上的定时器和拥塞

`tcp_sock` 前面这一层放连接级定时器和拥塞操作表。完整字段在 `inet_connection_sock.h:88`，数据路径先认这些：

```c
struct inet_connection_sock {
	struct inet_sock	icsk_inet;          /* 必须是第一个 */
	struct request_sock_queue icsk_accept_queue;
	struct inet_bind_bucket	*icsk_bind_hash;
	struct inet_bind2_bucket *icsk_bind2_hash;
	unsigned long		icsk_timeout;
	struct timer_list	icsk_retransmit_timer;
	struct timer_list	icsk_delack_timer;
	/* keepalive 与 mptcp 定时器共用一个 union */
	__u32			icsk_rto;
	const struct tcp_congestion_ops *icsk_ca_ops;
	const struct inet_connection_sock_af_ops *icsk_af_ops;
	__u8			icsk_ca_state:5,
				icsk_ca_initialized:1,
				icsk_ca_setsockopt:1,
				icsk_ca_dst_locked:1;
	__u8			icsk_retransmits;
	__u8			icsk_pending;
	__u8			icsk_backoff;
	/* 其余 pending/ato、ca_priv 按头文件顺序接在后面 */
};
```

`tcp_congestion_ops`（`include/net/tcp.h:1060`）里必须实现的是 `ssthresh`、`cong_avoid`、`undo_cwnd`。`tcp_cong_avoid`（`tcp_input.c:3258`）在 `:3262` 调用 `icsk->icsk_ca_ops->cong_avoid(sk, ack, acked)`。

cubic 的表（`net/ipv4/tcp_cubic.c:478`）：

```c
static struct tcp_congestion_ops cubictcp = {
	.init		= cubictcp_init,
	.ssthresh	= cubictcp_recalc_ssthresh,
	.cong_avoid	= cubictcp_cong_avoid,   /* :324 */
	.set_state	= cubictcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= cubictcp_cwnd_event,
	.pkts_acked	= cubictcp_acked,
	.name		= "cubic",
};
```

模块初始化 `tcp_register_congestion_control(&cubictcp)`（`:545`）。`ss -i` 里的 `cubic` 就是这个名字。窗口变量 `snd_cwnd`、`snd_ssthresh` 在 `tcp_sock` 里，算法只负责改它们。

---

## 8.3 `struct tcp_sock`

字段按 `include/linux/tcp.h:177`。RFC 序号用主机序。

```c
struct tcp_sock {
	struct inet_connection_sock inet_conn; /* 必须是第一个 */
	u16	tcp_header_len;     /* 要发出的 TCP 头长度 */
	u16	gso_segs;
	__be32	pred_flags;         /* 头预测：0x5?10<<16 + snd_wnd */

	u64	bytes_received;
	u32	segs_in;
	u32	data_segs_in;
	u32	rcv_nxt;            /* 下一个想收的序号 */
	u32	copied_seq;         /* 用户还没拷走的起点 */
	u32	rcv_wup;            /* 上次发窗口更新时的 rcv_nxt */
	u32	snd_nxt;            /* 下一个要发的序号 */
	u32	segs_out;
	u32	data_segs_out;
	u64	bytes_sent;
	u64	bytes_acked;
	u32	dsack_dups;
	u32	snd_una;            /* 还没被确认的第一个字节 */
	u32	snd_sml;
	u32	rcv_tstamp;
	u32	lsndtime;
	u32	last_oow_ack_time;
	u32	compressed_ack_rcv_nxt;
	u32	tsoffset;
	struct list_head tsq_node;             /* TCP Small Queues */
	struct list_head tsorted_sent_queue;   /* 已发未 SACK，按时间 */

	u32	snd_wl1;
	u32	snd_wnd;            /* 对端窗口 */
	u32	max_window;
	u32	mss_cache;          /* 有效 MSS，不含 SACK 选项 */
	u32	window_clamp;
	u32	rcv_ssthresh;
	u8	scaling_ratio;
	struct tcp_rack {
		u64 mstamp;
		u32 rtt_us;
		u32 end_seq;
		u32 last_delivered;
		u8  reo_wnd_steps;
		u8  reo_wnd_persist:5, dsack_seen:1, advanced:1;
	} rack;
	u16	advmss;
	u8	compressed_ack;
	u8	dup_ack_counter:2, tlp_retrans:1, unused:5;
	u32	chrono_start;
	u32	chrono_stat[3];
	u8	chrono_type:2, rate_app_limited:1, fastopen_connect:1,
		fastopen_no_cookie:1, is_sack_reneg:1, fastopen_client_fail:2;
	u8	nonagle:4, thin_lto:1, recvmsg_inq:1, repair:1, frto:1;
	u8	repair_queue;
	u8	save_syn:2, syn_data:1, syn_fastopen:1, syn_fastopen_exp:1,
		syn_fastopen_ch:1, syn_data_acked:1, is_cwnd_limited:1;
	u32	tlp_high_seq;
	u32	tcp_tx_delay;
	u64	tcp_wstamp_ns;
	u64	tcp_clock_cache;
	u64	tcp_mstamp;
	u32	srtt_us;            /* 平滑 RTT << 3，单位微秒 */
	u32	mdev_us;
	u32	mdev_max_us;
	u32	rttvar_us;
	u32	rtt_seq;
	struct minmax rtt_min;
	u32	packets_out;        /* 在途包 */
	u32	retrans_out;
	u32	max_packets_out;
	u32	cwnd_usage_seq;
	u16	urg_data;
	u8	ecn_flags;
	u8	keepalive_probes;
	u32	reordering;
	u32	reord_seen;
	u32	snd_up;

	struct tcp_options_received rx_opt;

	u32	snd_ssthresh;
	u32	snd_cwnd;
	u32	snd_cwnd_cnt;
	u32	snd_cwnd_clamp;
	u32	snd_cwnd_used;
	u32	snd_cwnd_stamp;
	u32	prior_cwnd;
	u32	prr_delivered;      /* Recovery 里新送到对端的包 */
	u32	prr_out;
	u32	delivered;
	u32	delivered_ce;
	u32	lost;
	u32	app_limited;
	u64	first_tx_mstamp;
	u64	delivered_mstamp;
	u32	rate_delivered;
	u32	rate_interval_us;
	u32	rcv_wnd;
	u32	write_seq;          /* 发送缓冲的尾 */
	u32	notsent_lowat;
	u32	pushed_seq;
	u32	lost_out;
	u32	sacked_out;

	struct hrtimer	pacing_timer;
	struct hrtimer	compressed_ack_timer;
	struct sk_buff	*lost_skb_hint;
	struct sk_buff	*retransmit_skb_hint;
	struct rb_root	out_of_order_queue; /* 乱序段，要拿着 sock 锁 */
	struct sk_buff	*ooo_last_skb;
	struct tcp_sack_block duplicate_sack[1];
	struct tcp_sack_block selective_acks[4]; /* TCP_NUM_SACKS = 4 */
	struct tcp_sack_block recv_sack_cache[4];
	struct sk_buff	*highest_sack;
	int	lost_cnt_hint;
	u32	prior_ssthresh;
	u32	high_seq;
	u32	retrans_stamp;
	u32	undo_marker;
	int	undo_retrans;
	u64	bytes_retrans;
	u32	total_retrans;
	u32	rto_stamp;
	u16	total_rto;
	u16	total_rto_recoveries;
	u32	total_rto_time;
	u32	urg_seq;
	unsigned int	keepalive_time;
	unsigned int	keepalive_intvl;
	int	linger2;
#ifdef CONFIG_BPF
	u8	bpf_sock_ops_cb_flags;     /* sockops 只点名，见 ebpf 章 */
	u8	bpf_chg_cc_inprogress:1;
#endif
	u16	timeout_rehash;
	u32	rcv_ooopack;
	u32	rcv_rtt_last_tsecr;
	struct { u32 rtt_us; u32 seq; u64 time; } rcv_rtt_est;
	struct { u32 space; u32 seq; u64 time; } rcvq_space;
	struct { u32 probe_seq_start; u32 probe_seq_end; } mtu_probe;
	u32	plb_rehash;
	u32	mtu_info;
#if IS_ENABLED(CONFIG_MPTCP)
	bool	is_mptcp;
#endif
#if IS_ENABLED(CONFIG_SMC)
	bool	(*smc_hs_congested)(const struct sock *sk);
	bool	syn_smc;
#endif
#if defined(CONFIG_TCP_MD5SIG) || defined(CONFIG_TCP_AO)
	const struct tcp_sock_af_ops *af_specific;
#ifdef CONFIG_TCP_MD5SIG
	struct tcp_md5sig_info __rcu *md5sig_info;
#endif
#ifdef CONFIG_TCP_AO
	struct tcp_ao_info __rcu *ao_info;
#endif
#endif
	struct tcp_fastopen_request *fastopen_req;
	struct request_sock __rcu *fastopen_rsk;
	struct saved_syn *saved_syn;
};
```

`snd_una` 到 `snd_nxt` 是已发送未确认。`write_seq` 比 `snd_nxt` 更靠后的部分还在 `sk_write_queue` 里，没交给 IP。`copied_seq` 到 `rcv_nxt` 是已经收进队列、用户还没读走的字节。

重传的 skb 不在 `sk_write_queue` 里另排一条链表：`sk->tcp_rtx_queue` 和 `sk_send_head` 占同一个 union（`sock.h:456`）。乱序接收在 `out_of_order_queue` 这棵红黑树上，序号补齐后才拼回 `sk_receive_queue`。

---

## 8.4 发送

```text
tcp_sendmsg                         net/ipv4/tcp.c:1343
  lock_sock
  tcp_sendmsg_locked                :1042
    从 msghdr 拷进 sk_write_queue 尾部的 skb，不够就新分配
    MSG_ZEROCOPY 且路由有 NETIF_F_SG：挂 ubuf_info（:1056）
    等待内存：在 sk_wq 上睡，词汇见 locks-and-ds/08
    tcp_push                        :709
      __tcp_push_pending_frames     :741
        tcp_write_xmit              net/ipv4/tcp_output.c:2692
          受 snd_cwnd、snd_wnd、nagle 限制
          tcp_transmit_skb          :1442
            克隆一份给 IP，原 skb 留着等 ACK
            icsk_af_ops 填头
            ip_queue_xmit           net/ipv4/ip_output.c:552
```

`tcp_transmit_skb` 克隆而不是把原 skb 交出去，这样超时可以原样重传。克隆的共享规则在 [02](./02-sk-buff.md)。ACK 到了之后 `tcp_ack`（`tcp_input.c:3917`）推进 `snd_una`，确认过的 skb 从重传树摘下，`sk_wmem_alloc` 在析构里下降，`sk_write_space` 叫醒还在等发送缓冲的进程。

---

## 8.5 接收

```text
tcp_v4_rcv
  tcp_v4_do_rcv                     tcp_ipv4.c:1720
    TCP_ESTABLISHED：
      sock_rps_save_rxhash          RFS 记下应用 CPU，见 15
      sk_mark_napi_id
      tcp_rcv_established           tcp_input.c:6046
        快路径：序号、窗口、头预测对得上
          tcp_queue_rcv             进 sk_receive_queue
          tcp_event_data_recv / sk_data_ready
        慢路径：
          tcp_ack                   :3917 的函数，调用点在 :6207 一带
          tcp_data_queue            :5185
            乱序进 out_of_order_queue
    其他状态：tcp_rcv_state_process :6664
      处理 SYN、FIN、ACK 驱动状态迁移

tcp_recvmsg                         net/ipv4/tcp.c:2593
  从 copied_seq 起把 sk_receive_queue 拷给用户
  队列空：在 sk_wq 上睡，直到 sk_data_ready
```

`tcp_rcv_established` 的快路径依赖 `pred_flags`。头预测失败、带选项、或者序号不连续，就落到带 `tcp_validate_incoming` 和 `tcp_data_queue` 的慢路径。用户读走数据后 `copied_seq` 前进，窗口在 `tcp_receive_window` 里按 `rcv_wnd` 和已占用缓冲计算，下一次 ACK 带出去。

重传定时器是 `icsk_retransmit_timer`。超时增加 `icsk_backoff` 和 `total_rto`，从 `tcp_rtx_queue` 取出最早的 skb 再 `tcp_transmit_skb`。`ss -i` 里的 `retrans` 对应这里的计数。本机 6.12 上一次 loopback `ss -m -ti` 打出过 `cubic`、`cwnd:10`、`rto` 和 `skmem`，原文在 [16](./16-observability.md)，那是 sampled on 6.12 的数字。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
