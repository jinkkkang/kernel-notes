# tracing demos

对照 v6.6.160 的 tracefs：`set_ftrace_filter`、`current_tracer`、`events/sched/sched_switch`、`kprobe_events`。脚本在 instance `kn-demo` 里做，做完删除 instance。顶层 `kprobe_events` 只在 03 里短暂出现，结束时删掉。

非 root 的 capability 集是空的，tracefs 模式是 `700`。下面用 root 跑。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `01-function-filter.sh` | instance 里只跟踪 `do_sys_openat2`，读 `trace` | 实测，sampled on 6.12.113+ |
| `02-sched-switch-event.sh` | 打开 `sched:sched_switch`，读几行 | 实测，sampled on 6.12.113+ |
| `03-kprobe-events.sh` | `p:myprobe __x64_sys_getpid`，enable 后调 `getpid` | 实测，sampled on 6.12.113+ |
| `04-function-graph.sh` | 对 `do_sys_openat2` 开 function_graph | **RECIPE**。默认直接退出 |

```text
sudo ./01-function-filter.sh
sudo ./02-sched-switch-event.sh
sudo ./03-kprobe-events.sh
# 04 默认不跑。确认要付 graph 的开销再：
# sudo TRACE_RUN_GRAPH=1 ./04-function-graph.sh
```

当次 stdout 摘录（root，sampled on 6.12.113+）。pid 和时间戳每次都会变。

`01` 的事件行：

```text
cat-3802    [003] .....   250.031103: do_sys_openat2 <-__x64_sys_openat
head-3803   [002] .....   250.031279: do_sys_openat2 <-__x64_sys_openat
```

同一时刻顶层 `enabled_functions`：

```text
do_sys_openat2 (1)    tramp: 0xffffffffc0203000 (function_trace_call+0x0/0x110) ->function_trace_call+0x0/0x110
```

`02`：

```text
bash-3799  [001] d..2.  250.182904: sched_switch: prev_comm=bash prev_pid=3799 prev_prio=120 prev_state=S ==> next_comm=swapper/1 next_pid=0 next_prio=120
sleep-3808 [003] d..2.  250.183478: sched_switch: prev_comm=sleep prev_pid=3808 prev_prio=120 prev_state=S ==> next_comm=swapper/3 next_pid=0 next_prio=120
```

`03`：

```text
p:kprobes/myprobe __x64_sys_getpid
python3-3811 [000] .....  250.309356: myprobe: (__do_sys_getpid+0x0/0x30)
```

`format` 里 `__probe_ip` 在 offset 8、8 字节。打印成 `__do_sys_getpid` 是当次 kallsyms 的符号名。删掉之后 `kprobe_events` 长度为 0。顶层 `current_tracer` 保持 `nop`。

`04`：

```text
# RECIPE：未设置 TRACE_RUN_GRAPH=1，不打开 function_graph
```
