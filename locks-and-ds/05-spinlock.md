# 5. 自旋锁

**结论**：spinlock 保护**短**临界区。抢不到就**忙等**（spin），所以持锁时**禁止睡眠**（不能 `mutex_lock`、不能 `copy_to_user`、不能 `kmalloc(GFP_KERNEL)` 走回收）。需要挡中断重入时用 `_irq` / `_irqsave` 变体。

> 类型：`include/linux/spinlock_types*.h`；API：`include/linux/spinlock.h`。非 `PREEMPT_RT` 下 `spinlock_t` 基本是包着一层的 `raw_spinlock`。

---

## 5.1 `raw_spinlock` vs `spinlock`

```c
/* include/linux/spinlock_types_raw.h:14 */
context_lock_struct(raw_spinlock) {
	arch_spinlock_t raw_lock;   /* 中文：架构相关的真正票锁/队列锁位 */
#ifdef CONFIG_DEBUG_SPINLOCK
	unsigned int magic, owner_cpu;
	void *owner;                /* 中文：调试：谁持有 */
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map; /* 中文：lockdep 依赖图节点 */
#endif
};
typedef struct raw_spinlock raw_spinlock_t;
```

```c
/* include/linux/spinlock_types.h:17 非 RT */
context_lock_struct(spinlock) {
	union {
		struct raw_spinlock rlock;  /* 中文：普通内核：spinlock ≈ raw_spinlock */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
		struct {
			u8 __padding[LOCK_PADSIZE];
			struct lockdep_map dep_map;
		};
#endif
	};
};
typedef struct spinlock spinlock_t;
```

| 类型 | 何时用 |
|---|---|
| `spinlock_t` | **默认**：驱动/子系统一般锁 |
| `raw_spinlock_t` | 必须在「绝对不能变成睡眠锁」的上下文（部分调度、中断极底层）；RT 下语义更「硬」 |

**PREEMPT_RT**：`spinlock_t` 可映射成可睡眠的 rt 锁；真正忙等的是 `raw_spinlock_t`。写可移植代码时：中断/原子上下文用的锁要想清楚是 raw 还是普通。

初始化：

```c
DEFINE_SPINLOCK(my_lock);           /* spinlock.h / types 宏 */
DEFINE_RAW_SPINLOCK(my_raw);
spin_lock_init(&p->lock);           /* 动态 */
```

---

## 5.2 API 变体表（背这张）

| API | 额外动作 | 使用场景 |
|---|---|---|
| `spin_lock` / `spin_unlock` | 关抢占（非 RT） | 只与进程上下文互斥，同 CPU 中断不会碰该锁 |
| `spin_lock_bh` / `spin_unlock_bh` | 再禁 softirq | 与 softirq/tasklet 共享数据 |
| `spin_lock_irq` / `spin_unlock_irq` | 再禁本地硬中断 | 与硬中断共享；假定进临界区前 IRQ 是开的 |
| `spin_lock_irqsave` / `spin_unlock_irqrestore` | 保存 IRQ 标志再关 | **最稳妥**：不知进来时 IRQ 开关状态 |
| `spin_trylock*` | 失败立即返回 | 避免死锁或顺带做非阻塞路径 |

行号锚点（`include/linux/spinlock.h`）：

- `spin_lock` `:344`
- `spin_lock_bh` `:350`
- `spin_lock_irq` `:374`
- `spin_lock_irqsave` `:386`
- `spin_unlock` `:398`
- `spin_unlock_irqrestore` `:422`

```c
unsigned long flags;

spin_lock_irqsave(&obj->lock, flags);  /* 中文：关本地 IRQ + 抢锁；flags 存旧状态 */
/* 改共享数据、改 list —— 越短越好 */
spin_unlock_irqrestore(&obj->lock, flags);
```

### C 对照：`flags` 为什么是「值」传递宏？

`spin_lock_irqsave(lock, flags)` 是宏，会**写入**你传入的 `flags` 变量。必须是 `unsigned long flags;` 左值，不能写成字面量。

---

## 5.3 规则（违反就等 lockdep / hard lockup）

1. **持锁不睡眠**：任何可能 `schedule()` 的路径都禁止  
2. **临界区要短**：自旋浪费的是别的 CPU 的时间  
3. **同 CPU 中断也要防**：若硬中断里也会拿同一把锁 → 进程上下文必须 `_irq`/`_irqsave`  
4. **锁顺序固定**：多锁时所有人按同一顺序获取，否则 AB-BA 死锁（lockdep 专门抓这个）  
5. **不要递归同锁**（普通 spinlock 非递归）

```text
进程上下文：spin_lock_irqsave(&L)
     ↑ 同 CPU 硬中断若也 spin_lock(&L) → 自死锁
硬中断：必须用不会再被同锁重入的规则；通常中断里 lock，进程侧 irqsave
```

---

## 5.4 关键路径直觉（非 RT）

```text
spin_lock_irqsave
  → 保存 flags，local_irq_disable
  → preempt_disable（嵌在锁里）
  → arch_spin_lock(&lock->rlock.raw_lock)   /* 真正自旋 */
  → lockdep 记账

spin_unlock_irqrestore
  → arch_spin_unlock
  → 恢复 IRQ（按 flags）
  → preempt_enable
```

调试配置下还有 `magic`、`owner` 检查双重解锁、未初始化锁等。

---

## 5.5 函数表

| 名字 | 文件:行 | 说明 |
|---|---|---|
| `DEFINE_SPINLOCK` | `spinlock_types.h:44` | 静态定义 |
| `spin_lock` | `spinlock.h:344` | 基版加锁 |
| `spin_lock_irqsave` | `:386` | 加锁 + 存 IRQ |
| `spin_unlock_irqrestore` | `:422` | 解锁 + 恢复 IRQ |
| `spin_trylock` | 同文件 | 非阻塞 |
| `raw_spin_lock*` | 同文件前部 | 硬自旋版本 |

---

*下一节：可以睡觉的锁（mutex/rwsem）和乐观读的 seqlock。*
