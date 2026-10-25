# 5. 自旋锁

**结论**：spinlock 保护**短**临界区。抢不到就**忙等**（spin），所以持锁时**禁止睡眠**（不能 `mutex_lock`、不能 `copy_to_user`、不能 `kmalloc(GFP_KERNEL)` 走回收）。需要挡中断重入时用 `_irq` / `_irqsave` 变体。

> 类型：`include/linux/spinlock_types_raw.h`、`include/linux/spinlock_types.h`。API：`include/linux/spinlock.h`。
> `CONFIG_PREEMPT_RT` 在 `kernel/Kconfig.preempt:70`。关掉时 `spinlock_t` 包着一把 `raw_spinlock`；打开时 `spinlock_t` 换成 `rt_mutex_base`。

---

## 5.1 `raw_spinlock` vs `spinlock`

```c
/* include/linux/spinlock_types_raw.h:14 */
typedef struct raw_spinlock {
	arch_spinlock_t raw_lock;   /* 中文：架构相关的锁字；x86 包含 asm-generic/qspinlock_types.h */
#ifdef CONFIG_DEBUG_SPINLOCK
	unsigned int magic, owner_cpu;
	void *owner;                /* 中文：调试：谁持有、在哪颗 CPU */
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map; /* 中文：lockdep 依赖图节点 */
#endif
} raw_spinlock_t;
```

`magic` 的初值是 `SPINLOCK_MAGIC`（`0xdead4ead`，同文件 `:25`）。

```c
/* include/linux/spinlock_types.h:17 —— 非 PREEMPT_RT */
typedef struct spinlock {
	union {
		struct raw_spinlock rlock;  /* 中文：普通内核：spinlock 包着 raw_spinlock */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define LOCK_PADSIZE (offsetof(struct raw_spinlock, dep_map))
		struct {
			u8 __padding[LOCK_PADSIZE];
			struct lockdep_map dep_map;  /* 中文：和 rlock.dep_map 叠在同一位置 */
		};
#endif
	};
} spinlock_t;
```

```c
/* include/linux/spinlock_types.h:50 —— CONFIG_PREEMPT_RT */
typedef struct spinlock {
	struct rt_mutex_base	lock;   /* 中文：可睡眠、带优先级继承的 rt 锁 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
} spinlock_t;
```

| 类型 | 何时用 |
|---|---|
| `spinlock_t` | **默认**：驱动/子系统一般锁。非 RT 时内部是 `raw_spinlock`；RT 时是 `rt_mutex_base` |
| `raw_spinlock_t` | 必须继续忙等的锁（部分调度、中断极底层）。RT 下它也不会变成睡眠锁 |

初始化：

```c
DEFINE_SPINLOCK(my_lock);           /* 非 RT：spinlock_types.h:43；RT：同文件 :69 */
DEFINE_RAW_SPINLOCK(my_raw);        /* spinlock_types_raw.h:71 */
spin_lock_init(&p->lock);           /* spinlock.h:331（DEBUG_SPINLOCK）或 :341 */
```

---

## 5.2 API 变体表（背这张）

表里 `spin_*` 的行号是**非 RT** 包装（`spinlock.h` 在 `:318` 的 `#ifndef CONFIG_PREEMPT_RT` 里）。它们转到 `&lock->rlock` 上的 `raw_spin_*`。`raw_spin_*` 在这个 `#ifndef` 之前，RT 下也还是忙等。

`CONFIG_PREEMPT_RT` 时，`spinlock.h:455` 改为包含 `include/linux/spinlock_rt.h`。那里的 `spin_lock`（`:43`）调用 `rt_spin_lock`（实现 `kernel/locking/spinlock_rt.c:52`，抢不到会走到 `rtlock_slowlock`，可以睡眠）。`spin_lock_irq`（`spinlock_rt.h:92`）的函数体就是 `rt_spin_lock(lock)`。`spin_lock_irqsave`（`:97`）把 `flags` 写成 0，再调用 `spin_lock`。Kconfig 的帮助文字写了 interrupt threading（`kernel/Kconfig.preempt:78`）。这套 `spin_lock_irqsave` 不保存 EFLAGS。原子上下文继续用 `raw_spin_lock*`。

| API | 行号 | 额外动作 | 使用场景 |
|---|---|---|---|
| `spin_lock` / `spin_unlock` | `:349` / `:389` | 关抢占（非 RT） | 只与进程上下文互斥，同 CPU 中断不会碰该锁 |
| `spin_lock_bh` / `spin_unlock_bh` | `:354` / `:394` | 再禁 softirq | 与 softirq/tasklet 共享数据 |
| `spin_lock_irq` / `spin_unlock_irq` | `:374` / `:399` | 再禁本地硬中断 | 与硬中断共享；假定进临界区前 IRQ 是开的 |
| `spin_lock_irqsave` / `spin_unlock_irqrestore` | `:379` / `:404` | 保存 IRQ 标志再关 | **最稳妥**：不知进来时 IRQ 开关状态 |
| `spin_trylock` | `:359` | 失败立即返回 0 | 避免死锁或顺带做非阻塞路径 |
| `spin_trylock_bh` / `spin_trylock_irq` | `:409` / `:414` | 同上，并处理 BH / IRQ | 非阻塞 + 关 BH 或关 IRQ |
| `spin_trylock_irqsave` | `:419` | 失败时恢复 flags | 非阻塞 + 保存 IRQ |
| `spin_lock_nested` | `:364` | 带 lockdep subclass | 同一把锁类要嵌套时告诉 lockdep |
| `raw_spin_lock` | `:217` | 直接操作 `raw_spinlock_t` | RT 下仍忙等的那一层 |
| `raw_spin_lock_irqsave` | `:241`（另一配置分支 `:263`） | 保存 flags 再忙等 | 原子上下文里的 irqsave |

```c
unsigned long flags;

spin_lock_irqsave(&obj->lock, flags);  /* 中文：关本地 IRQ + 抢锁；flags 必须是左值 */
/* 改共享数据、改 list —— 越短越好 */
spin_unlock_irqrestore(&obj->lock, flags);
```

### C 对照：`flags` 为什么是「值」传递宏？

`spin_lock_irqsave`（`:379`）是宏，里面会调用 `raw_spin_lock_irqsave`，后者把旧的 IRQ 标志写进 `flags`。必须是 `unsigned long flags;` 左值，不能写成字面量。宏里还有 `typecheck(unsigned long, flags)`。

---

## 5.3 规则（违反就等 lockdep / hard lockup）

1. **持 `raw_spinlock` 不睡眠**：任何可能 `schedule()` 的路径都禁止。非 RT 的 `spinlock_t` 同样禁止。RT 的 `spinlock_t` 本身可以睡，原子上下文改用 `raw_spinlock_t`
2. **临界区要短**：自旋浪费的是别的 CPU 的时间
3. **同 CPU 中断也要防**：若硬中断里也会拿同一把锁 → 进程上下文必须 `_irq`/`_irqsave`
4. **锁顺序固定**：多锁时所有人按同一顺序获取，否则 AB-BA 死锁（lockdep 专门抓这个）
5. **不要递归同锁**（普通 spinlock 非递归；嵌套要用 `spin_lock_nested` 并讲清楚 subclass）

```text
进程上下文：spin_lock_irqsave(&L)
     ↑ 同 CPU 硬中断若也 spin_lock(&L) → 自死锁
硬中断：必须用不会再被同锁重入的规则；通常中断里 lock，进程侧 irqsave
```

---

## 5.4 关键路径直觉（非 RT，SMP 内联路径）

`spin_lock` 先到 `raw_spin_lock(&lock->rlock)`。`_irqsave` 的常见内联实现是 `include/linux/spinlock_api_smp.h:104` 的 `__raw_spin_lock_irqsave`（`CONFIG_GENERIC_LOCKBREAK` 关掉，或开了 lockdep 时走这里）。`spinlock.c` 里没内联时只是再调这个函数。

```text
spin_lock_irqsave(lock, flags)                  /* spinlock.h:379 */
  → raw_spin_lock_irqsave(&lock->rlock, flags)
  → __raw_spin_lock_irqsave                     /* spinlock_api_smp.h:104 */
       local_irq_save(flags)                    /* 中文：保存标志再关本地硬中断 */
       preempt_disable()                        /* 中文：关抢占，避免自己被换走还握着锁 */
       spin_acquire(&lock->dep_map, ...)        /* 中文：lockdep 先记账 */
       LOCK_CONTENDED → do_raw_spin_lock        /* spinlock.h:184 */
            → arch_spin_lock(&lock->raw_lock)   /* 中文：真正自旋；x86 上是队列锁 */

spin_unlock_irqrestore                          /* spinlock.h:404 → spinlock_api_smp.h:146 */
  → spin_release(&lock->dep_map)                /* 中文：lockdep 销账 */
  → do_raw_spin_unlock → arch_spin_unlock
  → local_irq_restore(flags)                    /* 中文：按进来时的 flags 恢复 IRQ */
  → preempt_enable()
```

调试配置下 `do_raw_spin_lock` 换成 `spinlock.h:180` 声明的外部函数，会检查 `magic`、`owner`，抓双重解锁和未初始化锁。

---

## 5.5 函数表

| 名字 | 文件:行 | 说明 |
|---|---|---|
| `raw_spinlock_t` | `spinlock_types_raw.h:14` | 真正忙等的锁 |
| `spinlock_t`（非 RT） | `spinlock_types.h:17` | 包着 `rlock` |
| `spinlock_t`（RT） | `spinlock_types.h:50` | `rt_mutex_base` |
| `DEFINE_SPINLOCK` | `spinlock_types.h:43`（RT 为 `:69`） | 静态定义 |
| `DEFINE_RAW_SPINLOCK` | `spinlock_types_raw.h:71` | 静态 raw 锁 |
| `spin_lock_init` | `spinlock.h:331` 或 `:341` | 动态初始化 |
| `spin_lock` | `:349` | 基版加锁 |
| `spin_lock_bh` | `:354` | 加锁 + 关 BH |
| `spin_lock_irq` | `:374` | 加锁 + 关本地 IRQ |
| `spin_lock_irqsave` | `:379` | 加锁 + 存 IRQ |
| `spin_unlock` | `:389` | 基版解锁 |
| `spin_unlock_irqrestore` | `:404` | 解锁 + 恢复 IRQ |
| `spin_trylock` | `:359` | 非阻塞 |
| `__raw_spin_lock_irqsave` | `spinlock_api_smp.h:104` | irqsave 内联路径 |

---

*下一节：可以睡觉的锁（mutex/rwsem）和乐观读的 seqlock。*
