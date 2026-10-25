# 3. 树与映射容器

**结论**：链表擅长「一串东西随便挂」；需要**按键查找 / 按范围查询 / 分配整数 ID** 时，内核用红黑树、xarray、maple tree、IDR/IDA、hashtable。本节给**选型直觉 + 关键 API**，不抄实现。

> 密度故意低于 list：用到哪个再回 v6.6.160 头文件抠细节。

---

## 3.1 怎么选（先看这张表）

| 需求 | 优先考虑 | 典型用户 |
|---|---|---|
| 有序键、自己比较，嵌入式节点 | `rb_root` / `rb_root_cached` | 调度、定时器、部分文件系统 |
| 稀疏整数下标 → 指针 | `xarray` | 页缓存等 |
| 范围查询（区间 → 对象） | `maple_tree` | **VMA**：`mm_struct.mm_mt`（`include/linux/mm_types.h:755`） |
| 分配/查找整数 ID → 指针 | `idr`；只要 ID 不要指针用 `ida` | 设备号、项目 ID |
| 自己算 hash、桶内链起来 | `DEFINE_HASHTABLE` + `hlist` | 大量「字符串/指针 → 对象」缓存 |

全部一样：**容器不替你加业务锁**。`xarray` / `maple_tree` 结构体里自带 `spinlock_t`，也可以改成外部锁，语义以头文件为准。

---

## 3.2 红黑树：`rb_node` / `rb_root`

```c
/* include/linux/rbtree_types.h:5
 * 末尾 aligned(sizeof(long))：注释写 CRIS 需要这个对齐。
 */
struct rb_node {
	unsigned long  __rb_parent_color; /* 中文：父指针和颜色挤在一个 long 里 */
	struct rb_node *rb_right;         /* 中文：右子 */
	struct rb_node *rb_left;          /* 中文：左子 */
} __attribute__((aligned(sizeof(long))));

struct rb_root {
	struct rb_node *rb_node;          /* 中文：根；空树为 NULL */
};

/* --- 缓存最左节点的变体 --- */
struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;      /* 中文：缓存最左节点，rb_first_cached 变 O(1) */
};
```

空树初始化器在同文件 `:31`：

```c
#define RB_ROOT (struct rb_root) { NULL, }
#define RB_ROOT_CACHED (struct rb_root_cached) { {NULL, }, NULL }
```

同样是**侵入式**：业务结构体里嵌 `struct rb_node node;`，比较键是你自己写的。

| API | 位置 | 作用 |
|---|---|---|
| `RB_ROOT` / `RB_ROOT_CACHED` | `rbtree_types.h:31` | 空树初始化 |
| `rb_link_node` | `rbtree.h:59` | 挂到叶子位置（还没上色） |
| `rb_insert_color` | 声明 `rbtree.h:39`，实现 `lib/rbtree.c:434` | 旋转上色 |
| `rb_erase` | 声明 `rbtree.h:40`，实现 `lib/rbtree.c:440` | 删除并保持红黑性质 |
| `rb_next` / `rb_prev` / `rb_first` / `rb_last` | `rbtree.h:44`–`:47` | 中序遍历 |
| `rb_entry(ptr, type, member)` | `rbtree.h:28` | 同 `container_of` |
| `rb_first_cached` | `rbtree.h:106` | `(root)->rb_leftmost` |

**直觉**：查找/插入平均 O(log n)；你负责「往左还是往右」的比较，树负责平衡。

```c
/* 插入前自己走一遍树；link / parent 交给 rb_link_node */
struct rb_node **link = &root->rb_node, *parent = NULL;
while (*link) {
	parent = *link;
	if (key < rb_entry(parent, typeof(*obj), node)->key)
		link = &parent->rb_left;
	else
		link = &parent->rb_right;
}
rb_link_node(&obj->node, parent, link);
rb_insert_color(&obj->node, root);
```

---

## 3.3 XArray：整数下标的「数组」

```c
/* include/linux/xarray.h:296
 * 头文件把 xa_flags / xa_head 标成 private：不要当公开字段直接改。
 * xa_head：全空时是 NULL；只有下标 0 有值时直接存那个指针；否则指向 xa_node。
 */
struct xarray {
	spinlock_t	xa_lock;   /* 中文：默认内部锁；也可拿去保护你自己的数据 */
	gfp_t		xa_flags;
	void __rcu *	xa_head;   /* 中文：树根；RCU 友好 */
};
```

| API | 位置 | 作用 |
|---|---|---|
| `DEFINE_XARRAY(name)` | `xarray.h:331` | 静态空数组（`flags = 0`） |
| `xa_init` | `:393` | 运行时初始化 |
| `xa_load(xa, index)` | 声明 `:351` | 按下标取指针 |
| `xa_store(xa, index, entry, gfp)` | 声明 `:352` | 存 |
| `xa_erase(xa, index)` | 声明 `:353` | 删 |
| `xa_alloc` | `:867` | 分配空闲下标 |
| `xa_alloc_cyclic` | `:970` | 从某个游标循环分配 |
| `xa_for_each` | `:500` | 遍历非空槽 |

**何时用**：键是（或可变成）`unsigned long` 下标，要稀疏存储、常与 RCU 读路径配合。页缓存那条线会大量见到。读代码时看到 `xa_` 前缀就回到这里。

---

## 3.4 Maple tree：给范围查询用的

`lockdep_map_p` 在有无 lockdep 时不是同一种类型（`maple_tree.h:184` / `:197`）：

```c
#ifdef CONFIG_LOCKDEP
typedef struct lockdep_map *lockdep_map_p;
#else
typedef struct { /* nothing */ } lockdep_map_p;
#endif
```

```c
/* include/linux/maple_tree.h:218 */
struct maple_tree {
	union {
		spinlock_t	ma_lock;            /* 中文：树自己的自旋锁 */
		lockdep_map_p	ma_external_lock;   /* 中文：外部锁时给 lockdep 用；无 LOCKDEP 时是空结构 */
	};
	unsigned int	ma_flags;
	void __rcu      *ma_root;               /* 中文：树根 */
};
```

| 概念 | 位置 | 说明 |
|---|---|---|
| `MTREE_INIT` | `maple_tree.h:233` | 静态初始化 |
| `mt_init` | `:633` | 运行时初始化空树 |
| `mtree_load` / `mtree_store` / `mtree_erase` | `:311` / `:326` / `:328` | 简单点操作 |
| `MA_STATE` | `:436` | 声明一个 `struct ma_state`，带上树、起点、终点 |
| `mas_walk` | `:462` | 按当前状态走到一个区间 |
| 与 VMA | `mm_struct.mm_mt`（`mm_types.h:755`） | 用 maple tree 管内存映射区间 |

`ma_flags` 里和锁有关的位（`maple_tree.h:171`–`:173`；掩码 `MT_FLAGS_LOCK_MASK` 在 `:170`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `MT_FLAGS_LOCK_IRQ` | `0x100` | 用这棵树的锁时要 irq-safe |
| `MT_FLAGS_LOCK_BH` | `0x200` | bh-safe |
| `MT_FLAGS_LOCK_EXTERN` | `0x300` | 不使用 `ma_lock`，锁在外面 |
| `MT_FLAGS_USE_RCU` | `0x02` | 树处于 RCU 模式 |

本节只要求：见到 `maple_tree` / `mas_walk` 知道「这是区间树，不是普通链表」。细规则留给 MM 章。

---

## 3.5 IDR / IDA

```c
/* include/linux/idr.h:19 */
struct idr {
	struct radix_tree_root	idr_rt;  /* 中文：内部基数树根 */
	unsigned int		idr_base; /* 中文：ID 起点偏移 */
	unsigned int		idr_next; /* 中文：循环分配的启发位置 */
};
```

| API | 位置 | 用途 |
|---|---|---|
| `DEFINE_IDR` | `idr.h:56` | 静态初始化 |
| `idr_init` | `:149` | 运行时初始化 |
| `idr_alloc(idr, ptr, start, end, gfp)` | 声明 `:114`；`end <= 0` 的含义在 `lib/idr.c:70` | 在 `[start, end)` 分配 ID 并挂上 `ptr`；`end <= 0` 时最大 ID 是 `INT_MAX` |
| `idr_find` / `idr_remove` | `:119` / `:118` | 查找 / 释放 ID |
| `idr_for_each` | `:120` | 回调式遍历 |
| `idr_get_next` | `:122` | 找下一个已占用 ID |
| `idr_for_each_entry` | `:187` | `for` 宏，内部调 `idr_get_next` |
| `idr_destroy` | `:125` | 拆掉基数树 |
| `ida_alloc` / `ida_free` | `:273` / `:258` | **只要唯一整数，不要指针载荷** |

**何时用**：用户可见的「句柄式」整数（设备次设备号、内部对象 ID）。只是「下标→指针」时，也常直接上 xarray。

---

## 3.6 Hashtable 宏（桶数组 + hlist）

```c
/* include/linux/hashtable.h:16
 * DEFINE_HASHTABLE(name, bits) 展开成 1<<bits 个 hlist_head，
 * 每个桶用 HLIST_HEAD_INIT（first = NULL）。
 */
DEFINE_HASHTABLE(my_table, 8);   /* 中文：256 个桶 */

hash_add(my_table, &obj->hnode, key);          /* hashtable.h:60 → hlist_add_head */
hash_for_each_possible(my_table, obj, hnode, key) {  /* :165 */
	if (obj->key == key)
		return obj;
}
hash_del(&obj->hnode);                         /* :105 */
```

| API | 行号 | 说明 |
|---|---|---|
| `DEFINE_HASHTABLE(name, bits)` | `:16` | 静态桶数组，长度 `1 << bits` |
| `hash_init` | `:52` | 动态初始化已有数组 |
| `hash_min` | `:31` | 4 字节及以下用 `hash_32`，否则 `hash_long` |
| `hash_add` / `hash_add_rcu` | `:60` / `:69` | 头插到对应桶 |
| `hash_del` / `hash_del_rcu` | `:105` / `:114` | 删除 |
| `hash_for_each` | `:126` | 全表遍历 |
| `hash_for_each_possible` | `:165` | 单桶候选遍历 |
| `hash_empty` | `:99` | 是否全空 |

本质是 [02-list](./02-list.md) 的 `hlist` + `hash_32` / `hash_long`。并发同样要自己选锁或 RCU（`hash_add_rcu` 走 `hlist_add_head_rcu`）。

---

## 3.7 小结

```text
随便挂 / 遍历多、查找少     → list_head
哈希查找                     → hashtable + hlist
有序比较键                   → rbtree
整数下标 → 指针              → xarray（或 idr）
只要分配整数 ID              → ida
地址范围 / 区间              → maple_tree
```

---

## 3.8 可执行 Demo：哈希桶 + hlist 风格

### 用户态（本机已跑通）

源码：[demos/03_hash_hlist.c](./demos/03_hash_hlist.c)——固定 8 桶、字符串键、`hlist_node.pprev` 头插/删除/查找。
（红黑树完整旋转代码较长；xarray / maple / IDR 见下方内核模块示例。）

```bash
cd locks-and-ds/demos
gcc -Wall -O2 -o 03_hash_hlist 03_hash_hlist.c
./03_hash_hlist
```

> **sampled on 6.12**。环境：2026-10-22 16:40:47 CST（Asia/Shanghai）；Linux 6.12.94+；gcc (Debian 14.2.0-19) 14.2.0

```text
insert key=apple value=1 -> bucket[7]
insert key=banana value=2 -> bucket[6]
insert key=cherry value=3 -> bucket[2]
insert key=date value=4 -> bucket[3]
insert key=apple2 value=5 -> bucket[1]
bucket[1]: {apple2=5}
bucket[2]: {cherry=3}
bucket[3]: {date=4}
bucket[6]: {banana=2}
bucket[7]: {apple=1}
lookup banana -> banana
  value=2
delete cherry
lookup cherry after del -> NULL
bucket[1]: {apple2=5}
bucket[3]: {date=4}
bucket[6]: {banana=2}
bucket[7]: {apple=1}
```

| Demo | 内核直觉 |
|---|---|
| `buckets[HASH_SIZE]` + `hlist_add_head` | `DEFINE_HASHTABLE` + 桶内 `hlist` |
| `pprev` 删除 O(1) | `hlist_del`：`*pprev = next` |
| `container_of(n, struct item, node)` | `hlist_entry` / `hlist_for_each_entry` |

### 内核模块示例：IDR（示例勿直接生产加载）

源码：[demos/kmod_idr_example.c](./demos/kmod_idr_example.c)。调用的 `idr_alloc` / `idr_find` / `idr_remove` / `idr_for_each_entry` / `idr_destroy` 与 `include/linux/idr.h` 的声明一致（`idr_alloc` 在 `:114`）。示例里 `end` 传 0：`lib/idr.c:70` 规定 `end <= 0` 时最大 ID 是 `INT_MAX`。下面命令是 **RECIPE**：

```bash
# RECIPE：未对 v6.6.160 编译或加载
cd locks-and-ds/demos
make -f Makefile.example
# sudo insmod kmod_idr_example.ko && dmesg | tail
# sudo rmmod kmod_idr_example
```

xarray / maple tree 的用法更长，建议在实验机对照 `Documentation/` 与 v6.6.160 头文件试。本目录不放「假完整」用户态移植，以免和真实 API 脱节。


*下一节：原子计数与 `refcount_t`——锁和对象生命周期的地基。*
