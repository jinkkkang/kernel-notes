# 2. 核心对象

**结论**：记四件事——**`cgroup`（树上的节点）**、**`cgroup_subsys`（一种控制器）**、**`cgroup_subsys_state` / css（该控制器在某节点上的状态）**、**`css_set`（一个进程当前持有的「各 css 组合」）**。进程不直接挂一长串 cgroup，而是挂一个 `css_set`。

> 定义：`include/linux/cgroup-defs.h`  
> 任务指针：`task_struct->cgroups`（`include/linux/sched.h`，约 `:1352`）

---

## 2.1 关系一张图

```text
task_struct.cgroups ──RCU──► css_set
                               │
                               ├─ subsys[cpu]    → css → cgroup (可能是祖先)
                               ├─ subsys[memory] → css → cgroup
                               ├─ …
                               └─ dfl_cgrp       → 默认层次上「这个进程所在」的 cgroup

cgroup.self = 一个特殊 css（ss == NULL），指回自己
cgroup.subsys[ssid] → 本节点上该控制器的 css（未启用则可能为空，有效 css 在祖先）
```

**线程组**：默认按**进程**迁移（写 `cgroup.procs`）；v2 另有 thread mode（`cgroup.threads`）。同一线程组通常共享同一套资源视图。

**迁移不带动已有子进程**：把父进程挪走，已经 fork 出来的孩子仍留在原地——这是运维踩坑经典题。

---

## 2.2 `struct cgroup_subsys_state`（css）

`include/linux/cgroup-defs.h:181`

| 成员（节选） | 中文 |
|---|---|
| `struct cgroup *cgroup` | 挂在哪个 cgroup 节点上 |
| `struct cgroup_subsys *ss` | 属于哪个子系统；`cgroup.self` 的 ss 为 NULL |
| `struct percpu_ref refcnt` | 引用计数；`css_get` / `css_put` / `css_tryget` |
| `struct css_rstat_cpu __percpu *rstat_cpu` | 分层统计 per-CPU（见 [08](./08-psi-rstat.md)） |
| `struct list_head sibling` / `children` | 同父兄弟 / 子 css 链表 |
| `int id` | 子系统内唯一 ID；root 常为 1 |
| `unsigned int flags` | 状态标志 |
| `u64 serial_nr` | 全局递增序号，保证 children 有序可断点迭代 |
| `atomic_t online_cnt` | 自己与孩子 online 计数，防父先 offline |
| `destroy_work` / `destroy_rwork` | 销毁与 RCU 回收 |

控制器私有结构的第一成员几乎总是 `struct cgroup_subsys_state css`（例如 `struct mem_cgroup`、`struct pids_cgroup`），再用 `container_of` 找回大结构——与 [locks-and-ds/01](../locks-and-ds/01-c-primer.md) 同一套路。

---

## 2.3 `struct css_set`

`include/linux/cgroup-defs.h:286`

| 成员（节选） | 中文 |
|---|---|
| `struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]` | 每个子系统一个 css 指针；创建后基本不可变（除 boot） |
| `refcount_t refcount` | css_set 引用 |
| `struct css_set *dom_cset` | 域 cgroup 对应的 cset；threaded 时指最近 domain 祖先 |
| `struct cgroup *dfl_cgrp` | 默认层次上关联的 cgroup |
| `int nr_tasks` | 使用本 cset 的任务数（`css_set_lock`） |
| `struct list_head tasks` / `mg_tasks` / `dying_tasks` | 正常 / 迁移中 / 正在死的任务 |
| `struct list_head e_cset_node[…]` | 挂到「有效 css 所属 cgroup」上，便于按控制器遍历 |
| `struct hlist_node hlist` | 全局 hash，按内容查重用已有 cset |
| `struct list_head cgrp_links` | 与各层次 cgroup 的链接 |
| `mg_src_cgrp` / `mg_dst_cgrp` / … | 迁移上下文 |

**直觉**：css_set 是「配置指纹」。大量进程共享同一组合时，内核复用同一个 `css_set`，靠 `refcount` 活着。

辅助宏/内联：`task_css_set(task)`（`include/linux/cgroup.h:533`）、`task_dfl_cgroup(task)`（同文件约 `:604` 一带）。

---

## 2.4 `struct cgroup`

`include/linux/cgroup-defs.h:486`（大结构，分组记）

### 身份与树形

| 成员 | 中文 |
|---|---|
| `struct cgroup_subsys_state self` | 节点自身的 css |
| `unsigned long flags` | 如 `CGRP_FREEZE` 等 |
| `int level` | 深度；root = 0 |
| `int max_depth` / `max_descendants` | 用户可配上限（防树爆炸） |
| `int nr_descendants` / `nr_dying_descendants` | 活着的 / 垂死的后代计数 |
| `struct cgroup *dom_cgrp` | domain cgroup（threaded 子树用） |
| `struct cgroup *ancestors[]` | 含自己在内的祖先数组（flex array） |

### 控制器开关

| 成员 | 中文 |
|---|---|
| `u32 subtree_control` | 用户通过 `cgroup.subtree_control` 配置的「允许孩子启用」掩码 |
| `u32 subtree_ss_mask` | **有效**掩码（可含依赖控制器） |
| `struct cgroup_subsys_state __rcu *subsys[]` | 本节点各控制器 css |

### kernfs / 文件 / 统计

| 成员 | 中文 |
|---|---|
| `struct kernfs_node *kn` | 对应 `/sys/fs/cgroup/...` 目录 |
| `struct cgroup_file procs_file` / `events_file` | `cgroup.procs` / `cgroup.events` |
| `struct cgroup_file psi_files[]` | `*.pressure` |
| `struct cgroup_rstat_base_cpu __percpu *rstat_base_cpu` | base 统计 |
| `struct psi_group *psi` | 压力记账 |
| `struct cgroup_freezer_state freezer` | v2 freezer 状态 |
| `struct cgroup_bpf bpf` | cgroup BPF 附着点 |

### 根

`struct cgroup_root`（`:658`）：一棵层次的根；v2 默认根即常见的 `/sys/fs/cgroup`。

---

## 2.5 `struct cgroup_subsys`

`include/linux/cgroup-defs.h:784`

| 回调（节选） | 中文 |
|---|---|
| `css_alloc` / `css_online` / `css_offline` / `css_free` … | css 生命周期 |
| `css_rstat_flush` | 刷该子系统的分层统计 |
| `can_attach` / `attach` / `cancel_attach` | 进程迁入 |
| `can_fork` / `fork` / `cancel_fork` | 创建子进程（如 pids） |
| `exit` / `release` | 任务退出路径 |
| `bool threaded` | 是否支持 v2 threaded 模式 |
| `bool implicit_on_dfl` | 默认层次隐式启用（对用户透明） |
| `dfl_cftypes` / `legacy_cftypes` | v2 / v1 接口文件表 |
| `const char *name` | 控制器名（出现在 `cgroup.controllers`） |

注册表：`include/linux/cgroup_subsys.h` 里的 `SUBSYS(name)` 宏展开。

---

## 2.6 kernfs / cgroup2 直觉

- cgroup2 是挂在 **kernfs** 上的伪文件系统：`mkdir` → `cgroup_mkdir`（`kernel/cgroup/cgroup.c:6116`），写文件 → 各 `cftype` 的 `.write` / `.seq_show`。
- 用户态「创建一个组」≈ `mkdir /sys/fs/cgroup/foo`；「把进程放进去」≈ `echo $PID > .../cgroup.procs`。
- 本机实测见 [09](./09-observability.md)：已是 `cgroup2 on /sys/fs/cgroup`。

---

## 2.7 关键函数表（对象层）

| 符号 | 位置 | 作用 |
|---|---|---|
| `task_css_set` | `include/linux/cgroup.h:533` | 取任务当前 css_set |
| `css_get` / `css_put` | `include/linux/cgroup.h`（及 cgroup.c 使用点） | css 引用 |
| `cgroup_mkdir` | `cgroup.c:6116` | 建子 cgroup |
| `cgroup_attach_task` | `cgroup.c:3090` | 迁移任务到目标 cgroup |
| `find_css_set` | `cgroup.c`（内部） | 按「旧 cset + 新 cgroup」查找/创建 cset |

---

*对照：`include/linux/cgroup-defs.h`、`include/linux/cgroup.h`、`include/linux/sched.h`。*
