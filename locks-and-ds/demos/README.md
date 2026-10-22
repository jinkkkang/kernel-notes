# locks-and-ds 可执行 Demo

用户态 C：**本目录可直接 `gcc` 编译运行**（教学实现，不是从内核拷 GPL 全文）。  
内核模块：`kmod_*.c` 为**完整示例源码**，需要本机内核头文件；标注「示例勿直接生产加载」。

## 环境（生成笔记时的 box）

见同目录构建说明；笔记里粘贴的实测输出带 Asia/Shanghai 时间与 `uname`/`gcc`。

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
# 需 /lib/modules/$(uname -r)/build
make -f Makefile.example
# sudo insmod kmod_list_example.ko   # 仅实验机
# dmesg | tail
# sudo rmmod kmod_list_example
```

| 文件 | 对应章节 | 说明 |
|---|---|---|
| `kmod_list_example.c` | 02 | `LIST_HEAD` + `list_for_each_entry` |
| `kmod_idr_example.c` | 03 | `idr_alloc` / `idr_find` / `idr_remove` |
| `Makefile.example` | — | out-of-tree 模块 Makefile 模板 |

**不要**把这些模块当生产代码加载；API 随内核版本可能微调，笔记钉 **v7.3**。
