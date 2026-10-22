# memory-management demos

用户态示例用本机 `gcc` 能编过。内核模块 **只提供源码**，本环境不 `insmod`。

| 文件 | 做什么 |
|---|---|
| [01_maps_smaps.c](./01_maps_smaps.c) | `mmap` 一段匿名页，打印 `/proc/self/maps` 里自己的行，并读 `smaps_rollup` |
| [02_fault.c](./02_fault.c) | 匿名映射后逐页写入，用 `getrusage` 看 minor fault 增加 |
| [kmod_slab_cache.c](./kmod_slab_cache.c) | 示例：`kmem_cache_create` + alloc/free。RECIPE，不装载 |

```bash
gcc -Wall -O2 -o /tmp/maps_smaps 01_maps_smaps.c && /tmp/maps_smaps
gcc -Wall -O2 -o /tmp/fault 02_fault.c && /tmp/fault
```

不要在生产机上跑会把内存打满的变体。这两个示例各碰几兆。
