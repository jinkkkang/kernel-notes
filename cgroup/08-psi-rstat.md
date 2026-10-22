# 8. PSI 与 rstat

**结论**：**usage 告诉你用了多少，PSI 告诉你「因等资源而干瞪眼」多少**。rstat 则是 cgroup 把 per-CPU 计数**汇总成你在 sysfs 里读到的数字**的基建——不是业务控制器，但读 `cpu.stat` / 部分 memory 统计时绕不开。

> PSI：`kernel/sched/psi.c`、`include/linux/psi*.h`  
> rstat：`kernel/cgroup/rstat.c`

---

## 8.1 PSI：some / full

压力文件示例（本机 root `memory.pressure`）：

```text
some avg10=0.00 avg60=0.00 avg300=0.00 total=...
full avg10=0.00 avg60=0.00 avg300=0.00 total=...
```

| 词 | 含义 |
|---|---|
| **some** | 有一部分任务在等这类资源（CPU/memory/io） |
| **full** | 相关任务**全都**在等（更「僵」） |
| `avg10/60/300` | 近 10/60/300 秒的百分比均线 |
| `total` | 累计卡顿时间（微秒量级计数，读文档确认单位） |

资源类型：`cpu.pressure`、`memory.pressure`、`io.pressure`，以及 `cgroup.pressure`（若启用）。`struct cgroup` 上有 `struct psi_group *psi`（`cgroup-defs.h`），文件句柄在 `psi_files[]`。

系统级：`struct psi_group psi_system`（`kernel/sched/psi.c:175`）。任务通过 `task_psi_group()`（`:885`）找到所属组；状态变化走 `psi_group_change`（`:796`）等。

**为何「只看 usage 不够」**：

- `memory.current` 高但 `memory.pressure` 接近 0 → 可能只是吃得多，还没在回收里打架。  
- `cpu.stat` usage 不高但 `cpu.pressure` 高 → 可能在等 CPU（抢不到），而不是「算得很欢」。  
- 限流是否伤人：对照 `cpu.stat` 的 `throttled_usec` **和** pressure。

---

## 8.2 rstat：分层统计怎么汇总

热路径上各 CPU 改自己的 per-CPU 计数，然后标记「脏了」；读 sysfs 或周期性路径再 **flush** 汇到父级。

| 符号 | 位置 | 作用 |
|---|---|---|
| `__css_rstat_updated` | `rstat.c:71` | 把 css 挂进该 CPU 的更新链表（可重入，兼顾 irq/nmi） |
| `css_rstat_updated` | `:129` | BPF 可见包装 |
| `css_rstat_flush` | `:409` | 刷一整棵更新子树；调 `ss->css_rstat_flush` |
| `css_rstat_init` / `css_rstat_exit` | `:442` / `:487` | css 生命周期里的 rstat 初始化 |

哪些 css 参与：`css_uses_rstat()`（`:22`）——`cgroup.self`（base stat）或实现了 `ss->css_rstat_flush` 的子系统。

```text
per-CPU 计数更新
  → __css_rstat_updated(css, cpu)
读文件 / 显式 flush
  → css_rstat_flush(css)
       → 按 CPU 收集 updated list
       → 对每个 css 调 ss->css_rstat_flush 或 base_stat flush
       → 向上汇总
```

**直觉**：别假设「每次 `cat memory.current` 都是全局原子精确到指令」——有缓存与批汇；对排障够用，对写基准测试要心里有数。

---

## 8.3 和控制器的关系

| 你读的文件 | 背后常碰到 |
|---|---|
| `cpu.stat` | 调度记账 + rstat/base | 
| `memory.stat` / events | memcg vmstats（自身 per-CPU）+ 事件计数 |
| `*.pressure` | PSI |
| `io.stat` | blkcg rstat（`blk-cgroup.c` 内有 `__blkcg_rstat_flush`） |

---

*对照：`kernel/sched/psi.c`、`kernel/cgroup/rstat.c`。*
