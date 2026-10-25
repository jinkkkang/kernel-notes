# 5. 负载、迁移与亲和

**结论**：单 CPU 上 EEVDF 决定「这个核上谁先跑」；**跨 CPU** 靠 `load_balance` 把可跑任务挪匀。周期 / idle / newidle 从不同入口进来，最后都调到同一个 `load_balance`（`fair.c:11332`）。PELT 给「谁重」提供平滑后的负载，所以短毛刺不一定立刻迁进程。

---

## 5.1 PELT（点到）

文件：`kernel/sched/pelt.c`。

| 符号 | 行号 | 作用 |
|---|---|---|
| `___update_load_sum` | 180 | 按时间把 `load_sum` / `runnable_sum` / `util_sum` 衰减并累加 |
| `___update_load_avg` | 257 | 用上面的和除出 `load_avg` / `runnable_avg` / `util_avg` |
| `__update_load_avg_se` | 306 | 更新一个 `sched_entity` |
| `__update_load_avg_cfs_rq` | 320 | 更新一个 `cfs_rq` |

时间轴用 PELT 时钟（`rq->clock_pelt`）。任务睡眠、队列被限流时要把时钟停住，避免「睡着还在涨利用率」。入队出队里的 `update_load_avg`（见 [04](./04-enqueue-wakeup.md)）就是在喂这些函数。

负载均衡看的是平滑后的 `load_avg` / `util_avg` / `h_load`，外加人数 `nr_running`。`h_load` 在 `cfs_rq` 上（`sched.h:608`），是组的权重再乘一层递归份额，给跨组比较用。

---

## 5.2 均衡发生在哪些场景

| 场景 | 入口 | 何时 |
|---|---|---|
| 周期均衡 | `trigger_load_balance`（`fair.c:12540`）在 tick 里被 `core.c:5690` 调用；到点就 `raise_softirq(SCHED_SOFTIRQ)`。softirq 跑 `run_rebalance_domains`（`:12515`）→ `rebalance_domains`（`:11779`）→ `load_balance` | 本 CPU 的 `next_balance` 到了 |
| newidle | `balance_fair`（`:8300`）在 rq 上没有可跑任务时调用 `sched_balance_newidle`（`:12368`） | `__pick_next_task` 选人前，fair 的 `sched_class.balance`。本 CPU 即将 idle，顺手拉活 |
| active balance | `active_load_balance_cpu_stop`（`:11641`） | 忙核用 stop 类线程把任务推到目标核 |
| nohz | `nohz_balancer_kick`（`:11935`）挑一个 CPU 做代表；`nohz_idle_balance`（`:12286`）在 `run_rebalance_domains` 开头先跑 | 动态 tick 停了的 idle CPU，由别人代为均衡 |

`load_balance`（`:11332`）骨架：

1. `should_we_balance`（`:11262`）。`CPU_NEWLY_IDLE`：本 rq 已经有人或有挂起唤醒就放弃，否则谁都可以做。其它情况：优先让组里第一个空闲核来做，没有空闲核就让 `group_balance_cpu`  
2. `find_busiest_group`（`:10906`）——找到更忙的调度组  
3. `find_busiest_queue`（`:11047`）——落到具体忙 rq  
4. 从忙 rq 上拉任务；拉不动且需要搬家当前任务时，再踢 active balance  

NUMA 调度域会带上 `SD_SERIALIZE`（`kernel/sched/topology.c:1646`）。`rebalance_domains`（`:11814`）看到这个标志就 `spin_trylock(&balancing)`（锁在 `fair.c:11719`）。拿不到锁就跳过这一层，避免多个 CPU 同时扫同一片 NUMA 域。

`sched_balance_newidle` 还有几道自己的门：`ttwu_pending` 非 0 直接返回（马上有人要被唤醒）；`rd->overload` 为 0 或 `avg_idle` 比这个域的 `max_newidle_lb_cost` 还短，也不拉（拉任务花的时间可能比剩下的 idle 还长）。域上有 `SD_BALANCE_NEWIDLE` 才真的 `load_balance(..., CPU_NEWLY_IDLE)`。特性 `NI_RANDOM`（默认开）用 `sched_rng()` 按该域最近的成功率掷骰子，失败过多次就少做几次。

---

## 5.3 亲和性与 cpuset

| 机制 | 作用 | 和均衡的关系 |
|---|---|---|
| `sched_setaffinity` / `cpus_mask` | 用户态绑核 | `select_task_rq` 和迁移必须落在 `cpus_ptr` 里 |
| cgroup **cpuset** | 容器/作业的 CPU 集合 | 见 [cgroup/05](../cgroup/05-cpu-cpuset.md)。独占 cpuset 会分成独立 `root_domain`，均衡不出这个岛 |
| `migration_disabled` | `task_struct` 上的计数（`include/linux/sched.h:841`） | 非 0 时这段代码不许把任务迁走；`is_migration_disabled`（`sched.h:1200`） |

怀疑「跑偏」时先看：

1. `/proc/<pid>/status` 里的 `Cpus_allowed` / `Cpus_allowed_list` 是不是被收成了一颗核  
2. cpuset 是不是把负载关进小岛，岛外空着  
3. NUMA 偏好是不是把算力和内存拆开（`rq->nr_numa_running` 这一路，深挖另章）  
4. active balance 有没有因为亲和、钉核、`nr_pinned` 而推不动

---

## 5.4 不均衡的可见症状（运维视角）

| 症状 | 更可能 | 先查 |
|---|---|---|
| 总 CPU 不高，但尾延迟差 | 单核排队 / 亲和过窄 | `ps -eLo psr`、affinity、perf sched |
| 有核 100%、有核闲 | 均衡间隔、钉核、cpuset 碎片、`SD_SERIALIZE` 这一层被跳过 | 调度域、`nr_migrations` |
| 迁移计数狂跳 | 负载信号抖、newidle 太积极 | `se->nr_migrations`、schedstat（若打开） |

---

## 5.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `___update_load_sum` | `pelt.c:180` | PELT 衰减累加 |
| `___update_load_avg` | `pelt.c:257` | 把和变成平均值 |
| `update_load_avg` | fair 侧，多处 | 入队出队时喂 PELT |
| `load_balance` | `fair.c:11332` | 一个域内的平衡 |
| `should_we_balance` | `:11262` | 这次轮不轮到本 CPU |
| `find_busiest_group` | `:10906` | 找更忙的组 |
| `find_busiest_queue` | `:11047` | 找更忙的 rq |
| `rebalance_domains` | `:11779` | 从本 CPU 的域链往上走 |
| `run_rebalance_domains` | `:12515` | `SCHED_SOFTIRQ` 处理函数 |
| `trigger_load_balance` | `:12540` | tick 里到点就踢 softirq，并 `nohz_balancer_kick` |
| `balance_fair` | `:8300` | 类回调：没人可跑就 newidle |
| `sched_balance_newidle` | `:12368` | 即将 idle 时拉任务 |
| `active_load_balance_cpu_stop` | `:11641` | stopper 线程把任务推走 |
| `nohz_balancer_kick` | `:11935` | 给 nohz idle 均衡挑一个执行者 |
| `nohz_idle_balance` | `:12286` | 代替 tick 停掉的 CPU 做均衡 |
| `select_task_rq_fair` | fair 的类槽 | 唤醒时选 CPU |

---

**导航**：[← 入队与唤醒](./04-enqueue-wakeup.md) · [章节目录](./README.md) · [观测 →](./08-observability.md)
