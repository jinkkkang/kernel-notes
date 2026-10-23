# 3. buddy 与页分配（跳转）

这一节已经拆到 [03-page-alloc/](./03-page-alloc/README.md)。本文件只留旧链接，正文在子目录里。

> 内核版本仍是 **v7.3**（`c03c1a14507d`）。
> 导航：[上一节 物理布局](./02-physical-layout.md) · [目录](./README.md) · [下一节 SLUB](./04-slab.md)

## 现在从哪读

[03-page-alloc/README.md](./03-page-alloc/README.md) 有阅读顺序。直接进某一篇：

| 文件 | 内容 |
|---|---|
| [01-overview.md](./03-page-alloc/01-overview.md) | 总调用链、`alloc_context` |
| [02-gfp-and-scope.md](./03-page-alloc/02-gfp-and-scope.md) | GFP、scope API |
| [03-fastpath-freelist.md](./03-page-alloc/03-fastpath-freelist.md) | 水位、`get_page_from_freelist` |
| [04-pcp-and-rmqueue.md](./03-page-alloc/04-pcp-and-rmqueue.md) | PCP、`rmqueue` |
| [05-buddy-free.md](./03-page-alloc/05-buddy-free.md) | 释放与 buddy 合并 |
| [06-slowpath.md](./03-page-alloc/06-slowpath.md) | 慢路径 |
| [07-high-order-and-compaction.md](./03-page-alloc/07-high-order-and-compaction.md) | 高阶失败、compaction |
| [08-numa-policy.md](./03-page-alloc/08-numa-policy.md) | mempolicy、numaperf |
| [09-cma.md](./03-page-alloc/09-cma.md) | CMA |

## 旧小节 → 新位置

| 旧小节 | 新位置 |
|---|---|
| 3.1 调用链 | [01-overview.md](./03-page-alloc/01-overview.md) |
| 3.2 `get_page_from_freelist` | [03-fastpath-freelist.md](./03-page-alloc/03-fastpath-freelist.md) |
| 3.3 `rmqueue` → PCP → `__rmqueue` | [04-pcp-and-rmqueue.md](./03-page-alloc/04-pcp-and-rmqueue.md) |
| 3.4 GFP 标志 | [02-gfp-and-scope.md](./03-page-alloc/02-gfp-and-scope.md) |
| 3.5 slowpath | [06-slowpath.md](./03-page-alloc/06-slowpath.md) |
| 3.6 高阶分配为什么失败 | [07-high-order-and-compaction.md](./03-page-alloc/07-high-order-and-compaction.md) |
| 3.7 scope API | [02-gfp-and-scope.md](./03-page-alloc/02-gfp-and-scope.md) |
| 3.8 NUMA 策略与 numaperf | [08-numa-policy.md](./03-page-alloc/08-numa-policy.md) |
| 3.9 CMA | [09-cma.md](./03-page-alloc/09-cma.md) |
| 3.10 关键函数表 | 拆进上面各篇末尾的函数表 |
| （旧笔记只有一句的释放合并） | [05-buddy-free.md](./03-page-alloc/05-buddy-free.md) |

[上一节 物理布局](./02-physical-layout.md) · [目录](./README.md) · [下一节 SLUB](./04-slab.md)
