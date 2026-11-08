# 1. 总览

**结论**：发送是「系统调用 → L4 → L3 → 邻居 → qdisc → `ndo_start_xmit`」；接收是「设备 poll → GRO → `netif_receive_skb` → 协议 `handler` → 套接字接收队列」。云虚拟机在客户机驱动和宿主机后端之间多一跳 virtqueue。

> 对象定义分散在 `include/linux/skbuff.h`、`include/net/sock.h`、`include/linux/netdevice.h`。收包核心在 `net/core/dev.c`，GRO 在 `net/core/gro.c`。

C 新手先记住三层指针，不要把它们当成同一个东西：

| 对象 | 谁拿着 | 干什么 |
|------|--------|--------|
| `struct socket` | 系统调用和 `struct file` | `bind` / `connect` / `sendmsg` / `poll` 的操作表 |
| `struct sock` | 协议 | 收发队列、端口、内存记账、锁 |
| `struct sk_buff` | 队列和驱动 | 这一帧的元数据和数据 |

`struct file` 上的 `f_op` 把 `read`/`write`/`poll` 转进套接字。fd 表本身在 [vfs/04](../vfs/04-fd-and-open.md)。

---

## 1.1 发送与接收

```mermaid
flowchart LR
  sendmsg["sock_sendmsg"] --> L4["tcp_sendmsg / udp_sendmsg / unix_*_sendmsg"]
  L4 --> L3["ip_queue_xmit / ip_output"]
  L3 --> neigh["邻居解析"]
  neigh --> qdisc["__dev_queue_xmit"]
  qdisc --> ndo["ndo_start_xmit"]
  ndo --> dev["设备队列"]
  dev --> poll["NAPI poll"]
  poll --> gro["napi_gro_receive"]
  gro --> rcv["netif_receive_skb"]
  rcv --> ip["ip_rcv"]
  ip --> sockq["sk_receive_queue"]
```

IPv4 TCP 发送主链（行号是 v6.6.160）：

```text
__sys_sendmsg                         net/socket.c:2664
  sock_sendmsg                        :756
    proto_ops->sendmsg                inet_stream_ops.sendmsg → tcp_sendmsg
      tcp_sendmsg                     net/ipv4/tcp.c:1343
        tcp_sendmsg_locked            :1042
        tcp_push                      :709
          tcp_write_xmit              net/ipv4/tcp_output.c:2692
            tcp_transmit_skb          :1442
              ip_queue_xmit           net/ipv4/ip_output.c:552
                __dev_queue_xmit      net/core/dev.c:4416
                  dev_hard_start_xmit :3677
                    ndo_start_xmit    include/linux/netdevice.h:5003
```

接收对称地从驱动的 NAPI 回调进来。virtio-net 的 poll 是 `virtnet_poll`（`drivers/net/virtio_net.c:2222`），它最终把 skb 送进 `napi_gro_receive`（`net/core/gro.c:635`）。协议分发在 `__netif_receive_skb_core`（`net/core/dev.c:5498`）里按 `skb->protocol` 找 `packet_type`。IPv4 的登记是 `ip_packet_type.func = ip_rcv`（`net/ipv4/af_inet.c:1950`）。

本地 `AF_UNIX` 不经过 `net_device`。`unix_stream_sendmsg`（`net/unix/af_unix.c:2227`）把 skb 放进对端的 `sk_receive_queue`。就绪位仍由 [ipc/08](../ipc/08-unix-and-sysv.md) 的 `unix_poll` 解释，字节和 fd 在 [10](./10-af-unix.md)。

---

## 1.2 云上多一跳

客户机里的 virtio-net 和宿主机上的 tap 共用 virtqueue。客户机把描述符放进环，`virtqueue_notify` 踢一下；宿主机 `vhost_net` 的工作线程在 tap 套接字上 `sendmsg`/`recvmsg`，做完再 `vhost_signal` 写 eventfd。QEMU 把这个 eventfd 注册成 irqfd 之后，客户机才看到中断。irqfd 的注入在 [interrupts/11](../interrupts/11-kvm-irq-virt.md)，本章只写网络数据面。

```text
客户机进程
  TCP/IP
    virtio-net start_xmit / virtnet_poll     drivers/net/virtio_net.c
      virtqueue
        vhost_net handle_tx / handle_rx      drivers/vhost/net.c
          tap 套接字（tun 驱动）              drivers/net/tun.c
            宿主机协议栈或桥
```

vhost-user 把同一套 virtqueue 放到另一个用户态进程里。本章只点名，不读 QEMU。

多队列、RPS、XPS 和 IRQ 亲和决定这一跳落在哪颗 CPU。[04](./04-netdev-and-napi.md) 和 [06](./06-tx-path-and-qdisc.md) 只说明函数在哪被调用，算法和绑核在 [15](./15-mq-rps-xps-affinity.md)。

---

## 1.3 本章覆盖到哪

写：`sk_buff` 布局，套接字对象，NAPI 与 qdisc，IPv4 转发直觉，TCP/UDP 主路径，`AF_UNIX` 的 send/recv，netns 里的 veth 与 bridge，netfilter 五个 IPv4 挂钩，virtio-net 客户机与 `vhost_net`，多队列和观测。

不写：物理网卡的 DMA 环，Wi-Fi / RDMA / WireGuard，完整 IPv6，XDP 程序本体，vhost-user 用户态，以及 softirq、epoll、`nsproxy` 已经在邻章写过的部分。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
