# 10. 回收、swap、OOM

**结论**：v6.6.160 同时有传统 LRU 和 MGLRU（`CONFIG_LRU_GEN`）。`shrink_node` 在全局回收且 `lru_gen_enabled()` 为真时走 `lru_gen_shrink_node` 后返回；否则走 `shrink_node_memcgs`。kswapd 在后台把可用 zone 拉到 HIGH 水位以上；直接回收在分配者上下文里做同一件事，延迟算在那个任务上。没有 swap、也不能降级时，匿名页收不走（`MADV_FREE` 除外）。

> 主文件：`mm/vmscan.c`。OOM：`mm/oom_kill.c`。swap 槽：`include/linux/swap.h` 的 `swap_info_struct` / `swap_cluster_info`，分配在 `mm/swapfile.c`。压缩缓存：`mm/zswap.c`。
> 设计文档：`Documentation/mm/multigen_lru.rst`。管理接口：`Documentation/admin-guide/mm/multigen_lru.rst`。
> memcg 的 high/max/charge 不在这里重复，见 [cgroup/06-memory.md](../cgroup/06-memory.md)。
> 内核版本：**v6.6.160**。行号对这棵树。
> 导航：[上一节 THP](./09-thp-and-hugetlb.md) · [章目录](./README.md) · [下一篇 总入口](./10-01-overview.md)

## 本目录

| 文件 | 讲什么 | 旧小节 |
|---|---|---|
| [10-01-overview.md](./10-01-overview.md) | 水位、kswapd 与直接回收、`struct scan_control` 全成员、`shrink_node` 分叉、shrinker 入口、cgroup 衔接、6.12 采样 | 10.2、10.5 |
| [10-02-lru-classic.md](./10-02-lru-classic.md) | 经典双链表：入链、访问提升、比例、扫描、swappiness、workingset | 10.1 里 `lruvec` 的经典半边，其余是加厚 |
| [10-03-lru-problems.md](./10-03-lru-problems.md) | 经典 LRU 的问题。每条带出处，指回经典路径 | 新写 |
| [10-04-mglru-design.md](./10-04-mglru-design.md) | 代、档、页表遍历、Bloom、look-around、PID、memcg 代。结构体全成员 | 10.1 的结构体，设计按文档展开 |
| [10-05-mglru-code-path.md](./10-05-mglru-code-path.md) | 入代、访问、老化、驱逐、`min_ttl` | 10.1 的老化，路径加厚 |
| [10-06-lru-vs-mglru.md](./10-06-lru-vs-mglru.md) | 四个时刻对照、开关、观测、何时开关（RECIPE） | 新写 |
| [10-07-swap-zswap.md](./10-07-swap-zswap.md) | `swap_info_struct`、`swap_cluster_info`、`swap_map` 槽计数，以及 zswap / zsmalloc | 10.3 |
| [10-08-oom.md](./10-08-oom.md) | `oom_badness` 与杀死之后 | 10.4 |
| [10-09-mlock-unevictable.md](./10-09-mlock-unevictable.md) | mlock 与 unevictable | 10.6 |
| [10-10-migration.md](./10-10-migration.md) | `migrate_pages` | 10.7 |

## 阅读顺序

```text
01 回收总入口（谁在扫、scan_control、两条分叉）
  → 02 经典 LRU 在四个时刻做什么
  → 03 文档和源码注释里写了哪些问题
  → 04 MGLRU 的代、档、反馈
  → 05 MGLRU 代码路径
  → 06 两套并排，含开关和 RECIPE
  → 07 swap/zswap → 08 OOM → 09 mlock → 10 迁移
```

先看清 `shrink_node` 的分叉，再读经典路径。不先读 02，03 里的「指到哪段代码」对不上。没编 `CONFIG_LRU_GEN` 时，`lru_gen_enabled()` 恒为 false（`include/linux/mm_inline.h:295`），`lru_gen_age_node` / `lru_gen_shrink_node` / `lru_gen_shrink_lruvec` 是空函数（`mm/vmscan.c:6272` 起）。

## 旧小节 → 新位置

| 旧小节 | 新位置 |
|---|---|
| 10.1 谁在 LRU 上，含 MGLRU 结构与代际老化 | 结构在 [10-04-mglru-design.md](./10-04-mglru-design.md)；经典 `lruvec` 在 [10-02-lru-classic.md](./10-02-lru-classic.md)；老化在 [10-05-mglru-code-path.md](./10-05-mglru-code-path.md) |
| 10.2 kswapd 与直接回收 | [10-01-overview.md](./10-01-overview.md) |
| 10.3 swap 与 zswap | [10-07-swap-zswap.md](./10-07-swap-zswap.md) |
| 10.4 OOM | [10-08-oom.md](./10-08-oom.md) |
| 10.5 和 cgroup 章的衔接 | [10-01-overview.md](./10-01-overview.md) |
| 10.6 unevictable 与 mlock | [10-09-mlock-unevictable.md](./10-09-mlock-unevictable.md) |
| 10.7 `migrate_pages` | [10-10-migration.md](./10-10-migration.md) |
| 10.8 swap 槽（`swap_info_struct` / `swap_cluster_info` / `swap_map`） | [10-07-swap-zswap.md](./10-07-swap-zswap.md) |
| 10.9 关键函数表 | 拆进上面各篇末尾的函数表 |
| （新写）经典 LRU 的问题 | [10-03-lru-problems.md](./10-03-lru-problems.md) |
| （新写）两套对照 | [10-06-lru-vs-mglru.md](./10-06-lru-vs-mglru.md) |

[上一节 THP](./09-thp-and-hugetlb.md) · [章目录](./README.md) · [下一篇 总入口](./10-01-overview.md)
