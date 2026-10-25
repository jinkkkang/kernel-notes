# 8. PSI 与 rstat

**结论**：**用量告诉你用了多少，PSI 告诉你有多少时间在等资源**。rstat 是 cgroup 把每 CPU 计数收成 sysfs 里那个数字的基建。它不是业务控制器，但读 `cpu.stat`、`io.stat`、一部分 memory 统计时会碰到它。

> PSI：`kernel/sched/psi.c`、`include/linux/psi_types.h`、`include/linux/psi.h`  
> rstat：`kernel/cgroup/rstat.c`、`struct cgroup_rstat_cpu` 在 `include/linux/cgroup-defs.h:327`

整章都在 `CONFIG_PSI` 下才有 PSI 文件。rstat 本身不依赖 PSI。

---

## 8.1 PSI：some / full

压力文件由 `cgroup_psi_files[]`（`kernel/cgroup/cgroup.c:5302`）登记：

| 文件 | 条件 | 读出来是什么 |
|---|---|---|
| `cpu.pressure` | `CONFIG_PSI` | some / full |
| `memory.pressure` | 同上 | some / full |
| `io.pressure` | 同上 | some / full |
| `irq.pressure` | 还要 `CONFIG_IRQ_TIME_ACCOUNTING` | **只有 full**（`psi.c:1273`，`only_full = res == PSI_IRQ`） |
| `cgroup.pressure` | `CONFIG_PSI` | 一个整数 0 或 1，是本节点 PSI 的开关，不是 some/full |

`cgroup.pressure` 的读函数 `cgroup_pressure_show`（`cgroup.c:3855`）打印 `psi->enabled`。写成 1 或 0 时，`cgroup_pressure_write`（`:3865`）用 `cgroup_file_show` 把 `psi_files[]` 显示或藏起来。

some/full 的文本由 `psi_show`（`psi.c:1255`）打印。`total` 是累计时间，内部是纳秒，打印前除以 `NSEC_PER_USEC`，所以单位是**微秒**（`:1285`）。

```text
some avg10=0.00 avg60=0.00 avg300=0.00 total=...
full avg10=0.00 avg60=0.00 avg300=0.00 total=...
```

| 词 | 含义（`psi_types.h:51` 的注释） |
|---|---|
| **some** | 有任务卡在这类资源上，同时还有任务在干活 |
| **full** | 有任务卡住，而且没有任务在干活 |
| `avg10/60/300` | 近 10 / 60 / 300 秒的百分比，打印成 `整数.两位小数` |
| `total` | 累计卡住的微秒 |

系统级（`/proc/pressure` 那一组，以及 cgroup 根）有一个例外：`psi_show` 认为 CPU 的 **full** 在系统级没有定义，那一行打成 0（`:1282`）。cgroup 非根节点仍有 cpu full。

资源枚举 `enum psi_res`（`psi_types.h:41`）：`PSI_IO`、`PSI_MEM`、`PSI_CPU`，`CONFIG_IRQ_TIME_ACCOUNTING` 时再加 `PSI_IRQ`，最后是 `NR_PSI_RESOURCES`。`struct cgroup` 的 `psi_files[NR_PSI_RESOURCES]` 用的就是这个下标。

每个 cgroup 有 `struct psi_group *psi`。根节点不单独分配：`cgroup_psi()`（`include/linux/psi.h:35`）在 inode 号为 1 时返回全局的 `psi_system`，否则返回 `cgrp->psi`。

`struct psi_group psi_system` 定义在 `kernel/sched/psi.c:171`。

任务找自己的组：`task_psi_group()`（`:892`）。`CONFIG_CGROUPS` 且 `psi_cgroups_enabled` 打开时，走 `cgroup_psi(task_dfl_cgroup(task))`，否则用 `&psi_system`。

状态变化的核心是 `psi_group_change`（`:799`）。任务侧的包装：

| 符号 | 位置 | 作用 |
|---|---|---|
| `psi_group_change` | `psi.c:799` | 在某个 CPU 上改一组任务计数，并记下 some/full 时间 |
| `task_psi_group` | `:892` | 任务 → `psi_group` |
| `psi_task_change` | `:916` | 任务的等待位变了（例如开始/结束 IO 等待） |
| `psi_task_switch` | `:933` | 切换时把「在 CPU 上」从 prev 挪到 next |
| `psi_memstall_enter` / `psi_memstall_leave` | `:1060` / `:1091` | 内存回收把当前任务标成 memstall |
| `psi_cgroup_alloc` / `psi_cgroup_free` | `psi.c:1116` / `:1135` | cgroup 创建/销毁时分配或释放 `psi_group` |
| `psi_show` | `:1255` | 格式化 some/full |
| `cgroup_io_pressure_show` 等 | `cgroup.c:3753` 起 | 四个 `*.pressure` 的 seq_show，转去 `psi_show` |

**为何只看用量不够**（对照 [09](./09-observability.md) 里 sampled on 6.12 的数字）：

- `memory.current` 高但 `memory.pressure` 的 avg 接近 0：吃得多，采样窗口里没有在回收里干等。  
- `cpu.stat` 的 usage 不高但 `cpu.pressure` 高：在等 CPU，不是算得很欢。  
- 限流是否伤人：同时看 `cpu.stat` 的 `throttled_usec` 和 `cpu.pressure`。

---

## 8.2 rstat：分层统计怎么汇总

每 CPU 的热数据在 `struct cgroup_rstat_cpu`（`cgroup-defs.h:327`），指针是 `cgroup->rstat_cpu`。控制器要参加 flush 时，把 `css->rstat_css_node` 挂到 `cgroup->rstat_css_list` 上。

```c
struct cgroup_rstat_cpu {
	/* bsync 保护 bstat。热路径上更新的就是这两个 */
	struct u64_stats_sync bsync;
	struct cgroup_base_stat bstat;

	/* 上次读走时的快照，用来算要传给父节点的增量 */
	struct cgroup_base_stat last_bstat;

	/* 本 cgroup 加后代的每 CPU 累计时间。cgroupfs 还没把它做成独立文件 */
	struct cgroup_base_stat subtree_bstat;
	struct cgroup_base_stat last_subtree_bstat;

	/*
	 * 自上次 flush 以来，在这个 CPU 上有更新的孩子，
	 * 通过 updated_next 挂在父节点的 updated_children 上。
	 * 链表用本 cgroup 自己结尾，不是 NULL。
	 * 因此 updated_next == NULL 表示「不在名单上」。
	 * 锁是每 CPU 的 cgroup_rstat_cpu_lock。
	 */
	struct cgroup *updated_children;
	struct cgroup *updated_next;
};
```

`struct cgroup_base_stat`（`:299`）里是 `struct task_cputime cputime`。`CONFIG_SCHED_CORE` 时多一个 `u64 forceidle_sum`。

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_rstat_cpu` | `rstat.c:16` | `per_cpu_ptr(cgrp->rstat_cpu, cpu)` |
| `__cgroup_rstat_updated` | `:30` | 热路径：本节点和祖先挂进该 CPU 的 updated 树。已在树上就返回 |
| `cgroup_rstat_updated` | `:81` | 给 BPF 的包装，先检查 cpu 号合法 |
| `cgroup_rstat_cpu_pop_updated` | `:104` | 拆 updated 树。保证若访问了孩子，父节点会在孩子之后被访问 |
| `cgroup_base_stat_flush` | `:356` | 把本节点的 CPU 时间增量加到父节点 |
| `bpf_rstat_flush` | `:179` | 空的弱符号，给 BPF 统计收集器挂 |
| `cgroup_rstat_flush_locked` | `:187` | 对每个 possible CPU 拆树，再刷 base stat、BPF、以及 `rstat_css_list` 上的 `ss->css_rstat_flush` |
| `cgroup_rstat_flush` | `:246` | 拿 `cgroup_rstat_lock` 后调用上一行。可能睡眠 |
| `cgroup_rstat_flush_hold` / `cgroup_rstat_flush_release` | `:264` / `:275` | flush 之后暂时不让别人再 flush。`cpu.stat` 打印走这条（`:502`） |
| `cgroup_rstat_init` | `:281` | 分配 `rstat_cpu`，每 CPU 的 `updated_children` 先指向自己 |
| `cgroup_rstat_exit` | `:303` | 先 flush，确认没有残留 updated 指针，再 `free_percpu` |
| `cgroup_base_stat_cputime_show` | `:492` | 打印 `usage_usec` / `user_usec` / `system_usec` |

```text
某个 CPU 上改了本 cgroup 的统计
  → __cgroup_rstat_updated(cgrp, cpu)          # 本节点和祖先挂上 updated 链
读 cpu.stat / io.stat，或控制器自己要汇总
  → cgroup_rstat_flush(cgrp)                   # 或 flush_hold
       → 每个 CPU：cgroup_rstat_cpu_pop_updated
            → cgroup_base_stat_flush            # 基本 CPU 时间向上加
            → bpf_rstat_flush
            → 顺着 cgrp->rstat_css_list
                 → css->ss->css_rstat_flush(css, cpu)
```

参加后半段的控制器必须填 `cgroup_subsys.css_rstat_flush`。io 的实现是 `blkcg_rstat_flush`（`block/blk-cgroup.c:1083`）。memory 的实现是 `mem_cgroup_css_rstat_flush`（登记在 `memory_cgrp_subsys`，`mm/memcontrol.c:6921`）。

`memory.current` 读的是 `page_counter` 的 `usage`，那是原子计数，不经过这棵 updated 树。`memory.stat` 的细分才走 memcg 自己的 per-CPU vmstat，再在 `css_rstat_flush` 里向上加。所以「每次 cat 都精确到上一条指令」不成立：有批和缓存。排障够用；拿去当基准测试的瞬时值时要心里有数。

---

## 8.3 和控制器的关系

| 你读的文件 | 背后 |
|---|---|
| `cpu.stat` 的 usage/user/system | `cgroup_base_stat_cputime_show` → rstat |
| `cpu.stat` 的 `nr_throttled` 等 | cpu 控制器的 `cpu_extra_stat_show`，读 `cfs_bandwidth`，不经 rstat |
| `memory.current` / `memory.events` | `page_counter` 和 `memory_events[]` 原子计数 |
| `memory.stat` | `memcg_vmstats`，flush 回调是 `mem_cgroup_css_rstat_flush` |
| `*.pressure` | PSI 的 `psi_show` |
| `cgroup.pressure` | `psi_group->enabled` |
| `io.stat` | 先 `cgroup_rstat_flush`，再打 blkg；增量来自 `blkcg_rstat_flush` |

---

*对照：`kernel/sched/psi.c`、`kernel/cgroup/rstat.c`、`include/linux/cgroup-defs.h`。*
