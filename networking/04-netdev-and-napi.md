# 4. net_device 与 NAPI

**结论**：一块网卡是 `struct net_device`。驱动在硬中断里只做 `napi_schedule`，把 `napi_struct` 挂到当前 CPU 的 `softnet_data.poll_list`，再拉起 `NET_RX_SOFTIRQ`。`net_rx_action` 按预算调用 `poll`。RPS 把 skb 改投到别的 CPU 的 `backlog`，算法在 [15](./15-mq-rps-xps-affinity.md)。

> `napi_struct` 在 `include/linux/netdevice.h:356`，`napi_schedule` 在 `:496`。`net_device` 在 `:2092`，`net_device_ops` 在 `:1417`。`softnet_data` 在 `:3273`。`____napi_schedule` 在 `net/core/dev.c:4615`，`net_rx_action` 在 `:6880`，注册在 `:11957`。softirq 循环本身见 [interrupts/08](../interrupts/08-softirq-tasklet.md)。

---

## 4.1 `napi_struct`

```c
/* include/linux/netdevice.h:356
 * poll_list 只能由改 NAPI_STATE_SCHED 的那一方增删。
 */
struct napi_struct {
	struct list_head	poll_list;
	unsigned long		state;          /* NAPI_STATE_* */
	int			weight;         /* 一次 poll 的包预算 */
	int			defer_hard_irqs_count;
	unsigned long		gro_bitmask;
	int			(*poll)(struct napi_struct *, int);
#ifdef CONFIG_NETPOLL
	int			poll_owner;
#endif
	int			list_owner;     /* 被排到哪个 CPU */
	struct net_device	*dev;
	struct gro_list		gro_hash[GRO_HASH_BUCKETS];
	struct sk_buff		*skb;           /* GRO 正在合并的 skb */
	struct list_head	rx_list;        /* 待上交的 GRO_NORMAL */
	int			rx_count;
	unsigned int		napi_id;        /* busy poll 用这个号找回 NAPI */
	struct hrtimer		timer;
	struct task_struct	*thread;        /* threaded NAPI */
	struct list_head	dev_list;
	struct hlist_node	napi_hash_node;
};
```

`NAPI_POLL_WEIGHT` 是 64（`:2654`）。`netif_napi_add` 用这个默认值。virtio-net 的模块参数 `napi_weight` 初值也是它（`drivers/net/virtio_net.c:27`）。

`napi_schedule`（`:496`）先 `napi_schedule_prep`，成功再 `__napi_schedule`。关中断的变体是 `napi_schedule_irqoff`（`:508`）。

```c
/* net/core/dev.c:4615，调用时中断已经关掉 */
____napi_schedule(sd, napi)
{
	若 NAPI_STATE_THREADED 且 napi->thread 非空：
		wake_up_process(thread)，返回
	list_add_tail(&napi->poll_list, &sd->poll_list)
	napi->list_owner = 当前 CPU
	若 !sd->in_net_rx_action：
		raise_softirq_irqoff(NET_RX_SOFTIRQ)   /* :4649 */
}
```

已经在 `net_rx_action` 里时只入队，不再次拉 softirq。硬中断上下文里 `raise_softirq_irqoff` 只置位，等 `irq_exit_rcu` 再跑，这一点和中断章一致。

---

## 4.2 `softnet_data` 与预算

每 CPU 一份。收包 poll 链表、RPS 的 IPI、发送完成队列都在这里。

```c
/* include/linux/netdevice.h:3273 */
struct softnet_data {
	struct list_head	poll_list;
	struct sk_buff_head	process_queue;
	unsigned int		processed;       /* 本轮处理计数，/proc 里能看到 */
	unsigned int		time_squeeze;    /* 预算用尽的次数 */
#ifdef CONFIG_RPS
	struct softnet_data	*rps_ipi_list;
#endif
	bool			in_net_rx_action;
	bool			in_napi_threaded_poll;
#ifdef CONFIG_NET_FLOW_LIMIT
	struct sd_flow_limit __rcu *flow_limit;
#endif
	struct Qdisc		*output_queue;   /* NET_TX_SOFTIRQ 要接着跑的 qdisc */
	struct Qdisc		**output_queue_tailp;
	struct sk_buff		*completion_queue;
#ifdef CONFIG_XFRM_OFFLOAD
	struct sk_buff_head	xfrm_backlog;
#endif
	struct {
		u16 recursion;
		u8  more;                        /* netdev_xmit_more */
#ifdef CONFIG_NET_EGRESS
		u8  skip_txqueue;
#endif
	} xmit;
#ifdef CONFIG_RPS
	unsigned int		input_queue_head ____cacheline_aligned_in_smp;
	call_single_data_t	csd ____cacheline_aligned_in_smp;
	struct softnet_data	*rps_ipi_next;
	unsigned int		cpu;
	unsigned int		input_queue_tail;
#endif
	unsigned int		received_rps;
	unsigned int		dropped;
	struct sk_buff_head	input_pkt_queue; /* RPS backlog */
	struct napi_struct	backlog;         /* 这个 CPU 的 backlog NAPI */

	spinlock_t		defer_lock ____cacheline_aligned_in_smp;
	int			defer_count;
	int			defer_ipi_scheduled;
	struct sk_buff		*defer_list;
	call_single_data_t	defer_csd;
};
```

`net_rx_action`（`dev.c:6880`）的预算：

```c
time_limit = jiffies + usecs_to_jiffies(netdev_budget_usecs);
budget     = netdev_budget;          /* 初值 300，dev.c:4605 */
sd->in_net_rx_action = true;
把 sd->poll_list 摘到本地 list
while (list 非空 && budget > 0 && 时间未到)
	budget -= napi_poll(n, &repoll);   /* :6917，函数在 :6771 */
时间或预算用尽：time_squeeze++，剩下的 NAPI 留到下一轮
```

`netdev_budget_usecs` 的初值是 `2 * USEC_PER_SEC / HZ`（`:4607`）。`napi_poll` 调 `n->poll(n, weight)`。poll 返回的包数小于预算时，驱动调用 `napi_complete_done` 把自己从调度状态清掉。

`/proc/net/softnet_stat` 一行一个 CPU。列的含义在 [16](./16-observability.md)，本机 6.12 上读到 15 列。

---

## 4.3 接收队列与 RPS 挂钩

6.6 里每个 RX 队列是 `struct netdev_rx_queue`（`include/net/netdev_rx_queue.h:11`）。`net_device._rx` 指向数组，`num_rx_queues` / `real_num_rx_queues` 是分配数和启用数。

```c
struct netdev_rx_queue {
	struct xdp_rxq_info		xdp_rxq;
#ifdef CONFIG_RPS
	struct rps_map __rcu		*rps_map;
	struct rps_dev_flow_table __rcu	*rps_flow_table;
#endif
	struct kobject			kobj;
	struct net_device		*dev;
	netdevice_tracker		dev_tracker;
#ifdef CONFIG_XDP_SOCKETS
	struct xsk_buff_pool		*pool;
#endif
};
```

RPS 的 map 在 `netdevice.h:717`：`rps_map` 是变长 CPU 数组；`rps_dev_flow`（`:729`）记下这条流上次用的 CPU、硬件过滤器和 `last_qtail`；全局 `rps_sock_flow_table`（`:757`）在 `recvmsg` 里记下应用最后跑在哪颗 CPU。

挂钩只有两步，哈希怎么选 CPU 留到 [15](./15-mq-rps-xps-affinity.md)：

```text
get_rps_cpu                         net/core/dev.c:4715
  按 skb 记录的 rx queue 找到 netdev_rx_queue
  没有 rps_map 也没有 rps_flow_table：返回 -1，留在当前 CPU
  否则 skb_get_hash，再查 RFS / RPS

enqueue_to_backlog                  :4946
  目标 CPU 的 input_pkt_queue 长度不超过 netdev_max_backlog（初值 1000，:4600）
  且 skb_flow_limit 放行：挂到队尾
  队列从空变非空：napi_schedule 那个 CPU 的 sd->backlog
  否则 dropped++，SKB_DROP_REASON_CPU_BACKLOG
```

`rps_trigger_softirq`（`:4853`）在目标 CPU 的 IPI 里调用 `____napi_schedule(sd, &sd->backlog)`。单队列网卡想把协议栈摊到多核，走的就是这条，而不是再注册一个硬件队列。

---

## 4.4 `net_device_ops` 里和数据面有关的指针

结构从 `:1417` 排到 `:1662`。数据面先认这几个，后面还有 VF、FCoE、FDB、XDP 等，顺序以头文件为准：

```c
struct net_device_ops {
	int  (*ndo_init)(struct net_device *dev);
	void (*ndo_uninit)(struct net_device *dev);
	int  (*ndo_open)(struct net_device *dev);
	int  (*ndo_stop)(struct net_device *dev);
	netdev_tx_t (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev); /* :1422 */
	netdev_features_t (*ndo_features_check)(...);
	u16  (*ndo_select_queue)(...);          /* 驱动自己选 TX 队列时 */
	/* ndo_set_rx_mode、ndo_set_mac_address、ndo_change_mtu、
	 * ndo_tx_timeout、ndo_get_stats64、VLAN、VF、FCoE、
	 * ndo_rx_flow_steer（RFS 硬件过滤）、ndo_fix_features、
	 * ndo_bpf、ndo_xdp_xmit、ndo_xsk_wakeup
	 * 按 netdevice.h:1430 到 :1661 的声明顺序接在后面 */
};
```

`__netdev_start_xmit`（`:4998`）把 `softnet_data.xmit.more` 写成 `more`，然后调用 `ops->ndo_start_xmit`（`:5003`）。`xmit_one`（`dev.c:3660`）打 `trace_net_dev_start_xmit` 之后进这个内联。

---

## 4.5 `struct net_device`

字段按 `:2092`。热路径分成两段：接收在 `_rx` 附近，发送在 `_tx` 附近，中间用 `____cacheline_aligned_in_smp` 隔开。

```c
struct net_device {
	char			name[IFNAMSIZ];
	struct netdev_name_node *name_node;
	struct dev_ifalias __rcu *ifalias;
	unsigned long		mem_end, mem_start, base_addr; /* 旧式 IO 资源 */
	unsigned long		state;          /* __LINK_STATE_* */
	struct list_head	dev_list;
	struct list_head	napi_list;
	struct list_head	unreg_list;
	struct list_head	close_list;
	struct list_head	ptype_all;       /* 该设备上 ETH_P_ALL */
	struct list_head	ptype_specific;
	struct { struct list_head upper, lower; } adj_list;

	unsigned int		flags;           /* IFF_UP 等 */
	xdp_features_t		xdp_features;
	unsigned long long	priv_flags;      /* IFF_NO_QUEUE 等 */
	const struct net_device_ops *netdev_ops;
	const struct xdp_metadata_ops *xdp_metadata_ops;
	int			ifindex;
	unsigned short		gflags;
	unsigned short		hard_header_len;
	unsigned int		mtu;             /* 读多写少，写侧通常持 rtnl */
	unsigned short		needed_headroom;
	unsigned short		needed_tailroom;
	netdev_features_t	features;
	netdev_features_t	hw_features;
	netdev_features_t	wanted_features;
	netdev_features_t	vlan_features;
	netdev_features_t	hw_enc_features;
	netdev_features_t	mpls_features;
	netdev_features_t	gso_partial_features;
	unsigned int		min_mtu, max_mtu;
	unsigned short		type;            /* ARPHRD_* */
	unsigned char		min_header_len;
	unsigned char		name_assign_type;
	int			group;
	struct net_device_stats	stats;
	struct net_device_core_stats __percpu *core_stats;
	atomic_t		carrier_up_count, carrier_down_count;
#ifdef CONFIG_WIRELESS_EXT
	const struct iw_handler_def *wireless_handlers;
	struct iw_public_data	*wireless_data;
#endif
	const struct ethtool_ops *ethtool_ops;
#ifdef CONFIG_NET_L3_MASTER_DEV
	const struct l3mdev_ops	*l3mdev_ops;
#endif
#if IS_ENABLED(CONFIG_IPV6)
	const struct ndisc_ops *ndisc_ops;
#endif
#ifdef CONFIG_XFRM_OFFLOAD
	const struct xfrmdev_ops *xfrmdev_ops;
#endif
#if IS_ENABLED(CONFIG_TLS_DEVICE)
	const struct tlsdev_ops *tlsdev_ops;
#endif
	const struct header_ops *header_ops;
	unsigned char		operstate, link_mode, if_port, dma;
	unsigned char		perm_addr[MAX_ADDR_LEN];
	unsigned char		addr_assign_type, addr_len, upper_level, lower_level;
	unsigned short		neigh_priv_len, dev_id, dev_port, padded;
	spinlock_t		addr_list_lock;
	int			irq;
	struct netdev_hw_addr_list uc, mc, dev_addrs;
#ifdef CONFIG_SYSFS
	struct kset		*queues_kset;
#endif
#ifdef CONFIG_LOCKDEP
	struct list_head	unlink_list;
#endif
	unsigned int		promiscuity, allmulti;
	bool			uc_promisc;
#ifdef CONFIG_LOCKDEP
	unsigned char		nested_level;
#endif

	/* 协议私有指针 */
	struct in_device __rcu	*ip_ptr;
	struct inet6_dev __rcu	*ip6_ptr;
#if IS_ENABLED(CONFIG_VLAN_8021Q)
	struct vlan_info __rcu	*vlan_info;
#endif
#if IS_ENABLED(CONFIG_NET_DSA)
	struct dsa_port		*dsa_ptr;
#endif
#if IS_ENABLED(CONFIG_TIPC)
	struct tipc_bearer __rcu *tipc_ptr;
#endif
#if IS_ENABLED(CONFIG_ATALK)
	void			*atalk_ptr;
#endif
#if IS_ENABLED(CONFIG_AX25)
	struct ax25_dev __rcu	*ax25_ptr;
#endif
#if IS_ENABLED(CONFIG_CFG80211)
	struct wireless_dev	*ieee80211_ptr;
#endif
#if IS_ENABLED(CONFIG_IEEE802154) || IS_ENABLED(CONFIG_6LOWPAN)
	struct wpan_dev		*ieee802154_ptr;
#endif
#if IS_ENABLED(CONFIG_MPLS_ROUTING)
	struct mpls_dev __rcu	*mpls_ptr;
#endif
#if IS_ENABLED(CONFIG_MCTP)
	struct mctp_dev __rcu	*mctp_ptr;
#endif

	/* --- 接收热线 --- */
	const unsigned char	*dev_addr;
	struct netdev_rx_queue	*_rx;
	unsigned int		num_rx_queues;
	unsigned int		real_num_rx_queues;
	struct bpf_prog __rcu	*xdp_prog;       /* generic XDP，实现见 ebpf 章 */
	unsigned long		gro_flush_timeout;
	int			napi_defer_hard_irqs;
	unsigned int		gro_max_size;    /* GRO_MAX_SIZE = 8 * 65535 */
	unsigned int		gro_ipv4_max_size;
	unsigned int		xdp_zc_max_segs;
	rx_handler_func_t __rcu	*rx_handler;     /* bridge / bond / macvlan */
	void __rcu		*rx_handler_data;
#ifdef CONFIG_NET_XGRESS
	struct bpf_mprog_entry __rcu *tcx_ingress;
#endif
	struct netdev_queue __rcu *ingress_queue;
#ifdef CONFIG_NETFILTER_INGRESS
	struct nf_hook_entries __rcu *nf_hooks_ingress;
#endif
	unsigned char		broadcast[MAX_ADDR_LEN];
#ifdef CONFIG_RFS_ACCEL
	struct cpu_rmap		*rx_cpu_rmap;
#endif
	struct hlist_node	index_hlist;

	/* --- 发送热线 --- */
	struct netdev_queue	*_tx ____cacheline_aligned_in_smp;
	unsigned int		num_tx_queues;
	unsigned int		real_num_tx_queues;
	struct Qdisc __rcu	*qdisc;
	unsigned int		tx_queue_len;
	spinlock_t		tx_global_lock;
	struct xdp_dev_bulk_queue __percpu *xdp_bulkq;
#ifdef CONFIG_XPS
	struct xps_dev_maps __rcu *xps_maps[XPS_MAPS_MAX];
#endif
#ifdef CONFIG_NET_XGRESS
	struct bpf_mprog_entry __rcu *tcx_egress;
#endif
#ifdef CONFIG_NETFILTER_EGRESS
	struct nf_hook_entries __rcu *nf_hooks_egress;
#endif
#ifdef CONFIG_NET_SCHED
	DECLARE_HASHTABLE	(qdisc_hash, 4);
#endif
	struct timer_list	watchdog_timer;
	int			watchdog_timeo;
	spinlock_t		watchdog_lock;
	bool			watchdog_ref_held;
	u32			proto_down_reason;
	struct list_head	todo_list;
#ifdef CONFIG_PCPU_DEV_REFCNT
	int __percpu		*pcpu_refcnt;
#else
	refcount_t		dev_refcnt;
#endif
	struct ref_tracker_dir	refcnt_tracker;
	struct list_head	link_watch_list;
	u8			reg_state;
	bool			dismantle;
	enum { RTNL_LINK_INITIALIZED, RTNL_LINK_INITIALIZING } rtnl_link_state:16;
	bool			needs_free_netdev;
	void			(*priv_destructor)(struct net_device *dev);
#ifdef CONFIG_NETPOLL
	struct netpoll_info __rcu *npinfo;
#endif
	possible_net_t		nd_net;          /* 设备所在的 struct net */
	void			*ml_priv;
	enum netdev_ml_priv_type ml_priv_type;
	enum netdev_stat_type	pcpu_stat_type:8;
	union {
		struct pcpu_lstats __percpu	*lstats;
		struct pcpu_sw_netstats __percpu *tstats;
		struct pcpu_dstats __percpu	*dstats;
	};
#if IS_ENABLED(CONFIG_GARP)
	struct garp_port __rcu	*garp_port;
#endif
#if IS_ENABLED(CONFIG_MRP)
	struct mrp_port __rcu	*mrp_port;
#endif
#if IS_ENABLED(CONFIG_NET_DROP_MONITOR)
	struct dm_hw_stat_delta __rcu *dm_private;
#endif
	struct device		dev;
	const struct attribute_group *sysfs_groups[4];
	const struct attribute_group *sysfs_rx_queue_group;
	const struct rtnl_link_ops *rtnl_link_ops;
	unsigned int		gso_max_size;    /* GSO_MAX_SIZE = 8 * 65535 */
	unsigned int		tso_max_size;
	u16			gso_max_segs;
	u16			tso_max_segs;
	unsigned int		gso_ipv4_max_size;
#ifdef CONFIG_DCB
	const struct dcbnl_rtnl_ops *dcbnl_ops;
#endif
	s16			num_tc;
	struct netdev_tc_txq	tc_to_txq[TC_MAX_QUEUE];
	u8			prio_tc_map[TC_BITMASK + 1];
#if IS_ENABLED(CONFIG_FCOE)
	unsigned int		fcoe_ddp_xid;
#endif
#if IS_ENABLED(CONFIG_CGROUP_NET_PRIO)
	struct netprio_map __rcu *priomap;   /* 点名，控制器不在本章 */
#endif
	struct phy_device	*phydev;
	struct sfp_bus		*sfp_bus;
	struct lock_class_key	*qdisc_tx_busylock;
	bool			proto_down;
	unsigned		wol_enabled:1;
	unsigned		threaded:1;      /* threaded NAPI */
	struct list_head	net_notifier_list;
#if IS_ENABLED(CONFIG_MACSEC)
	const struct macsec_ops *macsec_ops;
#endif
	const struct udp_tunnel_nic_info *udp_tunnel_nic_info;
	struct udp_tunnel_nic	*udp_tunnel_nic;
	struct bpf_xdp_entity	xdp_state[__MAX_XDP_MODE];
	u8			dev_addr_shadow[MAX_ADDR_LEN];
	netdevice_tracker	linkwatch_dev_tracker;
	netdevice_tracker	watchdog_dev_tracker;
	netdevice_tracker	dev_registered_tracker;
	struct rtnl_hw_stats64	*offload_xstats_l3;
	struct devlink_port	*devlink_port;
};
```

驱动私有数据用 `netdev_priv(dev)`，紧挨在结构体后面。virtio-net 的 `virtnet_info` 就是这样挂上的。

发送队列 `struct netdev_queue`（`:623`）里和本章有关的成员：`qdisc`、`qdisc_sleeping`、`sb_dev`，以及 `CONFIG_BQL` 下的 `struct dql dql`（`:663`）。BQL 的记账在 [06](./06-tx-path-and-qdisc.md)。XPS 的 `xps_maps` 怎样选下标，在 [15](./15-mq-rps-xps-affinity.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
