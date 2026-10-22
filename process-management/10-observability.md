# 10. 观测与调试清单

**结论**：下列命令默认**不在**用户机器上执行；需要时自行跑。bpftrace 字段务必先 `bpftrace -lv` 核对。

## 10.1 `/proc` 与 ps

```bash
# 状态字母、tgid、pid、ppid
ps -eo pid,tid,ppid,tgid,stat,comm | head

# 单任务
cat /proc/<pid>/status | egrep 'Name|State|Tgid|Pid|PPid|NSpid|Threads'
cat /proc/<pid>/sched
ls /proc/<pid>/task                    # 线程组内所有 tid
cat /proc/<pid>/ns/pid; readlink /proc/<pid>/ns/pid

# 僵尸
ps -eo pid,ppid,stat,comm | awk '$3 ~ /Z/'
```

`NSpid` 字段可看到多层 pid ns 中的号（对应 `struct pid->numbers[]`）。

## 10.2 tracepoint（sched / task / signal）

```bash
# 列出
bpftrace -lv 'tracepoint:sched:sched_process_*'
bpftrace -lv 'tracepoint:signal:*'

# fork / exit（字段名以 -lv 为准）
bpftrace -e 'tracepoint:sched:sched_process_fork { printf("parent %d -> child %d\n", args->parent_pid, args->child_pid); }'
bpftrace -e 'tracepoint:sched:sched_process_exit { printf("exit pid %d\n", args->pid); }'
```

已核实：内核里 `trace_sched_process_exit(tsk, group_dead)` 在 `exit.c:957`。**bpftrace 导出的字段名可能与内核函数参数不完全同名**，以 `-lv` 为准。

## 10.3 perf / ftrace 模板

```bash
# 函数图（需 root，注意开销）
echo function_graph > /sys/kernel/tracing/current_tracer
echo kernel_clone > /sys/kernel/tracing/set_graph_function
# … 复现后
cat /sys/kernel/tracing/trace | head -100

perf probe -a 'kernel_clone'
perf record -e probe:kernel_clone -a -- sleep 5
```

## 10.4 信号观测

```bash
cat /proc/<pid>/status | egrep 'SigQ|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt'
# kill -0 测存在；strace -e signal,kill -p <pid>
```

## 10.5 对照源码的建议姿势

1. 先定版本（本章钉死 **v7.3**）
2. elixir 打开文件，搜索函数名
3. 观测只验证「发生了没有」，细节仍以源码为准

---

---

**导航**：[章节目录](./README.md) · [← 信号与进程](./09-signals.md)
