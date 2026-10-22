# kernel-notes

对着 Linux 内核真实源码整理的子系统笔记。默认版本：**v7.3**（最新稳定版，以当时 kernel.org 为准）。

源码不放在本仓库，请自行对照：

- [elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)
- [git.kernel.org stable](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)

## 目录

| 笔记 | 说明 |
|------|------|
| [process-management.md](./process-management.md) | 进程管理：`task_struct`、PID、fork/clone、exec、exit、状态、上下文切换 |

## 约定

- 讲原理落到文件路径、函数名、结构体；行号对应注明的内核版本。
- 观测命令只作参考，默认不在生产机上跑。
