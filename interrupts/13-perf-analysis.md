# 13. 性能分析配方

**结论**：先把现象分成「硬中断太密」「softirq 来不及」「中断打在错误的 CPU 上」三类，再对着 tracepoint 看时间，不要一上来就改亲和。下面的命令都是 **RECIPE**：本环境没有对着 v6.6.160 跑出过输出。

事件名以 [12](./12-observability.md) 的头文件为准。

---

## 13.1 先分类

```text
1. /proc/interrupts 里是哪一行在涨？设备 IRQ，还是 RES / CAL / 定时器？
2. /proc/softirqs 里 NET_RX / TIMER / SCHED 是否和它一起涨？
3. 同一行是不是只打在一颗 CPU 上，而这颗 CPU 的 si 或 hi 已经接近 100%？
4. 这颗 CPU 上有没有 ksoftirqd 占着时间？
```

| 分类 | 下一跳 |
|---|---|
| 某一设备 IRQ 每秒极高，处理函数很短 | 13.2 风暴。先看是不是没有合并、没有 NAPI |
| IRQ 计数一般，但 `NET_RX` 或 `TIMER` 很高，`ksoftirqd` 忙 | 13.3 softirq 饱和 |
| 多颗 CPU 空闲，一颗上堆着设备中断 | 13.4 亲和。先读 `effective_affinity`，再决定写不写 |
| `RES` 高、设备 IRQ 不高 | 调度 IPI。看 scheduler 章，不在这里调中断 |

`/proc/stat` 的 `hi` 是硬中断时间，`si` 是 softirq 时间。只有 `hi` 高时去看 `irq_handler_*`；`si` 高时去看 `softirq_*`。

---

## 13.2 硬中断风暴

```bash
# RECIPE：哪条 IRQ 的处理函数最频繁
perf record -e irq:irq_handler_entry -a -- sleep 3
perf script | head -40
```

读的时候按 `name=` 计数，而不是按平均值。风暴是某一两个名字占了绝大多数 `entry`。

```bash
# RECIPE：entry 到 exit 的耗时。脚本没有在本机跑过
# 事件字段以 include/trace/events/irq.h 为准：entry 有 irq，exit 有 irq 和 ret
perf record -e irq:irq_handler_entry,irq:irq_handler_exit -a -- sleep 2
perf script
```

若 `exit` 紧挨着 `entry`，处理函数本身不慢，问题是次数。网卡应已经在硬中断里 `napi_schedule`（[09](./09-deferred-work-choice.md)），然后把中断线 mask 住，等 `net_rx_action`（`net/core/dev.c:6880`）轮询。硬中断次数仍然等于包数时，驱动没有进入轮询，或者 `poll` 马上把中断打开了。

`ret` 一直是未处理：共享线上有一家设备在空转，或者向量送到了没有 `action` 的描述符。后者还会有 `No irq handler for vector`（`arch/x86/kernel/irq.c:264`），那是另一件事。

---

## 13.3 softirq 饱和与 ksoftirqd

`handle_softirqs` 在时间超过 `MAX_SOFTIRQ_TIME`（`msecs_to_jiffies(2)`，`kernel/softirq.c:499`）或重启超过 `MAX_SOFTIRQ_RESTART`（10，`:500`）或 `need_resched()` 时 `wakeup_softirqd`（`:601`）。所以看到 `ksoftirqd` 说明这一轮 softirq 主动让出来了，不是线程自己闲逛。

```bash
# RECIPE
perf record -e irq:softirq_entry,irq:softirq_exit -a -- sleep 2
perf script | head -50
```

`vec` 是 [08](./08-softirq-tasklet.md) 的枚举：3 是 `NET_RX`，1 是 `TIMER`，7 是 `SCHED`。`entry` 到 `exit` 很长，就去看对应的 `action`：`net_rx_action`、`run_timer_softirq`（`kernel/time/timer.c:2032`）或调度软中断。

```bash
# RECIPE：ksoftirqd 在不在跑。pid 从 ps 读，不要猜
ps -eo pid,comm,psr | grep ksoftirqd
```

非 RT 且没有强制中断线程时，大量 softirq 仍在硬中断出口的 `__do_softirq` 里跑（`softirq.c:443`），不一定进 `ksoftirqd`。这时 `si` 高、`ksoftirqd` 却不忙。别用「ksoftirqd 不忙」否定 softirq 饱和。

`force_irqthreads()` 为真时，`invoke_softirq` 改成只唤醒 `ksoftirqd`（`:445` 的条件）。硬中断会变短，延迟挪到 `ksoftirqd` 被调度上的那一刻。

---

## 13.4 亲和

先读，再写。

```bash
# RECIPE
grep . /proc/irq/<N>/smp_affinity_list /proc/irq/<N>/effective_affinity_list
```

`effective_affinity` 才是 APIC 里的目标（[12](./12-observability.md)）。只有一颗 CPU，而 `smp_affinity_list` 是一个范围，说明向量域还没（或不能）在范围里摊开。托管中断（debugfs `irqs/N` 里 `is_managed: 1`）不要手写 `smp_affinity`，掩码是 `irq_create_affinity_masks` 生成的。

写：

```bash
# RECIPE：把非托管中断钉到 CPU 2。写完再读 effective_affinity_list
echo 2 > /proc/irq/<N>/smp_affinity_list
```

写完若 dmesg 出现 `Affinity broken due to vector space exhaustion`（`vector.c:424`），目标 CPU 的向量窗没有空位，核心换了别的 CPU。这时去看 VECTOR 矩阵的 `avl` 列，而不是再写一次。

多队列网卡更常见的做法是少开队列，而不是把所有 IRQ 钉死：

```bash
# RECIPE
ethtool -L <ifname> combined <N>
```

队列数下来，MSI-X 条数下来，每 CPU 的 `act` 才会下来。分配失败的整条链在 [07](./07-irq-alloc-failure.md)。

---

## 13.5 ftrace 与 bpftrace 的形状

```bash
# RECIPE：只跟踪一个 IRQ 号。irq_handler_entry 的字段名以 trace 头文件为准
trace-cmd record -e irq:irq_handler_entry -f "irq == 42" -e irq:irq_handler_exit -f "irq == 42" sleep 1
trace-cmd report | head
```

没有 `trace-cmd` 时用 `perf record` 再 `perf script`，事件名相同。

bpftrace 一类的工具若存在，直方图的思路是：在 `irq:irq_handler_entry` 记下时间戳，在 `irq:irq_handler_exit` 做差。本环境没有跑过这类脚本，这里不给一份假装执行过的程序。字段至少包括 entry 的 `irq` 与 exit 的 `irq`（`include/trace/events/irq.h:53` 和 `:83`）。

系统向量用 `irq_vectors:local_timer_entry` / `local_timer_exit`，不要和 `irq:irq_handler_*` 混在一张图里：定时器中断不经过 `common_interrupt` 的 `irqaction`。

---

## 13.6 误判

| 误判 | 源码里实际是 |
|---|---|
| `/proc/interrupts` 里定时器那一行很高 = 设备风暴 | `arch_show_interrupts` 把 `LOCAL_TIMER_VECTOR` 单独列出。HZ 或 NO_HZ 恢复 tick 时它本来就会涨 |
| softirq 计数高 = 必须绑核 | 计数来自 `kstat_incr_softirqs_this_cpu`（`softirq.c:575`）。先看 `si` 和 `ksoftirqd` 的 CPU 时间 |
| 把中断摊到所有 CPU 一定更好 | 托管中断和 posted interrupt 依赖窄掩码。摊开可能触发向量耗尽，或破坏 `vmx_pi_update_irte` 的单 CPU 目标（`posted_intr.c:306` 的注释） |
| `ksoftirqd` 不在就没有 softirq 问题 | 非 RT 的 `invoke_softirq` 常常直接 `__do_softirq` |
| 平均处理时间正常 = 没有风暴 | 风暴看的是 `entry` 次数。每次 1 微秒、每秒一百万次，平均值仍然很短 |

---

**导航**：[← 观测](./12-observability.md) · [目录](./README.md)
