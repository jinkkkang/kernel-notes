# 17. 性能场景

**结论**：先分清时间花在 `NET_RX_SOFTIRQ`、qdisc、TCP 重传，还是 virtio 的单队列和 vhost 线程上。每一条都是「现象 → 看什么 → 改什么」。要改云上队列和绑核的步骤保持 RECIPE，实测栏留空。

> 机制在 [04](./04-netdev-and-napi.md) 到 [06](./06-tx-path-and-qdisc.md)、[13](./13-virtio-net-guest.md) 到 [16](./16-observability.md)。这里不重复结构体。

---

## 17.1 RX softirq 打满

现象：一颗核的 `si` 很高，`ksoftirqd` 吃 CPU，延迟上去，PPS 不再涨。

看什么：`/proc/net/softnet_stat` 该 CPU 的 `time_squeeze` 是否增加；`/proc/interrupts` 上 virtio input 是否只落在这一颗核。

改什么：设备有多队列就按 [15](./15-mq-rps-xps-affinity.md) 把队列摊开。只有一对队列就设 `rps_cpus`，让 `enqueue_to_backlog` 把 skb 分到别的 CPU。预算本身是 `netdev_budget` 和 `netdev_budget_usecs`，加大它们只是让一轮 softirq 更长，不增加核数。

**RECIPE-V5**（与 [15](./15-mq-rps-xps-affinity.md) 同一条）

目的：确认 RPS 之后 processed 从 IRQ 核挪到其他核。
命令：见第 15 章 RECIPE-V5。
期望看什么：`time_squeeze` 不再只在一颗核上跳。
实测（待补）：

---

## 17.2 qdisc 积压

现象：`tc -s qdisc` 的 backlog 字节持续非 0，应用 `send` 阻塞，驱动的 kicks 不多。

看什么：是 `pfifo_fast` 还是 `noqueue`。`noqueue` 的设备（veth 带 `IFF_NO_QUEUE`）积压不在 qdisc，而在对端或套接字。物理或 virtio 设备上还要看 BQL 的 `inflight` 是否顶住 `limit`，那是 `NETDEV_TX_BUSY` 把 skb 退回 qdisc。

改什么：对端或网卡已经停队列时，加长 qdisc 只增加延迟。先看 `tx_timeouts` 和链路是否通。确认是单流打满一条 tx 队列时，再考虑 XPS 和多队列，而不是换一个更深的 qdisc。

---

## 17.3 TCP 重传

现象：吞吐锯齿，`ss -i` 里 `retrans` 增加，`/proc/net/snmp` 的 `RetransSegs` 上涨。

看什么：`cwnd` 是否反复掉到很小，`rto` 是否远大于 `rtt`。丢包在客户机 `softnet_stat.dropped`、qdisc drops，还是对端。`snd_una` 不前进时，重传定时器会从 `tcp_rtx_queue` 再送，见 [08](./08-tcp.md)。

改什么：重传本身不是开销开关。先去掉 RPS 与 IRQ 亲和互相打架造成的乱序（乱序会让 cubic 误判丢包），再看路径上谁在丢。

---

## 17.4 小包 PPS

现象：带宽远不到线速，CPU 先满，每包都是一次 kick 和一次 softirq。

看什么：`ethtool -k` 的 GRO 是否打开；virtio 是否协商了 `VIRTIO_NET_F_MRG_RXBUF` 和 host TSO。`ethtool -S` 的 kicks 若接近包数，说明没有合并。

改什么：接收侧 GRO 把同流小包合成大 skb，再进 `ip_rcv`。发送侧 TSO 让一个 skb 带 `gso_size`，由宿主机或网卡分段。两种卸载的元数据都在 `skb_shared_info`，见 [02](./02-sk-buff.md)。

---

## 17.5 virtio 单队列

现象：客户机里只有 `input.0` / `output.0` 在涨，其余 vCPU 闲着，PPS 卡在一颗核的 softirq 上。

看什么：`ethtool -l` 的 Combined 是否为 1，而 `nproc` 大于 1。`curr_queue_pairs` 的取值规则在 [13](./13-virtio-net-guest.md)：设备没给 `VIRTIO_NET_F_MQ`，或者上限就是 1。

改什么：设备支持时把 combined 设成 vCPU 数，再绑 IRQ。设备不支持时只能 RPS，并且接受哈希与 IPI 的代价。

**RECIPE-V1**

目的：确认这台云客户机是单队列瓶颈还是已经开了 MQ。
命令：`ethtool -l eth0`；`nproc`。
期望看什么：Combined 为 1 且 vCPU 更多。
实测（待补）：

**RECIPE-V3**

目的：打开 MQ 并绑核之后，每队列都有 kicks。
命令：见 [15](./15-mq-rps-xps-affinity.md) 的 RECIPE-V3。
期望看什么：`ethtool -S` 里不是只有 rx0/tx0。
实测（待补）：

---

## 17.6 RPS 和 IRQ 亲和打架

现象：开了 RPS 之后延迟变差，`softnet_stat` 里 IRQ 那颗核的 processed 仍高，其他核的 `dropped` 也在涨。

看什么：`rps_cpus` 是不是全 1，同时 input IRQ 又绑在其中一颗核上。`get_rps_cpu` 在 IRQ 核上算完哈希再 `enqueue_to_backlog`。目标核的 `input_pkt_queue` 超过 `netdev_max_backlog` 就丢。

改什么：有多队列时清掉 `rps_cpus`（写成 0），让 NAPI 留在队列自己的核上。没有多队列时保留 RPS，但不要再把唯一的 IRQ 和所有应用线程绑在同一颗核上。RFS 需要 `rps_sock_flow_entries` 非 0，否则只有 RPS 的静态映射，流不会跟着 `recvmsg` 走。

---

## 17.7 vhost worker 抢同一颗核

现象：客户机 softirq 不高，宿主机上某个 `vhost-*` 线程和客户机的 vCPU 线程挤在同一颗物理核，PPS 上不去。

看什么：宿主机 `ps -eLo psr,comm` 里 vhost 的 CPU，以及这颗 CPU 上是否还有该客户机的 vCPU。`handle_rx` 从 tap `recvmsg` 再 `vhost_signal`，和客户机里的 `virtnet_poll` 如果共享物理核，两边轮流跑。

改什么：把 vhost 线程挪到没有跑这个客户机 vCPU 的核。客户机里的 RPS 解决不了宿主机线程的争用。

**RECIPE-V2**

目的：记下 vhost 线程和客户机 IRQ 是否重叠。
命令：见 [16](./16-observability.md) 的 RECIPE-V2。
期望看什么：`psr` 与 input IRQ 的 CPU 相同，且该客户机的 vCPU 也在这颗核上。
实测（待补）：

**RECIPE-V4**

目的：用 trace 区分时间在 `virtnet_poll` 还是 `handle_rx`。
命令：客户机抓 `net:netif_receive_skb`；宿主机对 `vhost_net` 的 `handle_rx` / `handle_tx` 做函数计数。权限和具体 `perf` 参数按那台机器的 tracefs 来，不在这里写死事件以外的开关。
期望看什么：两边的热点是否落在同一时间窗。
实测（待补）：

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
