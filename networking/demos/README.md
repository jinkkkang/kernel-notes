# networking demos

对照内核 **v6.6.160** 的套接字计数、`SCM_RIGHTS` 和 veth。用户态只用 libc 和 iproute2。

| 文件 | 验证点 |
|------|--------|
| [01-sock-stats.c](./01-sock-stats.c) | 建一条回路 TCP 和一个 UDP 套接字，`/proc/net/sockstat` 的 inuse 增加，`ss -m` 打出 skmem |
| [02-unix-scm-fd.c](./02-unix-scm-fd.c) | `socketpair(AF_UNIX, SOCK_STREAM)` 用 `SCM_RIGHTS` 传递管道写端 |
| [03-veth-ping.sh](./03-veth-ping.sh) | 两个 netns、一对 veth、一次 ping。需要 `CAP_NET_ADMIN` |

编译与运行：

```bash
cd networking/demos
make
./01-sock-stats
./02-unix-scm-fd
sudo -n ./03-veth-ping.sh
```

下面是写笔记时在本机跑的 stdout。主机内核 **6.12.113+**，`gcc -Wall -Wextra -O2`。行为按 v6.6.160 的 `udp_recvmsg` / `unix_stream_sendmsg` / `veth_xmit` 解释，数字是 sampled on 6.12。

`01-sock-stats`：

```text
kernel=6.12.113+
before
TCP: inuse 9 orphan 0 tw 7 alloc 15 mem 51
UDP: inuse 0 mem 0
tcp_listen_port=53309 udp_port=55613 fds=3,4,6,5
after
TCP: inuse 12 orphan 0 tw 7 alloc 18 mem 51
UDP: inuse 1 mem 0
ESTAB 0 0 127.0.0.1:53309 127.0.0.1:36308
 skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0) cubic ... cwnd:10
```

`inuse` 的差是这次多出来的监听套接字、连接两端。端口和 `rtt` 每次不同。完整 `ss` 行在 [09](../09-udp.md)。

`02-unix-scm-fd`：

```text
kernel=6.12.113+
payload=Z passed_fd_works=1 read_back=Q
```

`03-veth-ping.sh`：

```text
kernel=6.12.113+
veth-a qdisc noqueue state UP
veth-b inet 10.88.0.2/24
64 bytes from 10.88.0.2: icmp_seq=1 ttl=127 time=0.064 ms
veth_ping=ok
```

没有 `CAP_NET_ADMIN` 时脚本打印 `veth_ping=skipped` 并以 0 退出。云上改队列、RPS 和 vhost 的 RECIPE-V1 到 V5 在 [15](../15-mq-rps-xps-affinity.md)、[16](../16-observability.md)、[17](../17-perf-analysis.md)，实测栏是「待补」。
