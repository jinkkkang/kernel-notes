# 10.8 OOM

**结论**：慢路径里回收和压缩都没能拿出页，`__alloc_pages_may_oom` 调用 `out_of_memory`。选谁死看 `oom_badness`：rss、swap 项、页表页，再按 `oom_score_adj` 平移。被选中的进程要跑完退出才还得出页，OOM 打印之后空闲页不会立刻出现。

> `mm/oom_kill.c`。分配侧入口在 [03-page-alloc/06](../03-page-alloc/06-slowpath.md)。
> MGLRU 的 `min_ttl` 也会从 kswapd 进 `out_of_memory`，见 [05](./05-mglru-code-path.md)。
> 导航：[上一篇 swap](./07-swap-zswap.md) · [本目录](./README.md) · [下一篇 mlock](./09-mlock-unevictable.md)

---

慢路径里回收和压缩都没能拿出页，`__alloc_pages_may_oom`（`page_alloc.c:4044`）调用 `out_of_memory`（`oom_kill.c:1103`）。

```text
out_of_memory
  oom_badness                              # oom_kill.c:199，给每个候选打分
  oom_kill_process                         # :1008
    __oom_kill_process                     # :912
      发 SIGKILL，标 TIF_MEMDIE / 相关标志，让它的退出路径能分配
```

`oom_badness` 返回 `LONG_MIN` 的进程不参与选择：`oom_unkillable_task`、`find_lock_task_mm` 失败、`oom_score_adj == OOM_SCORE_ADJ_MIN`、mm 已标 `MMF_OOM_SKIP`、或正在 `in_vfork`。剩下的分数是三笔账加一次平移：

```c
points = get_mm_rss_sum(p->mm)
       + get_mm_counter_sum(p->mm, MM_SWAPENTS)
       + mm_pgtables_bytes(p->mm) / PAGE_SIZE;   /* rss + swap 项 + 页表页 */
adj = (long)p->signal->oom_score_adj;
adj *= totalpages / 1000;                        /* 把 -1000..1000 摊到总页数上 */
points += adj;
```

`oom_score_adj` 为 0 时，分数就是这个 mm 占的页。调到 `OOM_SCORE_ADJ_MIN` 直接排除，调高则在同样 rss 下更先被选中。`/proc/<pid>/oom_score_adj` 写的就是这个字段。`totalpages` 由 `constrained_alloc`（`:248`）按约束算：memcg OOM 用 `mem_cgroup_get_max`，cpuset / mempolicy 只用允许的节点。

memcg 的 OOM 是另一条：charge 撞上 `memory.max` 且回收失败。事件在 `memory.events` 的 `oom` / `oom_kill`，`memory.oom.group` 决定杀一个还是一组。实现仍在 `mm/memcontrol.c` 和 `oom_kill.c` 的 memcg 辅助函数里。不要把全局 `oom_kill` 计数当成某个容器的 OOM。本机 `oom_kill` 是 0。

OOM 之后不一定立刻有空闲页。被杀进程要跑退出、拆 VMA、还 folio。这段时间别的 `__GFP_NOFAIL` 分配会在 slowpath 里转。`dmesg` 里的 `Out of memory: Killed process` 是这个路径打的。

tracepoint（`include/trace/events/oom.h`）：`mark_victim`（`:76`）、`wake_reaper`（`:118`）、`reclaim_retry_zone`（`:34`）、`compact_retry`（`:183`）。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `__alloc_pages_may_oom` | `mm/page_alloc.c:4044` | 慢路径进入 OOM |
| `out_of_memory` | `mm/oom_kill.c:1103` | 全局 OOM |
| `oom_badness` | `mm/oom_kill.c:199` | rss + swap + 页表，再加 `oom_score_adj` |
| `constrained_alloc` | `:248` | OOM 约束与 `totalpages` |
| `oom_kill_process` | `:1008` | 选完之后杀 |
| `__oom_kill_process` | `:912` | SIGKILL |

---

[上一篇 swap](./07-swap-zswap.md) · [本目录](./README.md) · [下一篇 mlock](./09-mlock-unevictable.md)
