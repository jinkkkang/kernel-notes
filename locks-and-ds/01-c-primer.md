# 1. 读内核够用的 C

**结论**：内核 C 不是另一门语言，但有一套固定习惯——**嵌入式节点**、**宏当「带类型的内联」**、**`container_of` 从成员找回整对象**。把这几样练熟，再看 `list_head` / 锁 才不懵。

> 版本钉死：Linux **v7.3**（`c03c1a14507d`）。

---

## 1.1 指针、取址、`->` vs `.`

| 写法 | 含义 |
|---|---|
| `int x;` | 对象本身 |
| `&x` | 取地址，类型是 `int *` |
| `*p` | 解引用指针 |
| `obj.member` | 对象用 `.` |
| `ptr->member` | 等价于 `(*ptr).member` |

内核里几乎全是指针：`struct task_struct *p`，于是 `p->pid`。结构体**变量**少见，常见的是「指向堆上/静态对象的指针」。

```c
struct list_head head;          /* 栈上或全局的头结点 */
INIT_LIST_HEAD(&head);          /* 传入地址：函数要改的是 head 本身 */
```

---

## 1.2 `struct`、嵌套、匿名

内核对象喜欢**把节点嵌进自己**：

```c
struct my_obj {
	int id;
	struct list_head link;   /* 中文：嵌在对象里的链表节点，不是「指向节点的指针」 */
};
```

「链表挂的是 `link`，业务对象是 `my_obj`」——这是后面所有 `list_for_each_entry` 的前提。

v7.3 里锁类型常用 `context_lock_struct(name) { ... };` 生成带 lockdep 上下文注解的结构体（见 `include/linux/spinlock_types_raw.h:14` 的 `raw_spinlock`）。读笔记时把它当成「就是 `struct name`」即可。

---

## 1.3 `typedef`：内核怎么用

```c
/* include/linux/types.h:188 */
typedef struct {
	int __aligned(sizeof(int)) counter;  /* 中文：真正的计数器字段 */
} atomic_t;

typedef struct spinlock spinlock_t;      /* 中文：spinlock 是结构体标签，_t 是对外类型名 */
typedef struct refcount_struct {
	atomic_t refs;
} refcount_t;                            /* include/linux/refcount_types.h:15 */
```

习惯：

- `foo_t` 多半是 typedef；实现细节可能藏在 `struct foo` 里
- **不要**自己对内核对象乱 `typedef` 一层指针（内核风格不鼓励 `typedef struct foo *fooptr`）

---

## 1.4 `static inline`：头文件里的「小函数」

```c
/* include/linux/list.h:51 附近 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}
```

- `static`：本编译单元可见，避免多个 `.o` 链接冲突
- `inline`：建议编译器内联；热路径上的 list/atomic API 大量如此
- 定义在 **`.h`** 里：每个 `#include` 的 `.c` 都能直接用

---

## 1.5 常用宏：`ARRAY_SIZE`、`offsetof`、`likely`/`unlikely`

| 宏 | 位置 | 作用 |
|---|---|---|
| `ARRAY_SIZE(arr)` | `include/linux/array_size.h:11` | `sizeof(arr)/sizeof(arr[0])`，并拒绝「衰减成指针」的误用 |
| `offsetof(TYPE, MEMBER)` | `include/linux/stddef.h:16` | 成员相对结构体开头的字节偏移（编译期常量） |
| `likely(x)` / `unlikely(x)` | `include/linux/compiler.h` | 告诉编译器分支冷热（底层 `__builtin_expect`） |

```c
if (unlikely(err))
	return err;   /* 中文：错误路径少走，帮助生成更顺的快路径代码 */
```

---

## 1.6 核心：`container_of`（必须啃）

```c
/* include/linux/container_of.h:19 */
#define container_of(ptr, type, member) ({				\
	static_assert(__same_type(*(ptr), typeof_member(type, member)) || \
		      __same_type(*(ptr), void),			\
		      "pointer type mismatch in container_of()");	\
	(type *)((void *)(ptr) - offsetof(type, member)); })
```

**人话**：已知「指向成员 `member` 的指针 `ptr`」，求「外层 `type` 对象的指针」。

```text
  ┌────────────── my_obj ──────────────┐
  │  id                                │
  │  ┌──── link (list_head) ────┐      │
  │  │ next / prev              │◄── ptr 指向这里
  │  └──────────────────────────┘      │
  └────────────────────────────────────┘
       container_of(ptr, struct my_obj, link)
       = 把 ptr 往回减 offsetof(my_obj, link)
```

### C 对照

```c
char *p_member = ...;
struct my_obj *obj = (struct my_obj *)(p_member - offsetof(struct my_obj, link));
/* 内核用宏包起来，并做类型检查；新代码更推荐 container_of_const */
```

`list_entry` 就是它的别名（`include/linux/list.h:647`）：

```c
#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)
```

> 头文件注释写：新代码优先 `container_of_const`（保留 `const`）。读旧代码时两个都会见到。

---

## 1.7 头文件习惯（读代码用）

| 习惯 | 说明 |
|---|---|
| `include/linux/*.h` | 子系统对外 API |
| `include/uapi/linux/*.h` | 用户态可见 ABI（别和内核内部头搞混） |
| `include/asm-generic/` + `arch/.../include/asm/` | 架构相关；atomic/spinlock 底层常在这 |
| `#include <linux/foo.h>` | 内核构建系统已设好搜索路径 |
| 禁止随意循环依赖 | 类型前向声明 + 实现放 `.c` 很常见 |

读锁相关时常见拆分：

- `spinlock_types*.h`：类型与初始化器
- `spinlock.h`：`spin_lock*` API
- `mutex_types.h` / `mutex.h`：同上模式

---

## 1.8 本节函数 / 宏速查

| 名字 | 文件 | 用途 |
|---|---|---|
| `container_of` / `container_of_const` | `include/linux/container_of.h` | 成员指针 → 外层对象 |
| `offsetof` | `include/linux/stddef.h` | 成员偏移 |
| `ARRAY_SIZE` | `include/linux/array_size.h` | 数组长度 |
| `likely` / `unlikely` | `include/linux/compiler.h` | 分支提示 |
| `WRITE_ONCE` / `READ_ONCE` | `include/asm-generic/rwonce.h`（经 compiler 头引入） | 抑制编译器拆分/合并访存；并发下的「单次访问」 |

---

*下一节：把 `container_of` 用在真正的 `list_head` 上。*
