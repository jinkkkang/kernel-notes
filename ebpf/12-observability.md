# 12. 观测

**结论**：先看三个 sysctl：无特权装载是否关掉、JIT 是否打开、统计是否打开。然后看程序 fd 的 fdinfo，里面有 `prog_id`、`jited` 和 `verified_insns`。verifier 为什么拒绝，只存在于装载当时的 `log_buf` 里，内核不会为已拒绝的程序留一份日志。

> fdinfo 的格式在 `bpf_prog_show_fdinfo`（`kernel/bpf/syscall.c:2264`）。JIT 开关在 `net/core/sysctl_net_core.c:460`。无特权开关和 `bpf_stats_enabled` 在 `kernel/bpf/syscall.c:5832`。日志级别在 `include/linux/bpf_verifier.h:580`。

---

## 12.1 本机能直接读到的

sampled on 6.12.113+。非 root 读到：

```text
/proc/sys/kernel/unprivileged_bpf_disabled = 2
/proc/sys/kernel/bpf_stats_enabled        = 0
/proc/sys/net/core/bpf_jit_enable         = 1
```

同一时刻 root 读到（`bpf_jit_harden` 的 mode 是 `0600`，非 root 是 `Permission denied`）：

```text
/proc/sys/net/core/bpf_jit_harden   = 0
/proc/sys/net/core/bpf_jit_kallsyms = 1
/proc/sys/net/core/bpf_jit_limit    = 528482304
```

`mount` 里有一行：`bpf on /sys/fs/bpf type bpf (rw,nosuid,nodev,noexec,relatime,mode=700)`。非 root `ls /sys/fs/bpf` 得到 `Permission denied`。`/sys/kernel/btf/vmlinux` 对所有用户可读，大小 5530600 字节。`/sys/kernel/tracing` 对非 root 是 `Permission denied`。kprobe PMU 的 type 文件可读，值是 8；tracepoint PMU 的 type 是 2。这两个 type 是 perf 子系统的编号，和 v6.6 源码里的 `bpf_prog_type` 枚举不是一回事。

`unprivileged_bpf_disabled` 的 0/1/2 在 [10](./10-bpf-syscall-and-fs.md)。本机是 2：无特权的 `bpf()` 被拒绝，管理员仍可改这个值。`bpf_jit_enable` 为 1 表示 JIT 打开；为 2 时内核会打生产环境不该使用的警告（`sysctl_net_core.c:344`）。`bpf_jit_harden` 为 0 表示不做常量加固（`bpf_jit_blinding_enabled`，`include/linux/filter.h:1131`）。

`bpf_stats_enabled` 为 0 时，`bpf_prog_run` 不累加 `run_time_ns`。要看 fdinfo 里的运行时间，需要把它写成 1，或对有权限的进程发 `BPF_ENABLE_STATS`。写这个 sysctl 要 `CAP_SYS_ADMIN`（`bpf_stats_handler`，`syscall.c:5787`）。

---

## 12.2 程序 fdinfo

`bpf_prog_fops.show_fdinfo` 指向 `bpf_prog_show_fdinfo`（`:2294`）。在 `/proc/<pid>/fdinfo/<fd>` 里能看到：

```text
prog_type:          enum bpf_prog_type 的数值
prog_jited:         0 或 1
prog_tag:           insn 的 tag，十六进制
memlock:            prog->pages << PAGE_SHIFT
prog_id:
run_time_ns:        仅当统计键打开，否则一直是 0
run_cnt:
recursion_misses:   prog->active 挡掉的次数
verified_insns:     verifier 的 insn_processed
```

[demos/03-xdp-testrun](./demos/03-xdp-testrun.c) 在 `BPF_PROG_TEST_RUN` 之后、关闭 fd 之前读了自己的 fdinfo。当次（sampled on 6.12.113+，root）是：

```text
prog_type:	6
prog_jited:	1
prog_tag:	49e2f9eefcc1f0cd
memlock:	4096
prog_id:	20
run_time_ns:	0
run_cnt:	0
recursion_misses:	0
verified_insns:	12
```

`prog_type` 6 是 `BPF_PROG_TYPE_XDP`（`enum bpf_prog_type` 从 `UNSPEC` = 0 数，`bpf.h:974`）。`prog_jited` 为 1，和本机 `bpf_jit_enable = 1` 一致。`verified_insns` 12 等于这段手写程序的指令条数（`BPF_LD_MAP_FD` 占两条）。`map0` 从 0 变成 1，说明程序确实跑过；`run_cnt` 仍是 0，因为 `bpf_stats_enabled` 是 0，`bpf_prog_run` 不累加 `bpf_prog_stats`。`prog_id` 是这次启动里 idr 分配到的号，下次会变。

`recursion_misses` 来自 `bpf_prog_inc_misses_counter`。raw tracepoint 在 `__bpf_trace_run` 里发现 `prog->active` 已经不是 1 时累加它（`kernel/trace/bpf_trace.c:2318`）。XDP 和 tc 的重入如果走的是别的计数器（例如 `bpf_prog_active`），不会出现在这一行。读的时候对着挂钩看：raw tp 的丢失在这里，kprobe 路径的 `trace_call_bpf` 用的是另一把 per-cpu 计数，返回 0 并且不调用程序（`bpf_trace.c:116`）。

map 的 fdinfo 在 `bpf_map_show_fdinfo`（`syscall.c:805`），包含类型、key/value 大小、`max_entries`、id 和名字。link 的 fdinfo 由 `bpf_link_ops.show_fdinfo` 自己打印，XDP link 会带 ifindex 和 flags。

这些文件描述的是某个进程正握着的 fd。对象被 pin 到 bpffs 之后，即使原进程退出，只要 pin 还在，另一个进程 `BPF_OBJ_GET` 再打开，fdinfo 就还在。没有 pin、也没有残留 fd 时，内核不再保留这份文本。

---

## 12.3 verifier 日志

日志只在 `BPF_PROG_LOAD` 的那一次系统调用里写进用户提供的 `log_buf`。级别：

```c
/* include/linux/bpf_verifier.h:580 */
#define BPF_LOG_LEVEL1  1   /* 每条指令 */
#define BPF_LOG_LEVEL2  2   /* 每次跳转都打寄存器状态 */
#define BPF_LOG_STATS   4   /* 结束时的状态计数 */
#define BPF_LOG_FIXED   8   /* 缓冲不够则装载失败 */
```

`BPF_LOG_LEVEL` 是 `LEVEL1 | LEVEL2`。排障时用 `LEVEL1 | STATS` 通常够看是哪一条指令的哪一个寄存器被拒绝。`LEVEL2` 的输出按路径展开，容易超过几兆字节。用户态传入的 `union bpf_attr` 长度盖过 `log_true_size` 时，内核把完整日志长度写回去（`verifier.c:20923`），用来判断缓冲是不是被截断了。

成功装载之后，`verified_insns` 留在 fdinfo 里，完整的状态跟踪不留。想复现一次拒绝，要用同一段指令、同一个 `log_level` 再装一次。程序类型不同，同一段指令的结果不同，因为 helper 白名单和 ctx 字段不一样。

`BPF_F_TEST_STATE_FREQ` 让剪枝更勤，只对特权装载生效（`bpf_check`，`verifier.c:20827`）。它用来逼出和剪枝相关的 verifier 问题，会改变 `verified_insns`，不要拿它和默认装载的条数对比性能。

---

## 12.4 bpftool、bpffs、kallsyms

本机没有 bpftool。下面是 RECIPE，对应 v6.6 树里 `tools/bpf/bpftool` 的子命令，需要 `CAP_BPF` 以及被查看对象的权限：

```text
# RECIPE：本机未安装 bpftool，没有跑
bpftool prog show
bpftool map show
bpftool link show
bpftool btf dump file /sys/kernel/btf/vmlinux format raw | head
```

`prog show` 的 id 和 fdinfo 的 `prog_id` 是同一个 idr。`jited` 列对应 `prog_jited`。JIT 镜像在 `bpf_jit_kallsyms` 为 1 且程序已经 JIT 时会出现在 `/proc/kallsyms`，名字来自 `bpf_prog_kallsyms_add`（`bpf_prog_load` 在分配 id 之后调用，`syscall.c:2779`）。本机该 sysctl 为 1。demos 里的程序随进程退出关闭 fd，没有 pin，不会留在 bpffs 上。

bpffs 挂载点是 `bpf_init` 创建的（`kernel/bpf/inode.c:812`）。查看 pin 住的对象：

```text
# RECIPE：本机 /sys/fs/bpf 模式 700，非 root 不能列目录
# 需要 CAP_SYS_ADMIN 或目录的写权限才能 pin
ls /sys/fs/bpf
bpftool prog show pinned /sys/fs/bpf/<name>
```

`BPF_OBJ_GET_INFO_BY_FD` 比 fdinfo 多 map id 列表和 JIT 长度。bpftool 打出来的 `xlated` 指令是 verifier 改写之后的，`jited` 是 JIT 镜像的反汇编。两者都来自一次 `BPF_OBJ_GET_INFO_BY_FD`，不是把内核函数再解释一遍。

---

## 12.5 和 tracepoint 的关系

`/sys/kernel/tracing/events/.../enable` 打开的是 ftrace 的事件。它不列出已经 attach 的 BPF 程序。BPF 挂上 raw tracepoint 之后，事件可以在 ftrace 里仍是关闭的：raw tracepoint 的调用点由 `bpf_probe_register` 登记，不依赖 tracefs 的 enable 文件。

反过来，`perf` 或 bpftrace 打开 `tracepoint:sched:sched_switch` 时，内核会把该事件的跟踪打开。那是 perf 的订阅，不是 BPF 程序自己写了 enable。非 root 连事件目录都列不了，本机实测是 `Permission denied`。打开方式不要抄进生产机器的配方里不加能力说明；本章的跟踪命令一律 RECIPE，见 [13](./13-perf-analysis.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
