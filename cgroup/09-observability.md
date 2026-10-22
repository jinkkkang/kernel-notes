# 9. 观测清单

**结论**：先确认 **是不是 cgroup2、树挂在哪**，再读 `/proc/<pid>/cgroup` 定位节点，最后读该节点的 `*.current` / `*.max` / `*.pressure` / `*.events`。本机实测在 box 上采集（**不是**你的登录机）；笔记源码钉 v7.3，**运行内核是 6.12.94+**——接口同族，细节以运行内核为准。

---

## 9.1 该看哪些路径

| 路径 | 看什么 |
|---|---|
| `mount \| grep cgroup` | 是否 `cgroup2`、挂载选项（如 `nsdelegate`） |
| `/sys/fs/cgroup/cgroup.controllers` | 系统启用了哪些控制器 |
| `/sys/fs/cgroup/cgroup.subtree_control` | root 授权给子树的控制器 |
| `/proc/<pid>/cgroup` | 该进程在默认层次上的路径（v2 一行 `0::/path`） |
| `/proc/<pid>/ns/cgroup` | cgroup namespace inode |
| 目标目录下 `memory.*` / `cpu.*` / `io.*` / `pids.*` | 限额与用量 |
| `*.pressure` | 是否在「等资源」 |
| `memory.events` / `pids.events` | 是否曾撞顶、OOM |

---

## 9.2 命令参考（默认别在生产乱写）

```bash
# 只读体检
mount | grep -E 'cgroup'
cat /sys/fs/cgroup/cgroup.controllers
cat /proc/1/cgroup
cat /proc/self/cgroup
ls /sys/fs/cgroup

# 跟一个服务叶子（路径因发行版而异）
CG=/sys/fs/cgroup/system.slice/foo.service
cat $CG/memory.current $CG/memory.max
cat $CG/cpu.stat $CG/cpu.pressure
cat $CG/pids.current $CG/pids.max

# 可选：systemd 视角
systemd-cgls
systemd-cgtop
```

写操作（建子组、迁进程）见 [demos/](./demos/)——**先确认你有权限、且不在别人的生产树上练手**。

---

## 9.3 本机只读实测

**采集时间**：2026-10-22 19:38:21 CST（Asia/Shanghai）  
**uname**：`Linux grok-bot-vm-477963022 6.12.94+ #1 SMP PREEMPT_DYNAMIC Mon Oct 19 19:23:48 UTC 2026 x86_64 GNU/Linux`  
**笔记对照源码**：Linux **v7.3**（`c03c1a14507d` / 树 `VERSION=7.3.0`）

### 挂载

```text
cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,nsdelegate,memory_recursiveprot)
```

已是 **纯 cgroup2**；`nsdelegate` 表示可按 ns 委托。

### root 控制器

```text
# cat /sys/fs/cgroup/cgroup.controllers
cpuset cpu io memory hugetlb pids

# cat /sys/fs/cgroup/cgroup.subtree_control
cpuset cpu io memory hugetlb pids
```

### 进程归属

```text
# cat /proc/1/cgroup
0::/system.slice/pod-qmgk7hgiprfvvhqt3k6wwmnq3a-2a2e86ea

# cat /proc/self/cgroup
0::/agent
```

PID 1 在某个 pod 风格的 `system.slice` 子树；当前 shell/agent 在 `/agent`。

### `/sys/fs/cgroup/agent`（当前任务所在组，节选）

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

解读一句：`/agent` **没有**打 memory/pids/cpu 硬顶（都是 `max`），但 CPU pressure 的 `total` 非 0——说明历史上有过「等 CPU」的积压，不是「从未争用」。

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

原始转储：构建目录内 `capture/01-sysfs.txt`、`capture/02-agent.txt`、`capture/03-more.txt`（不进 git 亦可；笔记已摘关键行）。

---

## 9.4 常见误判

| 误判 | 实际 |
|---|---|
| 看 `/proc/<pid>/status` 当 cgroup 限额 | 限额在 sysfs 控制器文件 |
| v1 多挂载点的老文章套到 v2 | 先 `mount`：只有 `cgroup2` 就别找 `memory` 独立树 |
| 把进程迁走，以为孩子跟着走 | **不跟随**；要迁整棵业务树得自己遍历 |
| `memory.current` 迁组后「不对」 | 已分配页不一定随任务改归属（见 [06](./06-memory.md)） |
| `cpu.weight` 当绝对算力 | 是**相对**权重，同父兄弟之间比 |
| pressure 为 0 就认为没问题 | 也可能限额太松、机器很闲；结合 latency 指标 |
| cgroup ns 当成限额 | ns 只改**路径视图** |
| 在父节点开了 `subtree_control` 还往父里塞进程 | 撞 no-internal-process |

---

## 9.5 与其它章观测的分工

| 问题 | 去哪 |
|---|---|
| 进程状态、`/proc/<pid>/…` | process-management `10-observability` |
| 锁争用 | locks-and-ds `08` |
| cgroup 限额与压力 | **本章** |
| 调度延迟根因 | 未来 scheduler / performance 章 |

---

*实测盒：grok-bot-vm；源码钉 v7.3。*
