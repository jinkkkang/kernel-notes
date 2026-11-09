# eBPF

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本机观测快照内核是 **6.12.113+**（`uname -r`，4 核）。数字是在 6.12 上采样的实测；结构体与调用链以 v6.6.160 为准。两套内核不要混成一个行号。

**一句话**：`bpf(2)` 把一段 `bpf_insn` 装进 `bpf_prog`，verifier 按程序类型给的 ctx 证明内存访问合法，通过之后 JIT（或解释器）执行；map 是程序和用户态之间的共享表，`bpf_link` 把程序接到 kprobe、tracepoint、XDP、tc、cgroup 这些挂钩上。

## 阅读顺序

```text
01 总览
  → 02 程序与指令
  → 03 verifier
  → 04 map
  → 05 BTF 与 ctx
  → 06 attach 与 bpf_link
  → 07 XDP
  → 08 tc / cgroup / sock
  → 09 helper 与同步
  → 10 bpf(2) 与 bpffs
  → 11 用户态工具
  → 12 观测
  → 13 性能配方
```

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | cBPF 与 eBPF、和 ftrace/perf 的分工、覆盖边界 | 中 |
| [02-prog-and-insn.md](./02-prog-and-insn.md) | `bpf_insn`、`bpf_prog`、`bpf_prog_load`、解释器与 JIT | **厚** |
| [03-verifier.md](./03-verifier.md) | `bpf_check` / `do_check`、剪枝、helper 白名单、拒绝日志 | **厚** |
| [04-maps.md](./04-maps.md) | `bpf_map`、array/hash/percpu/lru/lpm、map-in-map | **厚** |
| [05-btf-and-ctx.md](./05-btf-and-ctx.md) | BTF、`bpf_prog_type` 与 ctx、CO-RE | 中偏厚 |
| [06-attach-and-link.md](./06-attach-and-link.md) | `bpf_link`、kprobe / tracepoint / raw_tp / fentry | **厚** |
| [07-xdp.md](./07-xdp.md) | `xdp_buff`、generic/driver/offload、动作码、AF_XDP 薄写 | **厚** |
| [08-tc-cgroup-sock.md](./08-tc-cgroup-sock.md) | tc / tcx、cgroup-bpf、sockops、sockmap、sk_lookup、netfilter | 中 |
| [09-helpers-and-sync.md](./09-helpers-and-sync.md) | helper 分类、RCU 与自旋约束、tail call | 较薄 |
| [10-bpf-syscall-and-fs.md](./10-bpf-syscall-and-fs.md) | `bpf(2)` 命令、权限、bpffs、pin | 中 |
| [11-tooling.md](./11-tooling.md) | libbpf、bpftool、bpftrace、perf 各站哪一层 | 中 |
| [12-observability.md](./12-observability.md) | fdinfo、sysctl、verifier log、JIT 开关、bpffs | **厚** |
| [13-perf-analysis.md](./13-perf-analysis.md) | 拒载、map 争用、XDP 丢包、attach 开销；RECIPE | **厚** |
| [demos/](./demos/) | map 用户态读写、XDP test_run、kprobe / tracepoint | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| kprobe / tracepoint / raw tracepoint 的 BPF 程序类型、ctx、`bpf_link` 怎么挂上 | ftrace 环缓冲、`TRACE_EVENT` 宏展开、`register_kprobe`、优化 trampoline、function / function_graph 在 [tracing/](../tracing/README.md)。本章在 `register_fentry` 调用 `register_ftrace_direct` 处停下，direct call 的实现见 [tracing/04](../tracing/04-dynamic-ftrace.md) |
| 子系统事件的字段语义（`sched_switch`、`irq:`、`block:`） | 各章 observability。本章只说明 BPF 侧怎么读 ctx |
| seccomp 的 cBPF 过滤与 `SECCOMP_RET_*` | [containerization/10-seccomp-caps-lsm.md](../containerization/10-seccomp-caps-lsm.md)、[syscalls/09-hooks.md](../syscalls/09-hooks.md) |
| `sk_buff`、NAPI、`ndo_start_xmit`、qdisc 本体 | [networking/](../networking/README.md)。XDP / tc / sockops 的程序与动作码在本章 |
| 物理网卡描述符环上的 XDP | 与 networking 相同，不写。原生 XDP 停在 `ndo_bpf` |
| cgroup 对象、`cgroup.bpf` 文件怎么来的 | [cgroup/](../cgroup/README.md)。本章写往 `cgroup->bpf` 上挂程序 |

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/uapi/linux/bpf.h`、`bpf_common.h` | 指令、命令、map/prog/attach 枚举、helper id、UAPI ctx |
| `include/linux/bpf.h` | `bpf_map`、`bpf_prog`、`bpf_link`、`bpf_map_ops` |
| `include/linux/filter.h` | `bpf_prog_run`、栈上限、JIT 开关的内联判断 |
| `include/linux/bpf_types.h` | prog/map 类型到 ops 与 ctx 类型 |
| `include/linux/bpf_verifier.h` | `bpf_verifier_env`、日志级别 |
| `kernel/bpf/syscall.c` | `bpf(2)`、`bpf_prog_load`、`link_create` |
| `kernel/bpf/verifier.c` | `bpf_check`、`do_check` |
| `kernel/bpf/core.c` | 解释器、`bpf_prog_select_runtime` |
| `kernel/bpf/trampoline.c` | fentry / fexit / modify_return 的 trampoline |
| `kernel/trace/bpf_trace.c` | kprobe 侧 `trace_call_bpf`、raw tp 的 `bpf_trace_run` |
| `arch/x86/net/bpf_jit_comp.c` | x86-64 JIT |
| `net/core/dev.c`、`include/net/xdp.h` | generic XDP、`bpf_prog_run_xdp` |
| `net/core/filter.c` | XDP redirect、部分网络 helper |
| `net/sched/cls_bpf.c`、`include/net/tcx.h` | cls_bpf 与 tcx |
| `kernel/bpf/cgroup.c` | cgroup-bpf |
| `net/netfilter/nf_bpf_link.c` | `NF_HOOK_OP_BPF` |
| `kernel/bpf/inode.c` | bpffs |
| `tools/lib/bpf/`、`tools/bpf/bpftool/` | libbpf 与 bpftool（用户态，随内核树发布） |

## 实测 vs RECIPE

- **实测**（sampled on 6.12.113+）：[12](./12-observability.md) 里非 root 能读的 sysctl、`/sys/kernel/btf/vmlinux` 的大小、bpffs 的挂载行。root 可读的 `bpf_jit_harden` / `bpf_jit_kallsyms` / `bpf_jit_limit` 也标了当次数字。[demos/](./demos/) 里 `04-map-user`、`03-xdp-testrun`、`01-kprobe-count` 用 root 跑过，stdout 贴在 demos README。
- **RECIPE**：没有 `bpftrace`、没有 `bpftool`、没有 `perf`、没有 `ip`。kprobe / tracepoint 的 bpftrace 脚本、把 XDP 挂到 veth 上的步骤，见 [demos/README.md](./demos/README.md) 和 [13](./13-perf-analysis.md)。本进程的 capability 集是空的，`/sys/fs/bpf` 模式是 `700`，非 root 列目录得到 `Permission denied`。

## 未覆盖

- ftrace 环缓冲、`TRACE_EVENT` 展开、kprobe 断点与优化 trampoline 的实现，见 [tracing/](../tracing/README.md)
- seccomp cBPF 的返回码语义
- 各子系统 tracepoint 的字段含义
- 物理网卡驱动里 XDP 的 DMA 环
- 硬件 offload 的具体网卡微码
- LSM hook 的安全语义（`BPF_PROG_TYPE_LSM` 只在 attach 表里点到）

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
