# 10.6 经典 LRU 与 MGLRU 对照

**结论**：两套实现共用 `shrink_node`、`shrink_folio_list`、`workingset_refault` 的外壳。分叉在入链、记访问、选谁扫、以及 refault 时把页放回哪一代。开关在 `/sys/kernel/mm/lru_gen/enabled`，工作集阀在 `min_ttl_ms`。debugfs 的 `lru_gen` 能读直方图，也能手动老化和主动回收。

> 接口以 `Documentation/admin-guide/mm/multigen_lru.rst` 为准。代码分叉见 [02](./10-02-lru-classic.md) 和 [05](./10-05-mglru-code-path.md)。
> 导航：[上一篇 代码路径](./10-05-mglru-code-path.md) · [本目录](./10-00-reclaim.md) · [下一篇 swap](./10-07-swap-zswap.md)

---

## 四个时刻

| 时刻 | 经典 LRU | MGLRU |
|---|---|---|
| 分配 / 入链 | `folio_add_lru` → 本 CPU `cpu_fbatches.lru_add` → `lru_add` → `list_add` 到 inactive 头。`PG_active` 已经立着才进 active | 同一个 batch。`lruvec_add_folio` 里 `lru_gen_add_folio` 成功就进 `lrugen->folios[gen][type][zone]` 的头。gen 由 `lru_gen_folio_seq` 按 `PG_active` / `PG_workingset` / `PG_referenced` 算。`lrugen->enabled` 为假则退回经典链 |
| 访问 | `folio_mark_accessed`：第一次 `PG_referenced`，第二次 `folio_activate` 升 active。映射页要等回收时 `folio_referenced` 走 rmap 清 young | `folio_mark_accessed` 改调 `lru_gen_inc_refs`，只加档，不搬链表。缺页期间 `lru_gen_enter_fault` 标记新近度，影响随后的 `folio_add_lru`。页表遍历用 `folio_update_gen`，rmap 用 `lru_gen_set_refs` 和 `lru_gen_look_around` |
| 回收 | `shrink_node_memcgs` → `get_scan_count`（swappiness + `lru_cost`）→ `shrink_active_list` / `shrink_inactive_list` → `shrink_folio_list` | 全局：`lru_gen_shrink_node` → `shrink_many` / `shrink_one` → `evict_folios` → `isolate_folios` / `sort_folio` → 同一个 `shrink_folio_list`。memcg 限额：`lru_gen_shrink_lruvec`。类和档由 `get_type_to_scan` 的 PID 选 |
| refault | `workingset_refault` 用 shadow 里的 `nonresident_age` 算距离，够近就 `folio_set_active`，并记 `WORKINGSET_ACTIVATE` / `WORKINGSET_RESTORE` | `workingset_refault` 转去 `lru_gen_refault`。距离用 `max_seq` 的差，小于 `MAX_NR_GENS` 算近。`lru_gen_test_recent` 为真可以再立 `PG_workingset`。`workingset_*` 计数两套都有 |

`shrink_folio_list` 内部还会再分叉一次：`folio_check_references`（`vmscan.c:893`）在 MGLRU 下走 `lru_gen_set_refs`，经典下看 `PG_referenced` 和 young 的次数。写出、unmap、放页是同一段。

```mermaid
flowchart TD
  add["folio_add_lru"] --> batch["本 CPU folio_batch"]
  batch --> addfn["lru_add / lruvec_add_folio"]
  addfn --> gen{"lru_gen_add_folio 成功?"}
  gen -->|是| mlists["lrugen.folios 某一代的头"]
  gen -->|否| clists["lruvec.lists inactive 或 active 的头"]
  touch["folio_mark_accessed"] --> sw{"lru_gen_enabled?"}
  sw -->|是| refs["lru_gen_inc_refs 加档"]
  sw -->|否| classic["PG_referenced 然后 folio_activate"]
```

```mermaid
flowchart TD
  sn["shrink_node"] --> root{"root_reclaim 且 MGLRU 开着或正在切换?"}
  root -->|是| lgn["lru_gen_shrink_node"]
  lgn --> many["shrink_many / shrink_one"]
  many --> ev["evict_folios → sort_folio → shrink_folio_list"]
  root -->|否| mem["shrink_node_memcgs"]
  lgn --> sw2{"正在切换?"}
  sw2 -->|是| mem
  sw2 -->|否| done["返回"]
  mem --> slab["shrink_lruvec + shrink_slab"]
  slab --> scn["get_scan_count"]
  scn --> lists["shrink_active_list / shrink_inactive_list"]
  lists --> sfl["shrink_folio_list"]
```

切换窗口里全局回收先跑 MGLRU，再落回 `shrink_node_memcgs`。见 [01](./10-01-overview.md) 里 `shrink_node` 的代码。

---

## 开关

`/sys/kernel/mm/lru_gen/` 是稳定 ABI（管理文档 Runtime options）。没编 `CONFIG_LRU_GEN` 时这个目录不存在，`init_lru_gen`（`vmscan.c:5938`）不会去创建。

`enabled`（`vmscan.c` 的 `lru_gen_enabled_attr`，`:5483`）按位开关。默认值取决于 `CONFIG_LRU_GEN_ENABLED`。硬件不支持的位写了也不生效；主开关关掉时，合法的值仍会被接受。

| 值 | 组件 | 文档里的后果 |
|---|---|---|
| `0x0001` | 主开关 | 关掉就回到经典 LRU |
| `0x0002` | 硬件会置 accessed 位时（例如 x86），成批清叶子 PTE 的访问位 | 文档写理论上可能加重 `mmap_lock` 竞争。关掉的话，连续映射热页的负载会有轻微性能损失，因为本来可以一次清一大片 |
| `0x0004` | 连非叶子 PTE 的访问位一起清 | 文档写只在 Intel 和 AMD 上验证过。关掉的损失可以忽略 |
| `y` / `Y` / `n` / `N` | 上面全部 | `y` 读回来是 `0x0007` |

文档里的例子：`echo y > enabled` 读到 `0x0007`；`echo 5` 读到 `0x0005`（主开关加非叶子，不批量清叶子）。

`min_ttl_ms`（属性 `lru_gen_min_ttl_attr`，`:5437`）保护最近 N 毫秒的工作集，保不住就 OOM。默认 0，关掉。文档（`:77-82`）：按人能察觉的卡顿大约 100ms 来估，`N=1000` 通常能去掉由颠簸引起的、受不了的 UI 卡顿；`N=3000` 更不明显，风险是过早 OOM。内核里变量是 `lru_gen_min_ttl`，单位 jiffies（`vmscan.c:4207`）。

---

## debugfs

`/sys/kernel/debug/lru_gen` 可读写，`init_lru_gen` 在 `:5941` 创建。多行命令可以，分隔符是 `,` 和 `;`。

`/sys/kernel/debug/lru_gen_full` 只读（`:5943`）。`CONFIG_LRU_GEN_STATS=y` 时，被赶走的代的历史留在这个文件里。否则 `NR_HIST_GENS` 是 1，历史只有当前格。

读 `lru_gen` 得到每个 memcg、每个 node 的直方图，不累计。每一行是 `min_gen_nr age_in_ms nr_anon_pages nr_file_pages`，直到 `max_gen_nr`。`min_gen` 最冷（`age_in_ms` 最大），`max_gen` 最热。bin 的个数由 `MAX_NR_GENS` 决定。

写 `+ memcg_id node_id max_gen_nr [can_swap [force_scan]]`：新建一代 `max_gen_nr+1`。`can_swap` 默认跟 swap 设置走，写成 1 会在 swap 关掉时仍扫匿名，反过来也一样。`force_scan` 默认 1；写成 0 用启发式减开销，覆盖也会变小。

写 `- memcg_id node_id min_gen_nr [swappiness [nr_to_reclaim]]`：赶走编号小于等于 `min_gen_nr` 的代。`min_gen_nr` 要小于 `max_gen_nr-1`，因为最年轻的两代还没老化完，等价于 active，不能被这条命令驱逐。`swappiness` 覆盖 `/proc/sys/vm/swappiness`，合法范围文档写的是 `[0-200, max]`，`max` 专用于只回收匿名。`nr_to_reclaim` 限制页数。

---

## 观测

两套都能看到的：

| 看什么 | 在哪 |
|---|---|
| 后台扫了多少、收回多少 | `/proc/vmstat` 的 `pgscan_kswapd`、`pgsteal_kswapd` |
| 直接回收 | `pgscan_direct`、`pgsteal_direct`，次数看 `allocstall_*` |
| 匿名还是文件 | `pgscan_anon` / `pgscan_file`，`pgsteal_anon` / `pgsteal_file` |
| refault | `workingset_refault_*`、`workingset_activate_*`、`workingset_restore_*`。有这些数不等于 MGLRU 开着 |
| kswapd 被 LOW 立刻叫醒 | `kswapd_low_wmark_hit_quickly` |
| 本机这组数 | [01](./10-01-overview.md) 末尾，6.12 快照 |

tracepoint（`include/trace/events/vmscan.h`，[13](./13-observability.md) 有表）：

| 事件 | 行 | 更贴哪套 |
|---|---|---|
| `mm_vmscan_kswapd_wake` / `kswapd_sleep` | `:76` / `:59` | 两套。kswapd 醒和睡 |
| `mm_vmscan_wakeup_kswapd` | `:151` | 两套。谁叫醒的 |
| `mm_vmscan_direct_reclaim_begin` / `end` | 模板 `:177`，事件 `:203` / `:249` | 两套。直接回收 |
| `mm_vmscan_lru_isolate` | `:364` | 两套。经典 `isolate_lru_folios`（`vmscan.c:1767`）和 MGLRU 扫描（`:4794`）都会打 |
| `mm_vmscan_lru_shrink_inactive` | `:477` | 经典 inactive（`:2043`）和 MGLRU `evict_folios`（`:4916`）都打这个名字 |
| `mm_vmscan_lru_shrink_active` | `:528` | 经典 `shrink_active_list`（`:2150`）。MGLRU 没有 active 链表这一步 |
| `mm_lru_insertion` | `include/trace/events/pagemap.h:28` | 两套。`lru_add` 末尾 |
| `mm_lru_activate` | `pagemap.h:61` | 经典升 active |

MGLRU 额外看 debugfs 直方图：每一代的 `age_in_ms` 和匿名/文件页数。`lru_gen_full` 在打开 `CONFIG_LRU_GEN_STATS` 时能看被赶走的代。经典路径没有这个直方图，冷热只有 active/inactive 的 `nr_active_*` / `nr_inactive_*`。

---

## 何时开、何时关（RECIPE）

本环境没有改这台机器的 `/sys/kernel/mm/lru_gen/enabled`，也没有读它。下面只复述管理文档，标 RECIPE，跑不跑由你自己的机器决定。

- **RECIPE：想用 MGLRU。** 内核要编 `CONFIG_LRU_GEN`。文档的 Quick start 是再编 `CONFIG_LRU_GEN_ENABLED=y`，启动即开。运行时 `echo y > /sys/kernel/mm/lru_gen/enabled`。文档写：除非某个组件有没预料到的副作用，否则位都该开着。
- **RECIPE：连续映射的热页多，又在意 `mmap_lock`。** 文档说 `0x0002` 理论上可能加重这把锁的竞争。要验证就清掉这一位（例如写 `0x0005`），并接受文档说的轻微损失。本篇不把「理论上」写成已经发生。
- **RECIPE：桌面或笔记本，没有 oomd，UI 会因为回收卡。** 文档把 `min_ttl_ms` 做成压力阀。`echo 1000 > min_ttl_ms` 是文档给的通常值；`3000` 更稳，可能过早 OOM。默认 0 是关掉。这会在保不住工作集时杀进程，不是只把回收变温和。
- **RECIPE：要关主开关。** 把 bit 0 清掉，或写 `n`。正在切换的窗口里 `shrink_node` 两条都会走，见 [01](./10-01-overview.md)。不要在切换过程中假设某一条计数立刻归零。
- **RECIPE：数据中心要估计工作集或提前腾冷页。** 用 debugfs 的 `+` / `-`，文档里的作业调度例子是：按间隔 `+` 出新一代，按冷页大小给机器排序；落地新作业前 `-` 掉冷代，腾不出来就换下一台。`min_gen_nr` 不要指到最年轻的两代。

没有文档句子支持的「数据库一定要关」「容器一定要开」，这里不写。

---

[上一篇 代码路径](./10-05-mglru-code-path.md) · [本目录](./10-00-reclaim.md) · [下一篇 swap 与 zswap](./10-07-swap-zswap.md)
