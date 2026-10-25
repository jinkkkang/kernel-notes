# Linux 进程管理（大章）

> 内核版本：**v6.6.160**。下文路径、函数、行号都对照 `/tmp/linux` 里的这份源码。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)

**一句话**：内核里没有单独的「进程对象」和「线程对象」，全是 `task_struct`。差别只在 `clone` 时哪些资源共享引用、哪些复制一份。本章从抽象讲到创建、exec、退出、切换和信号。调度器「选谁上 CPU」留给下一章。

---

## 目录

1. [总览与边界](./01-overview.md)
2. [任务抽象：task_struct](./02-task-struct.md)
3. [PID 与命名空间](./03-pid-namespaces.md)
4. [创建路径：fork / clone / clone3 / vfork](./04-fork-clone.md)
5. [内核线程与特殊任务](./05-kernel-threads.md)
6. [地址空间交接：exec](./06-exec.md)
7. [退出、僵尸与回收](./07-exit-reap.md)
8. [上下文切换（进程管理视角）](./08-context-switch.md)
9. [信号与进程](./09-signals.md)
10. [观测与调试清单](./10-observability.md)

## 未覆盖 / 下一章

本章刻意停下的部分：

- **调度算法**：公平类 EEVDF（`sched_entity` 上的 `vruntime`、`vlag`、`deadline`、`slice`）、实时（RT）、截止期限（DL）
- **完整 MM**：`dup_mm` 内部、缺页、大页
- **完整 ptrace / seccomp**
- **cgroup 冻结与 cpu controller 细节**

**下一章建议**：从 `__schedule` → `pick_next_task` → EEVDF 的 `sched_entity` 讲开，仍对照 v6.6.160 源码。

---

*笔记对照源码：`/tmp/linux`（v6.6.160）。*
