# 7. XDP

**结论**：XDP 程序的 ctx 在内核里是 `xdp_buff`，返回值是 `xdp_action`。generic 模式在 `__netif_receive_skb_core` 里、GRO 之前，把 skb 临时摆成 `xdp_buff` 再跑程序。driver 模式更早，在驱动的 NAPI poll 里调用 `bpf_prog_run_xdp`，挂载停在 `ndo_bpf`。三种模式用 `XDP_FLAGS_*` 区分，generic 和 driver 不能同时挂在同一块设备上。

> 动作码在 `include/uapi/linux/bpf.h:6310`。`xdp_buff` 在 `include/net/xdp.h:80`，`bpf_prog_run_xdp` 在 `:495`。generic 路径在 `net/core/dev.c`：`bpf_prog_run_generic_xdp`（`:5011`）、`do_xdp_generic`（`:5183`）。模式枚举 `bpf_xdp_mode` 在 `include/linux/netdevice.h:996`。收包主路径上的调用点见 [networking/05](../networking/05-rx-path.md)。

---

## 7.1 `xdp_buff` 与 `xdp_md`

```c
/* include/net/xdp.h:80 */
struct xdp_buff {
	void *data;              /* 当前包头，程序可改 */
	void *data_end;
	void *data_meta;         /* data 之前的元数据，合法时 <= data */
	void *data_hard_start;   /* 缓冲真正的起点 */
	struct xdp_rxq_info *rxq;
	struct xdp_txq_info *txq;
	u32 frame_sz;            /* 用来推 data_hard_end，尾部留给 skb_shared_info */
	u32 flags;               /* XDP_FLAGS_HAS_FRAGS 等，xdp.h:74 */
};
```

`xdp_prepare_buff`（`:130`）按 headroom 和长度填这四个指针。`data_meta` 在 `meta_valid` 为假时被设成 `data + 1`，这样 `data_meta > data`，程序看到的元数据长度是 0。帧尾留给 `skb_shared_info` 的算法是 `xdp_data_hard_end`（`:147`）：`data_hard_start + frame_sz - SKB_DATA_ALIGN(sizeof(struct skb_shared_info))`。

程序作者看到的是 `xdp_md`（`bpf.h:6321`），字段是 `__u32` 的 `data`、`data_end`、`data_meta`、`ingress_ifindex`、`rx_queue_index`、`egress_ifindex`。verifier 把前三个翻译成对 `xdp_buff` 指针的加载，后三个经过 `rxq` 或 `txq`。直接在程序里按 `xdp_buff` 的布局去量偏移是错的。

`XDP_PACKET_HEADROOM` 是 256（`bpf.h:6303`）。generic 路径如果 skb 的 headroom 小于它，或者 skb 是克隆的、非线性的，会先 `pskb_expand_head` 再 `skb_linearize`（`netif_receive_generic_xdp`，`dev.c:5115`）。原生 XDP 由驱动保证同样的 headroom，本章不进驱动的分配函数。

多帧时 `xdp->flags` 带 `XDP_FLAGS_HAS_FRAGS`。程序要访问分片，装载时带 `BPF_F_XDP_HAS_FRAGS`，`bpf_prog_load` 把它记进 `aux->xdp_has_frags`（`syscall.c:2699`）。

---

## 7.2 动作码

```c
/* include/uapi/linux/bpf.h:6310 ，从 0 递增 */
enum xdp_action {
	XDP_ABORTED = 0,
	XDP_DROP,
	XDP_PASS,
	XDP_TX,
	XDP_REDIRECT,
};
```

`bpf_prog_run_xdp`（`xdp.h:495`）在调用程序前清掉 per-cpu 的 `bpf_redirect_info`，然后 `__bpf_prog_run`。返回值原样交回调用者。bond 从设备上的 `XDP_TX` 在 `bpf_master_redirect_enabled_key` 打开时会被改成 `xdp_master_redirect`（`:511`）。

| 返回值 | generic 路径怎么处理 |
|--------|----------------------|
| `XDP_PASS` | `do_xdp_generic` 返回 `XDP_PASS`，`__netif_receive_skb_core` 继续做 VLAN、ptype、GRO 之后的协议投递 |
| `XDP_DROP` / `XDP_ABORTED` / 其他 | `netif_receive_generic_xdp` 里 `kfree_skb`（`dev.c:5137`）。非法值先 `bpf_warn_invalid_xdp_action` 和 `trace_xdp_exception`，再按丢弃处理 |
| `XDP_TX` | `generic_xdp_tx` |
| `XDP_REDIRECT` | `xdp_do_generic_redirect`。失败则 `kfree_skb_reason(..., SKB_DROP_REASON_XDP)` |

`bpf_prog_run_generic_xdp`（`:5011`）在跑程序之前把 skb 的 MAC 头摆进 `xdp->data`：`mac_len = skb->data - skb_mac_header(skb)`，`xdp_prepare_buff` 使用 `skb_headroom(skb) - mac_len`。程序返回后，如果 `data` 或 `data_end` 移动了，用 `__skb_pull` / `__skb_push` 和 `skb_set_tail_pointer` 写回 skb。以太网头被改过则重新 `eth_type_trans`。`XDP_TX` 和 `XDP_REDIRECT` 会把 MAC 头再 push 回去，因为这两条路要交出二层帧（`:5077` 的注释）。

元数据长度是 `xdp->data - xdp->data_meta`。`XDP_PASS` 且长度非 0 时 `skb_metadata_set`（`:5090`）。

---

## 7.3 三种模式

```c
/* include/linux/netdevice.h:996 */
enum bpf_xdp_mode {
	XDP_MODE_SKB = 0,    /* generic，XDP_FLAGS_SKB_MODE */
	XDP_MODE_DRV = 1,    /* 驱动 NAPI，XDP_FLAGS_DRV_MODE */
	XDP_MODE_HW  = 2,    /* offload，XDP_FLAGS_HW_MODE */
	__MAX_XDP_MODE
};
```

标志在 `include/uapi/linux/if_link.h:1300`：`XDP_FLAGS_SKB_MODE`、`DRV_MODE`、`HW_MODE`、`UPDATE_IF_NOEXIST`、`REPLACE`。`dev_xdp_mode`（`dev.c:9266`）按这些位选择模式；一个都不设时，设备有 `ndo_bpf` 就用 `XDP_MODE_DRV`，否则用 `XDP_MODE_SKB`。

`dev->xdp_state[__MAX_XDP_MODE]` 每个模式一个 `bpf_xdp_entity`（`netdevice.h` 里 `xdp_state` 数组，networking 章已经点过这个字段）。`dev_xdp_install`（`dev.c:9339`）拒绝 generic 与 driver 同时有效：错误字符串是 `Native and generic XDP can't be active at the same time`，返回 `-EEXIST`。offload 程序必须带 `HW_MODE`。绑在别的设备上的程序返回 `Program bound to different device`。`expected_attach_type` 为 `BPF_XDP_DEVMAP` 或 `BPF_XDP_CPUMAP` 的程序不能挂到设备上（`:9367`、`:9371`）。

安装时先 `bpf_prog_inc`，因为驱动假定引用已经算在自己头上、卸下时会 `bpf_prog_put`（`:9383` 的注释）。然后调用 `dev_xdp_bpf_op`（`:9277`）：

| 模式 | `bpf_op` |
|------|----------|
| `XDP_MODE_SKB` | `generic_xdp_install`（`:5859`） |
| `XDP_MODE_DRV` / `HW` | `dev->netdev_ops->ndo_bpf` |

`generic_xdp_install` 对 `XDP_SETUP_PROG` 做 `rcu_assign_pointer(dev->xdp_prog, new)`。从无到有时 `static_branch_inc(&generic_xdp_needed_key)`，并 `dev_disable_lro`、`dev_disable_gro_hw`。摘掉最后一个程序时 `static_branch_dec`。

`__netif_receive_skb_core` 在处理统计之后、VLAN 之前看这个 static key（`:5527`）：

```text
do_xdp_generic(rcu_dereference(skb->dev->xdp_prog), skb)
  返回值不是 XDP_PASS → NET_RX_DROP，离开核心函数
```

`do_xdp_generic` 调用 `netif_receive_generic_xdp`。被 redirect 再注入的 skb（`skb_is_redirected`）直接 `XDP_PASS`，避免环（`:5108`）。

driver 模式不经过这段。驱动在自己的 poll 里、组 skb 之前调用 `bpf_prog_run_xdp`。virtio-net 把程序放在接收队列的 `rq->xdp_prog`，`ndo_bpf` 是 `virtnet_xdp`，队列布局在 [networking/13](../networking/13-virtio-net-guest.md)。veth 的接收队列同样可以挂 `xdp_prog`，见 [networking/11](../networking/11-netns-veth-bridge.md)。本章不写 virtio 描述符或物理网卡 DMA 环上怎么把包切进 `xdp_buff`。驱动作者的合同是：poll 里构造 `xdp_buff`，调用 `bpf_prog_run_xdp`，按返回的 `xdp_action` 自己完成 TX 或把 redirect 交给 `xdp_do_redirect`。

`XDP_MODE_HW` 把 `netdev_bpf.command` 设成 `XDP_SETUP_PROG_HW`，仍调用 `ndo_bpf`。程序可以在装载时就 `bpf_prog_dev_bound_init`，verifier 走 `bpf_prog_offload_verifier_prep`。具体网卡把字节码变成自己的微码，不在本章。

link 路径是 `bpf_xdp_link_attach`（`dev.c:9676`）：在 rtnl 下按 `target_ifindex` 找设备，`bpf_link_init(..., BPF_LINK_TYPE_XDP, ...)`，再 `dev_xdp_attach_link`（`:9534`）。它调用 `dev_xdp_attach`（`:9435`）。link 和「直接传 prog 指针」的老 netlink 路径互斥：`link && (new_prog || old_prog)` 返回 `-EINVAL`（`:9449`）。link 只接受模式位，其他 flags 的 extack 是 `Invalid XDP flags for BPF link attachment`（`:9453`）。`XDP_FLAGS_MODES` 里置了多于一个位则 `Only one XDP mode flag can be set`（`:9458`）。一个模式位都不设、而设备上已经有多于一个程序时，`More than one program loaded, unset mode is ambiguous`（`:9463`）。关闭 link 走 `bpf_xdp_link_release`（`:9560`），在 rtnl 下 `dev_xdp_detach_link`。设备已经拆掉、`link->dev` 为空时不再 detach，注释写明这是和 `unregister_netdevice` 的竞争（`:9566`）。

---

## 7.4 redirect：devmap、cpumap、xskmap

程序调用 `bpf_redirect_map` 时，目标写进 per-cpu 的 `bpf_redirect_info`：`map_type`、`map_id`、目标指针。返回 `XDP_REDIRECT` 之后，调用者必须在同一次 softirq 里消费这份信息。`bpf_prog_run_xdp` 下次进入时会把残留清掉（`xdp.h:505`）。

`xdp_do_redirect`（`net/core/filter.c:4508`）看 `ri->map_type`。`BPF_MAP_TYPE_XSKMAP` 走 `__xdp_do_redirect_xsk`（`:4413`），里面是 `__xsk_map_redirect`。其他类型把 `xdp_buff` 转成 `xdp_frame`，走 `__xdp_do_redirect_frame`。

generic 的对应函数是 `xdp_do_generic_redirect_map`（`:4535`）：

| map | 调用 |
|-----|------|
| `DEVMAP` / `DEVMAP_HASH` | `dev_map_generic_redirect`，或带 `BPF_F_BROADCAST` 的 `dev_map_redirect_multi` |
| `XSKMAP` | `xsk_generic_rcv`，然后 `consume_skb` |
| `CPUMAP` | `cpu_map_generic_redirect` |

`bpf_devmap_val`（`bpf.h:6337`）的 `ifindex` 指定出口设备，可选再挂一个 `BPF_PROG_TYPE_XDP` 且 `expected_attach_type == BPF_XDP_DEVMAP` 的程序。`devmap` 在发送前可以再跑这个程序（`kernel/bpf/devmap.c` 里对 `bpf_prog_run_xdp` 的调用在 `:342`）。`bpf_cpumap_val`（`:6350`）带队列长度 `qsize` 和可选的 `BPF_XDP_CPUMAP` 程序。cpu map 把帧排到目标 CPU，在那边的 kthread 里再跑程序（`kernel/bpf/cpumap.c:199`）。

这两张 map 上的第二个程序不能挂到 `ndo_bpf`，`dev_xdp_install` 会拒绝。它们的 ctx 仍然是 `xdp_buff`。

`XDP_TX` 不查 map，把帧从进来的那块设备发回去。generic 实现是 `generic_xdp_tx`。driver 模式由驱动调用 `ndo_xdp_xmit`（声明在 `netdevice.h:1376`，函数指针在 `:1642`）。本章不展开某个驱动的发送环。

---

## 7.5 AF_XDP

`BPF_MAP_TYPE_XSKMAP` 的元素是 AF_XDP 套接字。redirect 进这张 map 之后，driver 路径的 `__xsk_map_redirect` 把 `xdp_buff` 交给套接字；generic 路径调用 `xsk_generic_rcv`（`net/xdp/xsk.c:379`），在 `xs->pool->rx_lock` 下进入 `__xsk_rcv`。

零拷贝时 `xdp->rxq->mem.type == MEM_TYPE_XSK_BUFF_POOL`，`xsk_rcv`（`:395`）改走 `xsk_rcv_zc`。池和 fill/completion 环的布局、`ndo_xsk_wakeup` 怎样踢驱动，属于套接字和驱动的合同。本章停在「XDP 动作是 `XDP_REDIRECT`，map 类型是 `XSKMAP`，帧进入 `xsk_rcv` / `xsk_generic_rcv`」。不写物理网卡描述符怎样和 UMEM 页面对齐。

用户态套接字是 `AF_XDP`。队列号在 bind 时选定，和 `xdp_rxq_info.queue_index` 对应。没有挂上 XDP 程序、或程序没有 redirect 到这个套接字时，AF_XDP 的接收队列是空的。

---

## 7.6 和后面协议栈的交界

`XDP_PASS` 之后 skb 才进入 [networking/05](../networking/05-rx-path.md) 里 generic XDP 调用点以下的部分：`ptype_all`、`sch_handle_ingress`、`rx_handler`、`ip_rcv`。XDP 丢弃的包不会出现在那些 tracepoint 里。抓包的 `ptype_all` 也在 generic XDP 之后，所以 generic 模式下 `tcpdump` 看不到已经被 `XDP_DROP` 的帧。driver 模式更早，驱动若在 `XDP_PASS` 之后才 `build_skb`，抓包同样看不到丢掉的帧。

tc ingress 在这之后，见 [08](./08-tc-cgroup-sock.md)。同一帧可以先被 XDP `PASS`，再被 cls_bpf 或 tcx 丢掉。两个返回值不是同一套枚举：XDP 用 `xdp_action`，tc 用 `TC_ACT_*` / `TCX_*`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
