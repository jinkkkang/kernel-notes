# 10.7 swap 与 zswap

**结论**：没有 swap 设备时，匿名页的回收只能丢掉 `MADV_FREE` / `LazyFree` 的页，或者什么都不做。有 swap 时，`shrink_folio_list` 把匿名 folio 写成 swap 项，PTE 变成 swap PTE，缺页走 `do_swap_page`。zswap 在写盘之前把页压进 zsmalloc 池，池满了才落到真正的 swap 设备。

> 换出入口在 `mm/page_io.c`。压缩在 `mm/zswap.c`，对象池在 `mm/zsmalloc.c`。
> 导航：[上一篇 对照](./06-lru-vs-mglru.md) · [本目录](./README.md) · [下一篇 OOM](./08-oom.md)

---

没有 swap 设备时，匿名页的回收只能：

- 丢掉 `MADV_FREE` / `LazyFree` 的页（内容允许丢失）
- 或者什么都不做

本机 `SwapTotal` 0，`Zswap` 0，`Zswapped` 0。匿名活跃集约 1.8GB（`AnonPages`）是钉在内存里的。

有 swap 时，`shrink_folio_list` 把匿名 folio 写成 swap 项，PTE 变成 swap PTE，缺页走 `do_swap_page`（`mm/memory.c:4881`）。写出的调用点在 [02](./02-lru-classic.md) 的 `pageout`。

zswap（`mm/zswap.c`）在写 swap 设备之前把页压进 zsmalloc 池。换出入口在 `mm/page_io.c:238`：`zswap_store` 返回真就不再写盘。换入在 `page_io.c:480`：`zswap_load` 不是 `-ENOENT` 就不再读盘。

`zswap_store`（`zswap.c:1488`）要求 folio 已锁且在 swap cache。`zswap_enabled` 为假就跳到 `check_old`，把这个偏移上可能残留的旧项擦掉，避免回写覆盖 swap 文件里的新数据。打开时：

```text
zswap_store(folio)                        # zswap.c:1488
  obj_cgroup_may_zswap 失败：先 shrink_memcg，再失败则放弃
  zswap_check_limits 到顶：放弃
  zswap_pool_current_get
  for 每一页：zswap_store_page            # 压缩，zs_malloc 进池
  成功：计 ZSWPOUT
  失败：xa_erase 掉这个 folio 每个偏移上的旧 zswap_entry
```

`zswap_load`（`:1594`）同样要求 locked + swapcache。返回值写在函数注释里：

| 返回 | folio 状态 | 含义 |
|---|---|---|
| `0` | 解锁，标 uptodate | 解压成功。标脏，`xa_erase` 删掉压缩副本，swap cache 成为唯一内存副本 |
| `-ENOENT` | 保持锁定 | 这个槽不在 zswap，调用者继续读盘。`zswap_never_enabled()` 也走这里 |
| `-EINVAL` | 解锁，不标 uptodate | 大 folio。注释写明 zswap 不完整装载大 folio，调用者（`do_swap_page`）会 SIGBUS |
| `-EIO` | 解锁，不标 uptodate | 槽在 zswap 里但解压失败，同样 SIGBUS |

池满了就落到真正的 swap 设备。`/proc/meminfo` 的 `Zswap` 是压缩后占用，`Zswapped` 是原页大小。zswap 仍然算内存，不是磁盘。它和 memcg 的交换限额（`memory.swap.*`，见 cgroup 章）是两层账。

压缩对象从 zsmalloc 来。`zs_malloc`（`mm/zsmalloc.c:1397`）按字节大小要一块，返回 handle，失败返回 `ERR_PTR`。

```text
zs_malloc(pool, size, gfp, nid)           # zsmalloc.c:1397
  size == 0：-EINVAL
  size > ZS_MAX_ALLOC_SIZE：-ENOSPC       # 太大，不进 zsmalloc
  cache_alloc_handle
  lookup_size_class                       # 按 size 找 size_class
  find_get_zspage                         # 这个 class 里还有空位的 zspage
    有：obj_malloc，fix_fullness_group
    无：alloc_zspage，再 obj_malloc，SetZsPageMovable
```

`struct zs_pool` / `struct size_class` 的成员表这里不列。定义在 `mm/zsmalloc.c`，字段随 fullness group 和 stat 变，本节只钉分配路径。handle 不是内核虚拟地址，映射时再 `kmap`。

`swappiness` 是匿名和文件的扫描比例，不是「百分之多少内存拿去 swap」。经典路径怎么用它，见 [02](./02-lru-classic.md) 的 `get_scan_count`。MGLRU 下 `min_seq[anon]` 和 `min_seq[file]` 可以错开，上限是 `MAX_NR_GENS-MIN_NR_GENS-1` 代（`mmzone.h:557`），见 [04](./04-mglru-design.md)。

槽本身的布局在 [11](./11-swap-table.md)。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `pageout` | `mm/vmscan.c:621` | 回收上下文写出脏页。匿名页从这里进 zswap / swap |
| `zswap_store` | `mm/zswap.c:1488` | 压缩换出 |
| `zswap_load` | `mm/zswap.c:1594` | 压缩换入 |
| `zswap_store_page` | `mm/zswap.c:1408` | 压缩一页并挂进 zswap 树 |
| `zs_malloc` | `mm/zsmalloc.c:1397` | 从 zsmalloc 池要一块 |
| `do_swap_page` | `mm/memory.c:4881` | 换入缺页 |

---

[上一篇 对照](./06-lru-vs-mglru.md) · [本目录](./README.md) · [下一篇 OOM](./08-oom.md)
