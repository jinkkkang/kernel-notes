# 9. 性能分析配方

**结论**：调度性能问题先分类，再定点测量。先回答「慢在睡、慢在等 CPU、还是慢在被掐」，再用 `perf` / ftrace / `bpftrace` 下钻。本章的命令都是 **RECIPE**：笔记作者的机器上 `perf` 和 `bpftrace` 都没装，这些命令没有跑出过输出。和 [08](./08-observability.md) 里 sampled on 6.12 的 `/proc` 数字分开看。

事件名都在 v6.6.160 的 `include/trace/events/sched.h` 里：`sched_switch`、`sched_wakeup`、`sched_wakeup_new`、`sched_waking`、`sched_migrate_task`。

---

## 9.1 方法论

```text
1. 现象：尾延迟？吞吐掉？单核打满？
2. 定性：
     - CPU 饱和？（util / PSI / 火焰图 on-CPU）
     - 调度延迟？（runnable 等待）
     - 自愿睡眠？（锁、IO、poll）
     - cgroup 节流？（cpu.stat 的 nr_throttled）
3. 只对症下钻，避免一上来就改 nice 或绑核
```

| 定性结果 | 下一跳 |
|---|---|
| on-CPU 算满 | 算法或锁持有过久——偏应用与 locking 章 |
| runnable 久等 | 本章：竞争、亲和、均衡、RT 抢占 |
| 睡在 D/S | IO / 锁。先别调调度器 |
| `nr_throttled` 涨 | [07](./07-cgroup-cpu.md)：quota |

---

## 9.2 RECIPE：`perf sched`

需要：`perf`、合适的 `perf_event_paranoid` 或 root、内核打开了 sched tracepoint。

```bash
# 采样一段时间
perf sched record -o /tmp/perf.data -- sleep 5
# 唤醒→运行延迟排行
perf sched latency -i /tmp/perf.data | head -40
# 时间线
perf sched timehist -i /tmp/perf.data | head -50
# 每个 CPU 上谁在跑
perf sched map -i /tmp/perf.data | head -30
```

读 `latency` 时看 **最大延迟** 和 **平均**。只盯平均会把尾延迟盖住。

---

## 9.3 RECIPE：`perf trace` / 事件

```bash
# 看 switch / wakeup（时间短、负载高时输出巨大）
perf trace -e sched:sched_switch,sched:sched_wakeup -- sleep 1
# 或 record + script
perf record -e sched:sched_switch -a -- sleep 2
perf script | head -50
```

`sched_stat_wait` 这一族要内核编了 `CONFIG_SCHEDSTATS` 并且运行时打开了 `kernel.sched_schedstats`。没开时事件是空的，别把「没有输出」理解成「没有人在等」。

---

## 9.4 RECIPE：`bpftrace` 唤醒→切换直方图

完整脚本见 [`demos/wakeup_latency.bt`](./demos/wakeup_latency.bt)。思路：

- `tracepoint:sched:sched_wakeup` 和 `sched_wakeup_new` 用 `args->pid` 记下时间戳  
- `tracepoint:sched:sched_switch` 在 `args->next_pid` 命中时算差值，`hist` 出微秒  

[`demos/sched_switch_hist.bt`](./demos/sched_switch_hist.bt) 按 `args->prev_state` 计数。v6.6.160 里这个字段是 `long`：0 表示切走时仍是 `TASK_RUNNING` 且不是抢占；带 `TASK_REPORT_MAX`（`0x100`）表示被抢占（trace 打印成 `R+`）；其它位是 `S` / `D` 等睡眠。

**作者的机器上没有 bpftrace，这两个脚本都没跑过。**

---

## 9.5 常见误判

| 误判 | 实际情况 |
|---|---|
| loadavg 高 = CPU 不够 | 里面包含不可打断睡眠。要看 `/proc/stat` 的 idle 和 PSI |
| 火焰图很满 = 调度器有问题 | 多数是应用自己在 on-CPU。调度器自身应很瘦 |
| 绑死单核一定降延迟 | 可能把排队关进一颗核，尾延迟更差 |
| nice 或 `cpu.weight` 能绕开 cgroup throttle | throttle 看的是带宽 `cpu.max`，不是权重 |
| 平均调度延迟正常 = 没问题 | 看最大值。交互线程被偶发迁走也会把尾延迟拉高 |
| 没开 schedstat 却信「精确 wait 指标」 | `sched_stat_*` 可能根本没有数据。先看 `CONFIG_SCHEDSTATS` 和 `kernel.sched_schedstats` |

---

## 9.6 场景小册

### A. 尾延迟高，平均还好

1. RECIPE：`perf sched latency` 看 max  
2. 查迁移是不是在跳（`sched_migrate_task`）  
3. 查 RT 进程、中断亲和、cpuset 是不是太窄  
4. 查是不是周期性 throttle（毛刺和 `cpu.max` 的 period 对齐）

### B. 单核 100%，整机利用率低，业务慢

1. 看该线程 `/proc/<pid>/stat` 的 `processor` 是不是一直同一颗  
2. `Cpus_allowed_list` / cpuset  
3. 业务本身是不是单线程。调度器搬不走这种上限

### C. 迁移抖动

1. RECIPE：短时间 `perf script` 看 `sched:sched_migrate_task` 的 `orig_cpu` / `dest_cpu`  
2. 是不是在无意义地跨 NUMA  
3. 要用 `sched_setaffinity` 或 cpuset 收紧的话，先量再改

### D. 怀疑 cgroup CPU 节流

1. `cpu.max`，以及 `cpu.stat` 的 `nr_periods` / `nr_throttled` / `throttled_usec`  
2. 加压时利用率是不是顶在 quota  
3. 对照 [07](./07-cgroup-cpu.md)

---

## 9.7 哪些是样本，哪些是配方

| 内容 | 状态 |
|---|---|
| PSI、loadavg、`/proc/stat`、那台机器的 Kconfig | **MEASURED**，sampled on 6.12，见 [08](./08-observability.md) |
| `perf sched` / `bpftrace` 的输出 | **未测**。命令和脚本是 RECIPE |
| EEVDF 的路径和行号 | **对照 v6.6.160 源码核对**。运行样本的内核是 6.12.94+ |

辅助脚本：[`demos/capture-sched.sh`](./demos/capture-sched.sh)。在有权限的机器上它会采集一份和 08 节同结构的快照，并多看一眼 debugfs `sched/`。

---

**导航**：[← 观测](./08-observability.md) · [章节目录](./README.md) · [其它调度类 →](./06-other-classes.md)
