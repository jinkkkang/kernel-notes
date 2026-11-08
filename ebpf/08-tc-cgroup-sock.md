# 8. tc、cgroup 与套接字 BPF

**结论**：tc 在 v6.6 有两条 BPF 入口。tcx 用 `bpf_link` 挂 `BPF_PROG_TYPE_SCHED_CLS`，在 `tcx_run` 里执行；经典 cls_bpf 仍是一个 tc classifier，由 `tc_run` 调到 `cls_bpf_classify`。cgroup-bpf 把程序挂在 `cgroup->bpf` 上，sockops 从 TCP 回调进 `__cgroup_bpf_run_filter_sock_ops`。netfilter 的 BPF link 登记的 `hook_ops_type` 是 `NF_HOOK_OP_BPF`。

> 收包侧调用点 `sch_handle_ingress` 在 `net/core/dev.c:4142`，发包侧 `sch_handle_egress` 在 `:4198`。cls_bpf 的分类函数在 `net/sched/cls_bpf.c:81`。cgroup 执行在 `kernel/bpf/cgroup.c`。netfilter link 在 `net/netfilter/nf_bpf_link.c:210`。skb 和 qdisc 本体见 [networking/](../networking/README.md)。

---

## 8.1 tcx 与 cls_bpf

`BPF_PROG_TYPE_SCHED_CLS` 的 ctx 是 `__sk_buff`，内核传入 `sk_buff`（`bpf_types.h:6`）。`BPF_PROG_TYPE_SCHED_ACT` 共用同一对 ops。返回值要和 tc 的动作码对齐。

tcx 的用户态返回值（`bpf.h:6292`）：

```c
enum tcx_action_base {
	TCX_NEXT     = -1,   /* 交给链上的下一个，也是未知返回值的归宿 */
	TCX_PASS     = 0,
	TCX_DROP     = 2,
	TCX_REDIRECT = 7,
};
```

注释写明这些数和 `TC_ACT_*` 保持一致：`TC_ACT_UNSPEC` 是 -1，`TC_ACT_OK` 是 0，`TC_ACT_SHOT` 是 2，`TC_ACT_REDIRECT` 是 7（`include/uapi/linux/pkt_cls.h:63`）。`tcx_action_code`（`include/net/tcx.h:150`）把 `TCX_NEXT` 和任何其他值收成 `TCX_NEXT`，`TCX_PASS` 时顺手把 `tc_classid` 写进 `skb->tc_index`。

`tcx_run`（`dev.c:4121`）沿着 `bpf_mprog_entry` 里的程序调用 `bpf_prog_run`。ingress 在调用前 `__skb_push` 到 MAC 头，返回前再 pull。某个程序的返回值不是 `TCX_NEXT` 就停。`static_branch` `tcx_needed_key` 为假时跳过这段，直接 `tc_run`。

`sch_handle_ingress` 的结构：

```text
entry = dev->tcx_ingress          空则直接返回 skb，本函数什么都不做
tcx_needed_key 为真：tcx_run
  返回值不是 TC_ACT_UNSPEC：按 verdict 处理
否则 tc_run(entry)
verdict：
  TC_ACT_REDIRECT → skb_do_redirect
  TC_ACT_SHOT     → kfree_skb_reason(SKB_DROP_REASON_TC_INGRESS)
  STOLEN / QUEUED / TRAP / CONSUMED → 消耗 skb
```

`tc_run`（`:4068`）取出 `entry->miniq`。没有 mini qdisc 就返回 `TC_ACT_UNSPEC`。有的话走 `tcf_classify`，cls_bpf 作为 classifier 注册在这条链上。`cls_bpf_classify`（`cls_bpf.c:81`）对链表里的每个 `cls_bpf_prog` 调用 `bpf_prog_run_data_pointers`。ingress 时先 push MAC 头再跑，和 tcx 一样把二层头暴露给程序。`exts_integrated` 时返回值经 `cls_bpf_exec_opcode` 变成 `TC_ACT_*`；否则返回 0 表示不匹配，继续下一个 classifier。

egress 对称，在 `sch_handle_egress`（`:4198`），丢弃原因是 `SKB_DROP_REASON_TC_EGRESS`。qdisc 本身的入队和 `ndo_start_xmit` 在 [networking/06](../networking/06-tx-path-and-qdisc.md)。

link 创建时 `BPF_PROG_TYPE_SCHED_CLS` 进 `tcx_link_attach`（`link_create` 的 switch，`syscall.c:5101`）。`struct tcx_link`（`include/net/tcx.h:20`）保存设备和 location。经典 cls_bpf 不走这个 link，它跟 tc filter 的生命周期，由 rtnetlink 安装。

`BPF_PROG_TYPE_SCHED_CLS` 在 `is_net_admin_prog_type` 里，装载需要 `CAP_NET_ADMIN` 或 `CAP_SYS_ADMIN`，再加上 `CAP_BPF`。

---

## 8.2 cgroup-bpf

`BPF_PROG_ATTACH` 对 cgroup 类型调用 `cgroup_bpf_prog_attach`（`kernel/bpf/cgroup.c:1205`）：`cgroup_get_from_fd(target_fd)`，再 `cgroup_bpf_attach`。`BPF_F_ALLOW_MULTI` 加上 `BPF_F_REPLACE` 时，用 `replace_bpf_fd` 指定被换掉的程序。

attach 标志的合同在 `bpf.h:1092` 的注释里：

| 标志 | 行为 |
|------|------|
| 默认 | 这棵子树不再允许往下挂 |
| `BPF_F_ALLOW_OVERRIDE` | 子 cgroup 挂了程序之后，父程序让位 |
| `BPF_F_ALLOW_MULTI` | 子程序和父程序都跑 |

link 形式是 `cgroup_bpf_link_attach`，`link_create` 对 `CGROUP_SKB`、`CGROUP_SOCK`、`CGROUP_SOCK_ADDR`、`SOCK_OPS`、`CGROUP_DEVICE`、`CGROUP_SYSCTL`、`CGROUP_SOCKOPT` 都进这里。

执行时从 cgroup 上的程序数组往父节点走。入口是 `bpf_prog_run_array_cg`。和网络数据路径相关的几次：

| 挂钩 | 程序类型 | 典型 attach |
|------|----------|-------------|
| inet ingress / egress | `CGROUP_SKB` | `BPF_CGROUP_INET_INGRESS` / `EGRESS` |
| 建套接字 | `CGROUP_SOCK` | `BPF_CGROUP_INET_SOCK_CREATE` |
| bind / connect / sendmsg | `CGROUP_SOCK_ADDR` | `BPF_CGROUP_INET4_BIND` 等 |
| setsockopt / getsockopt | `CGROUP_SOCKOPT` | `BPF_CGROUP_SETSOCKOPT` |
| 设备访问 | `CGROUP_DEVICE` | `BPF_CGROUP_DEVICE`，ctx 是 `bpf_cgroup_dev_ctx` |
| sysctl | `CGROUP_SYSCTL` | `BPF_CGROUP_SYSCTL` |

`CGROUP_SKB` 的返回值决定包是否继续。它和 XDP、tcx 的枚举不同，ctx 仍是 `__sk_buff`。cgroup 对象本身、层级和 `subtree_control` 在 [cgroup/](../cgroup/README.md)。`struct cgroup` 里的 `bpf` 成员只在这里被程序数组使用。

`__cgroup_bpf_check_dev_permission`（`cgroup.c:1609`）构造 `bpf_cgroup_dev_ctx`：`access_type` 把 access 放在高 16 位、dev_type 放在低 16 位，再加上 `major` / `minor`。这是 cgroup 设备控制的 BPF 形态，不是 LSM 的 `BPF_LSM_MAC`。

---

## 8.3 sockops、sockmap、sk_lookup

sockops 的程序类型是 `BPF_PROG_TYPE_SOCK_OPS`，ctx 是 `bpf_sock_ops`（用户态）和 `bpf_sock_ops_kern`（内核），见 [05](./05-btf-and-ctx.md)。attach 类型 `BPF_CGROUP_SOCK_OPS`。

宏 `BPF_CGROUP_RUN_PROG_SOCK_OPS`（`include/linux/bpf-cgroup.h:328`）在 `cgroup_bpf_enabled(CGROUP_SOCK_OPS)` 且能找到 full socket 时调用 `__cgroup_bpf_run_filter_sock_ops`（`cgroup.c:1598`）。监听套接字上的 request_sock 用旁边的 `BPF_CGROUP_RUN_PROG_SOCK_OPS_SK`（`:318`），因为 syncookie 路径上 `sk_to_full_sk` 找不回 listener。

`bpf_sock_ops.op` 区分回调。`bpf_sock_ops_cb_flags_set`（`net/core/filter.c:5813`）把程序写回的标志存进 `tcp_sk(sk)->bpf_sock_ops_cb_flags`，掩码是 `BPF_SOCK_OPS_ALL_CB_FLAGS`。TCP 栈根据这些标志决定后续是否再调用。`tcp_sock` 里这个字段的位置在 [networking/08](../networking/08-tcp.md)。

sockmap 有两张表：`BPF_MAP_TYPE_SOCKMAP` 和 `SOCKHASH`。用户态更新不走通用的 `map_update_elem`，`bpf_map_update_value` 专门调 `sock_map_update_elem_sys`（`syscall.c:166`）。插进去的是套接字 fd。

挂在 sockmap 上的程序类型：

| 类型 | attach | 作用 |
|------|--------|------|
| `SK_SKB` | `BPF_SK_SKB_STREAM_PARSER` / `STREAM_VERDICT` / `VERDICT` | 字节流切分和判决 |
| `SK_MSG` | `BPF_SK_MSG_VERDICT` | `sendmsg` 路径上的 `sk_msg`，ctx 是 `sk_msg_md` |
| `SK_REUSEPORT` | `BPF_SK_REUSEPORT_SELECT` 等 | reuseport 选择 |

判决的用户态值是 `SK_DROP = 0`、`SK_PASS = 1`（`bpf.h:6358`）。`sk_msg` 的数据在 `data` 与 `data_end` 之间，和 XDP 一样由 verifier 做范围检查。

`BPF_PROG_TYPE_SK_LOOKUP` 按网络命名空间挂，`netns_bpf_link_create`。IPv4 的运行入口是 `bpf_sk_lookup_run_v4`（`include/linux/filter.h:1462`）：从 `net->bpf.run_array[NETNS_BPF_SK_LOOKUP]` 取出程序数组，填 `bpf_sk_lookup_kern`（协议、四元组、`ingress_ifindex`），`BPF_PROG_SK_LOOKUP_RUN_ARRAY` 执行。程序选出的 `struct sock *` 写回调用者。它替换的是「这个五元组该落到哪个套接字」，不改路由表。

---

## 8.4 netfilter

`BPF_PROG_TYPE_NETFILTER` 的 ctx 就是 `struct bpf_nf_ctx`（`bpf_types.h:83`，两侧同类型）。`bpf_nf_link_attach`（`net/netfilter/nf_bpf_link.c:210`）在 `bpf_nf_check_pf_and_hooks` 通过后填写 `nf_hook_ops`：

```c
link->hook_ops.hook = nf_hook_run_bpf;
link->hook_ops.hook_ops_type = NF_HOOK_OP_BPF;   /* include/linux/netfilter.h:94 */
link->hook_ops.priv = prog;
link->hook_ops.pf = attr->link_create.netfilter.pf;
link->hook_ops.priority = attr->link_create.netfilter.priority;
link->hook_ops.hooknum = attr->link_create.netfilter.hooknum;
```

`flags` 非 0 则 `-EINVAL`。`BPF_F_NETFILTER_IP_DEFRAG` 置位时再调 `bpf_nf_enable_defrag`。五个 `NF_INET_*` 挂钩的位置在 [networking/12](../networking/12-netfilter-hooks.md)。本章只说明：这条 ops 的 `hook` 是 `nf_hook_run_bpf`，`hook_ops_type` 让 netfilter 知道它来自 BPF，priority 和 hooknum 由 link 的属性决定。iptables 规则语法不在这里。

`NF_HOOK` 在链为空时直接调用 `okfn`。BPF 程序挂上之后，链非空，`nf_hook_slow` 会走到 `nf_hook_run_bpf`。返回值用的是 netfilter 的 `NF_DROP` / `NF_ACCEPT` 这一组，不是 XDP 的动作码。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
