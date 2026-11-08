# 13. virtio-net 客户机

**结论**：客户机里的 virtio-net 把每个队列对做成一个 `receive_queue` 加一个 `send_queue`。收包在 `virtnet_poll` 里取描述符、拼 skb，交给 `napi_gro_receive`；发包的 `ndo_start_xmit` 是 `start_xmit`，把 skb 和 virtio 头排进发送 virtqueue 再 kick。

> 驱动在 `drivers/net/virtio_net.c`。特性位和头在 `include/uapi/linux/virtio_net.h`。`virtnet_netdev` 在 `:4039`，probe 在 `:4500`。和 [04](./04-netdev-and-napi.md)、[05](./05-rx-path.md) 接在 NAPI 与 `ndo_start_xmit` 上。

virtqueue 是客户机和宿主机共用的环。客户机写描述符，kick 通知宿主机；宿主机写 used ring，再通过 eventfd 注入中断。宿主机那一半在 [14](./14-virtio-net-host-vhost.md)。

---

## 13.1 特性位

`include/uapi/linux/virtio_net.h` 里和数据面直接有关的位：

| 宏 | 位 | 作用 |
|----|----|------|
| `VIRTIO_NET_F_CSUM` | 0 | 宿主机处理部分校验和 |
| `VIRTIO_NET_F_GUEST_CSUM` | 1 | 客户机处理收到的部分校验和 |
| `VIRTIO_NET_F_GUEST_TSO4` / `TSO6` | 7 / 8 | 客户机能收 TSO |
| `VIRTIO_NET_F_HOST_TSO4` / `TSO6` | 11 / 12 | 宿主机能收 TSO |
| `VIRTIO_NET_F_MRG_RXBUF` | 15 | 一个包可以跨多个接收缓冲 |
| `VIRTIO_NET_F_CTRL_VQ` | 17 | 有控制队列 |
| `VIRTIO_NET_F_MQ` | 22 | 多队列对，用控制命令设置对数 |
| `VIRTIO_NET_F_HASH_REPORT` | 57 | 头里带哈希报告 |
| `VIRTIO_NET_F_RSS` | 60 | RSS |

同文件还有 UFO、USO、状态、MAC、MTU、合并中断等。驱动在 `virtnet_probe` 里用 `virtio_has_feature` 决定 `mergeable_rx_bufs`、`has_cvq`、`big_packets`。

传统头：

```c
/* include/uapi/linux/virtio_net.h:188 */
struct virtio_net_hdr {
	__u8 flags;             /* VIRTIO_NET_HDR_F_* */
	__u8 gso_type;          /* VIRTIO_NET_HDR_GSO_* */
	__virtio16 hdr_len;     /* 以太网 + IP + TCP/UDP */
	__virtio16 gso_size;
	__virtio16 csum_start;
	__virtio16 csum_offset;
};

/* :201，协商了 MRG_RXBUF 就用这个 */
struct virtio_net_hdr_mrg_rxbuf {
	struct virtio_net_hdr hdr;
	__virtio16 num_buffers;
};
```

`csum_start` / `csum_offset` 和 skb 的 `CHECKSUM_PARTIAL` 是同一件事，单位不同：virtio 头相对包起点，skb 的 `csum_start` 相对 `head`。`gso_size` 对应 `skb_shared_info.gso_size`。

控制队列设置队列数（`:304`）：

```c
struct virtio_net_ctrl_mq {
	__virtio16 virtqueue_pairs;
};
/* VIRTIO_NET_CTRL_MQ 是 4，VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET 是 0 */
```

---

## 13.2 队列和 `virtnet_info`

virtqueue 编号（`virtio_net.c:355` 的注释）：`0` 是 rx0，`1` 是 tx0，然后 rx1、tx1，控制队列在最后。换算函数是 `vq2rxq`（`:368`）、`rxq2vq`（`:373`）、`vq2txq`（`:358`）、`txq2vq`（`:363`）。

```c
/* drivers/net/virtio_net.c:144 */
struct send_queue {
	struct virtqueue *vq;
	struct scatterlist sg[MAX_SKB_FRAGS + 2]; /* 头 + 线性 + 片段 */
	char name[16];                            /* output.$index */
	struct virtnet_sq_stats stats;
	struct virtnet_interrupt_coalesce intr_coal;
	struct napi_struct napi;
	bool reset;
};

/* :165 */
struct receive_queue {
	struct virtqueue *vq;
	struct napi_struct napi;
	struct bpf_prog __rcu *xdp_prog;          /* 原生 XDP，程序见 ebpf 章 */
	struct virtnet_rq_stats stats;
	struct virtnet_interrupt_coalesce intr_coal;
	struct page *pages;
	struct ewma_pkt_len mrg_avg_pkt_len;
	struct page_frag alloc_frag;
	struct scatterlist sg[MAX_SKB_FRAGS + 2];
	unsigned int min_buf_len;
	char name[16];                            /* input.$index */
	struct xdp_rxq_info xdp_rxq;
	struct virtnet_rq_dma *last_dma;
	bool do_dma;
};

/* :238 */
struct virtnet_info {
	struct virtio_device *vdev;
	struct virtqueue *cvq;
	struct net_device *dev;
	struct send_queue *sq;
	struct receive_queue *rq;
	unsigned int status;
	u16 max_queue_pairs;       /* 设备允许的上限 */
	u16 curr_queue_pairs;      /* 当前启用的对数 */
	u16 xdp_queue_pairs;
	bool xdp_enabled;
	bool big_packets;
	unsigned int big_packets_num_skbfrags;
	bool mergeable_rx_bufs;
	bool has_rss;
	bool has_rss_hash_report;
	u8 rss_key_size;
	u16 rss_indir_table_size;
	u32 rss_hash_types_supported;
	u32 rss_hash_types_saved;
	bool has_cvq;
	bool any_header_sg;
	u8 hdr_len;
	struct delayed_work refill;
	bool refill_enabled;
	spinlock_t refill_lock;
	struct work_struct config_work;
	bool affinity_hint_set;
	struct hlist_node node;
	struct hlist_node node_dead;
	struct control_buf *ctrl;
	u8 duplex;
	u32 speed;
	struct virtnet_interrupt_coalesce intr_coal_tx;
	struct virtnet_interrupt_coalesce intr_coal_rx;
	unsigned long guest_offloads;
	unsigned long guest_offloads_capable;
	struct failover *failover;
};
```

`virtnet_probe` 在 CPU 数不少于设备上限时把 `curr_queue_pairs` 设成上限，否则设成 `num_online_cpus()`（`:4691`），再 `netif_set_real_num_tx_queues` / `rx`（`:4709`）。真正告诉设备只用其中几对，要等有控制队列且协商了 `VIRTIO_NET_F_MQ`：`_virtnet_set_queues`（`:2680`）发 `VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET`，成功才写 `curr_queue_pairs`（`:2697`）。

每对队列各挂一个 NAPI。`init` 路径上 `netif_napi_add_weight(..., virtnet_poll, napi_weight)`（`:4318`），发送侧 `virtnet_poll_tx`，权重在没开 tx NAPI 时是 0（`:4320`）。

统计名在 `:107` 和 `:116`：发送是 `packets`、`bytes`、`xdp_tx`、`xdp_tx_drops`、`kicks`、`tx_timeouts`；接收是 `packets`、`bytes`、`drops`、`xdp_packets`、`xdp_tx`、`xdp_redirects`、`xdp_drops`、`kicks`。本机 6.12 上 `ethtool -S` 打出来的字符串带 `rx_` / `tx_` 前缀，和这张 6.6 表不是同一份，见 [16](./16-observability.md)。

---

## 13.3 收包

中断里不处理包。`virtqueue_napi_schedule`（`:436`）在 `napi_schedule_prep` 成功后 `virtqueue_disable_cb`，再 `__napi_schedule`。这和 [04](./04-netdev-and-napi.md) 的 `____napi_schedule` 接上，softirq 里才会跑 `virtnet_poll`。

```text
virtnet_poll                         :2222
  virtqueue_disable_cb
  virtnet_poll_cleantx               :2194  顺手收发送队列里用完的描述符
  virtnet_receive                    :2144
    循环到 budget：virtnet_rq_get_buf
    receive_buf                      :1811
      mergeable：receive_mergeable   :1664
      napi_gro_receive(&rq->napi)    :1874
  received < budget：virtqueue_napi_complete  :445
    napi_complete_done 之后若环里又有 buffer，立刻再 schedule
  空了就 try_fill_recv               :2048  把新的接收页排回 virtqueue
```

`napi_gro_receive` 之后的路径就是 [05](./05-rx-path.md)：GRO、`netif_receive_skb`、`ip_rcv`。合并接收时一个逻辑包占多个描述符，`num_buffers` 告诉驱动要收几块。填不满接收环时 `schedule_delayed_work(&vi->refill)`。

`rq->xdp_prog` 非空时在组 skb 之前跑 XDP。动作和 generic XDP 同一组名字，加载路径留给 ebpf 章。

---

## 13.4 发包

```c
/* virtio_net.c:4039 */
static const struct net_device_ops virtnet_netdev = {
	.ndo_open		= virtnet_open,          /* :2290 */
	.ndo_stop		= virtnet_close,
	.ndo_start_xmit		= start_xmit,            /* :2412 */
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= virtnet_set_mac_address,
	.ndo_set_rx_mode	= virtnet_set_rx_mode,
	.ndo_get_stats64	= virtnet_stats,
	.ndo_vlan_rx_add_vid	= virtnet_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= virtnet_vlan_rx_kill_vid,
	.ndo_bpf		= virtnet_xdp,
	.ndo_xdp_xmit		= virtnet_xdp_xmit,
	.ndo_features_check	= passthru_features_check,
	.ndo_get_phys_port_name	= virtnet_get_phys_port_name,
	.ndo_set_features	= virtnet_set_features,
	.ndo_tx_timeout		= virtnet_tx_timeout,
};
```

```text
start_xmit                           :2412
  qnum = skb_get_queue_mapping(skb)
  sq = vi->sq[qnum]
  free_old_xmit_skbs                 回收已经用过的发送描述符
  skb_tx_timestamp
  xmit_skb                           :2365
    把 virtio_net_hdr 和 skb 的线性区、片段排进 sq->sg
    virtqueue_add_sgs
  失败：统计 tx_fifo_errors，kfree skb，仍返回 NETDEV_TX_OK
  kick 或队列已停：virtqueue_kick_prepare && virtqueue_notify
    sq->stats.kicks++                :2461
```

`queue_mapping` 来自 [06](./06-tx-path-and-qdisc.md) 的 `netdev_pick_tx`。队列对少于流的哈希时，多条流挤在同一个 `send_queue` 上，kick 也挤在同一条 virtqueue。多队列和 CPU 的关系在 [15](./15-mq-rps-xps-affinity.md)。

发送完成有两条路。开了 tx NAPI 时，`skb_xmit_done`（`:459`）再 `virtqueue_napi_schedule`，由 `virtnet_poll_tx`（`:2319`）回收。没开时直接 `netif_wake_subqueue`。`virtnet_poll` 还会在收包预算里调用 `virtnet_poll_cleantx`，避免发送队列只靠自己的中断才能醒。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
