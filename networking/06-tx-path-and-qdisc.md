# 6. 发包与 qdisc

**结论**：`__dev_queue_xmit` 选定 `netdev_queue` 之后，有 `enqueue` 的 qdisc 先入队再 `sch_direct_xmit`；没有队列的设备直接 `dev_hard_start_xmit`。两者最后都调用 `ndo_start_xmit`。XPS 只决定队列下标，规则在 [15](./15-mq-rps-xps-affinity.md)。

> `dev_queue_xmit` 是内联，`include/linux/netdevice.h:3172`，转到 `__dev_queue_xmit`（`net/core/dev.c:4416`）。`Qdisc` 在 `include/net/sch_generic.h:72`。`sch_direct_xmit` 在 `net/sched/sch_generic.c:317`。

---

## 6.1 `__dev_queue_xmit`

```text
__dev_queue_xmit(skb, sb_dev)          net/core/dev.c:4416
  skb_reset_mac_header
  带 SKBTX_SCHED_TSTAMP：__skb_tstamp_tx(..., SCM_TSTAMP_SCHED)
  qdisc_pkt_len_init                 失败则 kfree_skb_reason
  rcu_read_lock_bh
  CONFIG_NET_EGRESS 时：
    nf_hook_egress                   见 12
    sch_handle_egress                tc egress，tcx / cls_bpf 见 ebpf/08
  IFF_XMIT_DST_RELEASE：skb_dst_drop，否则 skb_dst_force
  txq = netdev_core_pick_tx          内部 netdev_pick_tx（:4340）
  q = txq->qdisc
  trace_net_dev_queue
  若 q->enqueue：
    __dev_xmit_skb                   :3894
  否则（noqueue，loopback、veth 一类）：
    validate_xmit_skb
    HARD_TX_LOCK
    dev_hard_start_xmit              :3677
    HARD_TX_UNLOCK
```

`netdev_pick_tx`（`:4340`）在 XPS 静态键打开时调用 `get_xps_queue`（`:4284`）。没选中就用驱动的 `ndo_select_queue`，再没有就按流哈希映射到 `real_num_tx_queues`。`skb->queue_mapping` 记下结果，驱动在 `ndo_start_xmit` 里用 `skb_get_queue_mapping`。

`dev_hard_start_xmit`（`:3677`）沿 `skb->next` 走 GSO 切开的链表，每个 skb 调 `xmit_one`（`:3660`）。`xmit_one` 打 `trace_net_dev_start_xmit`，再 `__netdev_start_xmit` → `ndo_start_xmit`（`netdevice.h:5003`）。

返回值由 qdisc 消化。`NETDEV_TX_OK` 表示驱动收下了。`NETDEV_TX_BUSY` 表示队列停了，skb 回到 qdisc。

---

## 6.2 `struct Qdisc`

```c
/* include/net/sch_generic.h:72 */
struct Qdisc {
	int		(*enqueue)(struct sk_buff *skb, struct Qdisc *sch,
				   struct sk_buff **to_free);
	struct sk_buff *(*dequeue)(struct Qdisc *sch);
	unsigned int	flags;          /* TCQ_F_* */
	u32		limit;
	const struct Qdisc_ops *ops;
	struct qdisc_size_table __rcu *stab;
	struct hlist_node hash;
	u32		handle;
	u32		parent;
	int		depth;
	struct netdev_queue *dev_queue;
	struct net_rate_estimator __rcu *rate_est;
	struct gnet_stats_basic_sync __percpu *cpu_bstats;
	struct gnet_stats_queue __percpu *cpu_qstats;
	int		pad;
	refcount_t	refcnt;

	struct sk_buff_head	gso_skb ____cacheline_aligned_in_smp;
	struct qdisc_skb_head	q;          /* 普通包 */
	struct gnet_stats_basic_sync bstats;
	struct gnet_stats_queue	qstats;
	int		owner;
	unsigned long	state;
	unsigned long	state2;
	struct Qdisc	*next_sched;        /* 挂到 softnet_data.output_queue */
	struct sk_buff_head skb_bad_txq;
	spinlock_t	busylock ____cacheline_aligned_in_smp;
	spinlock_t	seqlock;
	struct rcu_head	rcu;
	netdevice_tracker dev_tracker;
	struct lock_class_key root_lock_key;
	long		privdata[] ____cacheline_aligned;
};
```

常用 `TCQ_F_*`（`:78`）：`TCQ_F_BUILTIN` 内建 qdisc 不计入引用，`TCQ_F_CAN_BYPASS` 允许绕过，`TCQ_F_NOLOCK` 不拿根锁，`TCQ_F_ONETXQUEUE` 表示 dequeue 出来的 skb 都属于 `dev_queue`。

默认操作表：

```c
/* net/sched/sch_generic.c:35 */
const struct Qdisc_ops *default_qdisc_ops = &pfifo_fast_ops;
```

`attach_one_default_qdisc`（`:1186`）里，设备带 `IFF_NO_QUEUE` 时改用 `noqueue_qdisc_ops`（`:1193`），CAN 设备强制 `pfifo_fast`。`pfifo_fast_enqueue` 在 `:755`，三个 band，按 `skb->priority` 的低位映射。`veth` 在 `drivers/net/veth.c:1754` 置 `IFF_NO_QUEUE`，所以一对 veth 的 qdisc 是 `noqueue`。本机 6.12 上 `ip netns` 里的 veth 显示 `qdisc noqueue`，`enp0s3` 显示 `pfifo_fast`，和这张表一致；数字是 sampled on 6.12。

```text
__dev_xmit_skb
  qdisc->enqueue
  若可以立刻发：
    qdisc_restart                     sch_generic.c:391
      dequeue
      sch_direct_xmit                 :317
        放开 qdisc 锁
        validate_xmit_skb_list        GSO、校验和
        HARD_TX_LOCK
        dev_hard_start_xmit
        HARD_TX_UNLOCK
        NETDEV_TX_BUSY：dev_requeue_skb
  队列被停：把 qdisc 挂到 softnet_data.output_queue
    NET_TX_SOFTIRQ 的 net_tx_action   dev.c:5301 再跑
```

`NET_TX_SOFTIRQ` 的注册和 `NET_RX_SOFTIRQ` 在同一处（`dev.c:11956`）。softirq 优先级是 TX 比 RX 更早，见 [interrupts/08](../interrupts/08-softirq-tasklet.md)。

---

## 6.3 BQL

字节队列限制记在 `netdev_queue.dql` 上（`netdevice.h:663`，`CONFIG_BQL`）。驱动在 `ndo_start_xmit` 里报告提交了多少字节：

```c
/* include/linux/netdevice.h:3544 */
netdev_tx_sent_queue(dev_queue, bytes)
{
	dql_queued(&dev_queue->dql, bytes);
	若 dql_avail < 0：
		set_bit(__QUEUE_STATE_STACK_XOFF)
		再看一次，有空间就清掉
}
```

完成时 `netdev_tx_completed_queue` 增加 `dql` 的完成计数，空间回来就清 `STACK_XOFF` 并唤醒 qdisc。队列停着的时候 `sch_direct_xmit` 看到 `netif_xmit_frozen_or_stopped`，skb 回到 qdisc，不调用驱动。

`dql_init` 在队列创建时调用（`dev.c:10179`）。sysfs 在 `/sys/class/net/<dev>/queues/tx-<n>/byte_queue_limits/`。本机 `enp0s3` 的 `tx-0` 在 6.12 上读到 `limit` 约 69107、`inflight` 为 0，这是当时的计数，不是 6.6 的常量。

---

## 6.4 XPS 挂钩

`get_xps_queue`（`dev.c:4284`）在 `xps_needed` 静态键为真时才工作。它先看 `XPS_RXQS` 这张按接收队列索引的表，再用 CPU 表 `xps_maps[XPS_CPUS]`。命中的下标写回 `queue_index`，由 `netdev_pick_tx` 采用。

map 的形状：

```c
/* include/linux/netdevice.h:805 */
struct xps_map {
	unsigned int	len;
	unsigned int	alloc_len;
	struct rcu_head	rcu;
	u16		queues[];   /* 这个 CPU 可以选的 TX 队列 */
};
/* :826 */
struct xps_dev_maps {
	struct rcu_head	rcu;
	unsigned int	nr_ids;
	s16		num_tc;
	struct xps_map __rcu *attr_map[]; /* 按下标是 CPU 或 RX 队列 */
};
```

单队列设备读 `xps_cpus` 会得到 `-ENOENT`：`xps_cpus_show`（`net/core/net-sysfs.c:1497`）在 `!netif_is_multiqueue(dev)` 时直接返回（`:1503`）。本机 `enp0s3` 只有一个 combined 队列，`cat xps_cpus` 就是这个结果。多队列怎么配，在 [15](./15-mq-rps-xps-affinity.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
