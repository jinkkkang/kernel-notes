# 16. 观测

**结论**：先看 `/proc/net/softnet_stat` 和 `ss -m`，再看设备的 qdisc 与 `ethtool -S`。客户机 virtio 的队列数用 `ethtool -l`，宿主机 vhost 线程用 `ps`。本机能读到的是 6.12 上的一张单队列 virtio 网卡；要改队列或抓 `vhost_net` 的配方留空。

> 统计变量：`netdev_budget` 初值 300（`net/core/dev.c:4605`），`netdev_max_backlog` 初值 1000（`:4600`）。tracepoint `trace_netif_receive_skb` 在 `__netif_receive_skb_core`（`:5511`），`trace_net_dev_start_xmit` 在 `xmit_one`（`:3670`）。

数字都是 sampled on 6.12.113+，4 核，`enp0s3` 的驱动是 `virtio_net`。不要把这些计数当成 v6.6.160 的常量。

---

## 16.1 `/proc/net` 与 sysctl

`/proc/net/softnet_stat` 一行一个 CPU。本机 4 行、每行 15 个十六进制字段。和 6.6 的 `softnet_data` 对得上的前几列是：

| 列 | 字段 | 含义 |
|----|------|------|
| 1 | `processed` | 交给协议栈的包 |
| 2 | `dropped` | `enqueue_to_backlog` 丢弃 |
| 3 | `time_squeeze` | `net_rx_action` 预算或时间用尽 |

后面的列包括 RPS 的 `received_rps` 和 CPU 编号。列数随内核增加，以当前 `/proc` 为准，不要用 6.12 的第 15 列去对 6.6 源码里没有的成员。

当时读到的 sysctl：

```text
/proc/sys/net/core/netdev_max_backlog = 1000
/proc/sys/net/core/netdev_budget = 300
/proc/sys/net/core/netdev_budget_usecs = 8000
/proc/sys/net/core/dev_weight = 64
/proc/sys/net/core/rps_sock_flow_entries = 0
```

`netdev_budget_usecs` 的 6.6 初值表达式是 `2 * USEC_PER_SEC / HZ`（`dev.c:4607`）。8000 与 HZ 为 250 时的这个表达式一致，但是本机内核是 6.12，不能据此断言这台机器的 HZ。

`/proc/net/sockstat` 的一次前后对照在 [09](./09-udp.md)。`/proc/net/snmp` 的 `Tcp:` 行有 `RetransSegs`、`InErrs`、`OutRsts`。本机当时 `RetransSegs` 是 9，`InErrs` 是 0，只说明那一刻的计数器。

`ss -s` 给套接字总数。`ss -m -ti` 给一条连接的 `skmem`、拥塞算法、`cwnd`、`rto`、`rtt`。demo 在回路上打出过：

```text
skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0)
cubic wscale:7,7 rto:204 rtt:0.02/0.01 mss:32768 cwnd:10
```

`rb` / `tb` 对应接收和发送缓冲上限，`cubic` 对应 [08](./08-tcp.md) 的 `cubictcp`。

---

## 16.2 设备与 qdisc

`ip -s link show enp0s3` 是设备计数。`tc qdisc show dev enp0s3` 当时是：

```text
qdisc pfifo_fast 0: root refcnt 2 bands 3 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
```

这和 `default_qdisc_ops = &pfifo_fast_ops` 一致。veth 的 `noqueue` 见 [11](./11-netns-veth-bridge.md)。

BQL 在 `tx-0/byte_queue_limits/`。当时 `limit` 约 69107，`inflight` 为 0。`inflight` 长期顶满且 `stall_cnt` 增加，说明 [06](./06-tx-path-and-qdisc.md) 的 `STACK_XOFF` 在起作用。

`rx-0/rps_cpus` 读出来是 `0`，`rps_flow_cnt` 是 `0`。`tx-0/xps_cpus` 节点在，`cat` 返回 “No such file or directory”：6.6 的 `xps_cpus_show` 对非多队列设备返回 `-ENOENT`（`net-sysfs.c:1503`）。这张网卡 `ethtool -l` 的 Combined 当前值是 1。

---

## 16.3 这台客户机上的 virtio

`ethtool -i enp0s3` 的 driver 是 `virtio_net`。`ethtool -l`：Pre-set maximums 与 Current 的 Combined 都是 1，RX/TX 单独的栏是 n/a。`ethtool -k` 里 checksum、scatter-gather、tcp-segmentation-offload、generic-receive-offload 为 on。这些开关对应客户机协商到的 `VIRTIO_NET_F_*`，位定义在 [13](./13-virtio-net-guest.md)，协商结果以客户机实际特性为准。

`ethtool -S` 当时能看到 `rx_kicks`、`tx_kicks`、`rx_drops` 以及 `rx0_*` / `tx0_*`。6.6 源码里 `virtnet_rq_stats_desc` 的名字是 `packets`、`bytes`、`drops`、`kicks` 等（`virtio_net.c:116`），没有 `rx_kicks` 这个前缀。6.12 的 ethtool 字符串和 6.6 的数组不是同一份，只把「有 kicks、drops 计数」当作这张卡在工作，不要逐项对行号。

`/proc/interrupts` 里这张卡是：

```text
virtio2-input.0
virtio2-output.0
```

input 的计数落在 CPU 3，output 落在 CPU 0。单队列时两边本来就可以不在同一颗核。

**RECIPE-V1**

目的：在目标云客户机记下队列、卸载和每队列计数，而不是沿用上面这台 6.12 虚拟机的数字。
命令：`ethtool -l eth0`；`ethtool -k eth0`；`ethtool -S eth0`；`nproc`。
期望看什么：Combined 与 vCPU 数；GRO/TSO 是否打开；每队列 kicks 是否只有 queue 0 在涨。
实测（待补）：

---

## 16.4 宿主机一侧

客户机里看不到 vhost 线程。宿主机上后端是 tap 时，线程名里带 `vhost`，队列的 kick/call 是 eventfd。

**RECIPE-V2**

目的：给客户机的每条 virtqueue 找到宿主机上的 vhost 线程和 IRQ。
命令：`ps -eLo pid,tid,psr,pcpu,comm | grep vhost`；`grep virtio /proc/interrupts`；`ls /proc/<vhostpid>/fd`。
期望看什么：线程数是否随客户机队列对增加；`psr` 是否和客户机 input IRQ 落在同一颗核。
实测（待补）：

guest 与 host 对照时各看各的计数，不要把客户机的 `tx_kicks` 和宿主机的 `handle_tx` 次数当成同一个变量。客户机 kick 一次，宿主机工作项可能把最多 `VHOST_NET_BATCH`（64）个包合成一批再 `sendmsg`。

---

## 16.5 丢包从哪一列看

| 现象 | 先看 |
|------|------|
| 软中断来不及 | `softnet_stat` 的 `time_squeeze`，以及 `ksoftirqd` 的 CPU |
| RPS 队列满 | 同一文件的 `dropped`，`skb` 的 drop reason 是 `SKB_DROP_REASON_CPU_BACKLOG` |
| qdisc 丢 | `tc -s qdisc show` 的 drops |
| 驱动环满 | `ethtool -S` 的 drops，virtio 还有 `tx_timeouts` |
| 套接字缓冲满 | `ss -m` 的 `skmem`，UDP 还有 `/proc/net/snmp` 的 `RcvbufErrors` |
| TCP 重传 | `RetransSegs`，`ss -i` 的 `retrans` |

tracepoint 名字：`netif_receive_skb`、`napi_gro_receive_entry`、`net_dev_queue`、`net_dev_start_xmit`、`net_dev_xmit`。函数位置见上面的引用。本机没有把 tracefs 打开来抓这些事件。

**RECIPE-V4**

目的：在有权限的机器上看 poll 和 vhost 各花多少时间。
命令：`perf stat -e irq:softirq_entry -a -- sleep 5`；`trace-cmd record -e net:netif_receive_skb -e net:net_dev_start_xmit`；需要时再加 `vhost_net` 的函数图。
期望看什么：`NET_RX_SOFTIRQ` 的次数是否集中在一个 CPU；`net_dev_start_xmit` 的设备是否是 virtio 或 tap。
实测（待补）：

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
