# ebpf demos

对照 v6.6.160 的 `bpf(2)`、`BPF_MAP_TYPE_ARRAY`、`BPF_PROG_TYPE_XDP` 和 kprobe 的 perf attach。字节码是手写的 `bpf_insn`，不链接 libbpf。编译用的是宿主机的 `linux/bpf.h`（本机内核 6.12.113+），用到的命令和 opcode 在 v6.6.160 的 uapi 里已经有。

非 root 的 capability 集是空的，`/proc/sys/kernel/unprivileged_bpf_disabled` 为 2。下面三个 C 程序用 root 跑。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `04-map-user.c` | `BPF_MAP_CREATE` / `UPDATE` / `LOOKUP` | 实测，sampled on 6.12.113+ |
| `03-xdp-testrun.c` | XDP 程序返回 `XDP_DROP`，`BPF_PROG_TEST_RUN` 一帧，map 加一 | 实测。没有挂网卡 |
| `01-kprobe-count.c` | `perf_event_open` kprobe `__x64_sys_getpid`（cpu 0），`PERF_EVENT_IOC_SET_BPF` | 实测 |
| `02-tp-sched-switch.bt` | bpftrace 按 `sched_switch` 的 `prev_state` 计数 | **RECIPE**。没有 bpftrace |

把 XDP 挂到 veth 上的 `ip link set ... xdpgeneric` 也是 RECIPE：本机没有 `ip`。步骤在 [13](../13-perf-analysis.md)。

```text
make
sudo ./04-map-user
sudo ./03-xdp-testrun
sudo ./01-kprobe-count
```

当次 stdout（root，sampled on 6.12.113+）：

```text
map_fd=3 key=0 value=42
prog_fd=4 retval=1 map0=1
prog_type:	6
prog_jited:	1
prog_tag:	49e2f9eefcc1f0cd
memlock:	4096
prog_id:	20
run_time_ns:	0
run_cnt:	0
recursion_misses:	0
verified_insns:	12
symbol=__x64_sys_getpid hits=100
```

`retval=1` 是 `XDP_DROP`。fdinfo 是同一次 `03-xdp-testrun` 在关闭程序 fd 前读到的；`prog_id` 每次装载都会变。`run_cnt` 为 0 是因为 `bpf_stats_enabled` 为 0，map 的 `map0=1` 说明程序跑过。kprobe 打开在 cpu 0 上，紧循环里的 100 次 `getpid` 都落在这次计数里。

`02-tp-sched-switch.bt`：

```text
# RECIPE：没有 bpftrace，未跑
bpftrace 02-tp-sched-switch.bt
```
