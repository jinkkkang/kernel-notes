# 网络栈

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.113+**（`uname -r`，4 核，网卡 `enp0s3` 驱动 `virtio_net`）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：一个报文在内核里是 `sk_buff`。发送从 `socket` 的 `proto_ops` 进到 `sock`，经 L4/L3、邻居和 `Qdisc`，由 `ndo_start_xmit` 交给设备；接收从 NAPI 的 `poll` 经 GRO 和 `netif_receive_skb` 回到套接字接收队列。云上的虚拟网卡在客户机是 virtio-net，在宿主机是 tap 加 `vhost_net`。

## 阅读顺序

```text
01 总览
  → 02 sk_buff
  → 03 socket / sock
  → 04 net_device 与 NAPI
  → 05 收包
  → 06 发包与 qdisc
  → 07 IPv4 路由与邻居
  → 08 TCP
  → 09 UDP
  → 10 AF_UNIX 收发
  → 11 netns / veth / bridge
  → 12 netfilter 挂钩（薄）
  → 13 virtio-net 客户机
  → 14 宿主机 tap 与 vhost_net
  → 15 多队列、RPS、XPS、绑核
  → 16 观测
  → 17 性能配方
```

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 收发总图、云上多一跳、覆盖边界 | 中 |
| [02-sk-buff.md](./02-sk-buff.md) | `sk_buff`、`skb_shared_info`、clone/copy、校验和与 GSO | **厚** |
| [03-socket-and-sock.md](./03-socket-and-sock.md) | `socket` / `sock` / `sock_common`、`proto` / `proto_ops`、队列与锁 | **厚** |
| [04-netdev-and-napi.md](./04-netdev-and-napi.md) | `net_device`、`napi_struct`、`softnet_data`、预算；RPS 只留挂钩 | **厚** |
| [05-rx-path.md](./05-rx-path.md) | poll、GRO、`netif_receive_skb`、`rx_handler`、`ip_rcv`；XDP 点名 | **厚** |
| [06-tx-path-and-qdisc.md](./06-tx-path-and-qdisc.md) | `dev_queue_xmit`、`Qdisc`、`ndo_start_xmit`、BQL；XPS 只留挂钩 | **厚** |
| [07-ipv4-route-neigh.md](./07-ipv4-route-neigh.md) | fib、`dst_entry`、`neighbour` / ARP；IPv6 薄对照 | 中 |
| [08-tcp.md](./08-tcp.md) | `tcp_sock`、状态、收发主路径、cubic | **厚** |
| [09-udp.md](./09-udp.md) | `udp_sendmsg` / `udp_recvmsg`、哈希表 | 中 |
| [10-af-unix.md](./10-af-unix.md) | stream/dgram 收发、`unix_sock`、`SCM_RIGHTS` | 中偏厚 |
| [11-netns-veth-bridge.md](./11-netns-veth-bridge.md) | veth 转发、bridge `rx_handler`、CNI 点名 | 中 |
| [12-netfilter-hooks.md](./12-netfilter-hooks.md) | `NF_INET_*`、`nf_hook_ops` | 薄 |
| [13-virtio-net-guest.md](./13-virtio-net-guest.md) | 客户机 virtio-net 特性、队列、NAPI、`start_xmit` | **厚** |
| [14-virtio-net-host-vhost.md](./14-virtio-net-host-vhost.md) | tap、`vhost_net` 数据面；vhost-user 点名 | **厚** |
| [15-mq-rps-xps-affinity.md](./15-mq-rps-xps-affinity.md) | MQ、IRQ 亲和、RPS/RFS、XPS | **厚** |
| [16-observability.md](./16-observability.md) | `/proc/net`、`ss`、ethtool；云上 RECIPE | **厚** |
| [17-perf-analysis.md](./17-perf-analysis.md) | softirq、qdisc、重传、virtio 单队列与绑核 | **厚** |
| [demos/](./demos/) | sockstat、UNIX 传 fd、veth ping | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| 套接字睡眠在 `sk_wq->wait` 上 | 等待队列词汇：[locks-and-ds/08-wait-queue-and-completion.md](../locks-and-ds/08-wait-queue-and-completion.md)。futex：[locks-and-ds/09-futex.md](../locks-and-ds/09-futex.md) |
| `tcp_poll` / `unix_poll` 报可读可写 | epoll 对象与 LT/ET：[ipc/03-epoll-objects.md](../ipc/03-epoll-objects.md)、[ipc/05-epoll-wait-and-modes.md](../ipc/05-epoll-wait-and-modes.md)。`AF_UNIX` 就绪侧仍在 [ipc/08-unix-and-sysv.md](../ipc/08-unix-and-sysv.md)，完整 send/recv 在 [10](./10-af-unix.md) |
| `sock_map_fd` 把 `socket` 装进 `struct file` | fd 表与 `read_iter`：[vfs/04-fd-and-open.md](../vfs/04-fd-and-open.md)、[vfs/05-read-write.md](../vfs/05-read-write.md) |
| `NET_RX_SOFTIRQ` / `NET_TX_SOFTIRQ` 上的 `net_rx_action` | softirq 循环与 `ksoftirqd`：[interrupts/08-softirq-tasklet.md](../interrupts/08-softirq-tasklet.md) |
| `vhost_signal` 写 eventfd | irqfd 与客户机 LAPIC：[interrupts/11-kvm-irq-virt.md](../interrupts/11-kvm-irq-virt.md) |
| 设备在 `struct net` 里，veth 两端可分属两个 ns | `nsproxy` 与 `copy_net_ns`：[containerization/05-net-ns.md](../containerization/05-net-ns.md)。本章写 veth 转发和 bridge |
| `sk->sk_cgrp_data`、`net_cls` / `net_prio` | cgroup 对象：[cgroup/](../cgroup/README.md)。这里只点名 |
| XDP、tc-BPF、sockops | 名字出现在收发包挂钩上。程序、verifier、map 留给以后的 ebpf 章 |

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/skbuff.h`、`net/core/skbuff.c` | `sk_buff`、clone/copy |
| `include/linux/net.h`、`net/socket.c` | `struct socket`、`proto_ops`、`sendmsg`/`recvmsg` |
| `include/net/sock.h`、`net/core/sock.c` | `sock`、`proto` |
| `include/linux/netdevice.h`、`net/core/dev.c`、`net/core/gro.c` | 设备、NAPI、RPS、收发包 |
| `include/net/sch_generic.h`、`net/sched/sch_generic.c` | `Qdisc`、`pfifo_fast` |
| `net/ipv4/` | IPv4、TCP、UDP、fib |
| `net/unix/af_unix.c` | `AF_UNIX` 收发 |
| `drivers/net/veth.c`、`net/bridge/` | veth、bridge |
| `include/linux/netfilter.h`、`net/netfilter/core.c` | netfilter 挂钩 |
| `drivers/net/virtio_net.c` | 客户机 virtio-net |
| `drivers/vhost/net.c`、`drivers/net/tun.c` | 宿主机 vhost 与 tap |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.113+）：[demos/](./demos/) 三个程序。[09](./09-udp.md)、[10](./10-af-unix.md)、[11](./11-netns-veth-bridge.md)、[16](./16-observability.md) 贴了当次 stdout。同一台机器上的 `enp0s3`（`virtio_net`，1 个 combined 队列）、`/proc/net/softnet_stat`、`pfifo_fast` 与 `xps_cpus` 的读法在 [16](./16-observability.md)。
- **RECIPE-V1..V5**：[15](./15-mq-rps-xps-affinity.md)、[16](./16-observability.md)、[17](./17-perf-analysis.md)。云上改队列、绑核、ftrace 的四行配方保持「实测（待补）」。本机没有宿主机 vhost 线程，也没有多队列 virtio 可对照。

## 未覆盖

- 物理网卡驱动（igb、ixgbe 一类）的描述符环
- Wi-Fi、RDMA、WireGuard
- 与 IPv4 对等厚度的 IPv6
- XDP / AF_XDP 的程序与 verifier
- vhost-user 的用户态实现，以及 QEMU 源码
- MPTCP、TCP-AO 的字段只在 `tcp_sock` 里按条件编译留着，不展开状态机

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
