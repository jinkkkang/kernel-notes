# 5. cpu / cpuset

**结论**：**cpu** 控制器管「能跑多少算力」（weight、max 带宽、pressure）；**cpuset** 管「能跑在哪些 CPU/内存节点」。调度内部的 `task_group` / EEVDF 细节留给 [scheduler/07](../scheduler/07-cgroup-cpu.md)——这里只钉 **控制器文件 ↔ 内核挂钩点**。

> cpu：`kernel/sched/core.c`（`cpu_cgrp_subsys`，`:11553`）  
> cpuset：`kernel/cgroup/cpuset.c`（`cpuset_cgrp_subsys`，`:3312`）  
> 接口说明：`Documentation/admin-guide/cgroup-v2.rst` 目录 **5-1**、**5-5**

---

## 5.1 cpu：用户可见文件（v2）

v2 文件表是 `cpu_files[]`（`kernel/sched/core.c:11501`）。每张表项外面都有配置开关，发行版没打开对应选项时，文件不会出现。

| 文件 | 配置 | 含义 |
|---|---|---|
| `cpu.stat` | 无（core 文件，`cgroup.c:5292`） | 总有 `usage_usec`、`user_usec`、`system_usec`（`rstat.c:527`）。cpu 控制器启用且 `CONFIG_CFS_BANDWIDTH` 时，`css_extra_stat_show` 再追加 `nr_periods`、`nr_throttled`、`throttled_usec`、`nr_bursts`、`burst_usec`（`core.c:11351`） |
| `cpu.stat.local` | core 文件 `:5296` | 只打本节点的 `throttled_usec`（`:11374`） |
| `cpu.weight` | `CONFIG_FAIR_GROUP_SCHED` | 相对权重。`CGROUP_WEIGHT_MIN` = 1（`include/linux/cgroup.h:39`），`CGROUP_WEIGHT_DFL` = 100（`:40`），`CGROUP_WEIGHT_MAX` = 10000（`:41`） |
| `cpu.weight.nice` | 同上 | 用 nice 值（`MIN_NICE..MAX_NICE`）映射到同一套 shares |
| `cpu.idle` | 同上 | 写 0 或 1，调用 `sched_group_set_idle`（`kernel/sched/fair.c:13118`），改 `task_group->idle`。为 1 时 `sched_group_set_shares` 直接返回 `-EINVAL`（`fair.c:13109`） |
| `cpu.max` | `CONFIG_CFS_BANDWIDTH` | 两列 `$MAX $PERIOD`，单位微秒。`$MAX` 写成 `max` 表示不限（`cpu_period_quota_parse`，`:11456`）。文档默认是 `max 100000` |
| `cpu.max.burst` | 同上 | 突发额度，微秒 |
| `cpu.uclamp.min` / `cpu.uclamp.max` | `CONFIG_UCLAMP_TASK_GROUP` | 给 schedutil 的利用率上下限提示 |
| `cpu.pressure` | `CONFIG_PSI` | PSI，见 [08](./08-psi-rstat.md)。这是 core 文件，不在 `cpu_files[]` 里 |

v1 的 `cpu.shares`、`cpu.cfs_quota_us`、`cpu.rt_runtime_us` 在 `cpu_legacy_files[]`（`:11270`），v2 的 `cpu_files[]` 里没有 RT 带宽文件。

`cgroup-v2.rst` 5-1 写明：cgroup2 还不控制实时进程，cpu 控制器要等所有 RT 进程都在 root 才能启用。源码里能直接对上的一处检查是 `CONFIG_RT_GROUP_SCHED` 下的 `sched_setscheduler`：目标组 `rt_runtime == 0` 时拒绝把任务改成 RT（`core.c:7778`）。

本机 `/sys/fs/cgroup/agent` 示例（sampled on 6.12）：`cpu.weight=100`，`cpu.max=max 100000`。

读写实现锚点（`kernel/sched/core.c`）：

| 符号 | 行号 | 作用 |
|---|---|---|
| `cpu_weight_read_u64` / `cpu_weight_write_u64` | `:11382` / `:11391` | 读写 weight，落到 `sched_group_set_shares` |
| `cpu_weight_nice_read_s64` / `cpu_weight_nice_write_s64` | `:11409` / `:11427` | nice ↔ shares |
| `cpu_idle_write_s64` | `:11263` | 写 idle，0 或 1 |
| `cpu_max_show` / `cpu_max_write` | `:11477` / `:11485` | 读写 `cpu.max` |
| `tg_set_cfs_bandwidth` | `:10890` | 把 period / quota / burst 写进调度组 |
| `cpu_extra_stat_show` | `:11337` | `cpu.stat` 里的节流计数 |
| `cpu_local_stat_show` | `:11363` | `cpu.stat.local` |
| `cpu_cgroup_css_alloc` | `:10608` | 分配 cpu css；非 root 时 `sched_create_group`，css 嵌在 `task_group` 里 |
| `cpu_cgroup_css_online` | `:10626` | `sched_online_group` |
| `cpu_files[]` | `:11501` | v2 cftype 表 |
| `cpu_cgrp_subsys` | `:11553` | 子系统对象 |

```c
struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_online	= cpu_cgroup_css_online,
	.css_released	= cpu_cgroup_css_released,
	.css_free	= cpu_cgroup_css_free,
	.css_extra_stat_show = cpu_extra_stat_show,
	.css_local_stat_show = cpu_local_stat_show,
#ifdef CONFIG_RT_GROUP_SCHED
	.can_attach	= cpu_cgroup_can_attach,
#endif
	.attach		= cpu_cgroup_attach,
	.legacy_cftypes	= cpu_legacy_files,
	.dfl_cftypes	= cpu_files,
	.early_init	= true,
	.threaded	= true,
};
```

`css_tg()`（`:10602`）用 `container_of(css, struct task_group, css)` 从 css 找回调度组。root 的 css 就是 `&root_task_group.css`（`:10615`）。

### 与调度章的分界

- **本章**：文件语义、`cpu_cgrp_subsys` 回调、weight/max 最终碰到 `task_group` / `tg_set_cfs_bandwidth`。
- **调度章**：`task_group` 成员、`cfs_rq`、EEVDF 怎么消费这些权重和配额。

---

## 5.2 cpuset

v2 文件在 `dfl_files[]`（`kernel/cgroup/cpuset.c:2984`）。名字在 cgroup 里会变成 `cpuset.cpus` 这种带前缀的形式。

| 文件 | 读写 | 含义 |
|---|---|---|
| `cpuset.cpus` | 读写，非 root | 允许的 CPU 列表。空掩码只在本节点没有任务时合法（`update_cpumask` 注释，`:1675`） |
| `cpuset.mems` | 读写，非 root | 允许的内存节点 |
| `cpuset.cpus.effective` | 只读 | 和祖先求交之后真正生效的 CPU |
| `cpuset.mems.effective` | 只读 | 生效的内存节点 |
| `cpuset.cpus.partition` | 读写，非 root | 分区根：把一部分 CPU 划成独占分区 |
| `cpuset.cpus.subpartitions` | 只读 | `CFTYPE_DEBUG`，只有打开 cgroup debug 才出现 |

v1 另有 `cpuset.cpu_exclusive`、`cpuset.sched_load_balance` 等，在 `legacy_files[]`（`:2880` 起），v2 的 `dfl_files[]` 不包含它们。

本机 `agent`（sampled on 6.12）：`cpuset.cpus` 为空，`cpuset.cpus.effective=0-7`。

关键路径：

| 符号 | 位置 | 作用 |
|---|---|---|
| `update_cpumask` | `cpuset.c:1663` | 解析用户写入的 CPU 列表，更新本节点并下推 |
| `update_cpumasks_hier` | `:1451` | 刷新子树的 effective cpumask，并按需重建调度域 |
| `cpuset_can_attach_check` | `:2356` | 这个 cpuset 现在能不能收人 |
| `cpuset_can_attach` | `:2371` | `can_attach`：迁入前拿 `cpuset_mutex` 做检查 |
| `cpuset_attach` | `:2496` | `attach`：迁移提交后改任务的 CPU / 内存节点亲和 |
| `cpuset_css_alloc` | `:3044` | 分配 `struct cpuset`；root 直接返回 `top_cpuset` |
| `cpuset_cgrp_subsys` | `:3312` | 见下 |

```c
struct cgroup_subsys cpuset_cgrp_subsys = {
	.css_alloc	= cpuset_css_alloc,
	.css_online	= cpuset_css_online,
	.css_offline	= cpuset_css_offline,
	.css_free	= cpuset_css_free,
	.can_attach	= cpuset_can_attach,
	.cancel_attach	= cpuset_cancel_attach,
	.attach		= cpuset_attach,
	.post_attach	= cpuset_post_attach,
	.bind		= cpuset_bind,
	.can_fork	= cpuset_can_fork,
	.cancel_fork	= cpuset_cancel_fork,
	.fork		= cpuset_fork,
	.legacy_cftypes	= legacy_files,
	.dfl_cftypes	= dfl_files,
	.early_init	= true,
	.threaded	= true,
};
```

改 cpuset 会改任务能跑的 CPU 集合；`sched_setaffinity` 是另一条用户入口，最后也落到任务的亲和掩码上。两条路径的汇合在调度章，这里不展开。

v1 挂载可以加 `CGRP_ROOT_CPUSET_V2_MODE`（`cgroup-defs.h:104`），让旧树上的 cpuset 用 v2 行为。判断在 `cpuset.c:259`：`cgroup_subsys_on_dfl(cpuset_cgrp_subsys)` 或该标志置位。

---

## 5.3 使用直觉

1. **只要相对份额** → 调 `cpu.weight`（1..10000），别一上来 `cpu.max`。  
2. **硬顶（防吵闹邻居）** → `cpu.max`。两列都是微秒：`$MAX $PERIOD`。  
3. **绑核** → `cpuset.cpus`；看实际结果用 `cpuset.cpus.effective`，空的 `cpus` 表示还没自己写过，生效集来自祖先。  
4. **看是否真的被掐** → `cpu.stat` 里的 `nr_throttled` / `throttled_usec`，再配 `cpu.pressure`。

---

*对照：`kernel/sched/core.c`、`kernel/cgroup/cpuset.c`。*
