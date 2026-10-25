# 3. 页分配

**结论**：普通页分配的 C 函数是 `__alloc_pages`（声明 `include/linux/gfp.h:177`，定义 `mm/page_alloc.c:4465`）。它先让 `current_gfp_context` 改写 gfp，填好 `struct alloc_context`，再调用 `get_page_from_freelist`。快路径只扫 zonelist 上水位够的 zone，从 per-CPU 列表或 buddy 拿页。拿不到就进 `__alloc_pages_slowpath`：先按新的水位再试一次，贵的分配可以先 compact，然后直接回收、再 compact，最后才可能 OOM。

编了 `CONFIG_NUMA` 时，最常见的 `alloc_pages()` 在 `mm/mempolicy.c`，先按当前进程的 mempolicy 算出节点，再调用 `__alloc_pages`。

阶、伙伴地址、拆开和合并的图在 [02 的 buddy 概念](./02-physical-layout.md#25-buddy-概念)。本目录从 `rmqueue` 和 `__free_one_page` 的调用链写起。

> 主文件：`mm/page_alloc.c`。水位在 `struct zone._watermark[]`。
> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。行号对这棵树。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 导航：[上一节 物理布局](./02-physical-layout.md) · [章目录](./README.md) · [下一篇 总入口](./03-01-overview.md)

## 本目录

| 文件 | 讲什么 | 旧小节 |
|---|---|---|
| [03-01-overview.md](./03-01-overview.md) | `__alloc_pages` 总调用链、`struct alloc_context` 全成员、快/慢路径总图 | 3.1 |
| [03-02-gfp-and-scope.md](./03-02-gfp-and-scope.md) | GFP 标志、`current_gfp_context`、NOFS/NOIO/PIN scope | 3.4、3.7 |
| [03-03-fastpath-freelist.md](./03-03-fastpath-freelist.md) | `get_page_from_freelist`、水位、`lowmem_reserve`、`watermark_boost` | 3.2 |
| [03-04-pcp-and-rmqueue.md](./03-04-pcp-and-rmqueue.md) | `struct per_cpu_pages`、`rmqueue`、migratetype 与窃取 | 3.3 |
| [03-05-buddy-free.md](./03-05-buddy-free.md) | `free_pages` / `__free_one_page` 合并、PCP 批量归还 | 旧 3.3 里只有一句，这里加厚 |
| [03-06-slowpath.md](./03-06-slowpath.md) | slowpath，以及和回收 / compaction / OOM 的交接 | 3.5 |
| [03-07-high-order-and-compaction.md](./03-07-high-order-and-compaction.md) | 高阶失败、`compact_zone` | 3.6，compaction 加厚 |
| [03-08-numa-policy.md](./03-08-numa-policy.md) | mempolicy、numactl、numaperf | 3.8 |
| [03-09-cma.md](./03-09-cma.md) | CMA 区与 `cma_alloc` | 3.9 |

## 阅读顺序

```text
01 总调用链（先把 alloc_context 和快/慢分叉看完）
  → 02 GFP 与 scope（调用者带进来的约束）
  → 03 快路径水位
  → 04 PCP 与 rmqueue
  → 05 释放与 buddy 合并
  → 06 慢路径（从这里跳到 10-00-reclaim.md）
  → 07 高阶与 compaction
  → 08 NUMA 策略
  → 09 CMA
```

先看清「这次分配的参数装在 `alloc_context` 里」，再看水位和 PCP。慢路径里的回收细节不在本目录展开，指到 [10-00-reclaim.md](./10-00-reclaim.md)。

`MAX_ORDER` 在 `include/linux/mmzone.h:29`，没有 `CONFIG_ARCH_FORCE_MAX_ORDER` 时是 10。buddy 的阶是 0 到 `MAX_ORDER`，一共 `NR_PAGE_ORDERS`（`MAX_ORDER + 1`）档。4K 页时 order 10 是 4MB。

## 旧小节 → 新位置

| 旧小节 | 新位置 |
|---|---|
| 3.1 调用链 | [03-01-overview.md](./03-01-overview.md) |
| 3.2 `get_page_from_freelist` | [03-03-fastpath-freelist.md](./03-03-fastpath-freelist.md) |
| 3.3 `rmqueue` → PCP → `__rmqueue` | [03-04-pcp-and-rmqueue.md](./03-04-pcp-and-rmqueue.md) |
| 3.4 GFP 标志 | [03-02-gfp-and-scope.md](./03-02-gfp-and-scope.md) |
| 3.5 slowpath | [03-06-slowpath.md](./03-06-slowpath.md) |
| 3.6 高阶分配为什么失败 | [03-07-high-order-and-compaction.md](./03-07-high-order-and-compaction.md) |
| 3.7 scope API | [03-02-gfp-and-scope.md](./03-02-gfp-and-scope.md) |
| 3.8 NUMA 策略与 numaperf | [03-08-numa-policy.md](./03-08-numa-policy.md) |
| 3.9 CMA | [03-09-cma.md](./03-09-cma.md) |
| 3.10 关键函数表 | 拆进上面各篇末尾的函数表 |
| （旧笔记只有一句的释放合并） | [03-05-buddy-free.md](./03-05-buddy-free.md) |

[上一节 物理布局](./02-physical-layout.md) · [章目录](./README.md) · [下一篇 总入口](./03-01-overview.md)
