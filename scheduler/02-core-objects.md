# 2. 核心对象

**结论**：记住五层——**`task_struct`（任务）** 挂 **`sched_entity`（fair 记账实体）**，实体挂在某 CPU 的 **`cfs_rq`** 上，所有类的队列收在每 CPU **`struct rq`**；组调度时再叠一层 **`task_group`**（对接 cgroup cpu）。选人算法走 **`sched_class` 函数表**。

> 定义主战场：`kernel/sched/sched.h`、`include/linux/sched.h`

---

## 2.1 关系一张图

```text
per-CPU struct rq
  ├─ cfs  (struct cfs_rq) ── tasks_timeline (RB, 按 deadline)
  │         ▲
  │         │ on_rq 实体
  │    sched_entity  ◄── task_struct.se   （普通 fair 任务）
  │         │
  │         └─ parent / my_q  （组调度：实体代表整个 task_group）
  ├─ rt / dl /（可选）scx
  ├─ curr / donor / idle / stop
  └─ next_class

task_struct
  ├─ policy / prio / static_prio / normal_prio / rt_priority
  ├─ se / rt / dl
  ├─ sched_class*
  └─ sched_task_group*  → task_group → cgroup css（cpu 控制器）
```

---

## 2.2 `struct sched_entity`（fair 实体）

`include/linux/sched.h:572`

### EEVDF / 时间账本

| 成员 | 中文 |
|---|---|
| `u64 deadline` | **虚拟截止时间** vd；EEVDF 按 earliest eligible deadline 排序 |
| `u64 vruntime` | 虚拟运行时间；权重越大涨得越慢 |
| `s64 vlag` | 近似 **virtual lag**（相对平均虚拟时间的欠/超账） |
| `u64 vprot` | 「保护」截止，用于最小量子保护（`protect_slice`） |
| `u64 slice` | 本次请求大小（默认 `sysctl_sched_base_slice`） |
| `u64 min_vruntime` / `min_slice` / `max_slice` | RB 树增强字段（子树聚合，供剪枝） |
| `u64 exec_start` / `sum_exec_runtime` / `prev_sum_exec_runtime` | 墙上执行时间记账 |

### 队列与标志

| 成员 | 中文 |
|---|---|
| `struct load_weight load` / `h_load` | 权重；层次负载 |
| `struct rb_node run_node` | 挂在 `cfs_rq->tasks_timeline` |
| `struct list_head group_node` | 组相关链表节点 |
| `unsigned char on_rq` | 是否在 cfs_rq 上 |
| `unsigned char sched_delayed` | 延迟出队等状态 |
| `unsigned char rel_deadline` / `custom_slice` | 相对 deadline / 自定义 slice |

### 组调度（`CONFIG_FAIR_GROUP_SCHED`）

| 成员 | 中文 |
|---|---|
| `int depth` | 组层次深度 |
| `struct sched_entity *parent` | 父实体（上层 group se） |
| `struct cfs_rq *cfs_rq` | 当前所在 cfs_rq |
| `struct cfs_rq *my_q` | 本组拥有的 per-CPU cfs_rq |
| `unsigned long runnable_weight` | 缓存的 runnable 权重 |
| `struct sched_avg avg` | PELT 平均（独立 cache line） |

任务与实体：普通 fair 任务的 `task_struct.se` 就是叶子实体；`entity_is_task(se)` 区分「真实任务」vs「代表 cgroup 的 group entity」。

---

## 2.3 `struct cfs_rq`

`kernel/sched/sched.h:681`

| 分组 | 成员（节选） | 中文 |
|---|---|---|
| 计数 | `nr_queued` / `h_nr_queued` / `h_nr_runnable` / `h_nr_idle` | 本层与层次（h_）排队/可跑数 |
| EEVDF 平均 | `sum_w_vruntime` / `sum_weight` / `zero_vruntime` / `sum_shift` | 构造加权平均虚拟时间 V |
| 树 | `struct rb_root_cached tasks_timeline` | 按 **deadline** 排序的 runnable 实体树 |
| 当前 | `curr` / `next` / `h_curr` | 当前跑的实体、buddy、层次当前 |
| 负载 | `load` / `avg` / `removed` | 即时负载与 PELT |
| 组 | `rq` / `tg` / `leaf_cfs_rq_list` / `h_load` … | 挂回 CPU rq、所属 `task_group` |
| 带宽 | `runtime_enabled` / `runtime_remaining` / `throttled*` | CFS bandwidth（cgroup cpu.max） |

**直觉**：每个 CPU 上至少有一个根 `rq->cfs`；开了组调度后，每个 `task_group` 在每个 CPU 还有自己的 `cfs_rq`。

---

## 2.4 `struct rq`（每 CPU 运行队列）

`kernel/sched/sched.h:1158`（大结构，分组记）

| 分组 | 成员（节选） | 中文 |
|---|---|---|
| 热计数 | `nr_running` / `ttwu_pending` / `cpu_capacity` | 可跑任务数、挂起的唤醒、容量 |
| 当前 | `curr` / `donor` / `idle` / `stop` | 正在执行者；proxy-exec 时 donor≠curr |
| 锁与切换 | `__lock` / `nr_switches` | rq 自旋锁、切换计数 |
| 子队列 | `cfs` / `rt` / `dl` /（可选）`scx` | 各类 runqueue |
| 类提示 | `next_class` | 下一次优先考虑的 sched_class |
| 其它 | `leaf_cfs_rq_list` / `clock*` / `numa_*` … | 负载均衡叶子列表、时钟、NUMA |

锁约定：改队列、选人，通常持 **`rq->__lock`**（`raw_spinlock_t`）。唤醒路径还涉及 `p->pi_lock`。见 [locks-and-ds](../locks-and-ds/)。

---

## 2.5 `struct sched_class`

`kernel/sched/sched.h:2623` —— **函数表**，不是「类对象里塞状态」。

| 回调（节选） | 中文 |
|---|---|
| `enqueue_task` / `dequeue_task` | 任务入/出该类队列 |
| `yield_task` / `yield_to_task` | 主动让出 |
| `wakeup_preempt` | 唤醒后是否抢占当前 |
| `balance` | 选人前的均衡机会 |
| `pick_task` | **选出下一个要跑的 task** |
| `put_prev_task` / `set_next_task` | 切换前后簿记 |
| `select_task_rq` / `migrate_task_rq` | 选 CPU / 迁移后处理 |
| `task_woken` / `set_cpus_allowed` | 唤醒后钩子、亲和性变更 |

fair 的实例：`DEFINE_SCHED_CLASS(fair)` 在 `fair.c:15855`。

---

## 2.6 `struct task_group`

`kernel/sched/sched.h:478` —— **cpu 控制器在调度器里的实体**。

| 成员（节选） | 中文 |
|---|---|
| `struct cgroup_subsys_state css` | 嵌入 css；`container_of` 找回 tg |
| `struct cfs_rq __percpu *cfs_rq` | 每 CPU 一组 fair 队列 |
| `unsigned long shares` | 权重份额（对接 cpu.weight） |
| `atomic_long_t load_avg` / `runnable_avg` | 组级负载 |
| `struct cfs_bandwidth cfs_bandwidth` | quota/period 带宽 |
| `parent` / `siblings` / `children` | 组树 |
| `struct scx_task_group scx` | sched_ext 侧组状态 |
| `uclamp*` | 利用率夹具（若开启） |

回链：[cgroup/05-cpu-cpuset.md](../cgroup/05-cpu-cpuset.md)、本章 [07](./07-cgroup-cpu.md)。

---

## 2.7 `task_struct` 上的调度字段

`include/linux/sched.h`（约 `:885`–`:938`）

| 字段 | 中文 |
|---|---|
| `static_prio` / `normal_prio` / `rt_priority` | nice 静态优先级 / 正常优先级 / RT 优先级 |
| `se` / `rt` / `dl` | 三类调度实体（嵌入） |
| `const struct sched_class *sched_class` | 当前所属调度类 |
| `struct task_group *sched_task_group` | 所属调度组 |
| `unsigned int policy` | `SCHED_*` 策略 |

策略切换会改 `sched_class` 指针（见 `core.c` 里 `SCHED_EXT`/`FIFO`/`NORMAL` 分支，约 `:4873`）。

---

## 2.8 关键函数 / 宏

| 符号 | 位置 | 作用 |
|---|---|---|
| `cpu_rq(cpu)` / `this_rq()` | sched.h | 取 per-CPU rq |
| `task_rq(p)` | sched.h | 任务当前 rq |
| `cfs_rq_of(se)` | fair 侧 | 实体所在 cfs_rq |
| `task_of(se)` | fair 侧 | 叶子实体 → task |
| `for_each_sched_entity(se)` | 组调度 | 沿 parent 向上 |
| `css_tg(css)` | sched.h:551 | css → task_group |

---

**导航**：[← 总览](./01-overview.md) · [章节目录](./README.md) · [EEVDF →](./03-eevdf.md)
