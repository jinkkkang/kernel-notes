# cgroup demos（用户态）

> 帮助把 [03](../03-hierarchy-control.md)、[10](../10-directory-walkthrough.md)、[11](../11-memory-stat-vs-meminfo.md) 的文件语义跑通。
> **不要**在共享生产机器上对不属于你的树 `echo` 限额。
> 会写盘的脚本默认 **DRY-RUN**（只打印计划），必须显式 `CG_WRITE=1` 才执行，且只在自己新建的 `cg-demo-*` 目录里操作，退出时清理。
> 本笔记刷新时在 cgroup v2 / 内核 **6.8.0** 的机器上全部实测过（sampled on 6.8）；接口名对照 v6.6.160。

## 文件

| 文件 | 写盘 | 说明 |
|---|---|---|
| `01_cg_smoke.sh` | 可选 | 打印当前 cgroup、controllers；`CG_DEMO_WRITE=1` 时建子组并把自己写进 `cgroup.procs` |
| `02_tree_walk.sh` | 否 | 按 [10](../10-directory-walkthrough.md) 的五步读树：v2 判定 → 根接口 → 子目录归属 → 控制器逐层收窄 → 叶子判定 → dying 统计 |
| `03_stat_vs_meminfo.sh` | 否 | 按 [11](../11-memory-stat-vs-meminfo.md) 对账根 `memory.stat` 与 `/proc/meminfo` |
| `04_memory_pids_limit.sh` | **是（需 CG_WRITE=1）** | 64M `memory.max` 下分配 200MB → 组内 OOM（exit 137）；`pids.max=32` 下 fork 失败并计入 `pids.events` |
| `05_cpu_throttle.sh` | **是（需 CG_WRITE=1）** | `cpu.max=20000 100000` 下忙等 3s → `nr_throttled` / `throttled_usec` / `cpu.pressure` |

## 用法

```bash
./02_tree_walk.sh
./03_stat_vs_meminfo.sh

CG_WRITE=1 ./04_memory_pids_limit.sh
CG_WRITE=1 ./05_cpu_throttle.sh
CG_WRITE=1 CG_PARENT=/sys/fs/cgroup/mydelegated ./04_memory_pids_limit.sh
```

## 实测记录（sampled on 6.8）

`04_memory_pids_limit.sh`：

```text
created /sys/fs/cgroup/cg-demo-4122708, memory.max=67108864 pids.max=32
--- child 1: allocate 200MB of anonymous memory under a 64M cap ---
  python exit=137 (137 = SIGKILL by memcg OOM)
  memory.events: max=36 oom=1 oom_kill=1
  memory.current after: 0MB
  memory.peak:          64MB
--- child 2: pids.max in action ---
  fork #32 failed: BlockingIOError(11, 'Resource temporarily unavailable')
  spawned 31 children, 33 fork failures
  pids.events: max 33
  pids.peak:   32
removed /sys/fs/cgroup/cg-demo-4122708
```

- `memory.peak` 停在 64MB：硬顶生效，页被压在 max 之下。
- `max=36 > oom=1`：撞到 max 后重试回收 36 次，其中一次进入 OOM 并杀掉当前任务。
- 子进程自己 fork 出的 31 个孩子也要算在 `pids.current` 里，加上父进程正好顶到 32，第 32 次 `fork()` 返回 `EAGAIN`。

`05_cpu_throttle.sh`：

```text
cpu.max=20000 100000  cpu.weight=100
self-reported wall: 3.04s (should be ~3s: wall time is NOT slowed, only quota)
cpu.stat:
usage_usec 610122
nr_periods 31
nr_throttled 30
throttled_usec 2397588
cpu.pressure:
some avg10=13.94 avg60=2.51 avg300=0.52 total=2397712
```

- 3 秒墙钟里只拿到约 0.61s CPU，接近配额 20%（`usage_usec / 3s`）。
- `nr_throttled ≈ nr_periods`：任务在每个 100ms 周期末都用光了配额被掐断。
- `throttled_usec` 与 `cpu.pressure` 的 `total` 基本相等：被节流时任务确实在"等 CPU"。

## 相关源码

- core：`kernel/cgroup/cgroup.c`（`cgroup_base_files[]`、`__cgroup_procs_write`、`cgroup_destroy_locked`）
- pids：`kernel/cgroup/pids.c`（`pids_try_charge`、`pids_can_fork`、`files[]`）
- cpu：`kernel/sched/core.c`（`cpu_max_write`、`cpu_extra_stat_show`、`cpu_files[]`）
- memory：`mm/memcontrol.c`（`memory_files[]`、`try_charge_memcg`、`memory_stats[]`）

内核模块 demo **不在本章**：cgroup 核心是 kernfs 状态机，控制器逻辑在 mm / sched / block 子系统里。
