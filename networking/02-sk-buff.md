# 2. sk_buff

**结论**：`struct sk_buff` 是报文的元数据，数据在 `head` 指向的一块缓冲区里；`skb_shared_info` 贴在这块缓冲区的末尾，保存分片、GSO 和克隆引用。`skb_clone` 共享数据，`skb_copy` 另做一份线性拷贝。

> `sk_buff` 在 `include/linux/skbuff.h:843`。`skb_shared_info` 在 `:572`。分配在 `net/core/skbuff.c` 的 `__alloc_skb`（`:624`）。`skb_shinfo(skb)`（`:1699`）把 `skb_end_pointer` 转成 `skb_shared_info *`。

C 新手把指针想成四层：`head` 是缓冲区起点，`data` 是当前协议头，`tail` 是已填数据的末尾，`end` 是 `skb_shared_info` 开始之前。`len` 是整包字节，含分片；`data_len` 只计分片，线性部分是 `len - data_len`。

---

## 2.1 队列头

`sk_buff` 的 `next`/`prev` 必须和 `sk_buff_head` 的前两个成员对齐，这样队列头可以伪装成一个 skb 哨兵。

```c
/* include/linux/skbuff.h:336 */
struct sk_buff_head {
	/* 这两个成员必须在最前，和 sk_buff 对齐 */
	struct_group_tagged(sk_buff_list, list,
		struct sk_buff	*next;
		struct sk_buff	*prev;
	);
	__u32		qlen;   /* 队列里的 skb 个数 */
	spinlock_t	lock;
};
```

`skb_queue_empty`（`:1834`）看 `next == (sk_buff *)list`。无锁读用 `skb_queue_empty_lockless`（`:1846`），`READ_ONCE` 读 `next`。套接字接收队列和 UNIX 对端入队都用这张链表。

---

## 2.2 `skb_shared_info`

它跟在线性缓冲区 `end` 之后。克隆体共享这一块，所以改 `nr_frags` 或 `gso_size` 之前要确认自己独占。

```c
/* include/linux/skbuff.h:572
 * __alloc_skb 会清掉 dataref 之前的字段（:585 的注释）。
 */
struct skb_shared_info {
	__u8		flags;       /* SKBFL_*，见 :497 */
	__u8		meta_len;
	__u8		nr_frags;    /* frags[] 里有效的段数 */
	__u8		tx_flags;    /* SKBTX_*，见 :467 */
	unsigned short	gso_size;    /* 软件分段时每段的载荷上限 */
	/* UFO 路径不一定填这个字段 */
	unsigned short	gso_segs;
	struct sk_buff	*frag_list;  /* 另一条 skb 链，FRAGLIST GSO */
	struct skb_shared_hwtstamps hwtstamps;
	unsigned int	gso_type;    /* SKB_GSO_* 位，枚举从 :636 */
	u32		tskey;

	atomic_t	dataref;     /* 高 16 位载荷引用，低 16 位总引用 */
	unsigned int	xdp_frags_size;

	/* 在 skb 析构之前必须一直有效 */
	void		*destructor_arg; /* zerocopy 的 ubuf_info */

	/* 必须是最后一个成员，pskb_expand_head 依赖这一点 */
	skb_frag_t	frags[MAX_SKB_FRAGS];
};
```

`dataref` 的切分在 `:626`：`SKB_DATAREF_SHIFT` 是 16，低 16 位是总引用，高 16 位是「只引用载荷、不许改头」的计数。头文件 `:600` 起的说明写明：传输层为重传克隆 skb，下层要在前面加自己的头，所以引用被拆成两半。

`skb_frag_t` 就是 `struct bio_vec`（`:362` 的 typedef）。段的页、偏移、长度和块层是同一套，见 [block-io/02](../block-io/02-bio-and-bvec.md)。默认 `MAX_SKB_FRAGS` 来自 `CONFIG_MAX_SKB_FRAGS`，未配置时是 17（`:349`）。

`gso_type` 的第一位是 `SKB_GSO_TCPV4`（`:637`）。后面还有 TCPv6、UDP、UDP L4、GRE 等。驱动看到 `gso_size != 0` 时，可以让网卡按 MSS 切段，也可以由 `skb_segment`（`net/core/skbuff.c:4446`）在软件里切开。

`flags` 里和零拷贝有关的位（`:497`）：

| 位 | 含义 |
|----|------|
| `SKBFL_ZEROCOPY_ENABLE` | 走 zcopy |
| `SKBFL_SHARED_FRAG` | 有的页可能被别的人改写，算校验和前要拷贝 |
| `SKBFL_PURE_ZEROCOPY` | 段上全是用户页，不记进内核内存 |
| `SKBFL_MANAGED_FRAG_REFS` | 页引用由 `ubuf_info` 管 |

`tx_flags`（`:467`）里 `SKBTX_HW_TSTAMP` 要硬件时间戳，`SKBTX_SW_TSTAMP` 在入队网卡时打软件时间戳，`SKBTX_SCHED_TSTAMP` 在进 qdisc 时打。`__dev_queue_xmit` 看到 `SKBTX_SCHED_TSTAMP` 会调 `__skb_tstamp_tx`（`net/core/dev.c:4428`）。

---

## 2.3 `struct sk_buff`

字段顺序按 `include/linux/skbuff.h:843`。带 `CONFIG_*` 的成员只有编进内核时才在。`__copy_skb_header` 一次 `memcpy` 的是 `headers` 这一组（`:927`）；`:897` 注释写明 `cloned` 那一组位域不在这次拷贝里。

```c
struct sk_buff {
	union {
		struct {
			/* 必须在最前，对齐 sk_buff_head */
			struct sk_buff		*next;
			struct sk_buff		*prev;
			union {
				struct net_device	*dev;        /* 进来或出去的设备 */
				unsigned long		dev_scratch; /* dev 为空时 UDP 收包可借用 */
			};
		};
		struct rb_node		rbnode; /* netem、IPv4 重组、TCP */
		struct list_head	list;
		struct llist_node	ll_node;
	};

	struct sock		*sk;     /* 拥有这块 skb 的套接字，可空 */

	union {
		ktime_t		tstamp;
		u64		skb_mstamp_ns; /* 最早出发时间，重传计时用 */
	};

	/* 每层可私用。跨层要保留就先 skb_clone。谁正在排队谁拥有这块 */
	char			cb[48] __aligned(8);

	union {
		struct {
			unsigned long	_skb_refdst; /* dst_entry，最低位是 noref */
			void		(*destructor)(struct sk_buff *skb);
		};
		struct list_head	tcp_tsorted_anchor;
#ifdef CONFIG_NET_SOCK_MSG
		unsigned long		_sk_redir;
#endif
	};

#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	unsigned long		_nfct;
#endif
	unsigned int		len,       /* 整包字节 */
				data_len;  /* 分片部分 */
	__u16			mac_len,
				hdr_len;

	/* 下面这些不在 __copy_skb_header 的拷贝里 */
	__u16			queue_mapping; /* 多队列设备的发送队列号 */

	__u8			cloned:1,      /* 头可能被克隆 */
				nohdr:1,       /* 只引用载荷 */
				fclone:2,      /* SKB_FCLONE_* */
				peeked:1,
				head_frag:1,   /* 从页片段分配，不是 kmalloc */
				pfmemalloc:1,
				pp_recycle:1;  /* page_pool 回收 */
#ifdef CONFIG_SKB_EXTENSIONS
	__u8			active_extensions;
#endif

	struct_group(headers,
	__u8			pkt_type:3;    /* PACKET_HOST 等 */
	__u8			ignore_df:1;
	__u8			dst_pending_confirm:1;
	__u8			ip_summed:2;   /* CHECKSUM_* */
	__u8			ooo_okay:1;

	__u8			tstamp_type:1;
#ifdef CONFIG_NET_XGRESS
	__u8			tc_at_ingress:1;
	__u8			tc_skip_classify:1;
#endif
	__u8			remcsum_offload:1;
	__u8			csum_complete_sw:1;
	__u8			csum_level:2;  /* 连续通过的校验和层数减一，最大 3 */
	__u8			inner_protocol_type:1;

	__u8			l4_hash:1;     /* hash 是四元组 */
	__u8			sw_hash:1;     /* 软件算的 hash */
#ifdef CONFIG_WIRELESS
	__u8			wifi_acked_valid:1;
	__u8			wifi_acked:1;
#endif
	__u8			no_fcs:1;
	__u8			encapsulation:1; /* 内层头有效 */
	__u8			encap_hdr_csum:1;
	__u8			csum_valid:1;
#ifdef CONFIG_IPV6_NDISC_NODETYPE
	__u8			ndisc_nodetype:2;
#endif
#if IS_ENABLED(CONFIG_IP_VS)
	__u8			ipvs_property:1;
#endif
#if IS_ENABLED(CONFIG_NETFILTER_XT_TARGET_TRACE) || IS_ENABLED(CONFIG_NF_TABLES)
	__u8			nf_trace:1;
#endif
#ifdef CONFIG_NET_SWITCHDEV
	__u8			offload_fwd_mark:1;
	__u8			offload_l3_fwd_mark:1;
#endif
	__u8			redirected:1;
#ifdef CONFIG_NET_REDIRECT
	__u8			from_ingress:1;
#endif
#ifdef CONFIG_NETFILTER_SKIP_EGRESS
	__u8			nf_skip_egress:1;
#endif
#ifdef CONFIG_TLS_DEVICE
	__u8			decrypted:1;
#endif
	__u8			slow_gro:1;
#if IS_ENABLED(CONFIG_IP_SCTP)
	__u8			csum_not_inet:1; /* CHECKSUM_PARTIAL 用 CRC32c */
#endif
#if defined(CONFIG_NET_SCHED) || defined(CONFIG_NET_XGRESS)
	__u16			tc_index;
#endif
	u16			alloc_cpu;

	union {
		__wsum		csum;          /* CHECKSUM_COMPLETE：整包校验和 */
		struct {
			__u16	csum_start;    /* 相对 head 的起点 */
			__u16	csum_offset;   /* 相对 csum_start，校验和写到哪 */
		};
	};
	__u32			priority;
	int			skb_iif;       /* 进入时的 ifindex */
	__u32			hash;
	union {
		u32		vlan_all;
		struct {
			__be16	vlan_proto;
			__u16	vlan_tci;
		};
	};
#if defined(CONFIG_NET_RX_BUSY_POLL) || defined(CONFIG_XPS)
	union {
		unsigned int	napi_id;     /* 收包 NAPI */
		unsigned int	sender_cpu;  /* XPS 上的发送 CPU */
	};
#endif
#ifdef CONFIG_NETWORK_SECMARK
	__u32		secmark;
#endif
	union {
		__u32		mark;
		__u32		reserved_tailroom;
	};
	union {
		__be16		inner_protocol;
		__u8		inner_ipproto;
	};
	__u16			inner_transport_header;
	__u16			inner_network_header;
	__u16			inner_mac_header;
	__be16			protocol;      /* 驱动看到的以太网类型 */
	__u16			transport_header;
	__u16			network_header;
	__u16			mac_header;
#ifdef CONFIG_KCOV
	u64			kcov_handle;
#endif
	); /* headers 组结束 */

	/* 必须放在末尾，alloc_skb 依赖这个顺序 */
	sk_buff_data_t		tail;
	sk_buff_data_t		end;
	unsigned char		*head;
	unsigned char		*data;
	unsigned int		truesize;  /* 记账用的缓冲区大小 */
	refcount_t		users;

#ifdef CONFIG_SKB_EXTENSIONS
	struct skb_ext		*extensions;
#endif
};
```

`cb[48]` 是各层的暂存。IPv4 用 `inet_skb_parm`，TCP 用 `tcp_skb_cb`，`AF_UNIX` 用 `UNIXCB`。一块 skb 同时只服务一层：下层要接着用，先 `skb_clone`。

`fclone` 的取值在 `:630`：`SKB_FCLONE_UNAVAILABLE` 是普通缓存，`SKB_FCLONE_ORIG` / `SKB_FCLONE_CLONE` 是成对分配的两个 skb。TCP 重传频繁克隆，成对分配可以少一次 `kmem_cache_alloc`。

---

## 2.4 校验和与 GSO

`ip_summed` 只有两位，取值在 `:246`：

| 值 | 宏 | 收包时表示 | 发包时表示 |
|----|----|------------|------------|
| 0 | `CHECKSUM_NONE` | 设备没算，`csum` 无意义 | 软件已经填好 |
| 1 | `CHECKSUM_UNNECESSARY` | 设备核对过 TCP/UDP/GRE 等，`csum` 仍无意义 | — |
| 2 | `CHECKSUM_COMPLETE` | 设备把整包校验和放进 `csum` | — |
| 3 | `CHECKSUM_PARTIAL` | GRO 或虚拟机对端留下的卸载 | 驱动从 `csum_start` 加到包尾，把结果写到 `csum_start + csum_offset` |

头文件 `:93` 起的说明要求：设备只要能给出整包 `csum`，就必须用 `CHECKSUM_COMPLETE`，不要用 `UNNECESSARY`。`csum_level` 记录连续通过了几层（层数减一）。

GSO 把多个 TCP 段合成一个 skb，`gso_size` 是每段载荷。网卡有 `NETIF_F_TSO` 时驱动直接提交；否则协议栈在 `validate_xmit_skb` 里软件分段。GRO 是反方向：NAPI 里把同流的小包合成一个大 skb，再交给协议。合成发生在 `dev_gro_receive`（`net/core/gro.c:472`），入口是 `napi_gro_receive`（`:635`）。

---

## 2.5 clone、copy、扩头

```text
skb_clone          net/core/skbuff.c:1896
  能用 fclone 的同伴就用 skb2，否则从 skbuff_cache 再分配一个 skb 头
  __skb_clone      共享 head 指向的缓冲区，dataref 加一
  页片段不复制

skb_copy           :1976
  __alloc_skb 一块新的线性缓冲
  skb_copy_bits 把头和分片都抄进去
  遇到 SKB_GSO_FRAGLIST 直接返回 NULL（:1982）

pskb_expand_head   :2086
  线性区要加头或加尾、或者 skb 已被克隆还想改数据时
  重新分配 head，把 skb_shared_info 挪到新的 end
```

`skb_cloned`（`skbuff.h:1939`）为真时，写数据之前要 `skb_unclone`（`:1945`），它内部会 `pskb_expand_head`。只读转发可以一直 clone。`skb_share_check`（`:2038`）在 `users != 1` 时 clone 一份再释放原来的，bridge 收包用的就是它（`net/bridge/br_input.c:345`）。

释放：`kfree_skb`（`:1262`）走丢包统计，`consume_skb`（`:1279`）表示正常用完。引用降到 0 才真正放缓冲区。`destructor` 常见的是 `sock_wfree`，用来减 `sk_wmem_alloc`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
