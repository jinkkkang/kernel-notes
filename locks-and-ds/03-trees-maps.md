# 3. 树与映射容器

**结论**：链表擅长「一串东西随便挂」；需要**按键查找 / 按范围查询 / 分配整数 ID** 时，内核用红黑树、xarray、maple tree、IDR/IDA、hashtable。本节给**选型直觉 + 关键 API**，不抄实现。

> 密度故意低于 list：用到哪个再回 elixir 抠细节。

---

## 3.1 怎么选（先看这张表）

| 需求 | 优先考虑 | 典型用户 |
|---|---|---|
| 有序键、自己比较，嵌入式节点 | `rb_root` / `rb_root_cached` | 调度、定时器、部分文件系统 |
| 稀疏整数下标 → 指针，API 现代 | `xarray` | 页缓存、IDR 底层思路的「新世界」 |
| 范围查询（区间 → 对象） | `maple_tree` | **VMA**（`mm_struct`） |
| 分配/查找整数 ID → 指针 | `idr`；只要 ID 不要指针用 `ida` | 设备号、项目 ID |
| 自己算 hash、桶内链起来 | `DEFINE_HASHTABLE` + `hlist` | 大量「字符串/指针 → 对象」缓存 |

全部一样：**容器不替你加业务锁**（xarray/maple 内部有自己的 `spinlock` 选项，但语义要读清文档）。

---

## 3.2 红黑树：`rb_node` / `rb_root`

```c
/* include/linux/rbtree_types.h:5 */
struct rb_node {
	unsigned long  __rb_parent_color; /* 中文：父指针 + 颜色挤在一个 long 里 */
	struct rb_node *rb_right;         /* 中文：右子 */
	struct rb_node *rb_left;          /* 中文：左子 */
} __attribute__((aligned(sizeof(long))));

struct rb_root {
	struct rb_node *rb_node;          /* 中文：根；空树为 NULL */
};

struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;      /* 中文：缓存最左节点，加速 rb_first */
};
```

同样是**侵入式**：业务结构体里嵌 `struct rb_node node;`，比较键是你自己写的。

| API | 头文件 | 作用 |
|---|---|---|
| `RB_ROOT` / `RB_ROOT_CACHED` | `rbtree_types.h` | 空树初始化 |
| `rb_link_node` + `rb_insert_color` | `rbtree.h` | 先挂到叶子位置，再旋转上色 |
| `rb_erase` | `:45` | 删除并保持红黑性质 |
| `rb_first` / `rb_next` / `rb_prev` / `rb_last` | `:49` 起 | 中序遍历 |
| `rb_entry(ptr, type, member)` | 宏 | 同 `container_of` |

**直觉**：查找/插入平均 O(log n)；你负责「往左还是往右」的比较函数，树负责平衡。

```c
/* 伪代码：插入前自己走一遍树 */
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

## 3.3 XArray：整数下标的现代「数组」

```c
/* include/linux/xarray.h:300 */
struct xarray {
	spinlock_t	xa_lock;   /* 中文：默认内部锁；也可外部接管 */
	gfp_t		xa_flags;
	void __rcu *	xa_head;   /* 中文：树根；RCU 友好 */
};
```

| API | 作用 |
|---|---|
| `DEFINE_XARRAY(name)` / `xa_init` | 定义/初始化 |
| `xa_load(xa, index)` | 按下标取指针 |
| `xa_store(xa, index, entry, gfp)` | 存 |
| `xa_erase(xa, index)` | 删 |
| `xa_alloc` / `xa_alloc_cyclic` | 分配空闲下标 |
| `xa_for_each` | 遍历非空槽 |

**何时用**：键是（或可变成）`unsigned long` 下标，要稀疏存储、常与 RCU 读路径配合。页缓存那条线会大量见到。

比老 `radix_tree` 更统一的 API；读代码时看到 `xa_` 前缀就回到这里。

---

## 3.4 Maple tree：给范围查询用的

```c
/* include/linux/maple_tree.h:260 */
struct maple_tree {
	union {
		spinlock_t		ma_lock;
#ifdef CONFIG_LOCKDEP
		struct lockdep_map	*ma_external_lock;
#endif
	};
	unsigned int	ma_flags;
	void __rcu      *ma_root;
};
```

| 概念 | 说明 |
|---|---|
| `MTREE_INIT` / `mt_init` | 初始化 |
| `mtree_load` / `mtree_store` / `mtree_erase` | 简单点操作 |
| `MA_STATE` / `mas_*` | **状态机式**遍历/插入：范围操作的主接口 |
| 与 VMA | 现代内核用 maple tree 管内存映射区间（进程章/MM 章衔接） |

本节只要求：见到 `maple_tree` / `mas_walk` 知道「这是区间树，不是普通链表」。细规则留给 MM 章。

---

## 3.5 IDR / IDA

```c
/* include/linux/idr.h:20 */
struct idr {
	struct radix_tree_root	idr_rt;  /* 中文：内部仍用基数树根 */
	unsigned int		idr_base;
	unsigned int		idr_next; /* 中文：启发下一分配位置 */
};
```

| API | 用途 |
|---|---|
| `DEFINE_IDR` / `idr_init` | 初始化 |
| `idr_alloc(idr, ptr, start, end, gfp)` | 在范围内分配 ID，并挂上 `ptr` |
| `idr_find` / `idr_remove` | 查找 / 释放 ID |
| `idr_for_each` / `idr_get_next` | 遍历 |
| `ida_alloc` / `ida_free`（`IDA`） | **只要唯一整数，不要指针载荷**（更省） |

**何时用**：用户可见的「句柄式」整数（设备次设备号、内部对象 ID）。新代码若只是「下标→指针」，也常直接上 xarray。

---

## 3.6 Hashtable 宏（桶数组 + hlist）

```c
/* include/linux/hashtable.h —— 典型用法 */
DEFINE_HASHTABLE(my_table, 8);   /* 中文：1<<8 个桶，每桶一个 hlist_head */

hash_add(my_table, &obj->hnode, key);
hash_for_each_possible(my_table, obj, hnode, key) {
	if (obj->key == key)
		return obj;
}
hash_del(&obj->hnode);
```

| API | 说明 |
|---|---|
| `DEFINE_HASHTABLE(name, bits)` | 静态桶数组 |
| `hash_init` | 动态初始化 |
| `hash_add` / `hash_add_rcu` | 头插到对应桶 |
| `hash_del` / `hash_del_rcu` | 删除 |
| `hash_for_each` / `hash_for_each_possible` | 全表 / 单桶候选遍历 |
| `hash_empty` | 是否全空 |

本质是 [02-list](./02-list.md) 的 `hlist` + `hash_min`/`hash_32` 一类散列。并发同样要自己选锁或 RCU。

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

*下一节：原子计数与 `refcount_t`——锁和对象生命周期的地基。*
