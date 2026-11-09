# 5. BTF 与上下文

**结论**：程序类型在 `bpf_types.h` 里绑两套 ctx：用户态按 UAPI 结构写偏移，内核运行时传入的是另一套结构。verifier 的 `convert_ctx_accesses` 做这个翻译。BTF 描述这两种结构以及 map value 里的锁和指针；CO-RE 的重定位记录在装载前由 libbpf 应用到指令的 `imm` 上。

> 类型表在 `include/linux/bpf_types.h`。BTF 头在 `include/uapi/linux/btf.h:11`。装载命令 `bpf_btf_load` 在 `kernel/bpf/syscall.c:4836`。CO-RE 记录是 `struct bpf_core_relo`（`include/uapi/linux/bpf.h:7332`），kind 枚举在 `:7268`。

---

## 5.1 BTF 里有什么

```c
/* include/uapi/linux/btf.h:11 */
struct btf_header {
	__u16	magic;       /* BTF_MAGIC 0xeB9F */
	__u8	version;     /* BTF_VERSION 1 */
	__u8	flags;
	__u32	hdr_len;
	__u32	type_off;    /* 相对本头结束处 */
	__u32	type_len;
	__u32	str_off;
	__u32	str_len;
};
```

类型区是一串 `struct btf_type`。`BTF_KIND_STRUCT` 的值是 4（`btf.h:64`），后面跟成员：名字在字符串区的偏移、类型 id、位偏移。内核把自己的类型放在 `/sys/kernel/btf/vmlinux`，`bpf_get_btf_vmlinux()` 在 verifier 启动时取出（`bpf_check` 里调用）。这份 BTF 损坏时，`bpf_check` 打 `in-kernel BTF is malformed` 并放弃依赖它的检查。

`BPF_BTF_LOAD` 把用户态的一份 BTF 做成 fd。程序的 `aux->btf` 指向它，map 的 `btf_key_type_id` / `btf_value_type_id` 指向同一份里的类型。value 里出现 `bpf_spin_lock`、`bpf_timer`、`kptr` 时，`map_check_btf` 根据这些成员填 `bpf_map.record`。没有 BTF，verifier 拒绝在这份 value 上使用 `bpf_spin_lock`（日志要求 map 带 BTF，`verifier.c:7645`）。

fentry 一类程序在装载时给出 `attach_btf_id`。`bpf_prog_load` 在没有 `attach_prog_fd` 时用 vmlinux BTF（`syscall.c:2662`）。`aux->attach_func_proto` 是这个 id 的函数原型，参数布局要和程序的 R1–R5 对上。

---

## 5.2 程序类型和两套 ctx

`BPF_PROG_TYPE(id, ops名, 用户态 ctx, 内核 ctx)` 在 `bpf_types.h` 展开进 verifier ops 和类型检查。用户态 ctx 是程序作者 include 的那个结构，偏移必须稳定；内核 ctx 是挂钩真正传进 `bpf_prog_run` 的指针。两者同名同布局时，`convert_ctx_access` 可以是直接的字段搬运；不同名时，UAPI 里的一个 `__u32` 字段会变成若干条加载。

和本章后面几节相关的行：

| 类型 | 用户态 ctx | 内核 ctx | 出处 |
|------|------------|----------|------|
| `SOCKET_FILTER` | `struct __sk_buff` | `struct sk_buff` | `bpf_types.h:3` |
| `SCHED_CLS` / `SCHED_ACT` | `__sk_buff` | `sk_buff` | `:6`、`:8` |
| `XDP` | `struct xdp_md` | `struct xdp_buff` | `:12` |
| `CGROUP_SKB` | `__sk_buff` | `sk_buff` | `:14` |
| `SOCK_OPS` | `struct bpf_sock_ops` | `struct bpf_sock_ops_kern` | `:29` |
| `SK_MSG` | `struct sk_msg_md` | `struct sk_msg` | `:33` |
| `KPROBE` | `bpf_user_pt_regs_t` | `struct pt_regs` | `:40` |
| `TRACEPOINT` | `__u64` | `u64` | `:41` |
| `RAW_TRACEPOINT` | `struct bpf_raw_tracepoint_args` | `u64` | `:45` |
| `TRACING` | `void *` | `void *` | `:49`，由 attach 的 BTF 原型决定 |
| `SK_LOOKUP` | `struct bpf_sk_lookup` | `struct bpf_sk_lookup_kern` | `:68` |
| `NETFILTER` | `struct bpf_nf_ctx` | `struct bpf_nf_ctx` | `:84`，`CONFIG_NETFILTER_BPF_LINK` |

x86 上 `bpf_user_pt_regs_t` 是通用定义 `typedef struct pt_regs`（`include/uapi/asm-generic/bpf_perf_event.h:7`）。kprobe 程序读的就是这份寄存器。断点怎样写进指令流，见 [tracing/08](../tracing/08-kprobes-core.md)。

`struct __sk_buff`（`bpf.h:6101`）的前段是稳定字段：`len`、`mark`、`protocol`、`ifindex`、`data`、`data_end`、`data_meta`。`data` / `data_end` 在程序里是指针，verifier 把它们标成 `PTR_TO_PACKET` 和 `PTR_TO_PACKET_END`。直接解引用 `sk_buff` 的内核字段不在这张 UAPI 上，那是 `BPF_PROG_TYPE_TRACING` 加 BTF 才能做的事。

```c
/* include/uapi/linux/bpf.h:6321 ，XDP 程序看到的 ctx */
struct xdp_md {
	__u32 data;
	__u32 data_end;
	__u32 data_meta;
	__u32 ingress_ifindex; /* 实际读 rxq->dev->ifindex */
	__u32 rx_queue_index;  /* rxq->queue_index */
	__u32 egress_ifindex;  /* txq->dev->ifindex */
};
```

内核侧的 `xdp_buff`（`include/net/xdp.h:80`）用的是真正的指针：`data`、`data_end`、`data_meta`、`data_hard_start`，外加 `rxq`。程序写 `xdp_md.data` 的加载，在 `convert_ctx_accesses` 之后变成对 `xdp_buff` 这些字段的加载。`ingress_ifindex` 还要再经过 `rxq`。所以 UAPI 里连续的 `__u32` 并不等于内核结构的内存布局。

`bpf_sock_ops`（`bpf.h:6629`）的第一个字段是 `op`，标明这次回调的原因。后面的 `remote_ip4`、`snd_cwnd`、`srtt_us` 只在 `is_fullsock` 非 0 时有意义，头文件注释写明否则读出来是 0。内核对象是 `bpf_sock_ops_kern`，TCP 栈在调用前填好。

raw tracepoint 的 UAPI 是：

```c
/* bpf.h:6979 */
struct bpf_raw_tracepoint_args {
	__u64 args[0];
};
```

`__bpf_trace_run`（`kernel/trace/bpf_trace.c:2314`）把 `u64 *args` 交给 `bpf_prog_run`。`args[0]` 起对应 tracepoint `TP_ARGS` 的参数，按 64 位存放。`BPF_PROG_TYPE_TRACEPOINT` 的类型表把 ctx 记成 `__u64`；它走 perf event，读到的是该事件 `TP_STRUCT__entry` 的记录。字段名以事件头文件为准，宏怎么展开见 [tracing/06](../tracing/06-tracepoint-and-TRACE_EVENT.md)。`sched_switch` 的记录布局在 `include/trace/events/sched.h:222`，调度章的观测节用同一份字段。

`BPF_PROG_TYPE_TRACING` 的 ctx 是 `void *`。fentry 的 R1 起是目标函数的参数，类型来自 `attach_btf_id` 的函数原型。fexit 还可以读返回值。这个类型的 `expected_attach_type` 在装载时就定成 `BPF_TRACE_FENTRY`、`BPF_TRACE_FEXIT`、`BPF_MODIFY_RETURN` 或 `BPF_TRACE_RAW_TP`，attach 时必须一致（`link_create` 里的检查，见 [06](./06-attach-and-link.md)）。

---

## 5.3 CO-RE

内核升级会挪动结构体成员。CO-RE 把「我要的是这个字段」记成重定位，而不是把偏移写死在立即数里。

```c
/* include/uapi/linux/bpf.h:7268 */
enum bpf_core_relo_kind {
	BPF_CORE_FIELD_BYTE_OFFSET = 0,  /* 字段字节偏移 */
	BPF_CORE_FIELD_BYTE_SIZE   = 1,
	BPF_CORE_FIELD_EXISTS      = 2,
	BPF_CORE_FIELD_SIGNED      = 3,
	BPF_CORE_FIELD_LSHIFT_U64  = 4,  /* 位域 */
	BPF_CORE_FIELD_RSHIFT_U64  = 5,
	BPF_CORE_TYPE_ID_LOCAL     = 6,
	BPF_CORE_TYPE_ID_TARGET    = 7,
	BPF_CORE_TYPE_EXISTS       = 8,
	BPF_CORE_TYPE_SIZE         = 9,
	BPF_CORE_ENUMVAL_EXISTS    = 10,
	BPF_CORE_ENUMVAL_VALUE     = 11,
	BPF_CORE_TYPE_MATCHES      = 12,
};
```

```c
/* :7332
 * insn_off 是要改的指令在程序里的字节偏移，改的是那条指令的 imm。
 * type_id 是「根」类型在配套 BTF 里的 id。
 * access_str_off 指向 BTF 字符串区：字段重定位的字符串是用冒号分开的成员下标。
 */
struct bpf_core_relo {
	__u32 insn_off;
	__u32 type_id;
	__u32 access_str_off;
	enum bpf_core_relo_kind kind;
};
```

头文件注释写明这份记录从 LLVM 到 libbpf，再从 libbpf 到内核。应用重定位的代码在 `tools/lib/bpf/relo_core.c`：它读目标内核的 BTF（通常是 vmlinux），算出当前的偏移或「字段是否存在」，写回指令的 `imm`。存在性检查（`BPF_CORE_FIELD_EXISTS`）让同一份字节码在字段被删掉时走另一条分支，而不是带一个错偏移进 verifier。

内核 verifier 看到的是已经填好 `imm` 的指令。它不重新解释 `bpf_core_relo`。所以 CO-RE 的失败通常发生在用户态加载之前：目标 BTF 里没有这个类型，或访问字符串对不上。程序若绕过 libbpf、自己把偏移写成常数，verifier 只检查这个常数对当前 ctx 是否越界，不会帮你对 vmlinux 的字段名。

tracing 程序直接按 BTF id 读内核结构时，同样依赖这份 vmlinux BTF。`bpf.h:966` 的注释说明 `KPROBE`、`TRACEPOINT`、`PERF_EVENT`、`RAW_TRACEPOINT` 没有稳定的 UAPI：内核结构一变，旧程序就可能读错。CO-RE 是用户态用来跟上这个变化的办法，不是 verifier 里的第二种类型系统。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
