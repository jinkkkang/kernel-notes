# memory-management demos

对照内核 **v6.6.160** 的接口。用户态示例只用 libc。内核模块只提供源码，这里不 `insmod`。

| 文件 | 做什么 |
|---|---|
| [01_maps_smaps.c](./01_maps_smaps.c) | `mmap` 一段匿名页，打印 `/proc/self/maps` 里自己的行，并读 `smaps_rollup` |
| [02_fault.c](./02_fault.c) | 匿名映射后逐页写入，用 `getrusage` 看 minor fault 增加 |
| [kmod_slab_cache.c](./kmod_slab_cache.c) | `kmem_cache_create`（五参数）加 alloc/free/destroy。RECIPE，不装载 |

用户态两个文件在写笔记时用本机 gcc 编过并跑过。主机内核是 6.12.94+，**在 6.12 上采样**；示例不读内核结构，行为只依赖 libc。

```bash
gcc -Wall -O2 -o /tmp/maps_smaps 01_maps_smaps.c && /tmp/maps_smaps
gcc -Wall -O2 -o /tmp/fault 02_fault.c && /tmp/fault
```

```bash
# RECIPE：本环境没有 v6.6 的内核构建树，不编译、不 insmod。
# 调用按 include/linux/slab.h 里的 v6.6.160 声明对过。
obj-m += kmod_slab_cache.o
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod kmod_slab_cache.ko
sudo awk '$1=="demo_obj"' /proc/slabinfo
sudo rmmod kmod_slab_cache
```

不要在生产机上跑会把内存打满的变体。这两个用户态示例各碰几兆。
