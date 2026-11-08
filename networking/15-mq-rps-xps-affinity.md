# 15. 多队列、RPS、XPS 与绑核

**结论**：硬件或 virtio 的多队列让每条队列有自己的中断和 NAPI；RPS 在只有一条接收队列时，用哈希把 skb 投到别的 CPU 的 backlog；XPS 在发送时按 CPU 选 `netdev_queue`。三者同时开、又把 IRQ 绑死在一颗核上，软中断会挤在一起。

> RPS 的查找是 `get_rps_cpu`（`net/core/dev.c:4715`），入队是 `enqueue_to_backlog`（`:4946`）。XPS 是 `get_xps_queue`（`:4284`）。结构在 [04](./04-netdev-and-napi.md) 和 [06](./06-tx-path-and-qdisc.md)，这里写选择规则。IRQ 亲和的通用机制在 [interrupts/06](../interrupts/06-request-and-flow.md)。

下面每条要改机器的命令都是 RECIPE。本机 `enp0s3` 只有一对 virtio 队列，读到的掩码写在 [16](./16-observability.md)，不填这些配方的实测栏。

---

## 15.1 virtio-net 的队列对

`virtnet_info.curr_queue_pairs` 是当前启用的对数，`max_queue_pairs` 是设备上限。probe 里如果在线 CPU 不少于上限，就取上限，否则取 `num_online_cpus()`（`virtio_net.c:4691`）。协商了 `VIRTIO_NET_F_MQ` 且有控制队列时，`_virtnet_set_queues`（`:2680`）把这个数写给设备。

队列数和 vCPU 数不一致时有两种常见结果。对数少于 vCPU，多颗 vCPU 的流哈希进同一条 `send_queue`，kick 和 NAPI 仍在少数核上。对数多于 vCPU，多出来的队列没有稳定的 CPU 可绑，中断在核之间跳。`ethtool -l` 看到的 combined 数对应 `curr_queue_pairs`。改它之前先看设备上限。

**RECIPE-V1**

目的：在云客户机里记下 virtio 队列数和 vCPU 数是否一致。
命令：`nproc`；`ethtool -l eth0`；`ls /sys/class/net/eth0/queues`。
期望看什么：`Current hardware settings` 的 Combined 与 `nproc`，以及 `rx-*` / `tx-*` 目录个数。
实测（待补）：

---

## 15.2 IRQ 亲和

每条 virtio 收发队列各有一个 MSI-X。`/proc/interrupts` 里名字是 `virtioN-input.i` 和 `virtioN-output.i`。亲和掩码在 `/proc/irq/<n>/smp_affinity`。中断落到哪颗核，`net_rx_action` 就在哪颗核上跑，除非 RPS 再把 skb 推走。

一核一队列的做法：队列 i 的 input IRQ 绑到 vCPU i，同时 XPS 让 vCPU i 的发送选 tx i。这样一条流的软中断、qdisc 和 vhost kick 尽量留在同一颗核，缓存还热着。

IRQ 绑在 CPU 0、队列却有多条时，所有 NAPI 仍从 CPU 0 开始。RPS 再散开的话，包先在 CPU 0 上算哈希，再 IPI 到别的核，多一次排队。

**RECIPE-V2**

目的：在宿主机看 vhost 线程和 virtio IRQ 落在哪颗核。
命令：`ps -eLo pid,psr,comm | grep vhost`；`grep virtio /proc/interrupts`；对每个 input IRQ `cat /proc/irq/<n>/smp_affinity_list`。
期望看什么：vhost 线程的 `psr` 是否和客户机 input IRQ 的 CPU 重叠。
实测（待补）：

---

## 15.3 RPS 与 RFS

`get_rps_cpu` 只在该 RX 队列的 `rps_map` 或 `rps_flow_table` 至少有一个非空时才算哈希（`dev.c:4741`）。都空则返回 -1，skb 留在中断所在的 CPU。

```text
skb_get_hash
若本队列有 rps_flow_table，且全局 rps_sock_flow_table 有记录：
  ident = ents[hash & mask]                 高位是流哈希，低位是 CPU
  高位对不上：放弃 RFS，去试 rps_map
  期望 CPU = ident & rps_cpu_mask
  与 flow 里记下的 cpu 不同，且旧 CPU 的 input_queue_head
  已经追上 last_qtail：可以换 CPU，避免乱序
否则若有 rps_map：
  cpu = map->cpus[reciprocal_scale(hash, map->len)]
enqueue_to_backlog(skb, cpu)
  队列长于 netdev_max_backlog：SKB_DROP_REASON_CPU_BACKLOG
```

全局表的写入在 `rps_record_sock_flow`（`netdevice.h:769`）：`val` 的低位是 `raw_smp_processor_id()`。`sock_rps_record_flow`（`include/net/sock.h:1172`）在 `sk_state == TCP_ESTABLISHED` 时调用它。INET 的 `inet_recvmsg` 在进入 `tcp_recvmsg` / `udp_recvmsg` 之前调用 `sock_rps_record_flow`（`net/ipv4/af_inet.c:880`）。这就是 RFS：让协议栈跟到应用上次读包的核，而不是只按哈希打散。UDP 未连接时 `sk_state` 不是 `TCP_ESTABLISHED`，这条记录不会发生。

`rps_cpu_mask` 是 `roundup_pow_of_two(nr_cpu_ids) - 1`（`netdevice.h:753` 的注释）。表项 32 位里，掩码以上的位用来核对是不是同一条流。

sysfs：`/sys/class/net/<dev>/queues/rx-<n>/rps_cpus` 是 CPU 掩码，`rps_flow_cnt` 是本队列流表大小。全局表项数是 `/proc/sys/net/core/rps_sock_flow_entries`。掩码为 0 表示这张 `rps_map` 不存在，`get_rps_cpu` 在两边都空时直接返回。

RPS 适合单队列虚拟网卡把软中断摊开。已经有和 vCPU 对齐的多队列时，再开一张覆盖全部 CPU 的 `rps_cpus`，会把刚按队列分开的流又混回 backlog。

**RECIPE-V5**

目的：改 RPS 掩码前后看 `softnet_stat` 的 processed 和 dropped 是否从单核挪走。
命令：改之前 `cat /proc/net/softnet_stat`；`echo ff > /sys/class/net/eth0/queues/rx-0/rps_cpus`；打一小波 UDP；再读 `softnet_stat`。
期望看什么：接收 IRQ 所在 CPU 的第一列不再独占增长，其他 CPU 的 processed 上升；队列过深时第二列（dropped）增加。
实测（待补）：

---

## 15.4 XPS

`get_xps_queue`（`dev.c:4284`）在 `xps_needed` 没打开时返回 -1。打开之后先查 `xps_maps[XPS_RXQS]`：用套接字记住的接收队列下标选发送队列，让回程留在同一对队列。没有再查 CPU 图 `XPS_CPUS`，下标是当前 CPU。

`netdev_pick_tx`（`:4340`）采用这个下标，写进 `skb->queue_mapping`。virtio-net 的 `start_xmit` 用它选 `vi->sq[qnum]`。

单队列设备没有 XPS 可配。`xps_cpus_show`（`net/core/net-sysfs.c:1497`）对 `!netif_is_multiqueue(dev)` 返回 `-ENOENT`（`:1503`）。多队列设备的文件在 `tx-<n>/xps_cpus`。

**RECIPE-V3**

目的：多队列打开前后看 PPS，并把队列绑到对应 vCPU。
命令：`ethtool -L eth0 combined N`（N 取 vCPU 数）；对每个 IRQ 写 `smp_affinity_list`；`ethtool -S eth0` 看每队列的 kicks。
期望看什么：combined 从 1 变成 N 之后，每队列 kicks 都有计数，而不是只涨 queue 0；IRQ 的 `/proc/interrupts` 列和 affinity 一致。
实测（待补）：

---

## 15.5 选型

| 情况 | 做法 |
|------|------|
| virtio 队列数可以等于 vCPU | 开 MQ，一核一队列，XPS 按 CPU，不必再铺开 RPS |
| 设备只有一对队列 | 用 RPS 把 `NET_RX_SOFTIRQ` 摊开；RFS 让 `recvmsg` 把流拉回应用所在的核 |
| IRQ 已经绑死，又把 `rps_cpus` 设成全部 CPU | 包在 IRQ 核上入队后再 IPI，延迟增加，backlog 可能顶到 `netdev_max_backlog` |
| 队列数和 vCPU 数取了不同的值 | `curr_queue_pairs` 与 affinity 对不齐，空队列不产生中断，忙队列的 NAPI 打满 |

vhost worker 是另一颗「核上的线程」。客户机 NAPI 和宿主机 `handle_rx` 如果被绑在同一颗物理核，两边的软中断互相抢。把 worker 和客户机的 input IRQ 错开，比在客户机里再加一层 RPS 更直接。现象和改法在 [17](./17-perf-analysis.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
