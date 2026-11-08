# 11. 用户态工具

**结论**：内核只认 `bpf(2)` 和 perf event 的 ioctl。libbpf 把 ELF 里的程序、map 和 CO-RE 重定位变成这些系统调用；bpftool 用同一套系统调用查询和 pin；bpftrace 与 `perf` 是更上面的前端，一个生成程序，一个提供 kprobe / tracepoint 的 event fd。

> libbpf 的装载入口 `bpf_object__load` 声明在 `tools/lib/bpf/libbpf.h:229`。CO-RE 应用在 `tools/lib/bpf/relo_core.c`。bpftool 的命令表在 `tools/bpf/bpftool/main.c:76`。`PERF_EVENT_IOC_SET_BPF` 在 `include/uapi/linux/perf_event.h:555`。

---

## 11.1 libbpf

`bpf_object__load` 的注释（`libbpf.h:223`）说明它把 BPF 对象装进内核。调用之前，对象来自 `bpf_object__open` 读到的 ELF：`.text` 里是 `bpf_insn`，`.maps` 描述 map，`.BTF` 和 `.BTF.ext` 带类型，`.rel` 里是 `bpf_core_relo`。

装载顺序在库里是：用 `relo_core.c` 对照 `/sys/kernel/btf/vmlinux`（或用户指定的 BTF）改写指令的 `imm`，然后对每张 map 发 `BPF_MAP_CREATE`，把返回的 fd 填进 `BPF_PSEUDO_MAP_FD` 那两条指令，再 `BPF_PROG_LOAD`。verifier 日志由调用者提供的缓冲接住。成功后每个程序和 map 都有 fd，`bpf_object__load` 返回 0。

attach 不在 `bpf_object__load` 里自动完成。程序段名（`SEC("kprobe/...")`、`SEC("xdp")` 这一类）由 libbpf 映射到 [06](./06-attach-and-link.md) 的某种 `BPF_LINK_CREATE` 或 perf event。`bpf_program__attach` 返回的是对 `bpf_link` fd 的封装。关闭 link 就摘掉挂钩，和直接拿着 link fd 调用 `close` 一样。

库也提供不经 ELF 的薄封装：`bpf_map_create`、`bpf_prog_load`，参数对应 `union bpf_attr` 的字段。本章的 [demos/](./demos/) 不链接 libbpf，直接用 `syscall(__NR_bpf)`，以便在没有开发包的机器上编译。指令布局和 libbpf 发出的是同一套。

skelton 头文件（`bpf_object__load_skeleton`，`libbpf.h:1567`）只是把 open、load、attach 收成一组生成好的函数，内核侧没有额外对象。

---

## 11.2 bpftool

bpftool 随内核树放在 `tools/bpf/bpftool`。顶层命令（`main.c:76`）：

```text
help, batch, prog, map, link, cgroup, perf, net,
feature, btf, gen, struct_ops, iter, version
```

`prog show` 的处理函数登记在 `prog.c:2511` 的 `{ "show", do_show }`。它遍历程序 id（`BPF_PROG_GET_NEXT_ID`），再 `BPF_PROG_GET_FD_BY_ID` 和 `BPF_OBJ_GET_INFO_BY_FD`，把类型、名字、tag、挂到的 map 打出来。`map dump` 对支持的类型循环 `BPF_MAP_GET_NEXT_KEY` 和 `BPF_MAP_LOOKUP_ELEM`。`prog load` / `map pin` 走 `BPF_OBJ_PIN`。

`btf dump` 读一份 BTF fd 或 `/sys/kernel/btf/vmlinux`，按 `btf_header` 打印类型。排障时用它核对 CO-RE 要找的字段还在不在，比猜偏移可靠。

`net` 子命令看 XDP 和 tc 挂在哪块设备上，数据来自 netlink，不是 `bpf_link` idr 的全集。`link show` 才是 `BPF_LINK_GET_NEXT_ID`。一块设备上的 generic XDP 若是用 netlink 而不是 link 挂的，会出现在 `net` 里，不一定出现在 `link show` 里。

`feature` 探测当前内核开了哪些 `bpf_prog_type` 和 helper。探测方法是试装一个最小程序，所以它需要和真正装载一样的能力。没有 `CAP_BPF` 时探测结果是一串 `-EPERM`，不能当成「内核没编这个类型」。

本机没有安装 bpftool。命令行的用法记在 [12](./12-observability.md) 和 [13](./13-perf-analysis.md)，标成 RECIPE。

---

## 11.3 perf 与 bpftrace

kprobe 和 tracepoint 的 event fd 来自 `perf_event_open`。`ioctl(PERF_EVENT_IOC_SET_BPF, prog_fd)` 把已经 `BPF_PROG_LOAD` 的程序接到这个 event 上。`perf` 用户态工具在 `perf record` / `perf trace` 里替你做这两步，也可以只打开 event、让别的进程提供程序 fd。

bpftrace 不在内核树里。它把脚本编译成 BPF，再走 libbpf 或自己的装载器，attach 的仍是 [06](./06-attach-and-link.md) 里的 kprobe、tracepoint 或 raw tracepoint。脚本里的 `args->prev_pid` 这类名字来自 tracepoint 的字段，和 `TP_STRUCT__entry` 一致。本仓库其他章里的 `*.bt` 是这种前端的 RECIPE，本章 [demos/02-tp-sched-switch.bt](./demos/02-tp-sched-switch.bt) 同样如此。

三者的分工：

| 工具 | 产生 BPF 指令 | 打开挂钩 | 适合 |
|------|----------------|----------|------|
| libbpf | 编译器产出的 ELF，库来装 | `bpf_link` 或 perf ioctl | 长期运行的程序 |
| bpftool | 不产生，只装已有的 ELF | `prog attach` / pin | 查看、pin、试装 |
| perf | 不产生 | `perf_event_open` | 采样、提供 kprobe/tracepoint fd |
| bpftrace | 自己编译脚本 | 库内部的 attach | 临时的计数和直方图 |

观测一个已经在跑的程序时，优先用 bpftool 和 fdinfo，而不是再挂一个 kprobe 到 `bpf_prog_run` 上。后者本身又是一个 BPF 程序，会改变 [09](./09-helpers-and-sync.md) 里的重入计数。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
