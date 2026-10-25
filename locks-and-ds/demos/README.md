# locks-and-ds 可执行 Demo

用户态 C：**本目录可直接 `gcc` 编译运行**（教学实现，不是从内核拷 GPL 全文）。
内核模块：`kmod_*.c` 为**完整示例源码**，需要本机内核头文件；标注「示例勿直接生产加载」。

## 环境（生成笔记时的 box）

用户态四份程序的粘贴输出是 **sampled on 6.12**（Linux 6.12.94+，gcc 14.2.0，2026-10-22）。
内核模块按 Linux **v6.6.160** 的 `include/linux/list.h`、`include/linux/idr.h` 对过符号，没有在 6.6 上 `make` / `insmod`。

## 用户态（优先跑这些）

```bash
cd locks-and-ds/demos

gcc -Wall -O2 -o 01_container_of 01_container_of.c && ./01_container_of
gcc -Wall -O2 -o 02_list 02_list.c && ./02_list
gcc -Wall -O2 -o 03_hash_hlist 03_hash_hlist.c && ./03_hash_hlist
gcc -Wall -O2 -pthread -o 04_atomic_race 04_atomic_race.c && ./04_atomic_race
```

| 文件 | 对应章节 | 演示什么 |
|---|---|---|
| `01_container_of.c` | [01-c-primer](../01-c-primer.md) | `offsetof` + 教学版 `container_of` |
| `02_list.c` | [02-list](../02-list.md) | 侵入式 `list_head`：add / del / for_each / `_safe` |
| `03_hash_hlist.c` | [03-trees-maps](../03-trees-maps.md) | 哈希桶 + `hlist` 风格 `pprev` |
| `04_atomic_race.c` | [04-atomics-refcount](../04-atomics-refcount.md) | 普通 RMW 丢更新 vs `_Atomic` |

## 内核模块示例（可选编译）

```bash
# RECIPE：需要 /lib/modules/$(uname -r)/build。本笔记未在 v6.6.160 上编译。
make -f Makefile.example
# sudo insmod kmod_list_example.ko   # 仅实验机
# dmesg | tail
# sudo rmmod kmod_list_example
```

| 文件 | 对应章节 | 说明 |
|---|---|---|
| `kmod_list_example.c` | 02 | `LIST_HEAD` + `list_for_each_entry` |
| `kmod_idr_example.c` | 03 | `idr_alloc` / `idr_find` / `idr_remove`；`end == 0` 在 v6.6.160 表示最大 ID 为 `INT_MAX` |
| `Makefile.example` | — | out-of-tree 模块 Makefile 模板 |

**不要**把这些模块当生产代码加载。笔记里的内核 API 钉 **v6.6.160**。
