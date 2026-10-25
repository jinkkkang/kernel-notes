# cgroup（控制组）大章

> 内核版本：**v6.6.160**（stable 6.6 LTS，tag 提交 `0aebf2870d53`，「Linux 6.6.160」）。下文路径、函数、行号、结构体字段都对照该版本源码核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 权威用户接口文档：`Documentation/admin-guide/cgroup-v2.rst`（目录编号 1–6、P、D、R）

**一句话**：cgroup 把进程挂到一棵树上，**core** 管树与迁移，**controllers** 沿树记账/限流；本笔记默认 **cgroup v2 统一层次**。

---

## 目录

1. [总览与边界](./01-overview.md)
2. [核心对象](./02-core-objects.md)
3. [层次与控制接口](./03-hierarchy-control.md)
4. [与进程生命周期的挂钩](./04-lifecycle-hooks.md)
5. [cpu / cpuset](./05-cpu-cpuset.md)
6. [memory](./06-memory.md)
7. [io / pids / 其它控制器](./07-io-pids-misc.md)
8. [PSI 与 rstat](./08-psi-rstat.md)
9. [观测清单](./09-observability.md)

## 建议阅读顺序

```text
01 总览 → 02 核心对象 → 03 层次接口 → 04 生命周期挂钩
                 ↓
            06 memory（最重控制器）→ 05 cpu/cpuset
                 ↓
            07 io/pids/misc → 08 PSI/rstat → 09 观测
```

先对象后接口：不懂 `css_set` 就看不懂 fork 时挂到哪。memory 提前于 cpu：限流语义（high/max/OOM）更「看得见」，且和后续 MM 章衔接。

## 与其它章的边界

| 本章覆盖 | 留给其它章 |
|---|---|
| cgroup core、v2 接口、各控制器**挂钩点** | 完整 EEVDF 选人（[scheduler/](../scheduler/README.md)） |
| `mem_cgroup` charge / limit 直觉 | 页回收、LRU、`kswapd` 深挖（[memory-management/](../memory-management/README.md)） |
| io 控制器 → `blk-cgroup` 入口 | blk-mq / elevator 内部 |
| cgroup namespace | 完整 pidns / mountns（见 [process-management/03](../process-management/03-pid-namespaces.md)） |
| freezer v2 与 `JOBCTL_TRAP_FREEZE` 衔接 | 完整信号/作业控制（[进程章 09](../process-management/09-signals.md)） |
| PSI / rstat 读法 | 完整 perf/ftrace 方法论 |

## 可选 demo

- [demos/README.md](./demos/README.md)：只读/最小创建子 cgroup 的 shell 示意（勿在生产乱写）

## 未覆盖 / 下一章

刻意薄写或不写：

- **cgroup v1 多树全面手册**（仅对照）
- **完整 `mem_cgroup` 回收与 writeback 域**
- **完整 cpu 带宽实现与 EEVDF 互动**
- **编排层**（systemd / Docker / K8s 策略）

**下一章建议**：调度（EEVDF）或内存管理——两者都会大量碰到本章的 cpu/memory 挂钩。

---

*笔记生成对照源码：Linux v6.6.160。*
