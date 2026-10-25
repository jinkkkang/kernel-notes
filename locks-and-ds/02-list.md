# 2. 双向链表与哈希链表

**结论**：内核链表是**侵入式**的——`list_head` 嵌在对象里，头结点常常自己也是一个 `list_head`（空环：`next==prev==&head`）。宏帮你用 `container_of` 找回对象。**链表 API 本身不加锁**，并发安全是调用者的事。

> 主文件：`include/linux/list.h`、`include/linux/types.h`、`include/linux/poison.h`。RCU 变体在 `include/linux/rculist.h`。

---

## 2.1 `list_head` 长什么样

```c
/* include/linux/types.h:191 */
struct list_head {
	struct list_head *next, *prev;  /* 中文：后继 / 前驱；仍是 list_head，不是业务对象 */
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
| `LIST_HEAD_INIT(name)` | `list.h:23` | `{ &(name), &(name) }`，给静态初始化器用 |
| `LIST_HEAD(name)` | `list.h:25` | 定义并初始化一个头：`struct list_head name = LIST_HEAD_INIT(name)` |
| `INIT_LIST_HEAD(list)` | `list.h:35` | 运行时把已有 `list_head` 置成空环（两次 `WRITE_ONCE`） |

```c
LIST_HEAD(zombie_list);           /* 中文：全局/函数内静态头 */
struct list_head local;
INIT_LIST_HEAD(&local);           /* 中文：栈上的头要手动 INIT */
```

---

## 2.3 插入与删除

底层拼接是 `list.h:149` 的 `__list_add`。先过 `__list_add_valid`（调试 / `CONFIG_LIST_HARDENED` 下查链坏了没），通过后改四根指针，把 `new` 插到 `prev` 与 `next` 之间；其中 `prev->next` 用 `WRITE_ONCE` 发布。

```c
/* include/linux/list.h:149 */
static __always_inline void __list_add(struct list_head *new,
				       struct list_head *prev,
				       struct list_head *next)
{
	if (!__list_add_valid(new, prev, next))
		return;

	next->prev = new;
	new->next = next;
	new->prev = prev;
	WRITE_ONCE(prev->next, new);
}
```

| 函数 | 行号 | 语义 |
|---|---|---|
| `list_add(new, head)` | `:173` | 插到 **头后**（LIFO / 栈式）：`__list_add(new, head, head->next)` |
| `list_add_tail(new, head)` | `:188` | 插到 **头前**（FIFO / 队列式）：`__list_add(new, head->prev, head)` |
| `list_del(entry)` | `:234` | 摘下；并把 `entry->next/prev` 写成 **poison** |
| `list_del_init(entry)` | `:292` | 摘下后 `INIT_LIST_HEAD(entry)`，节点可再次使用 |
| `list_empty(head)` | `:378` | `READ_ONCE(head->next) == head` |
| `list_move` / `list_move_tail` | `:303` / `:314` | 先删再加到另一条链 |

```c
/* include/linux/list.h:173 */
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

`POISON_POINTER_DELTA` 在同文件 `:12`：开了 `CONFIG_ILLEGAL_POINTER_VALUE` 时用那个配置值，否则是 0。

`list_del`（`list.h:234`）：

```c
static inline void list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}
```

若再 `list_del` 一次或跟着野指针走，调试配置下更容易炸在显眼地址，而不是「静默串到别的链」。

**注意**：`list_del` 后的节点**不能**直接再 `list_add`（poison 不是合法链接）；要用 `list_del_init`，或显式 `INIT_LIST_HEAD`。

---

## 2.4 遍历：从 `list_head` 回到对象

```c
/* list.h:776 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))
```

| 宏 | 行号 | 用途 |
|---|---|---|
| `list_for_each_entry(pos, head, member)` | `:776` | 正向遍历；`member` 是对象里 `list_head` 字段名 |
| `list_for_each_entry_reverse` | `:787` | 反向 |
| `list_for_each_entry_safe(pos, n, head, member)` | `:863` | **遍历时可能删除 `pos`**：循环头里先把下一节点存进 `n` |
| `list_for_each_entry_continue` | `:812` | 从当前位置的下一个继续 |
| `list_for_each_entry_from` | `:839` | 从当前位置（含自己）继续 |

`_safe` 的循环头（`:863`）是：

```c
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_first_entry(head, typeof(*pos), member),	\
		n = list_next_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member); 			\
	     pos = n, n = list_next_entry(n, member))
```

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
/* types.h:195 */
struct hlist_head {
	struct hlist_node *first;  /* 中文：只有 first，省一个指针 —— 适合哈希表一桶一头 */
};

struct hlist_node {
	struct hlist_node *next, **pprev;  /* 中文：pprev 指向「谁指向我」（前驱的 next 或 head->first） */
};
```

| API | 行号 | 说明 |
|---|---|---|
| `HLIST_HEAD_INIT` / `HLIST_HEAD` / `INIT_HLIST_HEAD` | `list.h:938` / `:939` / `:940` | 头初始化；`first = NULL` |
| `hlist_add_head` | `:1028` | 插入桶首 |
| `hlist_del` | `:999` | 删除；`next = LIST_POISON1`，`pprev = LIST_POISON2` |
| `hlist_del_init` | `:1012` | 删除后 `INIT_HLIST_NODE`，节点可再挂 |
| `hlist_for_each_entry` / `_safe` | `:1141` / `:1172` | 遍历对象 |

`pprev` 让删除变成 O(1)：不用找前驱节点。`__hlist_del`（`:982`）直接 `WRITE_ONCE(*pprev, next)`。

---

## 2.6 并发提醒（本章后面会反复用）

| 事实 | 含义 |
|---|---|
| list/hlist **不提供锁** | 多读者多写者必须自备 spinlock / mutex / RCU 规则 |
| 单 CPU、中断上下文 vs 进程上下文 | 常要 `spin_lock_irqsave` 挡同 CPU 硬中断重入 |
| RCU 链表变体 | `list_add_rcu`（`rculist.h:104`）/ `list_del_rcu`（`:155`）/ `list_for_each_entry_rcu`（`:418`），见 [07-rcu](./07-rcu.md) |
| `hlist_nulls` | 链尾不是 NULL，而是带编号的 nulls 标记（`include/linux/list_nulls.h`），查找时发现桶被并发搬迁可以重试 |

经典错误：遍历时删节点却不用 `_safe`；持着链的锁却调用可能睡眠的函数（见 [08](./08-observability.md)）。

---

## 2.7 函数表（核心）

| 名字 | 文件:行 | 说明 |
|---|---|---|
| `LIST_HEAD_INIT` / `LIST_HEAD` | `list.h:23` / `:25` | 静态空环 |
| `INIT_LIST_HEAD` | `:35` | 运行时空环 |
| `__list_add` | `:149` | 四指针拼接，带 valid 检查 |
| `list_add` | `:173` | 头插 |
| `list_add_tail` | `:188` | 尾插 |
| `list_del` | `:234` | 删除 + 两侧 poison |
| `list_del_init` | `:292` | 删除后重新 INIT |
| `list_move` / `list_move_tail` | `:303` / `:314` | 挪到另一条链 |
| `list_empty` | `:378` | `READ_ONCE(next) == head` |
| `list_entry` | `:607` | → `container_of` |
| `list_for_each_entry` | `:776` | 遍历对象 |
| `list_for_each_entry_safe` | `:863` | 可删遍历 |
| `HLIST_HEAD` / `INIT_HLIST_HEAD` | `:939` / `:940` | 哈希头 |
| `hlist_del` | `:999` | 哈希节点删除 + poison |
| `hlist_add_head` | `:1028` | 哈希桶头插 |

---

## 2.8 可执行 Demo：侵入式链表

### 用户态（本机已跑通）

源码：[demos/02_list.c](./demos/02_list.c)——最小 `list_head` / `list_add` / `list_add_tail` / `list_del` / `list_for_each_entry` / `_safe`。

```bash
cd locks-and-ds/demos
gcc -Wall -O2 -o 02_list 02_list.c
./02_list
```

> **sampled on 6.12**。环境：2026-10-22 16:40:47 CST（Asia/Shanghai）；Linux 6.12.94+；gcc (Debian 14.2.0-19) 14.2.0

```text
after add (expect C A B): {C age=30} {A age=10} {B age=20}
walk with list_for_each_entry:
  pos=0x7ffee9251360 tag=C age=30
  pos=0x7ffee9251320 tag=A age=10
  pos=0x7ffee9251340 tag=B age=20
deleting B during walk
after safe-delete B (expect C A): {C age=30} {A age=10}
```

| Demo 行为 | 内核对应 |
|---|---|
| `list_add_tail` 两次再 `list_add` | 同名 API：`list.h:188` 尾插 / `:173` 头插 |
| `list_for_each_entry` 打出业务对象 | 宏内 `list_entry` → `container_of` |
| `_safe` 里删 `age==20` | 遍历中删除必须暂存 next |

教学实现**故意不用 poison**，方便用户态打印；内核 `list_del` 会写入 `LIST_POISON*`。

### 内核模块示例（示例源码，勿直接生产加载）

完整模块：[demos/kmod_list_example.c](./demos/kmod_list_example.c) + [demos/Makefile.example](./demos/Makefile.example)。用的 `LIST_HEAD`、`list_add_tail`、`list_for_each_entry`、`list_for_each_entry_safe`、`list_del`、`kmalloc(GFP_KERNEL)` 在 v6.6.160 头文件里都有。下面命令是 **RECIPE**（本笔记没有对 6.6 编译过模块，也没有 insmod）：

```bash
# RECIPE
cd locks-and-ds/demos
make -f Makefile.example   # 需要 /lib/modules/$(uname -r)/build
# sudo insmod kmod_list_example.ko && dmesg | tail
# sudo rmmod kmod_list_example
```

模块里用真正的 `LIST_HEAD` / `list_add_tail` / `list_for_each_entry_safe`，在 `dmesg` 里打印 `id=`。


*下一节：红黑树、xarray、maple tree、IDR——链表不够用时的容器。*
