# kernel-notes

对着 Linux 内核真实源码整理的子系统笔记。默认版本：**v7.3**（最新稳定版，以当时 kernel.org 为准）。

源码不放在本仓库，请自行对照：

- [elixir.bootlin.com — linux/v7.3](https://elixir.bootlin.com/linux/v7.3/source)
- [git.kernel.org stable](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)

## 目录

| 笔记 | 说明 |
|------|------|
| [process-management/](./process-management/) | **进程管理（大章）**：总览与 `CLONE_*`、`task_struct`、PID/pidns、fork/clone、内核线程、exec、exit/回收、上下文切换入口、信号、观测清单。调度选人算法（EEVDF）另章。 |
| [locks-and-ds/](./locks-and-ds/) | **同步原语与内核数据结构（大章）**：读内核够用的 C、`list_head`/hlist、树与映射容器、atomic/refcount、spinlock、mutex/rwsem/seqlock、RCU、lockdep 与踩坑。面向 C 新手补嵌入式链表与宏。 |
| [cgroup/](./cgroup/) | **控制组（大章）**：cgroup v2 core 与对象模型、层次接口、fork/exit 挂钩与 cgroup ns、cpu/cpuset、memory、io/pids/freezer、PSI/rstat、本机观测。完整调度/MM/块层另章。 |

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

### 同步原语与内核数据结构子章节

| 文件 | 内容 |
|------|------|
| [README.md](./locks-and-ds/README.md) | 章节目录、与进程章边界、阅读顺序 |
| [01-c-primer.md](./locks-and-ds/01-c-primer.md) | 读内核够用的 C（含 `container_of`） |
| [02-list.md](./locks-and-ds/02-list.md) | `list_head` / hlist |
| [03-trees-maps.md](./locks-and-ds/03-trees-maps.md) | rbtree / xarray / maple / idr / hashtable |
| [04-atomics-refcount.md](./locks-and-ds/04-atomics-refcount.md) | atomic 与 `refcount_t` |
| [05-spinlock.md](./locks-and-ds/05-spinlock.md) | 自旋锁与 IRQ 变体 |
| [06-sleepable-locks.md](./locks-and-ds/06-sleepable-locks.md) | mutex / rwsem / seqlock |
| [07-rcu.md](./locks-and-ds/07-rcu.md) | RCU 与 tasks RCU 衔接 |
| [08-observability.md](./locks-and-ds/08-observability.md) | lockdep、lock_stat、踩坑清单 |

### cgroup 子章节

| 文件 | 内容 |
|------|------|
| [README.md](./cgroup/README.md) | 章节目录、阅读顺序、与其它章边界 |
| [01-overview.md](./cgroup/01-overview.md) | 总览与边界（v2 默认） |
| [02-core-objects.md](./cgroup/02-core-objects.md) | cgroup / css / css_set / subsys |
| [03-hierarchy-control.md](./cgroup/03-hierarchy-control.md) | subtree_control、迁移、委托 |
| [04-lifecycle-hooks.md](./cgroup/04-lifecycle-hooks.md) | fork/exit 挂钩、CLONE_INTO_CGROUP、cgroup ns |
| [05-cpu-cpuset.md](./cgroup/05-cpu-cpuset.md) | cpu / cpuset 控制器 |
| [06-memory.md](./cgroup/06-memory.md) | memory 控制器与 charge 直觉 |
| [07-io-pids-misc.md](./cgroup/07-io-pids-misc.md) | io / pids / freezer / 其它 |
| [08-psi-rstat.md](./cgroup/08-psi-rstat.md) | PSI 与 rstat |
| [09-observability.md](./cgroup/09-observability.md) | 观测与本机实测 |

## 约定

- 讲原理落到文件路径、函数名、结构体；行号对应注明的内核版本。
- 大章要求：关键结构体成员中文注释、每节关键函数表、关键路径中文注释摘录。
- 观测命令只作参考，默认不在生产机上跑。
