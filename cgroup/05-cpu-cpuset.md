# 5. cpu / cpuset

**结论**：**cpu** 控制器管「能跑多少算力」（weight、max 带宽、pressure）；**cpuset** 管「能跑在哪些 CPU/内存节点」。调度内部的 `task_group` / EEVDF 细节留给调度章——这里只钉 **控制器文件 ↔ 内核挂钩点**。

> cpu：`kernel/sched/core.c`（`cpu_cgrp_subsys`，`:10623`）  
> cpuset：`kernel/cgroup/cpuset.c`  
> 接口说明：`Documentation/admin-guide/cgroup-v2.rst` §5.1、§5.5

---

## 5.1 cpu：用户可见文件（v2）

| 文件 | 含义（直觉） |
|---|---|
| `cpu.weight` | 相对权重，默认 100；同父兄弟按权重分算力 |
| `cpu.weight.nice` | 用 nice 风格映射到 weight |
| `cpu.max` | 带宽上限：`$MAX $PERIOD`（微秒）；`max 100000` = 不限 |
| `cpu.max.burst` | 突发 |
| `cpu.pressure` | PSI（见 [08](./08-psi-rstat.md)） |
| `cpu.stat` | `usage_usec`、`user_usec`、`system_usec`、`throttled_usec` … |
| `cpu.idle` | idle 策略相关（若存在） |

本机 `/sys/fs/cgroup/agent` 示例：`cpu.weight=100`，`cpu.max=max 100000`（未限流）。

读写实现锚点（`kernel/sched/core.c`）：

| 符号 | 行号 | 作用 |
|---|---|---|
| `cpu_weight_read_u64` / `cpu_weight_write_u64` | `:10453` / `:10459` | weight |
| `cpu_max_write` | `:10556` | max |
| `cpu_files[]` | `:10571` | v2 cftype 表 |
| `cpu_cgroup_css_alloc` | `:9573` | 分配 cpu css（背后连 `task_group`） |
| `tg_set_cfs_bandwidth` | `:9880` | 带宽落到调度组 |
| `cpu_cgrp_subsys` | `:10623` | 子系统对象；`threaded = true` |

```c
struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_online	= cpu_cgroup_css_online,
	/* ... */
	.dfl_cftypes	= cpu_files,
	.early_init	= true,
	.threaded	= true,
};
```

### 与调度章的分界

- **本章**：文件语义、`cpu_cgrp_subsys` 回调名字、weight/max 最终会碰到 `task_group` / bandwidth API。
- **调度章**：`task_group` 成员、`cfs_rq`、EEVDF pick、负载均衡如何消费这些权重/配额。

---

## 5.2 cpuset

| 文件（常见） | 含义 |
|---|---|
| `cpuset.cpus` | 允许的 CPU 列表（空 = 继承有效集） |
| `cpuset.cpus.effective` | **实际生效**的 CPU（与父交集等之后） |
| `cpuset.mems` / `cpuset.mems.effective` | 内存节点 |
| `cpuset.cpus.partition` | 分区根（独占 CPU 等进阶用途） |
| `cpuset.cpus.exclusive` 等 | 独占相关 |

本机 `agent`：`cpuset.cpus` 为空，`cpuset.cpus.effective=0-7`（继承）。

关键路径：

| 符号 | 位置 | 作用 |
|---|---|---|
| `update_cpumask` | `cpuset.c:2491` | 更新某 cpuset 的 CPU 掩码并下推 |
| `update_cpumasks_hier` | `:2193` | 子树 effective 更新 |
| `cpuset_can_attach` | `:3176` | 迁入前检查 |

改 cpuset 常伴随任务亲和性更新；与 `sched_setaffinity` 用户接口是不同入口，最终都影响任务能跑的 CPU 集合。

---

## 5.3 使用直觉

1. **只要相对份额** → 调 `cpu.weight`，别一上来 `cpu.max`。  
2. **硬顶（防吵闹邻居）** → `cpu.max`（注意 period，别把单位搞反）。  
3. **绑核/隔离** → cpuset（或用户态 affinity）；容器编排常两者一起用。  
4. **看是否真的被掐** → `cpu.stat` 里的 `nr_throttled` / `throttled_usec`，再配 `cpu.pressure`。

---

*对照：`kernel/sched/core.c`、`kernel/cgroup/cpuset.c`。*
