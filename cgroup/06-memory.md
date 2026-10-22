# 6. memory

**结论**：memory 控制器给页（folio）**收费（charge）**到某个 `mem_cgroup`；超额时先走 **high** 的温和回收压力，再撞 **max** 的硬壁，必要时 **OOM**。本章讲接口与 charge 直觉；**页回收算法、LRU、kswapd 深挖留给 MM 章**。

> 源码：`mm/memcontrol.c`、`include/linux/memcontrol.h`  
> 文档：`cgroup-v2.rst` §5.2

---

## 6.1 用户可见文件（v2 核心）

| 文件 | 含义 |
|---|---|
| `memory.current` | 当前用量（字节） |
| `memory.peak` | 历史峰值 |
| `memory.min` / `memory.low` | 保护：尽量别从这组回收（low 弱于 min） |
| `memory.high` | **软顶**：超过后大力回收/主动降速，一般不立刻 OOM |
| `memory.max` | **硬顶**：冲不上去就回收；仍不够则 OOM；`max` = 无限 |
| `memory.oom.group` | OOM 时是否整组一起杀 |
| `memory.events` | `high`/`max`/`oom`/`oom_kill` 等计数 |
| `memory.stat` | anon/file/kernel/… 细分 |
| `memory.swap.*` | swap 用量与限额（`current`/`high`/`max`/`events`） |
| `memory.pressure` | PSI |
| `memory.reclaim` | 手动触发回收（写） |

本机 `/sys/fs/cgroup/agent`（只读快照，见 [09](./09-observability.md)）：`memory.max=max`，`memory.current` 约 4.7GB 量级，`memory.events` 全 0。

---

## 6.2 `struct mem_cgroup`（分组记）

`include/linux/memcontrol.h:202`

| 成员（节选） | 中文 |
|---|---|
| `struct cgroup_subsys_state css` | 嵌入的 css（第一成员） |
| `struct mem_cgroup_private_id id` | 可在 cgroup 死后仍标识对象的 ID |
| `struct page_counter memory` | 主计数器（v1/v2） |
| `struct page_counter swap` / `memsw` | v2 swap / v1 mem+swap |
| `struct work_struct high_work` | 超 high 时的处理工作 |
| `bool oom_group` | 对应 `memory.oom.group` |
| `atomic_long_t memory_events[]` | `memory.events` |
| `struct memcg_vmstats *vmstats` | `memory.stat` 后端 |
| `struct memcg_vmstats_percpu __percpu *vmstats_percpu` | per-CPU 热路径统计 |

计数器类型是 `page_counter`：`try_charge` / `charge` / `uncharge` 层级向上累加。

---

## 6.3 Charge 直觉

```text
分配 folio（匿名页、页缓存等）
  → mem_cgroup_charge() / __mem_cgroup_charge()   # memcontrol.c:5221
       → get_mem_cgroup_from_mm(mm)
       → charge_memcg → try_charge_memcg (:2645)
            → 先consume stock（批计量）
            → page_counter_try_charge(&memcg->memory / swap)
            → 失败：回收 / 重试 / 可能 OOM
  → 页打上 memcg 归属
释放 / 迁移
  → uncharge 路径（page_counter_uncharge 等）
```

| 符号 | 位置 | 作用 |
|---|---|---|
| `try_charge_memcg` | `mm/memcontrol.c:2645` | 尝试收费核心 |
| `__mem_cgroup_charge` | `:5221` | 从 mm 找 memcg 并 charge |
| `mem_cgroup_charge` | `include/linux/memcontrol.h:657` | 内联包装 |
| `mem_cgroup_swapin_charge_folio` | `memcontrol.c:5279` | swapin 收费 |
| `mem_cgroup_charge_hugetlb` | `:5245` | hugetlb 与 memcg 交叉 |

**归属规则直觉**（细节以文档 §5.2.4 为准）：页通常记在 **fault / 分配时** 的 memcg；进程迁 cgroup **不会**把已分配页全部搬记账——这是「迁移后 current 看起来不对」的常见原因之一。

---

## 6.4 high vs max vs OOM

| 水位 | 行为直觉 |
|---|---|
| 低于 high/max | 正常 |
| 越过 **high** | 触发回收压力（`high_work` 等），进程可能被主动 throttle；`memory.events` 的 `high`++ |
| 撞上 **max** | 分配路径卡在回收；仍失败 → OOM；`max`/`oom`/`oom_kill` 事件 |
| `oom.group=1` | 杀一组而不只杀「罪魁」单进程 |

`try_charge_memcg` 注释里强调：dying 任务、与 high 的批处理行为、和页面分配器本身策略并不完全同一套——读源码时别把三者混成一句「超限就杀」。

---

## 6.5 留给 MM 章的

- `shrinker`、lruvec、global vs memcg reclaim 谁先谁后  
- writeback domain（`cgwb_*`）与脏页  
- zswap 与 memcg 的完整交互  
- 与 cpuset.mems 的节点束缚如何进分配慢路径  

本章够用目标：**看见 sysfs 数字时，知道背后是 page_counter + charge 路径，而不是魔法。**

---

*对照：`mm/memcontrol.c`、`include/linux/memcontrol.h`。*
