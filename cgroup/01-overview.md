# 1. 总览与边界

**结论**：cgroup = **层次组织进程** + **沿树分配/限制资源**。core 管目录树、进程归属、控制器开关；各 **controller** 实现具体记账与限流。本笔记默认 **v2 单树（default hierarchy）**。

> 权威用户文档：`Documentation/admin-guide/cgroup-v2.rst`  
> 源码主线：`kernel/cgroup/cgroup.c`、`include/linux/cgroup-defs.h`

---

## 1.1 两层分工

| 层 | 干什么 | 你在 sysfs 上看到的 |
|---|---|---|
| **core** | 建树、挂进程、开/关子树控制器、委托、freeze/kill | `cgroup.*`、`cgroup.procs`、`cgroup.subtree_control` |
| **controllers** | 对资源记账/限流/压力信号 | `cpu.*`、`memory.*`、`io.*`、`pids.*`、`cpuset.*` … |

进程侧入口：`task_struct->cgroups` 指向一个 **`css_set`**（一组各子系统的 `cgroup_subsys_state`）。见 [02](./02-core-objects.md)。

---

## 1.2 v1 多树 vs v2 单树

| | v1（legacy） | v2（default / dfl） |
|---|---|---|
| 层次 | **每个控制器可以有自己的树**（或多控制器共树，配置复杂） | **一棵统一树**，控制器按需挂到子树 |
| 挂载 | 多个 `cgroup` 挂载点 | 通常一个 `cgroup2`（本机：`/sys/fs/cgroup`） |
| 进程归属 | 同一进程可在不同控制器树的不同节点 | 一个进程在树上**一个位置**（线程模式另说） |
| 约束 | 弱 | **top-down**、**no internal process**（见 [03](./03-hierarchy-control.md)） |
| 源码 | `kernel/cgroup/cgroup-v1.c` 等 | `cgroup.c` 默认层次路径 |

本笔记：**接口与语义以 v2 为准**；提到 v1 只为读旧文档/旧机时不迷路。

---

## 1.3 与 namespace / 容器（一句话）

- **cgroup**：资源限额与归属（「能用多少」）。
- **namespace**：可见性隔离（「能看见什么」）。`CLONE_NEWCGROUP` 得到 **cgroup namespace**，让容器内看到的 cgroup 路径从自己的 root 起算——**不替代**限额本身。
- Docker/K8s/systemd：用户态编排员，最终仍是 mkdir + 写 `cgroup.procs` / 控制器文件（或 `CLONE_INTO_CGROUP`）。

pidns 细节见 [process-management/03-pid-namespaces.md](../process-management/03-pid-namespaces.md)；cgroup ns 见 [04](./04-lifecycle-hooks.md)。

---

## 1.4 本章覆盖 / 不覆盖

| 覆盖 | 不覆盖（另章） |
|---|---|
| core 对象与 v2 接口 | 完整 EEVDF / `task_group` 调度内部 |
| fork/exit/attach 挂钩 | 完整 MM 回收、`shrinker` 实现 |
| cpu/cpuset/memory/io/pids/freezer **控制器侧** | blk-mq、文件系统实现 |
| PSI、rstat、本机观测 | 完整 perf/ftrace 方法论 |
| cgroup namespace | 其它 ns 全家桶深挖 |

---

## 1.5 源码地图（先认门牌）

| 路径 | 角色 |
|---|---|
| `include/linux/cgroup-defs.h` | `cgroup` / `css_set` / `cgroup_subsys` 等定义 |
| `include/linux/cgroup.h` | 对外 API、`task_css_set` 等 |
| `include/linux/cgroup_subsys.h` | 子系统注册表（`SUBSYS(...)`） |
| `kernel/cgroup/cgroup.c` | core：mkdir、attach、fork 钩子、v2 文件 |
| `kernel/cgroup/namespace.c` | cgroup namespace |
| `kernel/cgroup/rstat.c` | 分层统计 flush |
| `kernel/cgroup/cpuset.c` / `pids.c` / `freezer.c` / `rdma.c` / `misc.c` / `dmem.c` | 各控制器（部分） |
| `kernel/sched/core.c` | **cpu** 控制器（`cpu_cgrp_subsys`） |
| `mm/memcontrol.c` | **memory** 控制器 |
| `block/blk-cgroup.c` | **io** 控制器（`io_cgrp_subsys`） |
| `kernel/sched/psi.c` | PSI |

v7.3 子系统枚举见 `include/linux/cgroup_subsys.h`：`cpuset`、`cpu`、`cpuacct`、`io`、`memory`、`devices`、`freezer`、`net_cls`、`perf_event`、`net_prio`、`hugetlb`、`pids`、`rdma`、`misc`、`dmem`、（debug）等——是否出现在 `cgroup.controllers` 取决于 Kconfig 与挂载。

---

*对照：`/workspace/linux` v7.3。*
