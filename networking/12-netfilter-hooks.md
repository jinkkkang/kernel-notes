# 12. netfilter 挂钩

**结论**：IPv4 收发在五个固定点调用 `NF_HOOK`。挂钩函数按 `priority` 排成链，返回 `NF_ACCEPT` 继续，`NF_DROP` 丢掉 skb。iptables 和 nftables 都是往这些点上登记 `nf_hook_ops`，本章不写规则语法。

> 挂钩号在 `include/uapi/linux/netfilter.h:42`。`nf_hook_ops` 在 `include/linux/netfilter.h:97`。慢路径 `nf_hook_slow` 在 `net/netfilter/core.c:619`。

---

## 12.1 五个点

`enum nf_inet_hooks`（`include/uapi/linux/netfilter.h:42`）按声明顺序从 0 编号：

| 值 | 名字 | 谁调用 |
|----|------|--------|
| 0 | `NF_INET_PRE_ROUTING` | `ip_rcv`（`ip_input.c:569`），路由之前 |
| 1 | `NF_INET_LOCAL_IN` | `ip_local_deliver`（`:254`），交给传输层之前 |
| 2 | `NF_INET_FORWARD` | `ip_forward`（`ip_forward.c:162`） |
| 3 | `NF_INET_LOCAL_OUT` | `__ip_local_out`（`ip_output.c:118`），`okfn` 是 `dst_output` |
| 4 | `NF_INET_POST_ROUTING` | `ip_output`（`:426`）里的 `NF_HOOK_COND`（`:420`），`okfn` 是 `ip_finish_output` |
| 5 | `NF_INET_NUMHOOKS` | 个数 |
| 5 | `NF_INET_INGRESS` | 与 `NUMHOOKS` 同值，给设备 ingress 用 |

设备级还有 `NF_NETDEV_INGRESS` / `NF_NETDEV_EGRESS`（同文件 `:52`）。`__netif_receive_skb_core` 里的 `nf_ingress` 和 `__dev_queue_xmit` 里的 `nf_hook_egress` 走的是设备上的链，见 [05](./05-rx-path.md) 和 [06](./06-tx-path-and-qdisc.md)。

判决值（`:10`）：`NF_DROP` 0，`NF_ACCEPT` 1，`NF_STOLEN` 2，`NF_QUEUE` 3，`NF_REPEAT` 4。`NF_STOP` 是 5，注释标明留给用户态 nf_queue 兼容。`NF_STOLEN` 表示钩子拿走了 skb，调用者不能再碰。

---

## 12.2 `nf_hook_ops`

```c
/* include/linux/netfilter.h:78 */
struct nf_hook_state {
	u8 hook;
	u8 pf;                       /* NFPROTO_IPV4 等 */
	struct net_device *in;
	struct net_device *out;
	struct sock *sk;
	struct net *net;
	int (*okfn)(struct net *, struct sock *, struct sk_buff *);
};

/* :97 */
struct nf_hook_ops {
	struct rcu_head		rcu;
	nf_hookfn		*hook;
	struct net_device	*dev;    /* 非空则只对这块设备生效 */
	void			*priv;
	u8			pf;
	enum nf_hook_ops_type	hook_ops_type:8; /* NF_TABLES 或 BPF */
	unsigned int		hooknum;
	int			priority; /* 小的先跑 */
};
```

`nf_register_net_hook`（`net/netfilter/core.c:557`）把 ops 插进这份 `struct net` 的对应链。`NF_HOOK` 宏在链为空时直接调用 `okfn`，否则 `nf_hook_slow`（`:619`）按优先级遍历。conntrack 用 `_nfct` 记在 skb 上，字段在 [02](./02-sk-buff.md) 的 `CONFIG_NF_CONNTRACK` 段。

`hook_ops_type` 里的 `NF_HOOK_OP_BPF`（`netfilter.h:94`）表示这条 ops 来自 BPF。字节码和 verifier 留给 ebpf 章。

iptables 的 filter 表挂在 `LOCAL_IN` / `FORWARD` / `LOCAL_OUT`，nat 表挂在 `PRE_ROUTING` / `POST_ROUTING` / `LOCAL_OUT`。nftables 用同一组 `hooknum`，可以在一份链里混用。选哪一个是用户态规则的事：内核只保证优先级和 `okfn` 的顺序。

每份 netns 有自己的钩子表，所以容器里的 iptables 看不到宿主机的规则。表跟着 `struct net` 走，这一点和 [containerization/05](../containerization/05-net-ns.md) 里的 `netns_nf` 是同一件事。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
