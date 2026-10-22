# 2. 双向链表与哈希链表

**结论**：内核链表是**侵入式**的——`list_head` 嵌在对象里，头结点常常自己也是一个 `list_head`（空环：`next==prev==&head`）。宏帮你用 `container_of` 找回对象。**链表 API 本身不加锁**，并发安全是调用者的事。

> 主文件：`include/linux/list.h`、`include/linux/types.h`、`include/linux/poison.h`。

---

## 2.1 `list_head` 长什么样

```c
/* include/linux/types.h:206 */
struct list_head {
	struct list_head *next;  /* 中文：后继节点（仍是 list_head，不是业务对象） */
	struct list_head *prev;  /* 中文：前驱节点 */
};
```

空链表（初始化后）：

```text
        head
       ┌────┐
   ┌──►│next│──┐
   │   │prev│◄─┤
   └───┴────┘  │
       ▲───────┘
```

### C 对照：为什么不是 `Node { T data; Node *next }`？

教科书链表把「数据」放节点里；内核反过来——**数据里嵌节点**，这样：

1. 同一对象可以挂多条链（多个 `list_head` 成员）
2. 不需要为「装指针的节点」再分配一层
3. 遍历时用 `container_of` 回到业务结构体

```c
struct victim {
	int age;
	struct list_head node;   /* 挂到某条全局链上 */
};
```

---

## 2.2 初始化

| API | 位置 | 说明 |
|---|---|---|
| `LIST_HEAD(name)` | `list.h:33` | 定义并初始化一个头：`struct list_head name = { &name, &name }` 语义 |
| `INIT_LIST_HEAD(list)` | `list.h:51` | 运行时把已有 `list_head` 置成空环（`WRITE_ONCE`） |
| `LIST_HEAD_INIT(name)` | 同文件靠前 | 静态初始化器用 |

```c
LIST_HEAD(zombie_list);           /* 中文：全局/函数内静态头 */
struct list_head local;
INIT_LIST_HEAD(&local);           /* 中文：栈上的头要手动 INIT */
```

---

## 2.3 插入与删除

底层拼接（`list.h:165` 起 `__list_add`）：改四根指针，把 `new` 插到 `prev` 与 `next` 之间。

| 函数 | 行号约 | 语义 |
|---|---|---|
| `list_add(new, head)` | `:189` | 插到 **头后**（LIFO / 栈式） |
| `list_add_tail(new, head)` | `:204` | 插到 **头前**（FIFO / 队列式） |
| `list_del(entry)` | `:273` | 摘下；并把 `entry->next/prev` 写成 **poison** |
| `list_del_init(entry)` | 附近 | 摘下后 `INIT_LIST_HEAD(entry)`，节点可再次使用 |
| `list_empty(head)` | 同文件 | `READ_ONCE(head->next) == head` |
| `list_move` / `list_move_tail` | 同文件 | 先删再加到另一条链 |

```c
/* 直觉：list_add */
static __always_inline void list_add(struct list_head *new,
				     struct list_head *head)
{
	__list_add(new, head, head->next);  /* 中文：夹在 head 与原首元之间 */
}
```

### Poison（防 UAF 误用）

```c
/* include/linux/poison.h:23 */
#define LIST_POISON1  ((void *) 0x100 + POISON_POINTER_DELTA)
#define LIST_POISON2  ((void *) 0x122 + POISON_POINTER_DELTA)
```

`list_del` 后（`list.h:276`）：

```c
entry->next = LIST_POISON1;
entry->prev = LIST_POISON2;
```

若再 `list_del` 一次或跟着野指针走，调试配置下更容易炸在显眼地址，而不是「静默串到别的链」。

**注意**：`list_del` 后的节点**不能**直接再 `list_add`（poison 不是合法链接）；要用 `list_del_init`，或显式 `INIT_LIST_HEAD`。

---

## 2.4 遍历：从 `list_head` 回到对象

```c
/* list.h:834 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))
```

| 宏 | 用途 |
|---|---|
| `list_for_each_entry(pos, head, member)` | 正向遍历；`member` 是对象里 `list_head` 字段名 |
| `list_for_each_entry_reverse` | 反向 |
| `list_for_each_entry_safe(pos, n, head, member)` | **遍历时可能删除 `pos`**：额外暂存 `n = next` |
| `list_for_each_entry_continue` / `_from` | 从当前位置继续 |

```c
struct victim {
	int age;
	struct list_head node;
};

struct victim *v;
list_for_each_entry(v, &zombie_list, node) {
	/* 中文：v 已经是业务对象指针，不是 list_head* */
	do_something(v);
}
```

### C 对照：`_safe` 为什么需要？

```c
/* 危险：删掉 pos 后，pos->node.next 可能已是 poison */
list_for_each_entry(pos, head, node) {
	list_del(&pos->node);   /* 下一轮 pos = list_next_entry(pos) 就炸 */
}

/* 安全：事先把下一节点存进 n */
list_for_each_entry_safe(pos, n, head, node) {
	list_del(&pos->node);
	kfree(pos);
}
```

---

## 2.5 `hlist`：单指针头的哈希桶友好版

```c
/* types.h:210 */
struct hlist_head {
	struct hlist_node *first;  /* 中文：只有 first，省一个指针 —— 适合哈希表一桶一头 */
};

struct hlist_node {
	struct hlist_node *next, **pprev;  /* 中文：pprev 指向「谁指向我」（前驱的 next 或 head->first） */
};
```

| API | 约略行号 | 说明 |
|---|---|---|
| `HLIST_HEAD` / `INIT_HLIST_HEAD` | `list.h:997` | 头初始化 |
| `hlist_add_head` | `:1086` | 插入桶首 |
| `hlist_del` / `hlist_del_init` | `:1057` | 删除；同样 poison |
| `hlist_for_each_entry` / `_safe` | 同文件后部 | 遍历 |

`pprev` 让删除变成 O(1)：不用找前驱节点，直接 `*pprev = next`。

---

## 2.6 并发提醒（本章后面会反复用）

| 事实 | 含义 |
|---|---|
| list/hlist **不提供锁** | 多读者多写者必须自备 spinlock / mutex / RCU 规则 |
| 单 CPU、中断上下文 vs 进程上下文 | 常要 `spin_lock_irqsave` 挡同 CPU 硬中断重入 |
| RCU 链表变体 | `list_add_rcu` / `list_del_rcu` / `list_for_each_entry_rcu`（见 [07-rcu](./07-rcu.md)） |
| `hlist_nulls` | 用于「查找时桶可能被并发重建」的网络等场景（`include/linux/list_nulls.h`） |

经典错误：遍历时删节点却不用 `_safe`；持着链的锁却调用可能睡眠的函数（见 [08](./08-observability.md)）。

---

## 2.7 函数表（核心）

| 名字 | 文件:行 | 说明 |
|---|---|---|
| `INIT_LIST_HEAD` | `list.h:51` | 空环 |
| `list_add` | `:189` | 头插 |
| `list_add_tail` | `:204` | 尾插 |
| `list_del` | `:273` | 删除 + poison |
| `list_entry` | `:647` | → `container_of` |
| `list_for_each_entry` | `:834` | 遍历对象 |
| `list_for_each_entry_safe` | `:921` | 可删遍历 |
| `hlist_add_head` | `:1086` | 哈希桶头插 |
| `hlist_del` | `:1057` | 哈希节点删除 |

---

*下一节：红黑树、xarray、maple tree、IDR——链表不够用时的容器。*
