# 1. 总览

**结论**：经典 cBPF 是给套接字过滤用的累加器机器；eBPF 是 10 个 64 位寄存器的指令机，由 `bpf(2)` 装载，verifier 通过之后才能跑。观测事件的基础设施在 [tracing/](../tracing/README.md)，本章写 BPF 程序怎么被装进去、怎么挂到已经存在的挂钩上。

> cBPF 指令在 `include/uapi/linux/filter.h:24`。eBPF 指令在 `include/uapi/linux/bpf.h:72`，类编码的公共宏在 `include/uapi/linux/bpf_common.h`。系统调用入口是 `kernel/bpf/syscall.c:5599` 的 `SYSCALL_DEFINE3(bpf, ...)`。

---

## 1.1 两条指令机

cBPF 的一条指令是 `sock_filter`：16 位 `code`、两个 8 位跳转偏移 `jt`/`jf`、一个 32 位 `k`。程序只有一个累加器 A 和辅助寄存器 X，条件跳转同时带「真」和「假」两个偏移。`setsockopt(SO_ATTACH_FILTER)` 走的就是这份格式。内核在 `bpf_convert_filter`（`net/core/filter.c:569`）里把它译成 eBPF，再交给同一套运行时。

eBPF 的一条指令是 `bpf_insn`：8 位 `code`、4 位目的寄存器、4 位源寄存器、16 位有符号 `off`、32 位有符号 `imm`。寄存器是 `BPF_REG_0` 到 `BPF_REG_10`（`bpf.h:54`）。类位在 `bpf_common.h`：`BPF_LD` / `LDX` / `ST` / `STX` / `ALU` / `JMP` 占 `code` 的低 3 位；eBPF 另外用 `BPF_JMP32`（`0x06`）和 `BPF_ALU64`（`0x07`），定义在 `bpf.h:17`。`BPF_RET` 和 `BPF_MISC` 留在经典编码里，eBPF 用 `BPF_JMP | BPF_EXIT` 返回。

seccomp 仍然吃 cBPF。过滤器怎么串、`SECCOMP_RET_*` 是什么意思，在 [containerization/10](../containerization/10-seccomp-caps-lsm.md) 和 [syscalls/09](../syscalls/09-hooks.md)。本章不重写那条链。

---

## 1.2 一次装载在内核里经过什么

```text
bpf(BPF_PROG_LOAD)                         syscall.c:5599
  __sys_bpf                                :5458
    bpf_prog_load                          :2596
      能力检查、拷贝 insn 与 license
      find_prog_type                       :2057   按 bpf_prog_type 填 ops
      bpf_check                            verifier.c:20757
      bpf_prog_select_runtime              core.c:2447
        解释器或 bpf_int_jit_compile
      bpf_prog_alloc_id / bpf_prog_new_fd
```

程序类型决定三件事：ctx 的 C 类型（`include/linux/bpf_types.h`）、verifier 允许的 helper（`bpf_verifier_ops.get_func_proto`）、以及稍后能挂到哪一种 attach。类型枚举在 `bpf.h:974`，从 `BPF_PROG_TYPE_SOCKET_FILTER` 排到 `BPF_PROG_TYPE_NETFILTER`。

map 是另一条 `bpf()` 命令，`BPF_MAP_CREATE` 进 `map_create`（`syscall.c:1138`）。程序里的 `BPF_LD_MAP_FD` 在 verifier 里被解析成 map 指针；用户态用同一张 fd 做 lookup / update。

挂上执行点有两条用户态路径。老路径是 `BPF_PROG_ATTACH`（cgroup、部分网络）或 `perf_event_open` 加 `PERF_EVENT_IOC_SET_BPF`（`include/uapi/linux/perf_event.h:555`）。新路径是 `BPF_LINK_CREATE`，内核造一个 `bpf_link`，fd 关闭时按 `bpf_link_ops.release` 摘掉。两种路径的分发在 [06](./06-attach-and-link.md)。

---

## 1.3 和 ftrace、perf、bpftrace 的位置

ftrace 的环缓冲、`tracepoint` 宏、kprobe 断点是事件的生产端。BPF 是消费端里的一种：程序在事件发生时被调用，返回值有的被挂钩解释（XDP 的动作码、kprobe 的过滤返回值），有的只是顺带执行（计数进 map）。

| 角色 | 本章怎么对待 |
|------|----------------|
| ftrace / kprobe 的内核实现 | [tracing/](../tracing/README.md)。本章写 BPF 侧 attach 和 ctx |
| `perf` | 打开 kprobe / tracepoint 的 perf event，再把 `bpf_prog` 设进去 |
| bpftrace | 用户态前端，把脚本变成 BPF 程序再走上面的装载。源码不在内核树里 |
| libbpf / bpftool | 随内核树的 `tools/lib/bpf`、`tools/bpf/bpftool`。见 [11](./11-tooling.md) |

`trace_sys_enter` 在系统调用入口的第 4 步，顺序写在 [syscalls/09](../syscalls/09-hooks.md)。用 `BPF_PROG_TYPE_RAW_TRACEPOINT` 去订 `sys_enter` 时，本章只说明 `bpf_raw_tracepoint_open` 和 ctx 里的 `u64` 参数；tracepoint 本身怎么进环缓冲，见 [tracing/06](../tracing/06-tracepoint-and-TRACE_EVENT.md)。

---

## 1.4 本章写到哪

写：指令格式、`bpf_prog` 的装载和 JIT 入口、verifier 的状态与剪枝、map 的类型和锁、BTF/CO-RE 的数据布局、`bpf_link` 的 attach 表、XDP 从 `xdp_buff` 到动作码、tc/cgroup/sock/netfilter 的 BPF 挂钩、helper 与 tail call、bpffs 与权限、观测和排障配方。

不写：ftrace 环、`register_kprobe`、函数图 tracer（见 [tracing/](../tracing/README.md)）、seccomp 的 cBPF 语义、某个子系统事件的业务含义、物理网卡的描述符环。XDP 在驱动里的入口停在 `ndo_bpf`（`include/linux/netdevice.h:1640`）。virtio-net 队列上的 `rq->xdp_prog` 已经在 [networking/13](../networking/13-virtio-net-guest.md) 点过名，程序和动作码在 [07](./07-xdp.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
