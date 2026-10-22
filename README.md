# kernel-notes

对着 Linux 内核真实源码整理的子系统笔记。默认版本：**v7.3**（最新稳定版，以当时 kernel.org 为准）。

源码不放在本仓库，请自行对照：

- [elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)
- [git.kernel.org stable](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)

## 目录

| 笔记 | 说明 |
|------|------|
| [process-management/](./process-management/) | **进程管理（大章）**：总览与 `CLONE_*`、`task_struct`、PID/pidns、fork/clone、内核线程、exec、exit/回收、上下文切换入口、信号、观测清单。调度选人算法（EEVDF）另章。 |

### 进程管理子章节

| 文件 | 内容 |
|------|------|
| [README.md](./process-management/README.md) | 章节目录与未覆盖说明 |
| [01-overview.md](./process-management/01-overview.md) | 总览与边界 |
| [02-task-struct.md](./process-management/02-task-struct.md) | 任务抽象：task_struct |
| [03-pid-namespaces.md](./process-management/03-pid-namespaces.md) | PID 与命名空间 |
| [04-fork-clone.md](./process-management/04-fork-clone.md) | 创建路径：fork / clone / clone3 / vfork |
| [05-kernel-threads.md](./process-management/05-kernel-threads.md) | 内核线程与特殊任务 |
| [06-exec.md](./process-management/06-exec.md) | 地址空间交接：exec |
| [07-exit-reap.md](./process-management/07-exit-reap.md) | 退出、僵尸与回收 |
| [08-context-switch.md](./process-management/08-context-switch.md) | 上下文切换（进程管理视角） |
| [09-signals.md](./process-management/09-signals.md) | 信号与进程 |
| [10-observability.md](./process-management/10-observability.md) | 观测与调试清单 |

> 旧链接 [process-management.md](./process-management.md) 仍保留为跳转说明。

## 约定

- 讲原理落到文件路径、函数名、结构体；行号对应注明的内核版本。
- 大章要求：关键结构体成员中文注释、每节关键函数表、关键路径中文注释摘录。
- 观测命令只作参考，默认不在生产机上跑。
