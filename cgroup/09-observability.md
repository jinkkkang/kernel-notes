# 9. 观测清单

**结论**：先确认 **是不是 cgroup2、树挂在哪**，再读 `/proc/<pid>/cgroup` 定位节点，最后读该节点的 `*.current` / `*.max` / `*.pressure` / `*.events`。下面第 9.3 节的数字是作者机器上的采样，内核 **6.12.94+**（sampled on 6.12）。笔记里的结构体和函数对照的是 **Linux v6.6.160**。采样内核和笔记源码不是同一个版本，接口同属 cgroup v2，细节以你正在读的那棵树为准。

---

## 9.1 该看哪些路径

| 路径 | 看什么 |
|---|---|
| `mount \| grep cgroup` | 是否 `cgroup2`、挂载选项（如 `nsdelegate`、`memory_recursiveprot`） |
| `/sys/fs/cgroup/cgroup.controllers` | 系统启用了哪些控制器 |
| `/sys/fs/cgroup/cgroup.subtree_control` | root 授权给子树的控制器 |
| `/proc/<pid>/cgroup` | 该进程在默认层次上的路径（v2 一行 `0::/path`） |
| `/proc/<pid>/ns/cgroup` | cgroup namespace inode |
| 目标目录下 `memory.*` / `cpu.*` / `io.*` / `pids.*` | 限额与用量 |
| `cpu.pressure` / `memory.pressure` / `io.pressure` | 是否在等资源（some / full） |
| `cgroup.pressure` | 本节点 PSI 是否打开（0 或 1，不是 some/full） |
| `memory.events` / `pids.events` | 是否曾撞顶、OOM、fork 被拒 |
| `cgroup.events` | `populated`、`frozen` |
| `cgroup.stat` | `nr_descendants`、`nr_dying_descendants` |

---

## 9.2 命令参考

下面整段是 **RECIPE**。刷新本章时没有在 v6.6.160 上重新执行。第 9.3 节才是已经采过的输出。

```bash
# RECIPE：只读体检
mount | grep -E 'cgroup'
cat /sys/fs/cgroup/cgroup.controllers
cat /proc/1/cgroup
cat /proc/self/cgroup
ls /sys/fs/cgroup

# RECIPE：跟一个服务叶子（路径因发行版而异）
CG=/sys/fs/cgroup/system.slice/foo.service
cat $CG/memory.current $CG/memory.max
cat $CG/cpu.stat $CG/cpu.pressure
cat $CG/pids.current $CG/pids.max

# RECIPE：systemd 视角（用户态工具，不在内核树里）
systemd-cgls
systemd-cgtop
```

写操作（建子组、迁进程）见 [demos/](./demos/)——**先确认你有权限、且不在别人的生产树上练手**。demo 里的写入命令同样是 RECIPE。

---

## 9.3 本机只读实测

**标签**：sampled on 6.12  
**采集时间**：2026-10-22 19:38:21 CST（Asia/Shanghai）  
**uname**：`Linux grok-bot-vm-477963022 6.12.94+ #1 SMP PREEMPT_DYNAMIC Mon Oct 19 19:23:48 UTC 2026 x86_64 GNU/Linux`  
**笔记对照源码**：Linux **v6.6.160**

这些数字不在 v6.6.160 上复测。文件名和 v6.6.160 的 `cgroup_base_files[]` / `memory_files[]` / `cpu_files[]` 对得上；数值只说明那台 6.12 机器当时的状态。

### 挂载

```text
cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,nsdelegate,memory_recursiveprot)
```

已是 **纯 cgroup2**。`nsdelegate` 对应 `CGRP_ROOT_NS_DELEGATE`，`memory_recursiveprot` 对应 `CGRP_ROOT_MEMORY_RECURSIVE_PROT`（见 [03](./03-hierarchy-control.md)）。

### root 控制器

```text
# cat /sys/fs/cgroup/cgroup.controllers
cpuset cpu io memory hugetlb pids

# cat /sys/fs/cgroup/cgroup.subtree_control
cpuset cpu io memory hugetlb pids
```

这台机器的配置没有把 rdma、misc 编进可见控制器。`perf_event` 即使编进来也不会出现在这行里，因为它是 `implicit_on_dfl`（见 [01](./01-overview.md)）。

### 进程归属

```text
# cat /proc/1/cgroup
0::/system.slice/pod-qmgk7hgiprfvvhqt3k6wwmnq3a-2a2e86ea

# cat /proc/self/cgroup
0::/agent
```

PID 1 在某个 pod 风格的 `system.slice` 子树；当时的 shell 在 `/agent`。

### `/sys/fs/cgroup/agent`（当时任务所在组，节选）

采集稍后补强于 19:39:06 CST：

```text
memory.current   4718989312
memory.max       max
memory.high      max
memory.low       0
pids.current     462
pids.max         max
cpu.weight       100
cpu.max          max 100000
cpuset.cpus      (空)
cpuset.cpus.effective  0-7
cgroup.events    populated 1 / frozen 0

memory.events:
low 0
high 0
max 0
oom 0
oom_kill 0
oom_group_kill 0

memory.pressure:
some avg10=0.00 avg60=0.00 avg300=0.00 total=2276444
full avg10=0.00 avg60=0.00 avg300=0.00 total=2240333

cpu.pressure:
some avg10=0.01 avg60=0.20 avg300=0.44 total=25726418
full avg10=0.01 avg60=0.07 avg300=0.13 total=11926907
```

解读一句：`/agent` 没有打 memory/pids/cpu 硬顶（都是 `max`），但 CPU pressure 的 `total` 非 0——说明历史上有过「等 CPU」的积压。`total` 的单位是微秒（见 [08](./08-psi-rstat.md)）。

### `system.slice` / `containerd.service`（对照叶子）

```text
# system.slice
memory.current  325636096
controllers     cpuset cpu io memory hugetlb pids

# system.slice/containerd.service
memory.current  55984128
memory.max      max
cpu.weight      100
pids.current    13
pids.max        max
memory.pressure some/full avg*=0.00 total=0
```

### root `cpu.stat` / `memory.pressure`（19:38 一带）

```text
# cpu.stat（root）
usage_usec 1657892687
user_usec  1039369934
system_usec 618522753
nr_throttled 0
throttled_usec 0

# memory.pressure（root）
some avg10=0.00 avg60=0.00 avg300=0.00 total=2915844
full avg10=0.00 avg60=0.00 avg300=0.00 total=2868743
```

原始转储当时在构建目录的 `capture/01-sysfs.txt`、`capture/02-agent.txt`、`capture/03-more.txt`。那些文件不在本仓库里；上面是从中摘出的关键行。

---

## 9.4 常见误判

| 误判 | 实际 |
|---|---|
| 看 `/proc/<pid>/status` 当 cgroup 限额 | 限额在 sysfs 控制器文件 |
| v1 多挂载点的老文章套到 v2 | 先 `mount`：只有 `cgroup2` 就别找独立的 memory 挂载点 |
| 把进程迁走，以为孩子跟着走 | **不跟随**；要迁整棵业务树得自己遍历 |
| `memory.current` 迁组后「不对」 | 已分配的内存留在创建它的 memcg（见 [06](./06-memory.md)） |
| `cpu.weight` 当绝对算力 | 是**相对**权重，范围 1..10000，同父兄弟之间比 |
| pressure 的 avg 为 0 就认为没问题 | 也可能限额太松、机器很闲；`total` 仍可能非 0 |
| `cgroup.pressure` 读成 some/full | 它只是 0/1 开关 |
| cgroup ns 当成限额 | ns 只改**路径视图** |
| 在非 root 上开了 `subtree_control` 还往该节点塞进程 | 撞 no-internal-process（threaded 子树除外） |

---

## 9.5 与其它章观测的分工

| 问题 | 去哪 |
|---|---|
| 进程状态、`/proc/<pid>/…` | [process-management/10-observability.md](../process-management/10-observability.md) |
| 锁争用 | [locks-and-ds/08-observability.md](../locks-and-ds/08-observability.md) |
| cgroup 限额与压力 | **本章** |
| 调度延迟、cpu 控制器和 EEVDF | [scheduler/07-cgroup-cpu.md](../scheduler/07-cgroup-cpu.md)、[scheduler/08-observability.md](../scheduler/08-observability.md) |

---

*采样：sampled on 6.12。源码对照：Linux v6.6.160。*
