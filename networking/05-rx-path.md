# 5. 收包路径

**结论**：NAPI `poll` 从设备取出 skb 后交给 `napi_gro_receive`；合并不了的包进入 `__netif_receive_skb_core`，先过 generic XDP 和 ingress，再跑 `rx_handler`，最后按 `skb->protocol` 找到 `packet_type->func`。IPv4 的 func 是 `ip_rcv`。

> 核心函数在 `net/core/dev.c`。GRO 在 `net/core/gro.c`。IPv4 入口在 `net/ipv4/ip_input.c`。XDP 的 `xdp_buff`、动作码和 generic/driver 模式在 [ebpf/07](../ebpf/07-xdp.md)，这里只留下调用点。

---

## 5.1 从 poll 到 GRO

驱动的 `poll` 在 `net_rx_action` 里被调用，见 [04](./04-netdev-and-napi.md)。virtio-net 的实现是 `virtnet_poll`（[13](./13-virtio-net-guest.md)）。没有硬件 NAPI 的路径走 `netif_rx`（`dev.c:5285`）或 `__netif_rx`（`:5253`），它们把 skb 放进 backlog，再被同一套 `net_rx_action` 收走。

```text
napi_gro_receive                 net/core/gro.c:635
  skb_mark_napi_id               把 napi->napi_id 写进 skb
  dev_gro_receive                :472
    同流且可合并：挂进 napi->gro_hash，返回 GRO_MERGED / GRO_HELD
    否则 GRO_NORMAL
  napi_skb_finish
    GRO_NORMAL：netif_receive_skb
```

`dev_gro_receive` 按流哈希放进 `napi_struct.gro_hash`。刷出条件包括流结束、`gro_flush_timeout`，以及预算用尽。合成后的 skb 仍走下面的 `netif_receive_skb`。

---

## 5.2 `__netif_receive_skb_core`

`netif_receive_skb`（`dev.c:5963`）是导出给驱动的入口，里面会处理 RPS（`get_rps_cpu`，见 [04](./04-netdev-and-napi.md) 和 [15](./15-mq-rps-xps-affinity.md)），然后进核心函数。

```c
/* net/core/dev.c:5498 的骨架，省略统计 */
__netif_receive_skb_core(pskb, pfmemalloc, ppt_prev)
{
	trace_netif_receive_skb(skb);
	skb_reset_network_header / transport / mac_len;
	skb->skb_iif = skb->dev->ifindex;
	__this_cpu_inc(softnet_data.processed);

	/* generic XDP：设备没有原生 XDP 时走这里。程序见 ebpf/07 */
	if (generic_xdp_needed)
		do_xdp_generic(skb->dev->xdp_prog, skb);   /* :5531，函数 :5183 */
		/* 返回不是 XDP_PASS：丢弃并返回 */

	vlan 外层标签：skb_vlan_untag

	ptype_all 以及 dev->ptype_all          /* tcpdump 抓到的就是这里 */
	sch_handle_ingress                     /* CONFIG_NET_INGRESS，:5570 */
	nf_ingress                             /* 见 12 */

	若 skb 带 vlan tag：vlan_do_receive，成功则 another_round

	rx_handler = skb->dev->rx_handler;     /* :5598 */
	switch (rx_handler(&skb)) {
	RX_HANDLER_CONSUMED:  返回，桥自己处理完了
	RX_HANDLER_ANOTHER:   换了 skb->dev，回到 another_round
	RX_HANDLER_EXACT:     只投递精确协议
	RX_HANDLER_PASS:      继续
	}

	按 skb->protocol 查 ptype_base[ntohs(type) & PTYPE_HASH_MASK]  /* :5661 */
	再查 orig_dev->ptype_specific
	最后 pt_prev->func(...)                 /* deliver_skb，:2275 */
}
```

`deliver_skb`（`:2275`）对还要继续投递的 skb 先 `skb_clone`，最后一个 `packet_type` 直接拿原 skb，调用 `pt->func`。IPv4 登记：

```c
/* net/ipv4/af_inet.c:1950，inet_init 里 dev_add_pack（:2065） */
static struct packet_type ip_packet_type = {
	.type = cpu_to_be16(ETH_P_IP),
	.func = ip_rcv,
	.list_func = ip_list_rcv,
};
```

`rx_handler` 同一块设备只能有一个。`netdev_rx_handler_register`（`dev.c:5421`）在忙时返回错误。bridge 在 `br_add_if` 里注册 `br_handle_frame`（`net/bridge/br_if.c:630`，handler 在 `br_input.c:333`）。bond、macvlan、openvswitch 用同一张钩子。返回值枚举的注释在 `netdevice.h:433` 附近。

generic XDP 的程序跑在 `bpf_prog_run_generic_xdp`（`dev.c:5011`）。动作码、`do_xdp_generic` 和 `ndo_bpf` 的安装在 [ebpf/07](../ebpf/07-xdp.md)。原生 XDP 在驱动 poll 里更早发生，virtio-net 的接收队列上有 `rq->xdp_prog`（见 [13](./13-virtio-net-guest.md)）。

---

## 5.3 `ip_rcv` 到传输层

```text
ip_rcv                              net/ipv4/ip_input.c:560
  ip_rcv_core                       基本头检查，失败 NET_RX_DROP
  NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING, ..., ip_rcv_finish)

ip_rcv_finish                       :435
  ip_rcv_finish_core                :317     查路由，skb_dst_set
  dst_input(skb)                    :449     调 dst->input

本地：
  ip_local_deliver                  :242
    分片则 ip_defrag
    NF_HOOK(..., NF_INET_LOCAL_IN, ..., ip_local_deliver_finish)
      ip_protocol_deliver_rcu       调用在 :233，函数在 :187

转发：
  ip_forward                        net/ipv4/ip_forward.c:85
    NF_INET_FORWARD 之后 ip_output
```

`ip_protocol_deliver_rcu` 用的协议表在 `af_inet.c`：

| 协议 | 结构 | 行 | handler |
|------|------|----|---------|
| TCP | `tcp_protocol` | 1755 | `tcp_v4_rcv` |
| UDP | `udp_protocol` | 1762 | `udp_rcv` |
| ICMP | `icmp_protocol` | 1768 | `icmp_rcv` |

`tcp_v4_rcv`（`net/ipv4/tcp_ipv4.c:2005`）查连接之后调 `tcp_v4_do_rcv`。已建立连接走 `tcp_rcv_established`（`tcp_input.c:6046`）。UDP 走 `__udp4_lib_rcv`（`udp.c:2448`）。两条路径在 [08](./08-tcp.md) 和 [09](./09-udp.md)。

路由结果放在 `skb->_skb_refdst`，类型是 `struct rtable`，里面嵌着 `dst_entry`。字段和邻居在 [07](./07-ipv4-route-neigh.md)。

---

## 5.4 和套接字队列的交接

传输层找到 `sock` 之后，若用户没有拿着 `sk_lock`，skb 进 `sk_receive_queue`，`sk_data_ready` 唤醒 `sk_wq`。若锁被占着，skb 进 `sk_backlog`，等进程释放锁再 `backlog_rcv`。队列字段在 [03](./03-socket-and-sock.md)。

`tcp_v4_do_rcv` 在已建立状态还会 `sk_mark_napi_id`（`tcp_ipv4.c:1732`），把收包 NAPI 记到套接字上。busy poll 用这个号直接调 `napi->poll`，不在这里展开循环；epoll 侧只在 [ipc/03](../ipc/03-epoll-objects.md) 点到 `napi_id`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
