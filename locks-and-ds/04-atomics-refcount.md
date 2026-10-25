# 4. 原子操作与引用计数

**结论**：多 CPU 同时改同一个计数，普通 `++`/`--` 会丢更新；`atomic_t` 提供「一整次」改内存的操作。对象生命周期更推荐 **`refcount_t`**——饱和、防回绕，踩 UAF 时更吵。本节为 spinlock / RCU 铺垫，**不讲完** C11 内存序教材。

---

## 4.1 `atomic_t` / `atomic64_t`

```c
/* include/linux/types.h:173 */
typedef struct {
	int counter;  /* 中文：唯一载荷；别直接碰，走 API */
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

#ifdef CONFIG_64BIT
typedef struct {
	s64 counter;
} atomic64_t;
#endif
```

`CONFIG_64BIT` 关掉时，这段里没有 `atomic64_t`。32 位 x86 在 `arch/x86/include/asm/atomic64_32.h:11` 自己定义：

```c
typedef struct {
	s64 __aligned(8) counter;  /* 中文：强制 8 字节对齐，方便 32 位机做 64 位原子 */
} atomic64_t;
```

初始化：`ATOMIC_INIT(0)` 或 `atomic_set(&v, 0)`。

### 常用操作

对外入口是 `include/linux/atomic.h`，它在 `:82` 拉进 `include/linux/atomic/atomic-instrumented.h`（带 KCSAN/KASAN 插桩）。下面行号是 instrumented 头里的包装函数。

| API | 行号 | 直觉 |
|---|---|---|
| `atomic_read` | `:30` | 读（注释写 relaxed） |
| `atomic_set` | `:65` | 无条件写 |
| `atomic_inc` | `:433` | +1 |
| `atomic_dec` | `:590` | -1 |
| `atomic_add` | `:102` | 加一个整数 |
| `atomic_sub` | `:268` | 减一个整数 |
| `atomic_add_return` | `:120` | 加并**返回新值**；注释写 full ordering |
| `atomic_inc_return` | `:450` | +1 并返回新值 |
| `atomic_dec_and_test` | `:1372` | 减一后是否变成 0（释放对象前最常见） |
| `atomic_cmpxchg` | `:1191` | CAS：旧值等于 `old` 才换成 `new`，返回看到的旧值 |
| `atomic_try_cmpxchg` | `:1270` | CAS：成功返回 true，失败把当前值写回 `*old` |
| `atomic_fetch_add` | `:194` | 加上去，返回**旧值** |

64 位版本同名加 `atomic64_` 前缀（instrumented 头从 `:1594` 的 `atomic64_read` 起）。

```c
if (atomic_dec_and_test(&obj->users)) {
	/* 中文：我是最后一个持有者，负责拆对象 */
	kfree(obj);
}
```

`include/linux/atomic.h:10` 起把内存序分成四档，和函数名后缀对应：无后缀是全序，`_acquire` / `_release` / `_relaxed` 是另外三档。

---

## 4.2 内存序：内核里你先记住三档

完整 `memory_order` 枚举留给书本。读内核 API 名后缀时：

| 后缀 / 形态 | 粗线条含义 | 典型用途 |
|---|---|---|
| `_relaxed` | 原子性有，跨 CPU 重排约束弱 | 统计计数 |
| `_acquire` / `_release` | 配对：release 之前的写，对之后的 acquire 读可见 | 发布指针、解锁一侧 |
| 无后缀（不少 `*_return`、锁内部） | 头文件称 fully ordered | 锁的 fast path |

配合宏：

- `WRITE_ONCE` / `READ_ONCE`：`include/asm-generic/rwonce.h:58` / `:47`，单次访问，防编译器拆成两次
- `smp_store_release` / `smp_load_acquire`：架构头没抢先 `#define` 时，由 `include/asm-generic/barrier.h` 提供。`CONFIG_SMP` 下是 `:172` / `:176`；非 SMP 是 `:194` / `:203`
- `smp_mb()`：同文件 `:99` 或 `:113`，全屏障（贵，少用）

**C 对照**：别写成 `obj->flag = 1;` 就以为另一边立刻看见完整对象——发布指针要用 RCU 的 `rcu_assign_pointer` 或至少 release 语义（见 [07](./07-rcu.md)）。

---

## 4.3 `refcount_t`：专为引用计数打磨

类型和 API 都在 `include/linux/refcount.h`。

```c
/* include/linux/refcount.h:111 */
typedef struct refcount_struct {
	atomic_t refs;   /* 中文：内部仍是 atomic，但 API 带饱和与告警 */
} refcount_t;

#define REFCOUNT_INIT(n)	{ .refs = ATOMIC_INIT(n), }
#define REFCOUNT_MAX		INT_MAX
#define REFCOUNT_SATURATED	(INT_MIN / 2)
```

| API | 行号 | 说明 |
|---|---|---|
| `REFCOUNT_INIT(n)` | `:115` | 静态初值（常见 `REFCOUNT_INIT(1)`） |
| `refcount_set` | `:134` | 运行时设值 |
| `refcount_inc` | `:265` | +1；0 上再加会 `WARN`（加到已释放的对象） |
| `refcount_dec` | `:357` | -1；减完仍 ≥ 0 才正常，否则饱和告警 |
| `refcount_inc_not_zero` | `:243` | 已是 0 则失败（对象正在销毁） |
| `refcount_dec_and_test` | `:331` | 减到 0 返回 true → 可释放 |
| `refcount_add` | `:220` | 一次加多份 |
| `refcount_sub_and_test` | `:308` | 减多份，减到 0 返回 true |

饱和值 `REFCOUNT_SATURATED` 是 `INT_MIN / 2`（`:117`）。计数坏掉后**卡住**，避免回绕成「又像有引用」导致 UAF。告警走 `refcount_warn_saturate`（`:127`）。

```c
void get_obj(struct obj *o)
{
	refcount_inc(&o->refs);
}

void put_obj(struct obj *o)
{
	if (refcount_dec_and_test(&o->refs))
		kfree_rcu(o, rcu);   /* 或普通 kfree，取决于可见性规则；见 07 */
}
```

与 `atomic_t` 选型：

| 场景 | 用谁 |
|---|---|
| 对象引用、生命周期 | **`refcount_t`**（字段名 `refs`） |
| 统计、标志位拼装、锁内部 | `atomic_t` / `atomic_long_t` |
| RCU 友好的特殊计数 | `rcuref_t`（`types.h:185`），字段名是 `refcnt`，和 `refcount_t` 不是同一个结构 |

```c
/* include/linux/types.h:185 */
typedef struct {
	atomic_t refcnt;
} rcuref_t;
```

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
| `atomic_read` / `atomic_set` / `atomic_inc` | `include/linux/atomic/atomic-instrumented.h` | 通用原子（经 `atomic.h` 引入） |
| `atomic_dec_and_test` | 同文件 `:1372` | 释放路径常用 |
| `atomic_try_cmpxchg` | 同文件 `:1270` | CAS，失败写回旧值 |
| `refcount_inc` / `refcount_dec_and_test` | `include/linux/refcount.h:265` / `:331` | 安全引用计数 |
| `REFCOUNT_INIT` / `REFCOUNT_SATURATED` | 同文件 `:115` / `:117` | 静态初值 / 饱和哨兵 |
| `rcuref_t` | `include/linux/types.h:185` | 另一套 RCU 引用计数，字段 `refcnt` |

---

## 4.6 可执行 Demo：丢更新 vs 原子加

源码：[demos/04_atomic_race.c](./demos/04_atomic_race.c)——16 线程 × 200 万次；普通读-改-写 vs C11 `atomic_fetch_add`。

```bash
cd locks-and-ds/demos
gcc -Wall -O2 -pthread -o 04_atomic_race 04_atomic_race.c
./04_atomic_race
```

> **sampled on 6.12**。环境：2026-10-22 16:40:47 CST（Asia/Shanghai）；Linux 6.12.94+；gcc (Debian 14.2.0-19) 14.2.0
> `plain RMW` 的具体亏损数每次不同；`atomic_fetch_add` 应始终等于 expected。

```text
threads=16 iterations_each=2000000
expected                  = 32000000
plain RMW (racy)          = 2503832  (LOST UPDATES)
atomic_fetch_add          = 32000000  (OK)
```

| Demo | 内核 |
|---|---|
| `plain_counter` 显式 RMW | 多 CPU 上普通 `++` 会丢更新 |
| `atomic_fetch_add_explicit(..., memory_order_relaxed)` | `atomic_fetch_add_relaxed` / 统计计数用的 relaxed 档 |
| （本 demo 未演示） | 对象生命周期优先 `refcount_t`（见上文） |


*下一节：自旋锁——不能睡、可关中断的短临界区。*
