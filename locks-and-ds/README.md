# 同步原语与内核数据结构（大章）

> 内核版本：**v7.3**（commit `c03c1a14507d`）。下文路径/函数/行号均在本机 `/workspace/linux` 对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)

**一句话**：先把读内核绕不开的 C 习惯和「嵌入式链表」吃透，再谈谁能睡、谁不能睡，最后才是 RCU——写者等读者走完，而不是互相挡着。

面向 **C 语言新手**：本章不是完整 C 教程，而是「内核里你会反复撞上的写法」；每遇到宏藏指针算术，都会有一小段「C 对照」。

---

## 目录

1. [读内核够用的 C](./01-c-primer.md)
2. [双向链表与哈希链表](./02-list.md)
3. [树与映射容器](./03-trees-maps.md)
4. [原子操作与引用计数](./04-atomics-refcount.md)
5. [自旋锁](./05-spinlock.md)
6. [可睡眠锁与序列锁](./06-sleepable-locks.md)
7. [RCU](./07-rcu.md)
8. [观测、lockdep 与踩坑清单](./08-observability.md)


## 可执行 Demo

目录：[demos/](./demos/)（说明见 [demos/README.md](./demos/README.md)）。

| Demo | 章节 | 类型 |
|---|---|---|
| `01_container_of.c` | 01 | 用户态，本机已验证 |
| `02_list.c` | 02 | 用户态，本机已验证 |
| `03_hash_hlist.c` | 03 | 用户态，本机已验证 |
| `04_atomic_race.c` | 04 | 用户态，本机已验证 |
| `kmod_list_example.c` | 02 | 内核模块**示例** |
| `kmod_idr_example.c` | 03 | 内核模块**示例** |

用户态一键：

```bash
cd locks-and-ds/demos
gcc -Wall -O2 -o 01_container_of 01_container_of.c && ./01_container_of
gcc -Wall -O2 -o 02_list 02_list.c && ./02_list
gcc -Wall -O2 -o 03_hash_hlist 03_hash_hlist.c && ./03_hash_hlist
gcc -Wall -O2 -pthread -o 04_atomic_race 04_atomic_race.c && ./04_atomic_race
```

## 建议阅读顺序

```text
01 C 入门 → 02 list_head → 04 atomic/refcount
                 ↓
            05 spinlock → 06 mutex/rwsem/seqlock
                 ↓
            07 RCU（可回头看 02 的 RCU 链表变体）
                 ↓
            03 树/映射（用到再深挖）→ 08 观测
```

先链表后锁：内核几乎所有容器都是「节点嵌在对象里」，不理解 `container_of` 后面全是雾。先原子后锁：锁的底层常常就是原子 + 屏障。RCU 放锁之后：你需要先知道「写者自己怎么互斥」。

## 与 process-management 的边界

| 本章覆盖 | 留给 process-management / 其他章 |
|---|---|
| `list_head` / 常用容器直觉 | `task_struct` 成员语义、fork/exit 全路径 |
| spinlock / mutex / rwsem / seqlock / RCU | 调度选人（EEVDF）、完整 MM |
| `refcount_t` 与对象生命周期套路 | 完整 cgroup / 完整 ptrace |
| tasks RCU 与 exit 的**衔接点** | `do_exit` / `release_task` 细节（见进程章） |
| lockdep、`/proc/lock_stat` 怎么读 | 完整 ftrace/perf 方法论（性能章） |

## 未覆盖 / 下一章

刻意薄写或不写：

- **完整内存序教材**（C11 `memory_order_*` 全表）——只点到内核常用的 acquire/release/FULL
- **PREEMPT_RT 下 spinlock→rt_mutex 的完整语义**——标注差异，不展开实现
- **SRCU / RCU Tasks Trace / 整棵 rcu_node 树**——点到名字与用途
- **完整 lockdep 图论与所有警告类别**

**下一章建议**：调度（EEVDF）或中断/软中断——两者都会大量用到本章的锁与 list。

---

*笔记生成对照源码：`/workspace/linux`（v7.3，`c03c1a14507d`）。*
