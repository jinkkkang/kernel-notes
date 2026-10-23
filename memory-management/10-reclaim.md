# 10. 回收、swap、OOM（跳转）

这一节已经拆到 [10-reclaim/](./10-reclaim/README.md)。本文件只留旧链接，正文在子目录里。

> 内核版本仍是 **v7.3**（`c03c1a14507d`）。
> 导航：[上一节 THP](./09-thp-and-hugetlb.md) · [目录](./README.md) · [下一节 KVM](./11-kvm-memory.md)

## 现在从哪读

[10-reclaim/README.md](./10-reclaim/README.md) 有阅读顺序。直接进某一篇：

| 文件 | 内容 |
|---|---|
| [01-overview.md](./10-reclaim/01-overview.md) | 水位、kswapd、`scan_control`、分叉、shrinker |
| [02-lru-classic.md](./10-reclaim/02-lru-classic.md) | 经典双链表 LRU |
| [03-lru-problems.md](./10-reclaim/03-lru-problems.md) | 经典 LRU 的问题（带出处） |
| [04-mglru-design.md](./10-reclaim/04-mglru-design.md) | MGLRU 设计与结构体 |
| [05-mglru-code-path.md](./10-reclaim/05-mglru-code-path.md) | MGLRU 代码路径 |
| [06-lru-vs-mglru.md](./10-reclaim/06-lru-vs-mglru.md) | 两套对照、开关、RECIPE |
| [07-swap-zswap.md](./10-reclaim/07-swap-zswap.md) | swap、zswap |
| [08-oom.md](./10-reclaim/08-oom.md) | OOM |
| [09-mlock-unevictable.md](./10-reclaim/09-mlock-unevictable.md) | mlock |
| [10-migration.md](./10-reclaim/10-migration.md) | `migrate_pages` |
| [11-swap-table.md](./10-reclaim/11-swap-table.md) | swap table |

## 旧小节 → 新位置

| 旧小节 | 新位置 |
|---|---|
| 10.1 谁在 LRU 上，含 MGLRU 结构与代际老化 | 结构在 [04-mglru-design.md](./10-reclaim/04-mglru-design.md)；经典 `lruvec` 在 [02-lru-classic.md](./10-reclaim/02-lru-classic.md)；老化在 [05-mglru-code-path.md](./10-reclaim/05-mglru-code-path.md) |
| 10.2 kswapd 与直接回收 | [01-overview.md](./10-reclaim/01-overview.md) |
| 10.3 swap 与 zswap | [07-swap-zswap.md](./10-reclaim/07-swap-zswap.md) |
| 10.4 OOM | [08-oom.md](./10-reclaim/08-oom.md) |
| 10.5 和 cgroup 章的衔接 | [01-overview.md](./10-reclaim/01-overview.md) |
| 10.6 unevictable 与 mlock | [09-mlock-unevictable.md](./10-reclaim/09-mlock-unevictable.md) |
| 10.7 `migrate_pages` | [10-migration.md](./10-reclaim/10-migration.md) |
| 10.8 swap table | [11-swap-table.md](./10-reclaim/11-swap-table.md) |
| 10.9 关键函数表 | 拆进上面各篇末尾的函数表 |
| （新写）经典 LRU 的问题 | [03-lru-problems.md](./10-reclaim/03-lru-problems.md) |
| （新写）两套对照 | [06-lru-vs-mglru.md](./10-reclaim/06-lru-vs-mglru.md) |

[上一节 THP](./09-thp-and-hugetlb.md) · [目录](./README.md) · [下一节 KVM](./11-kvm-memory.md)
