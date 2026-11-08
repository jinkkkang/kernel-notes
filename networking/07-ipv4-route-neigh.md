# 7. IPv4 路由与邻居

**结论**：`ip_rcv_finish_core` 查出的结果是嵌在 `struct rtable` 里的 `dst_entry`，`dst->input` 决定本地投递还是转发；出去之前 `dst->output` 经过 `neighbour`，由 ARP 把下一跳 IP 填成链路地址。

> `dst_entry` 在 `include/net/dst.h:26`。`rtable` 在 `include/net/route.h:60`。`fib_lookup` 在 `include/net/ip_fib.h:311`。`neighbour` 在 `include/net/neighbour.h:137`。ARP 处理在 `net/ipv4/arp.c`。

IPv6 用同一套 `dst_entry` 和 `neighbour`（表号 `NEIGH_ND_TABLE`），地址字段在 `sock_common` 的 `skc_v6_*`。本章不把 IPv6 的 fib6 再写一遍。

---

## 7.1 `dst_entry` 与 `rtable`

```c
/* include/net/dst.h:26，只列数据路径会碰到的成员，顺序与源码一致 */
struct dst_entry {
	union {
		struct net_device	*dev;
		struct net_device __rcu	*dev_rcu;
	};
	struct dst_ops		*ops;
	unsigned long		_metrics;
	unsigned long		expires;
#ifdef CONFIG_XFRM
	struct xfrm_state	*xfrm;
#else
	void			*__pad1;
#endif
	int			(*input)(struct sk_buff *);
	int			(*output)(struct net *net, struct sock *sk, struct sk_buff *skb);
	unsigned short		flags;       /* DST_NOPOLICY 等，:42 */
	short			obsolete;    /* 非 0 则要用 ops->check 再确认 */
	unsigned short		header_len;
	unsigned short		trailer_len;
	/* 后面还有 error、tclassid、lwtstate、__refcnt 等，见 dst.h:26 之后 */
};
```

`input` 和 `output` 是这条路由的下一动作。本地路由的 `input` 进 `ip_local_deliver`，转发路由进 `ip_forward`。`obsolete` 的取值：`DST_OBSOLETE_NONE` 是 0，`DST_OBSOLETE_DEAD` 是 2，`DST_OBSOLETE_FORCE_CHK` 是 -1（`dst.h:60`）。

IPv4 把路由缓存成 `rtable`：

```c
/* include/net/route.h:60 */
struct rtable {
	struct dst_entry	dst;
	int			rt_genid;
	unsigned int		rt_flags;
	__u16			rt_type;         /* RTN_UNICAST / RTN_LOCAL 等 */
	__u8			rt_is_input;
	__u8			rt_uses_gateway;
	int			rt_iif;
	u8			rt_gw_family;
	union {
		__be32		rt_gw4;
		struct in6_addr	rt_gw6;
	};
	u32			rt_mtu_locked:1,
				rt_pmtu:31;
};
```

`skb_dst`（`skbuff.h:1114`）从 `_skb_refdst` 取出指针，最低位表示没有单独拿引用。`skb_rtable`（`:1201`）再转成 `rtable *`。

查找入口是 `fib_lookup`（`ip_fib.h:311`）：RCU 下取 `RT_TABLE_MAIN` 的 `fib_table`，再 `fib_table_lookup`（`net/ipv4/fib_trie.c:1423`）。策略路由还有别的表，`struct net` 里的 `rules_ops` 和 `ipv4` 由 [containerization/05](../containerization/05-net-ns.md) 的 `struct net` 带着，每份 netns 各有一份。自定义规则很多时，`ip_extract_route_hint`（`ip_input.c:584`）会放弃批量收包的 hint。

发送侧连接第一次解析路由后，结果留在 `sk->sk_dst_cache`。`ip_queue_xmit`（`ip_output.c:552`）能复用就复用，不能就再查。

---

## 7.2 `neighbour`

下一跳 IP 还不是网卡能发出去的地址。`neighbour` 把主键（IPv4 地址）映到链路地址 `ha[]`。

```c
/* include/net/neighbour.h:137 */
struct neighbour {
	struct neighbour __rcu	*next;
	struct neigh_table	*tbl;
	struct neigh_parms	*parms;
	unsigned long		confirmed;
	unsigned long		updated;
	rwlock_t		lock;
	refcount_t		refcnt;
	unsigned int		arp_queue_len_bytes;
	struct sk_buff_head	arp_queue;   /* 地址还没解析时先排队 */
	struct timer_list	timer;
	unsigned long		used;
	atomic_t		probes;
	u8			nud_state;   /* NUD_* */
	u8			type;
	u8			dead;
	u8			protocol;
	u32			flags;
	seqlock_t		ha_lock;
	unsigned char		ha[ALIGN(MAX_ADDR_LEN, sizeof(unsigned long))];
	struct hh_cache		hh;          /* 缓存好的硬件头 */
	int			(*output)(struct neighbour *, struct sk_buff *);
	const struct neigh_ops	*ops;
	struct list_head	gc_list;
	struct list_head	managed_list;
	struct rcu_head		rcu;
	struct net_device	*dev;
	netdevice_tracker	dev_tracker;
	u8			primary_key[];   /* IPv4 就是 4 字节地址 */
};
```

状态在 `include/uapi/linux/neighbour.h`：

| 宏 | 值 | 含义 |
|----|----|------|
| `NUD_NONE` | 0x00 | 还没有条目 |
| `NUD_INCOMPLETE` | 0x01 | 正在问 |
| `NUD_REACHABLE` | 0x02 | 最近确认过 |
| `NUD_STALE` | 0x04 | 过期，先用着，再探测 |
| `NUD_DELAY` | 0x08 | 推迟探测 |
| `NUD_PROBE` | 0x10 | 正在单播探测 |
| `NUD_FAILED` | 0x20 | 失败，`arp_queue` 丢掉 |
| `NUD_NOARP` | 0x40 | 这条链路不需要 ARP |
| `NUD_PERMANENT` | 0x80 | 静态，GC 不删 |

`NUD_NOARP` 和 `NUD_PERMANENT` 是伪状态，头文件 `:74` 的注释写明它们不会自行迁移。环回和同一主机上的某些虚拟设备走 `NUD_NOARP`。

表号在 `neighbour.h:240`：`NEIGH_ARP_TABLE` 是 0，`NEIGH_ND_TABLE` 是 1。

```text
neigh_lookup                         net/core/neighbour.c:619
  没有则 __neigh_create              :735
    状态 NUD_INCOMPLETE，ops->solicit 发 ARP 请求
    skb 先进 arp_queue

neigh_resolve_output                 :1548
  neigh_event_send 仍需要解析：返回，包留在 arp_queue
  否则读 ha_lock，填硬件头，neigh->output 或 hh 缓存
    再 dev_queue_xmit
```

ARP 请求的接收在 `arp_process`（`net/ipv4/arp.c:701`），由 `arp_rcv` 在 `:954` 调进来。对得上的应答把 `nud_state` 写成 `NUD_REACHABLE`，再把 `arp_queue` 里的 skb 放出去。

`hh_cache` 把已经填好的以太网头留在邻居上。下一次 `neigh_resolve_output` 看到 `hh.hh_len` 就不必再组头。`header_ops->cache` 为空的设备没有这份缓存。

---

## 7.3 和 netns 的关系

路由表、邻居表、ARP 参数都挂在 `struct net` 上，跟着网络命名空间走。把网卡从一份 netns 挪到另一份是 `__dev_change_net_namespace`，过程在 [containerization/05](../containerization/05-net-ns.md)。挪走之后，旧 ns 里指向这块 `net_device` 的 `neighbour->dev` 会失效，条目被清掉。veth 两端可以在创建时就放进不同的 ns，转发本身在 [11](./11-netns-veth-bridge.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
