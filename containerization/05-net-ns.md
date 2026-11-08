# 5. 网络命名空间

**结论**：`CLONE_NEWNET` 做出一份新的 `struct net`。这份结构里嵌着这张网的设备表、IPv4/IPv6、以及打开了 netfilter 时的 `netns_nf`。环回设备 `lo` 由 pernet 初始化注册进来。把一块已经存在的网卡移进这个 ns，走 `__dev_change_net_namespace`。veth 是一对互相指向的 `net_device`；bridge 和 CNI 怎么用这对设备做转发，在 [networking/11](../networking/11-netns-veth-bridge.md)。

> `struct net` 在 `include/net/net_namespace.h:61`。`copy_net_ns` 在 `net/core/net_namespace.c:495`。移动设备在 `net/core/dev.c:11321`。

---

## 5.1 `struct net`

字段按 6.6.160 的顺序。条件编译块在打开对应配置时才出现在结构里。

```c
/* include/net/net_namespace.h:61 */
struct net {
	/* --- 常被写的第一段 --- */
	refcount_t		passive;        /* 降到 0 才能真正释放 */
	spinlock_t		rules_mod_lock;
	atomic_t		dev_unreg_count;
	unsigned int		dev_base_seq;   /* rtnl_mutex 保护 */
	u32			ifindex;        /* 本 ns 的接口号分配 */
	spinlock_t		nsid_lock;
	atomic_t		fnhe_genid;

	struct list_head	list;           /* 挂在 net_namespace_list */
	struct list_head	exit_list;
	struct llist_node	defer_free_list;
	struct llist_node	cleanup_list;

#ifdef CONFIG_KEYS
	struct key_tag		*key_domain;
#endif
	struct user_namespace   *user_ns;       /* 拥有这个 net ns 的 user ns */
	struct ucounts		*ucounts;
	struct idr		netns_ids;      /* 别的 net ns 看本 ns 的 id */
	struct ns_common	ns;

	struct ref_tracker_dir  refcnt_tracker;
	struct ref_tracker_dir  notrefcnt_tracker;

	/* --- 设备 --- */
	struct list_head	dev_base_head;
	struct proc_dir_entry	*proc_net;
	struct proc_dir_entry	*proc_net_stat;

#ifdef CONFIG_SYSCTL
	struct ctl_table_set	sysctls;
#endif

	struct sock		*rtnl;          /* 本 ns 的 rtnetlink */
	struct sock		*genl_sock;
	struct uevent_sock	*uevent_sock;

	struct hlist_head	*dev_name_head;
	struct hlist_head	*dev_index_head;
	struct xarray		dev_by_index;
	struct raw_notifier_head netdev_chain;

	u32			hash_mix;
	struct net_device	*loopback_dev;  /* lo */

	/* --- 协议族，每份 net 各有一份 --- */
	struct list_head	rules_ops;
	struct netns_core	core;
	struct netns_mib	mib;
	struct netns_packet	packet;
#if IS_ENABLED(CONFIG_UNIX)
	struct netns_unix	unx;
#endif
	struct netns_nexthop	nexthop;
	struct netns_ipv4	ipv4;           /* 路由、fib 在这里 */
#if IS_ENABLED(CONFIG_IPV6)
	struct netns_ipv6	ipv6;
#endif
#if IS_ENABLED(CONFIG_IEEE802154_6LOWPAN)
	struct netns_ieee802154_lowpan	ieee802154_lowpan;
#endif
#if defined(CONFIG_IP_SCTP) || defined(CONFIG_IP_SCTP_MODULE)
	struct netns_sctp	sctp;
#endif
#ifdef CONFIG_NETFILTER
	struct netns_nf		nf;             /* iptables/nft 的每 ns 状态 */
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	struct netns_ct		ct;
#endif
#if defined(CONFIG_NF_TABLES) || defined(CONFIG_NF_TABLES_MODULE)
	struct netns_nftables	nft;
#endif
#if IS_ENABLED(CONFIG_NF_FLOW_TABLE)
	struct netns_ft		ft;
#endif
#endif
#ifdef CONFIG_WEXT_CORE
	struct sk_buff_head	wext_nlevents;
#endif
	struct net_generic __rcu	*gen;
	struct netns_bpf	bpf;

#ifdef CONFIG_XFRM
	struct netns_xfrm	xfrm;
#endif
	u64			net_cookie;

#if IS_ENABLED(CONFIG_IP_VS)
	struct netns_ipvs	*ipvs;
#endif
#if IS_ENABLED(CONFIG_MPLS)
	struct netns_mpls	mpls;
#endif
#if IS_ENABLED(CONFIG_CAN)
	struct netns_can	can;
#endif
#ifdef CONFIG_XDP_SOCKETS
	struct netns_xdp	xdp;
#endif
#if IS_ENABLED(CONFIG_MCTP)
	struct netns_mctp	mctp;
#endif
#if IS_ENABLED(CONFIG_CRYPTO_USER)
	struct sock		*crypto_nlsk;
#endif
	struct sock		*diag_nlsk;
#if IS_ENABLED(CONFIG_SMC)
	struct netns_smc	smc;
#endif
} __randomize_layout;
```

套接字创建时记下 `sock_net`。之后查路由读的是这份 `ipv4`，查连接跟踪读的是这份 `ct`。`iptables` / `nft` 的规则表跟着 `netns_nf` 走，所以容器里的防火墙规则看不见宿主机那份。规则怎么匹配包，是 netfilter 的事，本章停在「状态在 `struct net` 里」。

`/proc/<pid>/net` 是这份 `proc_net`。`ip netns` 一类工具通过 nsfs 的 net 链接进入，看到的就是这个 proc。

---

## 5.2 创建

`copy_net_ns`（`net/core/net_namespace.c:495`）：没有 `CLONE_NEWNET` 就 `get_net` 共享。有的话 `inc_net_namespaces`（ucount，失败 `-ENOSPC`），`net_alloc`，`preinit_net`，然后在 `pernet_ops_rwsem` 下调用 `setup_net`（`:320`）。

`setup_net` 把 `user_ns`、`net_cookie`、`netns_ids` 填上，再沿着 `pernet_list` 调每个 `pernet_operations` 的 init（`:342`）。环回设备是其中一步：`loopback_net_init`（`drivers/net/loopback.c:209`）`alloc_netdev(..., "lo", ...)`，`dev_net_set(dev, net)`，然后 `net->loopback_dev = dev`（`:225`）。新 net ns 里一开始就有 `lo`，没有宿主机的 eth0。

失败时 `setup_net` 按相反顺序跑已经成功的 pernet exit。`copy_net_ns` 把新 ns 的引用交回 `create_new_namespaces`。

ops 在 `net/core/net_namespace.c:1500` 的 `netns_operations`。`/proc/<pid>/ns/net` 的 `type` 是 `CLONE_NEWNET`。

---

## 5.3 设备怎么进这个 ns

一块 `net_device` 的 net ns 由 `dev_net()` 读。换 ns 的内核入口是 `__dev_change_net_namespace`（`net/core/dev.c:11321`）。包装 `dev_change_net_namespace`（`include/linux/netdevice.h:4037`）把新 ifindex 传 0。rtnetlink 的 `IFLA_NET_NS_PID` / `IFLA_NET_NS_FD` 路径调到它（`net/core/rtnetlink.c:2827` 的 `__dev_change_net_namespace`，以及 `:3546` 的 `dev_change_net_namespace`）。

移动时设备会从旧 ns 的 `dev_base_head` 摘下，在新 ns 里重新注册，ifindex 按新 ns 的计数重分配（调用者也可以指定）。名字冲突由 `pat` 参数改名。这一步要 rtnl，并且调用者在两边的 user ns 上具备相应能力。物理网卡、veth 的一端、macvlan 都走这条。

---

## 5.4 veth、bridge、CNI：只点到

`drivers/net/veth.c` 的 `veth_newlink`（`:1824`）先 `rtnl_create_link` 做出对端，再 `register_netdevice`，两边的私有数据用 `rcu_assign_pointer(priv->peer, ...)` 互指（`:1928` 与 `:1934`）。对端可以在创建时就放进另一个 net ns（rtnl 属性里带 ns fd）。一端发包，另一端在自己的 `net_device` 上收。队列、XDP、GSO 的细节不在本章。

bridge 是另一块 `net_device`，加上转发库。它把多个端口收进同一个二层域。端口可以是 veth 在宿主机那一端。转发数据库、vlan、stp 在 [networking/11](../networking/11-netns-veth-bridge.md)。

CNI 是用户态约定：运行时建一个 net ns，插件建 veth、把一端移进去、在里面配地址和路由，有的插件再把另一端插进 bridge。插件进程在内核里留下的就是 veth 和 bridge，见 [networking/11](../networking/11-netns-veth-bridge.md)。runc 在 `initProcess.start` 里调用 `createNetworkInterfaces`（见 [09](./09-runc-create-path.md)），把已经准备好的接口送进容器的 net ns。

容器里看到的路由是 `net->ipv4` 里那份 fib。宿主机的默认路由不会跟着 `copy_net_ns` 拷贝过去；地址和路由是用户态在新 ns 里重新配的。

---

## 5.5 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/net/net_namespace.h` | `struct net` | 61 | 一个网络命名空间 |
| `net/core/net_namespace.c` | `copy_net_ns` | 495 | `CLONE_NEWNET` |
| `net/core/net_namespace.c` | `setup_net` | 320 | 跑 pernet init |
| `drivers/net/loopback.c` | `loopback_net_init` | 209 | 注册 `lo` |
| `net/core/dev.c` | `__dev_change_net_namespace` | 11321 | 把设备移到另一个 net |
| `net/core/rtnetlink.c` | （调用点） | 2827 / 3546 | `IFLA_NET_NS_*` 进到上面 |
| `drivers/net/veth.c` | `veth_newlink` | 1824 | 建一对 veth |
| `net/core/net_namespace.c` | `netns_operations` | 1500 | proc ns ops |

---

**导航**：[← pid / user / uts / ipc / time](./04-pid-user-uts-ipc-time-ns.md) · [目录](./README.md) · [容器里的 cgroup →](./06-cgroup-in-containers.md)
