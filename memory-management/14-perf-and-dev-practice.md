# 14. 按症状排障

**结论**：先用 `/proc` 判断是泄漏、外碎片、直接回收，还是 OOM / THP / balloon。trace 和 bpf 是第二步。本机能跑的只有用户态小示例和只读 `/proc`；perf、bpftrace、KVM、balloon 操作标 RECIPE。

> 快照内核 6.12.94+。源码行号仍是 v7.3。
> 导航：[上一节 观测](./13-observability.md) · [目录](./README.md)

---

## 14.1 泄漏：slab 涨，或用户页涨

### 内核对象（slab）

症状：`SUnreclaim` 单调涨，重启才掉；`MemAvailable` 掉，`AnonPages` 不明显。

```text
# 实测：本机非 root 读 /proc/slabinfo 失败（Permission denied）。
# 总量仍可读：
grep -E 'Slab|SReclaimable|SUnreclaim' /proc/meminfo
```

快照：`Slab 338408 kB`，其中不可回收 `81472 kB`。这是一个时点，不是泄漏曲线。要判断泄漏，隔几分钟再读，看差值。

```text
# RECIPE：需要 root。slabinfo 的列见 04-slab.md。
sudo slabtop -s c
sudo awk 'NR>2 {print}' /proc/slabinfo | sort -k3 -n | tail
# 对象名对得上调用点之后，开 kmemleak（内核要 CONFIG_DEBUG_KMEMLEAK，很多发行版内核没编）：
# echo scan > /sys/kernel/debug/kmemleak
# cat /sys/kernel/debug/kmemleak
```

kmemleak 标 RECIPE：本机没有确认 debugfs 里有 `kmemleak`，也不该在这台 box 上打开调试分配器。

用户自己的 cache：看名字是不是还在 slabtop 里，对象数是否等于进程还持有的。模块示例在 [demos/kmod_slab_cache.c](./demos/kmod_slab_cache.c)，不在本机 insmod。

### 用户页

症状：`AnonPages` 涨，`Cached` 不涨。

```text
# 按 RSS 排序。本机可以跑，输出随负载变，不贴进笔记当基准。
ps -eo pid,rss,comm --sort=-rss | head
# 某个进程：
# grep -E 'RssAnon|RssFile|RssShmem' /proc/<pid>/status
```

`smaps_rollup` 的 `Anonymous` 和 `Rss` 对不上时，差在共享和 THP 记账。先看 rollup，再对单个映射看 `smaps`。

**本机实测**（6.12.94+）：

`demos/02_fault.c`：`pagesize=4096`，触摸 256 页。两次运行 `ru_minflt` 分别是 72→328 和 73→329，增量都是 256；`ru_majflt` 保持 0。基线会随加载器差一两页，增量稳定。

`demos/01_maps_smaps.c`：`mmap` 2MB 匿名并写第一页后，`/proc/self/maps` 里有一行 `rw-p` 且路径为空的映射；同一次运行的 `smaps_rollup` 里 `Anonymous` 104 kB、`AnonHugePages` 0、`Swap` 0。rollup 含堆和加载器，不只有那 2MB。编译命令在 [demos/README.md](./demos/README.md)。

---

## 14.2 直接回收 / compact 延迟

症状：延迟尖刺，`ps` 里业务线程偶尔 `D`，同时 `allocstall_*` 或 `compact_stall` 在涨。

看什么：

| 计数 | 尖刺期间应该怎样 | 本机快照（累计，不是尖刺窗口） |
|---|---|---|
| `allocstall_normal` + `allocstall_movable` | 窗口内增加 | 11 + 373 |
| `pgscan_direct` / `pgsteal_direct` | 增加 | 19314 / 18131 |
| `compact_stall` / `compact_fail` | 直接压缩才涨 | 0 / 0 |
| PSI `memory` `some` | avg10 离开 0 | 当前 avg10=0，total 约 2.9s |

`compact_stall` 为 0 而 `compact_daemon_wake` 为 72：后台 kcompactd 在扫，分配路径没有同步压缩。这种对延迟的影响小，除非 kcompactd 和业务抢同一把 `zone->lock`。

```text
# RECIPE：ftrace。需要 tracefs 可写，本笔记没有在 box 上打开。
# cd /sys/kernel/tracing
# echo 1 > events/compaction/enable
# echo 1 > events/vmscan/mm_vmscan_wakeup_kswapd/enable
# echo 1 > tracing_on
# 复现之后：
# cat trace | head
# echo 0 > tracing_on

# RECIPE：bpftrace 若没装或没权限，不要装到生产机。
# bpftrace -e 'tracepoint:vmscan:mm_vmscan_wakeup_kswapd { @[comm] = count(); }'
```

读输出：`mm_vmscan_wakeup_kswapd` 的 order 大于 0，是高阶分配在叫 kswapd，先看 buddyinfo 高阶列，而不是先加内存。order 0 且 PSI full 升高，是真的缺页级别的内存压力。

`compact_fail` 持续涨、buddyinfo 的 order 9/10 仍是 0：不可移动页钉死 pageblock。`pagetypeinfo` 能确认，本机读它没权限。缓解是减少不可移动分配、让用户页走 `GFP_MOVABLE`，或者把可拔内存放进 `ZONE_MOVABLE`。不要用 `drop_caches` 当常规手段，它丢的是缓存，不整理不可移动页。

---

## 14.3 OOM

症状：`dmesg` 有 `Out of memory: Killed process`，或 cgroup `memory.events` 的 `oom_kill` 增加。

本机全局 `oom_kill` 是 0。没有 OOM 可复盘。

```text
# 全局
dmesg -T | grep -E 'Out of memory|Killed process'   # RECIPE：本机 dmesg 往往要 root
grep oom_kill /proc/vmstat

# cgroup。路径以实际挂载为准。
# grep oom /sys/fs/cgroup/<path>/memory.events
```

读 `dmesg` 时分清：

- `oom-kill:constraint=CONSTRAINT_MEMCG`：容器限额。去看那个 cgroup 的 `memory.max` 和 `memory.stat`，不是加宿主机内存。charge 语义在 cgroup 章。
- `constraint=CONSTRAINT_NONE`：全局。看 `MemAvailable`、swap、是不是匿名页没有地方去（本机就是没有 swap）。
- 被杀的是 qemu：guest 里可能只有一次突然重启，guest 的 `oom_kill` 仍是 0。

`oom_score_adj` 决定谁先死。不要在排障时把随机进程设成 `-1000`，那会把 OOM 推给别人。

---

## 14.4 THP 毛刺

症状：延迟周期性尖刺，`thp_fault_alloc` 在涨，或者 `thp_fault_fallback` 在涨同时 `compact_stall` 在涨。

本机这些计数全是 0，**没有 THP 毛刺可观测**。下面是配方。

```text
# 当前策略
cat /sys/kernel/mm/transparent_hugepage/enabled
cat /sys/kernel/mm/transparent_hugepage/defrag
grep -E 'thp_fault|thp_collapse|thp_split' /proc/vmstat

# 进程里有没有大页
grep -E 'AnonHugePages|FilePmdMapped' /proc/<pid>/smaps_rollup
```

读法：

| 现象 | 含义 |
|---|---|
| `enabled=always` 且 `defrag=always`，`compact_stall` 随缺页涨 | 缺页在同步压缩。把 defrag 改成 `defer` 或 `madvise`，让业务 VMA 自己 `MADV_HUGEPAGE` |
| `thp_fault_fallback` 涨、`compact_stall` 不涨 | 大页分配直接失败，退回 4KB。功能还在，只是没有大页。看 buddyinfo order 9 |
| `thp_collapse_alloc` 涨，尖刺和缺页对不上 | `khugepaged` 在后台折叠，拿 `mmap_lock`。加大 `scan_sleep_millisecs`，或对延迟敏感的 VMA `MADV_NOHUGEPAGE` |
| alloc 和 fallback 都是 0 | 没走 THP。先看 sysfs，不要去调 compact |

sysfs 路径在 v7.3 见 [09](./09-thp-and-hugetlb.md)。6.12 的目录布局大体相同，以这台机器上 `ls` 的结果为准。本笔记没有改这台机器的 THP 开关。

---

## 14.5 balloon 过猛

症状：guest `MemAvailable` 骤降，应用变慢，但宿主看起来还有内存；或者 guest `MemFree` 还很高，宿主已经 OOM（FPR 的方向反了）。

本机 `balloon_inflate` 累计 4718592 页，`balloon_deflate` 2097152 页。这是开机以来的累计，**不是当前气球大小**。当前大小看设备，不看这两个差。

```text
# guest
grep -E 'MemTotal|MemAvailable|MemFree' /proc/meminfo
grep balloon /proc/vmstat

# RECIPE：需要 qemu 监视器和 virtio-balloon 设备。本 box 不操作。
# (qemu) info balloon
# 若 guest 已经在回收/OOM，先把气球放大（把内存还给 guest），再查是谁把 inflate 设这么低。
```

判断过猛的标准：guest PSI memory 的 `full` avg10 非 0，同时宿主 `MemAvailable` 还宽裕，并且 inflate 计数在窗口内增加。这时先停止继续 inflate。`DEFLATE_ON_OOM` 是 guest 快死时的最后手段，不应当成正常水位调节。

FPR 过猛的症状不同：guest `MemFree` 不降，宿主把报告走的页分给了别人，guest 一旦分配这些页就在 EPT/宿主缺页上卡住。看宿主侧该 qemu 的 major fault 是否和 guest 的分配同时涨。命令在 [11](./11-kvm-memory.md)，本机没有 `/dev/kvm` 实验，标 RECIPE。

---

## 14.6 本机跑过的

| 项 | 结果 |
|---|---|
| `gcc` 编译 `demos/01_maps_smaps.c`、`demos/02_fault.c` | 作为完成标准的一部分执行，见仓库提交前的编译 |
| 读 `/proc/meminfo`、`vmstat`、`buddyinfo`、PSI | 数字在 [13](./13-observability.md)，来自快照，不是笔记生成时又采了一次 |
| `/proc/pagetypeinfo`、`/proc/slabinfo` | 快照即 Permission denied |
| perf / bpftrace / kmemleak | 没跑。RECIPE |
| 打开 tracefs、改 THP、改 balloon、嵌套 KVM | 没跑。RECIPE |

用户态缺页示例的预期：`02_fault` 触摸 256 页，`ru_minflt` 的增量应不少于触摸页数（进程自身的其他缺页会使增量更大）。`ru_majflt` 应不变，因为匿名页不读盘。

---

[上一节 观测](./13-observability.md) · [目录](./README.md)
