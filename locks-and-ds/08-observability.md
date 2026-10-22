# 8. 观测、lockdep 与踩坑清单

**结论**：同步问题优先让 **lockdep** 说话；热点再看 `/proc/lock_stat`（需配置）和 `perf`/`bpftrace`。下面清单按「C 新手读内核 / 改实验模块」频率排序。

> 笔记钉 **v7.3**。观测命令默认在实验机跑，别一上来砸生产。

---

## 8.1 lockdep：怎么读

开启：`CONFIG_PROVE_LOCKING` / `CONFIG_DEBUG_LOCK_ALLOC`（发行版 debug 内核常见）。

锁对象上的 `dep_map`（见 spinlock/mutex 结构体）会记录：

- 获取顺序 → 抓 **AB-BA**
- 等待类型（spin vs sleep）→ 抓「持 spinlock 却去拿 mutex」一类
- 硬中断 / softirq 上下文是否安全

典型内核日志关键词：

| 信号 | 含义 |
|---|---|
| `WARNING: possible circular locking dependency detected` | 锁序成环 |
| `BUG: sleeping function called from invalid context` | 原子上下文睡了（常伴持锁） |
| `inconsistent lock state` / `hard-irqs-on` vs `soft-irqs-disabled` | IRQ 状态与锁类型不配 |
| `lockdep` + 两把锁的名字与调用栈 | **先读栈**，再改锁序或拆锁 |

处理套路：

1. 看报告里的 **lock-class** 名字与 **held locks** 列表  
2. 统一全局锁序（文档化：先 A 后 B）  
3. 或缩小临界区，避免交叉  

相关头文件：`include/linux/lockdep.h`；实现：`kernel/locking/lockdep.c`。

---

## 8.2 `/proc/lock_stat`

条件：`CONFIG_LOCK_STAT`。实现侧可见 `kernel/locking/lockdep.c:77` 的 `lock_stat` 开关，以及 `kernel/locking/lockdep_proc.c` 的 `lock_stat_open`（约 `:641`）导出 proc 内容。

```bash
# 需要内核打开 CONFIG_LOCK_STAT
cat /proc/lock_stat | head
# 清零（权限足够时）
echo 0 > /proc/lock_stat   # 具体写语义以当前内核文档为准；有的版本用 echo 0 重置
```

读什么：各 lock class 的 wait/hold 时间分布——**争用热点**，不是正确性证明。lockdep 管「对不对」，lock_stat 管「慢不慢」。

若文件不存在：当前内核没编进 `LOCK_STAT`，换 debug 内核或自己编一版实验内核。

---

## 8.3 perf / bpftrace / ftrace（点名）

| 工具 | 用途 |
|---|---|
| `perf lock`（若发行版支持） | 锁争用概景 |
| `perf probe` / `perf trace` | 打到 `mutex_lock`、`synchronize_rcu` 等 |
| `bpftrace` | 如追踪长时间持锁：`kprobe:mutex_lock` + 时间差（注意开销与安全） |
| `tracepoint:rcu:*`（若有） | RCU 宽限期/回调相关 |
| `echo 1 > /sys/kernel/debug/tracing/...` | ftrace 抓锁函数（debugfs） |

完整配方留给以后的 performance 章；这里只要知道「锁问题不全靠 printk」。

---

## 8.4 C 新手 / 内核新手踩坑清单

### 链表

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 遍历时 `list_del` 不用 `_safe` | 下一轮走 poison / 野指针 | `list_for_each_entry_safe` |
| `list_del` 后再 `list_add` | poison 未清理 | `list_del_init` 或先 `INIT_LIST_HEAD` |
| 以为 list API 自带锁 | 数据竞争 | 外层 spinlock/mutex/RCU |
| 搞不清 `member` 参数 | 编译过了运行飞 | `member` 是**字段名**，不是类型 |

### 锁

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 持 `spin_lock` 时 `mutex_lock` / `copy_to_user` / `msleep` | hung / lockdep 怒吼 | 缩短临界区或改用 mutex |
| 与中断共享数据却用 plain `spin_lock` | 同 CPU 自死锁 | `spin_lock_irqsave` |
| 两边锁序不一致 | 偶发死锁 | 全局规定顺序 |
| RT 内核上把 `spinlock_t` 当绝对不睡 | 语义变化 | 原子上下文用 `raw_spinlock_t` |

### 原子 / 引用

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 对象生命周期用裸 `atomic_dec` 不检查饱和 | 回绕 UAF | `refcount_t` |
| `atomic_set` 当发布完整对象 | 读者看见半初始化 | 先写字段，再 atomic/RCU 发布 |

### RCU

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 普通赋值替换 `rcu_assign_pointer` | 重排导致读者看花 | 用发布宏 |
| `list_del_rcu` 后立刻 `kfree` | UAF | `synchronize_rcu` / `call_rcu` |
| 在 spinlock 里 `synchronize_rcu` | 睡眠非法 | `call_rcu` |
| 出了 `rcu_read_unlock` 还用着指针 | UAF | 临界区内用完或改持引用 |

### `container_of`

| 坑 | 后果 | 正确做法 |
|---|---|---|
| `type` / `member` 写错 | 静默指到火星 | 对照结构体定义；相信 `static_assert` 报错 |
| 对 NULL 调 `container_of` | 炸 | 先判空（或用 `*_safe` 变体） |

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
| `lock_stat` proc | `kernel/locking/lockdep_proc.c` | `/proc/lock_stat` |
| `CONFIG_LOCK_STAT` | Kconfig | 统计开关 |
| RCU 跟踪 | `kernel/rcu/` + tracing | 宽限期诊断（进阶） |

---

*大章收束。建议按 README 的顺序过一遍，再拣一个带 list+spinlock 的驱动/子系统源文件对照着读。*
