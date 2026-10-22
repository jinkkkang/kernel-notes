# 4. 原子操作与引用计数

**结论**：多 CPU 同时改同一个计数，普通 `++`/`--` 会丢更新；`atomic_t` 提供「一整次」改内存的操作。对象生命周期更推荐 **`refcount_t`**——饱和、防回绕，踩 UAF 时更吵。本节为 spinlock / RCU 铺垫，**不讲完** C11 内存序教材。

---

## 4.1 `atomic_t` / `atomic64_t`

```c
/* include/linux/types.h:188 */
typedef struct {
	int __aligned(sizeof(int)) counter;  /* 中文：唯一载荷；别直接碰，走 API */
} atomic_t;

#ifdef CONFIG_64BIT
typedef struct {
	s64 counter;
} atomic64_t;
#endif
```

初始化：`ATOMIC_INIT(0)` 或 `atomic_set(&v, 0)`。

### 常用操作（`include/linux/atomic.h` → `atomic-instrumented.h`）

| API | 直觉 |
|---|---|
| `atomic_read` / `atomic_set` | 读 / 无条件写 |
| `atomic_inc` / `atomic_dec` | +1 / -1 |
| `atomic_add` / `atomic_sub` | 加减 |
| `atomic_add_return` / `atomic_inc_return` | 加减并**返回新值** |
| `atomic_dec_and_test` | 减一后是否变成 0（释放对象前最常见） |
| `atomic_cmpxchg` / `atomic_try_cmpxchg` | CAS：相等才换成新值 |
| `atomic_fetch_*` | 返回**旧值**的那一类 |

64 位版本同名加 `atomic64_` 前缀。

```c
if (atomic_dec_and_test(&obj->users)) {
	/* 中文：我是最后一个持有者，负责拆对象 */
	kfree(obj);
}
```

---

## 4.2 内存序：内核里你先记住三档

完整 `memory_order` 枚举留给书本。读内核 API 名后缀时：

| 后缀 / 形态 | 粗线条含义 | 典型用途 |
|---|---|---|
| 无后缀或 `_relaxed` | 原子性有，跨 CPU 重排约束弱 | 统计计数 |
| `_acquire` / `_release` | 配对：release 之前的写，对之后的 acquire 读可见 | 发布指针、解锁一侧 |
| 全序（不少 `*_return`、锁内部） | 更强的「两边都别乱序」 | 锁的 fast path |

配合宏：

- `WRITE_ONCE` / `READ_ONCE`：单次访问，防编译器拆成两次
- `smp_store_release` / `smp_load_acquire`：显式 release/acquire
- `smp_mb()`：全屏障（贵，少用）

**C 对照**：别写成 `obj->flag = 1;` 就以为另一边立刻看见完整对象——发布指针要用 RCU 的 `rcu_assign_pointer` 或至少 release 语义（见 [07](./07-rcu.md)）。

---

## 4.3 `refcount_t`：专为引用计数打磨

```c
/* include/linux/refcount_types.h:15 */
typedef struct refcount_struct {
	atomic_t refs;   /* 中文：内部仍是 atomic，但 API 带饱和与告警 */
} refcount_t;
```

| API | 说明 |
|---|---|
| `REFCOUNT_INIT(n)` / `refcount_set` | 初始值（常见 `REFCOUNT_INIT(1)`） |
| `refcount_inc` / `refcount_dec` | ±1；异常时 `WARN` + 饱和 |
| `refcount_inc_not_zero` | 已是 0 则失败（对象正在销毁） |
| `refcount_dec_and_test` | 减到 0 返回 true → 可释放 |
| `refcount_add` / `refcount_sub_and_test` | 加减多份 |

饱和值 `REFCOUNT_SATURATED`（`refcount.h`）：计数坏掉后**卡住**，避免回绕成「又像有引用」导致 UAF。

```c
void get_obj(struct obj *o)
{
	refcount_inc(&o->refs);
}

void put_obj(struct obj *o)
{
	if (refcount_dec_and_test(&o->refs))
		kfree_rcu(o, rcu);   /* 或普通 kfree，取决于可见性规则 */
}
```

与 `atomic_t` 选型：

| 场景 | 用谁 |
|---|---|
| 对象引用、生命周期 | **`refcount_t`** |
| 统计、标志位拼装、锁内部 | `atomic_t` / `atomic_long_t` |
| RCU 友好的特殊计数 | 还有 `rcuref_t`（`types.h:200` 起）——进阶再说 |

---

## 4.4 和锁、RCU 的关系（预告）

```text
atomic / refcount     →  单变量的正确加减
spinlock / mutex      →  保护「一整团」不变量（多字段、链表结构）
RCU                   →  读多写少：读者几乎无锁，写者更新后等宽限期再释放内存
```

持 spinlock 时可以 `atomic_*`；但**不能**假设「我用了 atomic 就不用锁」——链表指针摆动、多字段一起变，仍要锁或 RCU 发布规则。

---

## 4.5 函数表

| 名字 | 位置 | 说明 |
|---|---|---|
| `atomic_read/set/inc/dec/...` | `include/linux/atomic*.h` | 通用原子 |
| `atomic_dec_and_test` | 同上 | 释放路径常用 |
| `refcount_inc/dec_and_test` | `include/linux/refcount.h` | 安全引用计数 |
| `REFCOUNT_INIT` | 同上 | 静态初值 |

---

*下一节：自旋锁——不能睡、可关中断的短临界区。*
