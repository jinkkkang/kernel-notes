# 5. 负载、迁移与亲和

**结论**：单 CPU 上 EEVDF 决定「这个核上谁先跑」；**跨 CPU** 靠负载均衡把 runnable 任务挪匀。v7.3 里经典名 `load_balance` 已收成 **`sched_balance_rq`**（`fair.c:13734`），周期性 / idle / newidle 等入口经 `sched_balance_domains` / `sched_balance_trigger` 进来。PELT 给「谁重」提供平滑后的负载信号。

---

## 5.1 PELT（点到）

文件：`kernel/sched/pelt.c`。核心更新：`___update_load_avg`（约 `:258`）。

| 概念 | 直觉 |
|---|---|
| `sched_avg` | 每个 `sched_entity` / `cfs_rq` 上的指数衰减平均 |
| `load_avg` / `util_avg` / `runnable_avg` | 负载、利用率、可跑平均 |
| 时间轴 | 用调度时钟；睡眠/节流时要停表，避免「睡着还涨利用率」 |

入队出队里的 `update_load_avg(...)`（见 [04](./04-enqueue-wakeup.md)）就是在喂 PELT。负载均衡看的是 **平滑后的画像**，不是瞬间 `nr_running` 一张快照——所以短毛刺不一定立刻迁进程。

---

## 5.2 均衡发生在哪些场景

| 场景 | 入口（fair.c 一带） | 何时 |
|---|---|---|
| 周期均衡 | `sched_balance_domains` `:14243` ← `sched_balance_trigger` `:15031` | tick/软中断里触发 |
| newidle | `sched_balance_newidle` `:14856` | 即将 idle，顺手拉活 |
| active balance | `active_load_balance_cpu_stop` `:14152` | 忙核推任务到目标核（stopper 线程） |
| nohz | `nohz_*balance*` | 动态 tick 下的 idle 均衡 |

`sched_balance_rq`（`:13734`）骨架：

1. `should_we_balance` —— 本 CPU 是不是该域负责者  
2. `sched_balance_find_src_group` —— 找到更忙的组  
3. `sched_balance_find_src_rq` —— 落到具体忙 rq  
4. 尝试拉任务；不行再 kick active balance  

大域可设 `SD_SERIALIZE`，用 `sched_balance_running` 原子变量避免多个均衡者同时扫巨型拓扑。

---

## 5.3 亲和性与 cpuset

| 机制 | 作用 | 和均衡的关系 |
|---|---|---|
| `sched_setaffinity` / `cpu_allowed` | 用户态绑核 | `select_task_rq` / 迁移必须落在允许掩码内 |
| cgroup **cpuset** | 容器/作业 CPU 集合 | 见 [cgroup/05](../cgroup/05-cpu-cpuset.md)；均衡不会突破 cpuset |
| `migrate_disable` | 短临界区禁止迁移 | 均衡与部分唤醒路径要尊重 |

**怀疑「跑偏」时先看**：

1. 任务 `Cpus_allowed`（`/proc/<pid>/status`）是否被误绑到单核  
2. cpuset 是否把负载关进小岛，而岛外空闲  
3. 是否 NUMA 偏好 / `memory_nodes` 把算力和内存拆开（深挖另章）  
4. 是否 active balance 失败导致「看着不均但迁不动」（亲和、钉核、`PF_*`）

---

## 5.4 不均衡的可见症状（运维视角）

| 症状 | 更可能 | 先查 |
|---|---|---|
| 总 CPU 不高，但尾延迟差 | 单核排队 / 亲和过窄 | `ps -eLo psr,...`、affinity、perf sched |
| 有核 100%、有核闲 | 均衡慢、钉核、cpuset 碎片 | 域拓扑、是否 SERIALIZE 跳过 |
| 迁移计数狂跳 | 抖动迁 | `nr_migrations`（se）、schedstat（若开）、是否错误的负载信号 |

---

## 5.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `___update_load_avg` | pelt.c:258 | PELT 更新 |
| `sched_balance_rq` | fair.c:13734 | 域内平衡（旧 load_balance） |
| `should_we_balance` | `:13625` | 是否由本 CPU 执行 |
| `sched_balance_domains` | `:14243` | 遍历域 |
| `sched_balance_trigger` | `:15031` | 触发入口 |
| `sched_balance_newidle` | `:14856` | newidle 平衡 |
| `active_load_balance_cpu_stop` | `:14152` | 主动推任务 |

---

**导航**：[← 入队与唤醒](./04-enqueue-wakeup.md) · [章节目录](./README.md) · [观测 →](./08-observability.md)
