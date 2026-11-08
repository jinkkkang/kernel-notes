# 11. netns、veth 与 bridge

**结论**：每份网络命名空间有自己的 `struct net` 和设备表。veth 的 `ndo_start_xmit` 把 skb 交给对端 `net_device` 再 `__netif_rx`。bridge 不在发包函数里做转发，而是在端口的 `rx_handler` 里查 FDB。

> `struct net` 的全字段在 [containerization/05](../containerization/05-net-ns.md)，这里不重抄。`copy_net_ns` 和 `__dev_change_net_namespace` 也在那一章。veth 在 `drivers/net/veth.c`，bridge 收包在 `net/bridge/br_input.c`。

---

## 11.1 这一章用到的 `struct net`

设备挂在 `dev_base_head` 上，按名字和 ifindex 另有哈希。`loopback_dev` 是这份 ns 的 `lo`。IPv4 路由在 `net->ipv4`，邻居表跟着同一份 `net`。`rtnl` 是这份 ns 的 rtnetlink 套接字。

把已经存在的网卡移进别的 ns 走 `__dev_change_net_namespace`（`net/core/dev.c:11321`，containerization 章已对过这个行号）。ifindex 按新 ns 重新分配。veth 创建时可以在 rtnl 属性里直接指定对端的 netns fd，不必先建在当前 ns 再移动。

---

## 11.2 veth

```c
/* drivers/net/veth.c:72 */
struct veth_priv {
	struct net_device __rcu	*peer;
	atomic64_t		dropped;
	struct bpf_prog		*_xdp_prog;
	struct veth_rq		*rq;
	unsigned int		requested_headroom;
};

/* :59 */
struct veth_rq {
	struct napi_struct	xdp_napi;
	struct napi_struct __rcu *napi;
	struct net_device	*dev;
	struct bpf_prog __rcu	*xdp_prog;
	struct xdp_mem_info	xdp_mem;
	struct veth_rq_stats	stats;
	bool			rx_notify_masked;
	struct ptr_ring		xdp_ring;
	struct xdp_rxq_info	xdp_rxq;
	struct page_pool	*page_pool;
};
```

`veth_newlink`（`:1824`）建一对设备，`rcu_assign_pointer` 让两边的 `peer` 互指。`priv_flags` 带 `IFF_NO_QUEUE`（`:1754`），所以 qdisc 是 `noqueue`，见 [06](./06-tx-path-and-qdisc.md)。

```text
veth_xmit                            veth.c:347
  rcu 下读 peer
  按 skb 的 queue_mapping 选对端的 veth_rq
  对端开了 GRO 且包适合聚合：走对端 NAPI
  否则 veth_forward_skb               :320
    __dev_forward_skb                 改 skb->dev、pkt_type
    __netif_rx                        进对端 CPU 的 backlog
```

`__dev_forward_skb` 会做 `skb_scrub_packet` 一类清理，让对端协议栈把这帧当成从那块网卡收到的以太网帧。对端若在另一个 netns，`skb->dev` 的 `nd_net` 就是那个 ns，随后的 `ip_rcv` 查的是对端的路由表。

XDP 程序可以挂在 `veth_rq.xdp_prog` 上。动作码和 generic/driver 模式在 [ebpf/07](../ebpf/07-xdp.md)。

---

## 11.3 bridge

端口加入桥时 `netdev_rx_handler_register(dev, br_get_rx_handler(dev), p)`（`net/bridge/br_if.c:630`）。普通端口的 handler 是 `br_handle_frame`（`br_input.c:333`，选择函数 `:453`）。

```text
br_handle_frame                     br_input.c:333
  skb_share_check                   :345
  源 MAC 非法则丢
  链路本地地址按 group_fwd_mask 决定上交还是转发
  NF_BR_PRE_ROUTING                 :264 起，钩子空则直接往下
  br_handle_frame_finish            :76
    br_fdb_update                   学习源 MAC，调用点 :120 一带
    端口状态不是 FORWARDING：丢或只上交
    命中单播：从那个端口发出
    未命中或广播：洪泛到其他端口
    目的地是本机：br_pass_frame_up   :33，:208 附近返回
```

桥设备自己的 `ndo_start_xmit` 是 `br_dev_xmit`（`net/bridge/br_device.c:28`），本机协议栈从桥的 IP 发出去时走它，再按 FDB 从某个端口出去。端口收进来的帧不经过 `br_dev_xmit`，只经过 `rx_handler`。这就是「桥是一块 `net_device`，转发发生在端口的接收钩子上」。

`rx_handler` 的返回值在 [05](./05-rx-path.md)。`RX_HANDLER_CONSUMED` 表示桥已经把 skb 处理完，协议栈不再按原端口的 `protocol` 上交。要上交本机时，桥另做一份 skb，把 `dev` 改成桥设备，再 `__netif_receive_skb`。

FDB 项是 `net_bridge_fdb_entry`（`net/bridge/br_private.h`）。学习发生在 `br_handle_frame_finish` 看见源 MAC 的时候。老化、VLAN、STP 状态只影响这项能不能转发，本章不逐个展开。

---

## 11.4 CNI 在内核里留下什么

CNI 是用户态约定：运行时准备好 netns，插件用 rtnetlink 建 veth、把一端移进容器 ns、配地址和路由，有的插件再把宿主机那一端插进 bridge。内核侧就是上面三步，没有单独的「CNI 对象」。runc 把已经准备好的接口送进容器 ns 的位置在 [containerization/09](../containerization/09-runc-create-path.md)。

---

## 11.5 本机一对 veth

[demos/03-veth-ping.sh](./demos/03-veth-ping.sh) 用 `CAP_NET_ADMIN` 建两个 netns，一对 veth，`10.88.0.1/24` 与 `10.88.0.2/24`，从 A ping B。sampled on 6.12.113+：

```text
veth-a qdisc noqueue state UP
veth-b inet 10.88.0.2/24
64 bytes from 10.88.0.2: icmp_seq=1 ttl=127 time=0.064 ms
veth_ping=ok
```

`noqueue` 对应 `IFF_NO_QUEUE`。`ttl=127` 是经过一次转发后的常见结果：两端各在自己的 ns 里把对方当成直连邻居，ICMP 仍经过各自的 `ip_output`。MAC 地址和 rtt 每次创建都不同。没有 `CAP_NET_ADMIN` 时脚本打印 `veth_ping=skipped`，那就算 RECIPE，不编延迟。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
