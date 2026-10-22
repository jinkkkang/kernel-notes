# 9. 性能分析配方

**结论**：调度性能问题 = **分类** + **定点测量**。先回答「慢在睡、慢在等 CPU、还是慢在被掐」，再用 `perf` / ftrace / `bpftrace` 下钻。本章给可抄配方；**在笔记作者的 box 上 `perf`/`bpftrace` 均未安装**——下列命令标为 **RECIPE（推测可跑，需你机器有工具与权限）**，与 [08](./08-observability.md) 的 MEASURED 区分。

---

## 9.1 方法论

```text
1. 现象：尾延迟？吞吐掉？单核打满？
2. 定性：
     - CPU 饱和？（util / PSI / 火焰图 on-CPU）
     - 调度延迟？（runnable 等待）
     - 自愿睡眠？（锁、IO、poll）
     - cgroup 节流？（cpu.stat throttled）
3. 只对症下钻，避免一上来就「调 nice / 乱绑核」
```

| 定性结果 | 下一跳 |
|---|---|
| on-CPU 算满 | 算法/锁持有过久（持锁自旋）——偏应用与 locking 章 |
| runnable 久等 | 本章：竞争、亲和、均衡、RT 抢占 |
| 睡在 D/S | IO / 锁 —— 不是先调调度器 |
| throttled 涨 | [07](./07-cgroup-cpu.md)：quota |

---

## 9.2 RECIPE：`perf sched`

需：`perf`、通常 root / `perf_event_paranoid` 合适、内核带 tracepoint。

```bash
# 采样一段时间
perf sched record -o /tmp/perf.data -- sleep 5
# 唤醒→运行延迟排行
perf sched latency -i /tmp/perf.data | head -40
# 时间线
perf sched timehist -i /tmp/perf.data | head -50
# 也可
perf sched map -i /tmp/perf.data | head -30
```

读 `latency` 时：关注 **最大延迟** 与 **平均**；只盯平均值会掩盖尾延迟。

---

## 9.3 RECIPE：`perf trace` / 事件

```bash
# 看 switch / wakeup（时间短、负载高时输出巨大）
perf trace -e sched:sched_switch,sched:sched_wakeup -- sleep 1
# 或 record + script
perf record -e sched:sched_switch -a -- sleep 2
perf script | head -50
```

---

## 9.4 RECIPE：`bpftrace` 唤醒→切换直方图

完整脚本见 [`demos/wakeup_latency.bt`](./demos/wakeup_latency.bt)。思路：

- `tracepoint:sched:sched_wakeup` 记 `comm`/pid → 时间戳  
- `tracepoint:sched:sched_switch` 当 next 命中时算 delta，`@us = hist(delta)`  

另附 [`demos/sched_switch_hist.bt`](./demos/sched_switch_hist.bt)：按 prev_state 粗分「自愿切出 vs 被抢占」。

**本 box：未跑通（无 bpftrace）——标 SPECULATED recipe。**

---

## 9.5 常见误判

| 误判 | 实际情况 |
|---|---|
| loadavg 高 = CPU 不够 | 含不可打断睡眠；要看 `/proc/stat` idle% 与 PSI |
| 火焰图很「满」= 调度器有 bug | 多数是应用 on-CPU；调度器自身应很瘦 |
| 绑死单核一定降延迟 | 可能制造岛内排队，尾延迟更差 |
| nice 能解决 cgroup throttle | throttle 是带宽，不是权重能绕开 |
| 平均调度延迟正常 = 没问题 | 看 P99/max；交互线程被偶发迁走也会炸尾延迟 |
| 没开 schedstat 却信「精确 wait 指标」 | 指标可能恒为 0；先查 Kconfig |

---

## 9.6 场景小册

### A. 尾延迟高，平均还好

1. `perf sched latency` 看 max  
2. 查是否迁移狂跳（`sched_migrate_task`）  
3. 查 RT 进程、irq 亲和、cpuset 过窄  
4. 查是否周期性 throttle（period 对齐）

### B. 单核 100%，整机利用率低，业务慢

1. 看该线程 `psr` 是否钉死  
2. `Cpus_allowed_list` / cpuset  
3. 是否单线程业务逻辑本身 —— 调度器救不了

### C. 迁移抖动

1. 短时间 `perf script -e sched:sched_migrate_task`  
2. 是否无意义的跨 NUMA  
3. 适度 `sched_setaffinity` 或 cpuset **有意**收紧（先测后改）

### D. 怀疑 cgroup CPU 节流

1. `cpu.max`、`cpu.stat` 的 `nr_throttled` / `throttled_usec`  
2. 加压时 util 是否顶在 quota  
3. 对照 [07](./07-cgroup-cpu.md)

---

## 9.7 MEASURED vs SPECULATED（本章汇总）

| 内容 | 状态 |
|---|---|
| PSI、loadavg、`/proc/stat`、配置项是否开启 | **MEASURED** on box（见 [08](./08-observability.md)） |
| `perf sched` / `bpftrace` 输出 | **未测**；脚本为 RECIPE |
| EEVDF 源码路径与行号 | **对照 v7.3 树核对**（非运行中 box 内核） |

辅助脚本：[`demos/capture-sched.sh`](./demos/capture-sched.sh) —— 在你有权限的机器上采集一份与 08 节同结构的快照。

---

**导航**：[← 观测](./08-observability.md) · [章节目录](./README.md) · [其它调度类 →](./06-other-classes.md)
