# 10.11 swap table

**结论**：v7.3 的 swap 槽状态在 `struct swap_table` 里，每个 cluster 一张，和槽一一对应。定义在 `mm/swap_table.h:10`。`Documentation/mm/swap-table.rst` 讲的就是这张表。槽状态以头文件 `:26-33` 的布局为准。

> 导航：[上一篇 迁移](./10-10-migration.md) · [本目录](./10-00-reclaim.md) · [下一节 KVM](./11-kvm-memory.md)

`SWAPFILE_CLUSTER`（`mm/swap.h:32`）：`CONFIG_THP_SWAP` 时等于 `HPAGE_PMD_NR`，否则 256（`:35`）。

## `struct swap_table`（`swap_table.h:10`）

| 成员 | 中文 |
|---|---|
| `atomic_long_t entries[SWAPFILE_CLUSTER]` | 这个 cluster 里每个槽一项。一项是一个 `atomic_long_t` |

## `struct swap_memcg_table`（`:15`，仅 `CONFIG_MEMCG` 的读写路径会用）

| 成员 | 中文 |
|---|---|
| `unsigned short id[SWAPFILE_CLUSTER]` | 每个槽的 memcg 私有 id。挂在 `swap_cluster_info->memcg_table` 上，和 entry 里的类型分开存 |

头文件 `:26-33` 写的布局（低位是类型标记）：

| 类型 | 低位标记 | 含义 |
|---|---|---|
| NULL | 全 0（`SWP_TB_NULL`） | 空槽，可以分配 |
| Shadow | 最低位 `0b1`（`SWP_TB_SHADOW_MARK`） | 已换出、不在 swap cache。复用 xarray value 的格式，好和 workingset shadow 兼容。`SHADOW_VAL` 可以是 0，仍保持 shadow 形态占位。memcg id 嵌在 `SHADOW_VAL` 里 |
| PFN | `0b10`（`SWP_TB_PFN_MARK`） | 槽在用，而且有 cache 页。memcg 记在 page 上，不在这项里 |
| Pointer | 低三位 `0b100` | 头文件写 Unused。留给 8 字节对齐指针，低三位才能当标记 |
| Bad | 低三位 `0b1000` 且其余位全 1（`SWP_TB_BAD`） | 坏槽。保护 swap 头，或设备上的洞 |

高位是标志。`SWP_TB_FLAGS_BITS`（`:72`）是 `min(5, BITS_PER_LONG - SWP_TB_PFN_BITS)`。其中计数占 `SWP_TB_COUNT_BITS`，剩下的一位在 `SWAP_TABLE_HAS_ZEROFLAG` 为真时是 zero flag（`SWP_TB_ZERO_FLAG`，`:79`）。位数不够就把 zero 记到 `swap_cluster_info->zero_bitmap`（`__swap_table_set_zero`，`:267`）。

读写必须在 cluster 锁下，除了 `swap_table_get`（`:251`）走 RCU 读 `ci->table`。表指针空就当成 NULL 项。`__swap_table_set` / `__swap_table_xchg` / `__swap_table_get` 都 `VM_WARN` 偏移越过 `SWAPFILE_CLUSTER`。

`SWP_TABLE_USE_PAGE`（`:19`）在 `sizeof(struct swap_table) == PAGE_SIZE` 时为真，这时一张表正好占一页。

槽的分配、回收、cluster 锁状态机其余部分在 `mm/swapfile.c`，本节不展开。换入换出走 [07](./10-07-swap-zswap.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `swap_table_get` | `mm/swap_table.h:251` | RCU 读一项。表指针空当成 NULL |
| `__swap_table_set` | 同文件，`VM_WARN` 越界 | 持 cluster 锁写入 |
| `__swap_table_xchg` | 同文件 | 持锁交换 |
| `__swap_table_get` | 同文件 | 持锁读取 |
| `__swap_table_set_zero` | `:267` | 位数不够时把 zero 记到 `zero_bitmap` |

这几个是头文件里的 inline。cluster 的分配循环在 `mm/swapfile.c`，本节没有逐个对行号。

---

[上一篇 迁移](./10-10-migration.md) · [本目录](./10-00-reclaim.md) · [下一节 KVM](./11-kvm-memory.md)
