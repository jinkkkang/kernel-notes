# 10. 回收、swap、OOM

**结论**：v7.3 同时有传统 LRU 和 MGLRU（`CONFIG_LRU_GEN`）。`shrink_node` 在全局回收且 MGLRU 打开时走 `lru_gen_shrink_node`，否则走按 memcg 的 `shrink_node_memcgs`。kswapd 在后台把水位拉回 HIGH；直接回收在分配者上下文里做同一件事，延迟算在那个任务上。没有 swap 时匿名页收不走。

> 主文件：`mm/vmscan.c`。OOM：`mm/oom_kill.c`。swap 压缩缓存：`mm/zswap.c`。
> 设计文档：`Documentation/mm/multigen_lru.rst`。管理接口：`Documentation/admin-guide/mm/multigen_lru.rst`。
> memcg 的 high/max/charge 不在这里重复，见 [cgroup/06-memory.md](../../cgroup/06-memory.md)。
> 内核版本：**v7.3**（`c03c1a14507d`）。行号对这棵树。
> 旧文 [10-reclaim.md](../10-reclaim.md) 只留跳转。
> 导航：[上一节 THP](../09-thp-and-hugetlb.md) · [章目录](../README.md) · [下一节 KVM](../11-kvm-memory.md)

## 本目录

| 文件 | 讲什么 | 旧小节 |
|---|---|---|
| [01-overview.md](./01-overview.md) | 水位、kswapd 与直接回收、`struct scan_control` 全成员、`shrink_node` 分叉、shrinker 入口、cgroup 衔接、本机实测 | 10.2、10.5 |
| [02-lru-classic.md](./02-lru-classic.md) | 经典双链表：入链、访问提升、比例、扫描、swappiness、workingset | 10.1 里 `lruvec` 的经典半边，其余是加厚 |
| [03-lru-problems.md](./03-lru-problems.md) | 经典 LRU 的问题。每条带出处，指回经典路径 | 新写 |
| [04-mglru-design.md](./04-mglru-design.md) | 代、档、页表遍历、Bloom、look-around、PID、memcg 代。结构体全成员 | 10.1 的结构体，设计按文档展开 |
| [05-mglru-code-path.md](./05-mglru-code-path.md) | 入代、访问、老化、驱逐、`min_ttl` | 10.1 的老化，路径加厚 |
| [06-lru-vs-mglru.md](./06-lru-vs-mglru.md) | 四个时刻对照、开关、观测、何时开关（RECIPE） | 新写 |
| [07-swap-zswap.md](./07-swap-zswap.md) | swap 与 zswap / zsmalloc | 10.3 |
| [08-oom.md](./08-oom.md) | `oom_badness` 与杀死之后 | 10.4 |
| [09-mlock-unevictable.md](./09-mlock-unevictable.md) | mlock 与 unevictable | 10.6 |
| [10-migration.md](./10-migration.md) | `migrate_pages` | 10.7 |
| [11-swap-table.md](./11-swap-table.md) | `struct swap_table` | 10.8 |

## 阅读顺序

```text
01 回收总入口（谁在扫、scan_control、两条分叉）
  → 02 经典 LRU 在四个时刻做什么
  → 03 文档和提交说明里写了哪些问题
  → 04 MGLRU 的代、档、反馈
  → 05 MGLRU 代码路径
  → 06 两套并排，含开关和 RECIPE
  → 07 swap/zswap → 08 OOM → 09 mlock → 10 迁移 → 11 swap table
```

先看清 `shrink_node` 的分叉，再读经典路径。不先读 02，03 里的「指到哪段代码」对不上。MGLRU 没编进内核时，`lru_gen_enabled()` 恒为 false，05 里的老化函数体是 `BUILD_BUG()`，调用被裁掉。

[上一节 THP](../09-thp-and-hugetlb.md) · [章目录](../README.md) · [下一节 KVM](../11-kvm-memory.md)
