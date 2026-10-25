# 8. 观测、lockdep 与踩坑清单

**结论**：同步问题优先让 **lockdep** 说话；热点再看 `/proc/lock_stat`（需配置）和 `perf`/`bpftrace`。下面清单按「C 新手读内核 / 改实验模块」频率排序。

> 笔记钉 **v6.6.160**。观测命令默认在实验机跑，别一上来砸生产。本节的 shell 没有在作者机器上跑出采样，标成 **RECIPE**。

---

## 8.1 lockdep：怎么读

开启：`CONFIG_PROVE_LOCKING`（`lib/Kconfig.debug:1294`）/ `CONFIG_DEBUG_LOCK_ALLOC`（同文件 `:1429`）。发行版 debug 内核常见。

锁对象上的 `dep_map`（见 spinlock/mutex 结构体）会记录：

- 获取顺序 → 抓 **AB-BA**
- 等待类型（spin vs sleep）→ 抓「持 spinlock 却去拿 mutex」一类
- 硬中断 / softirq 上下文是否安全

典型内核日志关键词（字符串按 v6.6.160 源码）：

| 信号 | 位置 | 含义 |
|---|---|---|
| `WARNING: possible circular locking dependency detected` | `kernel/locking/lockdep.c:1974` | 锁序成环 |
| `BUG: sleeping function called from invalid context` | `kernel/sched/core.c:10241` | 原子上下文睡了（常伴持锁） |
| `WARNING: inconsistent lock state` | `lockdep.c:4004` | 同一把锁的 IRQ 用法自相矛盾 |
| `inconsistent {%s} -> {%s} usage.` | `lockdep.c:4008` | `%s` 来自 `usage_str[]`（`:651`） |

`usage_str` 由 `kernel/locking/lockdep_states.h` 的 `HARDIRQ` / `SOFTIRQ` 展开（`lockdep.c:645`）。你会看到的是 `HARDIRQ-ON-W`、`IN-HARDIRQ-W`、`SOFTIRQ-ON-W`、`IN-SOFTIRQ-W`，以及对应的 `-R`（读锁）和 `INITIAL USE`。同一段报告里还有（`:4186` 起）：

```text
hardirqs last  enabled at ...
hardirqs last disabled at ...
softirqs last  enabled at ...
softirqs last disabled at ...
```

处理套路：

1. 看报告里的 **lock-class** 名字与 **held locks** 列表
2. 统一全局锁序（文档化：先 A 后 B）
3. 或缩小临界区，避免交叉

相关头文件：`include/linux/lockdep.h`；实现：`kernel/locking/lockdep.c`。

---

## 8.2 `/proc/lock_stat`

条件：`CONFIG_LOCK_STAT`（`lib/Kconfig.debug:1358`）。

- 打开时，`kernel/locking/lockdep.c:74` 有 `static int lock_stat = 1;`，`:75` 把它做成模块参数
- 关掉时，`:77` 是 `#define lock_stat 0`，统计代码被编掉
- proc 文件由 `kernel/locking/lockdep_proc.c:639` 的 `lock_stat_open` 提供，`:723` `proc_create("lock_stat", ...)`
- 写入口是 `lock_stat_write`（`:675`）：读用户缓冲区**第一个字节**，是 `'0'` 就对每个在用的 lock class 调用 `clear_lock_stats`；其他字符什么都不清

```bash
# RECIPE：未在本机执行。需要 CONFIG_LOCK_STAT，并且对 /proc/lock_stat 有写权限。
cat /proc/lock_stat | head
echo 0 > /proc/lock_stat
```

读什么：各 lock class 的 wait/hold 时间分布——**争用热点**，不是正确性证明。lockdep 管「对不对」，lock_stat 管「慢不慢」。文件头会打出 `lock_stat version 0.4`（`lockdep_proc.c:572`）。

若文件不存在：当前内核没编进 `LOCK_STAT`，换 debug 内核或自己编一版实验内核。

---

## 8.3 perf / bpftrace / ftrace（点名）

| 工具 | 用途 |
|---|---|
| `perf lock`（若发行版的 perf 编了 lock 子命令） | 锁争用概景 |
| `perf probe` / `perf trace` | 打到 `mutex_lock`、`synchronize_rcu` 等 |
| `bpftrace` | 如追踪长时间持锁：`kprobe:mutex_lock` + 时间差（注意开销与安全） |
| `tracepoint:rcu:*` | `include/trace/events/rcu.h` 里有，例如 `rcu_utilization`（`:27`）、`rcu_stall_warning`（`:444`） |
| ftrace | debugfs 下的 `tracing`，抓锁函数 |

```bash
# RECIPE：未在本机执行。完整配方留给以后的 performance 章。
# perf lock record -a sleep 1
# bpftrace -e 'kprobe:mutex_lock { @[kstack] = count(); }'
```

这里只要知道「锁问题不全靠 printk」。

---

## 8.4 C 新手 / 内核新手踩坑清单

### 链表

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 遍历时 `list_del` 不用 `_safe` | 下一轮走 poison / 野指针 | `list_for_each_entry_safe`（`list.h:863`） |
| `list_del` 后再 `list_add` | `next`/`prev` 仍是 `LIST_POISON*` | `list_del_init`（`:292`）或先 `INIT_LIST_HEAD` |
| 以为 list API 自带锁 | 数据竞争 | 外层 spinlock/mutex/RCU |
| 搞不清 `member` 参数 | 编译过了运行飞 | `member` 是**字段名**，不是类型 |

### 锁

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 持非 RT `spin_lock` 或任意 `raw_spin_lock` 时 `mutex_lock` / `copy_to_user` / `msleep` | hung / lockdep 怒吼 | 缩短临界区或改用 mutex |
| 与中断共享数据却用 plain `spin_lock` | 同 CPU 自死锁 | `spin_lock_irqsave` |
| 两边锁序不一致 | 偶发死锁 | 全局规定顺序 |
| RT 内核上把 `spinlock_t` 当绝对不睡 | `spinlock_t` 已是 `rt_mutex_base`，可能睡眠 | 原子上下文用 `raw_spinlock_t` |

### 原子 / 引用

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 对象生命周期用裸 `atomic_dec` 不检查饱和 | 回绕 UAF | `refcount_t`（`refcount.h:111`） |
| `atomic_set` 当发布完整对象 | 读者看见半初始化 | 先写字段，再 atomic/RCU 发布 |

### RCU

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 普通赋值替换 `rcu_assign_pointer` | 重排导致读者看花 | 用 `rcupdate.h:531` 的发布宏 |
| `list_del_rcu` 后立刻 `kfree` | UAF | `synchronize_rcu` / `call_rcu` |
| 在 spinlock 里 `synchronize_rcu` | 睡眠非法 | `call_rcu` |
| 出了 `rcu_read_unlock` 还用着指针 | UAF | 临界区内用完或改持引用 |
| 在 idle 里 `rcu_read_lock` | lockdep：`used illegally while idle` | 先离开 idle / 用对的 RCU 变体 |

### `container_of`

| 坑 | 后果 | 正确做法 |
|---|---|---|
| `type` / `member` 写错 | 类型对不上时 `static_assert` 报 `pointer type mismatch in container_of()` | 对照结构体定义 |
| 对 NULL 调 `container_of` | 减偏移后变成一个野指针 | 先判空 |

---

## 8.5 和 process-management 观测的分工

| 本章 | 进程章 [10-observability](../process-management/10-observability.md) |
|---|---|
| lockdep、锁争用、RCU 误用 | `/proc/<pid>/...`、fork/exit、wchan |
| 同步原语对不对、热不热 | 任务状态、线程组、命名空间 |

两者一起看：`wchan` 卡在 `mutex`/`rwsem` 相关符号时，回到本章查是谁持锁太久。

---

## 8.6 本节入口表

| 名字 | 位置 | 说明 |
|---|---|---|
| lockdep 核心 | `kernel/locking/lockdep.c` | 依赖图与警告 |
| 环状依赖警告 | `lockdep.c:1974` | `possible circular locking dependency detected` |
| 锁状态不一致 | `lockdep.c:4004` | `inconsistent lock state` |
| 原子上下文睡眠 | `kernel/sched/core.c:10241` | `sleeping function called from invalid context` |
| `lock_stat` 开关 | `lockdep.c:74`（关掉时 `:77`） | 统计是否记账 |
| `lock_stat_open` / `lock_stat_write` | `kernel/locking/lockdep_proc.c:639` / `:675` | `/proc/lock_stat` 读 / 写 `'0'` 清零 |
| `CONFIG_LOCK_STAT` | `lib/Kconfig.debug:1358` | 统计开关 |
| `CONFIG_PROVE_LOCKING` | `lib/Kconfig.debug:1294` | 证明锁序 |
| RCU 跟踪 | `include/trace/events/rcu.h` | `rcu_utilization`、`rcu_stall_warning` 等 |

---

*大章收束。建议按 README 的顺序过一遍，再拣一个带 list+spinlock 的驱动/子系统源文件对照着读。*
