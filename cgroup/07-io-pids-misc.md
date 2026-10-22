# 7. io / pids / 其它控制器

**结论**：io 把「谁在发 I/O」接到 **blk-cgroup**；pids 防 fork 炸弹；freezer v2 用 `cgroup.freeze` 冻整棵子树。其余控制器点名即可。

---

## 7.1 io → blk-cgroup

| 文件（常见） | 含义 |
|---|---|
| `io.stat` | 按设备的 rbytes/wbytes/rios/wios … |
| `io.weight` | 相对权重（需支持的调度器/策略） |
| `io.max` | BPS/IOPS 上限 |
| `io.pressure` | PSI |
| `io.latency` / `io.cost.*` | 延迟目标、cost 模型（若启用） |

子系统：`struct cgroup_subsys io_cgrp_subsys`（`block/blk-cgroup.c:1524`），`css_alloc = blkcg_css_alloc`（`:1381`）。

```text
cgroup io 文件
  → blkcg / blkcg_gq 策略数据
  → 块层发 bio/request 时按 blkcg 记账与限速
```

**分界**：控制器如何挂上请求 → 本章；**blk-mq、电梯、驱动队列** → `block-io/` 章。

---

## 7.2 pids

| 文件 | 含义 |
|---|---|
| `pids.current` | 当前任务数（含线程） |
| `pids.max` | 硬上限；`max` = 不限 |
| `pids.peak` | 峰值 |
| `pids.events` | 超限事件 |

实现：`kernel/cgroup/pids.c`，`struct pids_cgroup`（文件内约 `:49`）。

| 符号 | 作用 |
|---|---|
| `pids_try_charge` / `pids_charge` / `pids_uncharge` | 层级加减计数 |
| `pids_can_fork`（`:278`） | fork 前占名额；失败 → fork 失败 |
| `pids_cancel_fork`（`:291`） | 回滚 |
| attach 路径 | 迁入时在新旧 pids 间转账 |

这是 **`ss->can_fork` 的教科书例子**：限额卡在创建路径，而不是创建后再杀。

---

## 7.3 freezer（v2）

v2 不靠独立 legacy freezer 子系统对外暴露老接口，而是 core 文件 + `kernel/cgroup/freezer.c`：

| 接口 | 含义 |
|---|---|
| `cgroup.freeze` | 写 `1` 冻、`0` 解冻 |
| `cgroup.events` 的 `frozen` | 是否已冻住 |

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_freeze` | `freezer.c:263` | 设置子树 freeze 状态并下推 |
| `cgroup_freeze_task` | `:152` | 对单任务施冻/解冻 |
| `cgroup_freezer_migrate_task` | `:225` | 迁移时带上冻态 |

与进程章衔接：`cgroup_post_fork` 若见 `CGRP_FREEZE`，给新生儿打 `JOBCTL_TRAP_FREEZE`（见 [04](./04-lifecycle-hooks.md)）。用户态观感类似「整组停住」，但实现走 jobctl/trap，不是简单 `SIGSTOP` 叙事能概括。

legacy：`kernel/cgroup/legacy_freezer.c`（v1）；新代码优先看 v2。

---

## 7.4 其它控制器（点名）

| 名字 | 源码线索 | 一句话 |
|---|---|---|
| `hugetlb` | `include/linux/cgroup_subsys.h` + MM hugetlb | 巨页池限额 |
| `rdma` | `kernel/cgroup/rdma.c` | RDMA 资源 |
| `misc` | `kernel/cgroup/misc.c` | 杂项可扩展计数器 |
| `dmem` | `kernel/cgroup/dmem.c`、`include/linux/cgroup_dmem.h` | 设备内存类（v7.3 已有 SUBSYS） |
| `cpuacct` | 偏 **v1** | CPU 记账；v2 多用 `cpu.stat` |
| `devices` | v1 味浓 | 设备白名单；v2 常用 eBPF/设备策略替代叙事 |
| `perf_event` | | perf 事件作用域 |
| `net_cls` / `net_prio` | | 网络类/优先级（多与 tc 协作） |

是否出现在 `cgroup.controllers`：看 Kconfig 与发行版挂载，不以本表「有源码」为准。

---

*对照：`block/blk-cgroup.c`、`kernel/cgroup/pids.c`、`freezer.c`、`cgroup_subsys.h`。*
