# kernel-notes

对着 Linux 内核真实源码整理的子系统笔记。默认版本：**v7.3**（最新稳定版，以当时 kernel.org 为准）。

源码不放在本仓库，请自行对照：

- [elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)
- [git.kernel.org stable](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)

## 目录

| 笔记 | 说明 |
|------|------|
| [process-management.md](./process-management.md) | **进程管理（大章）**：总览与 `CLONE_*`、`task_struct` 字段分组、PID/pidns、fork/clone/clone3、内核线程、exec、exit/僵尸回收、上下文切换入口、**信号与进程**、观测清单。调度选人算法（EEVDF）另章。 |

## 约定

- 讲原理落到文件路径、函数名、结构体；行号对应注明的内核版本。
- 大章要求：关键结构体成员中文注释、每节关键函数表、关键路径中文注释摘录。
- 观测命令只作参考，默认不在生产机上跑。
