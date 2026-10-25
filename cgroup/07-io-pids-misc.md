# 7. io / pids / 其它控制器

**结论**：io 把「谁在发 I/O」接到 **blk-cgroup**；pids 在 fork 前占名额，占不到就让 fork 失败；freezer v2 用 core 文件 `cgroup.freeze` 冻整棵子树。其余控制器只点到源码入口。

---

## 7.1 io → blk-cgroup

子系统对象在 `block/blk-cgroup.c:1528`：

```c
struct cgroup_subsys io_cgrp_subsys = {
	.css_alloc = blkcg_css_alloc,		/* :1371 */
	.css_online = blkcg_css_online,
	.css_offline = blkcg_css_offline,
	.css_free = blkcg_css_free,
	.css_rstat_flush = blkcg_rstat_flush,	/* :1083，内部再调 __blkcg_rstat_flush */
	.dfl_cftypes = blkcg_files,		/* 只有 io.stat */
	.legacy_cftypes = blkcg_legacy_files,	/* v1 的 blkio.reset_stats */
	.legacy_name = "blkio",
	.exit = blkcg_exit,
#ifdef CONFIG_MEMCG
	/* 默认层次上若 memory 还在，就和 io 一起启用，写回能找到属主 cgroup */
	.depends_on = 1 << memory_cgrp_id,
#endif
};
```

`blkcg_css_alloc` 分配的是 `struct blkcg`，css 嵌在里面。每个「cgroup × 请求队列」再有一个 `struct blkcg_gq`（代码里常叫 blkg），策略数据挂在 blkg 上。

v2 文件不是全在 `blk-cgroup.c` 里。core 只登记 `io.stat`；权重、上限、延迟由各 blkcg policy 用 `cgroup_add_dfl_cftypes(&io_cgrp_subsys, ...)` 追加（`blk-cgroup.c:1787`）。

| 文件 | 谁登记 | 配置 | 含义 |
|---|---|---|---|
| `io.stat` | `blkcg_files[]`，`blk-cgroup.c:1211` | `CONFIG_BLK_CGROUP` | 按设备的 `rbytes` / `wbytes` / `rios` / `wios` / `dbytes` / `dios`（`:1171`） |
| `io.max` | `throtl_files[]`，`block/blk-throttle.c:1700` | `CONFIG_BLK_DEV_THROTTLING` | BPS / IOPS 上限。非 root |
| `io.low` | 同文件 `:1693` | 还要 `CONFIG_BLK_DEV_THROTTLING_LOW` | 低阈值 |
| `io.weight` | `ioc_files[]`，`block/blk-iocost.c:3532` | `CONFIG_BLK_CGROUP_IOCOST` | 相对权重。非 root |
| `io.cost.qos` / `io.cost.model` | 同文件 `:3538` / `:3544` | 同上 | 只在 root（`CFTYPE_ONLY_ON_ROOT`） |
| `io.latency` | `block/blk-iolatency.c:1060` | `CONFIG_BLK_CGROUP_IOLATENCY` | 延迟目标 |
| `io.pressure` | core，`cgroup.c:5305` | `CONFIG_PSI` | PSI，见 [08](./08-psi-rstat.md) |

BFQ 自己的 `bfq.weight` 等在 `block/bfq-cgroup.c`，挂上之后名字是 `io.bfq.*`。那是调度器策略文件，不是 `io.weight`。

读 `io.stat` 时，非 root 会先 `cgroup_rstat_flush`（`blk-cgroup.c:1202`），再按 blkg 打印。每 CPU 的增量由 `blkcg_rstat_flush` → `__blkcg_rstat_flush`（`:1007`）收上来。

```text
用户读 io.stat / 写 io.max、io.weight
  → blkcg（css 所在的 cgroup 侧对象）
  → blkcg_gq：这个 cgroup 在某块请求队列上的那一份
  → 块层提交 bio / request 时按 blkg 记账、限速
```

控制器怎样挂上请求，到此为止。请求队列、elevator、驱动，是块层自己的事。

---

## 7.2 pids

`kernel/cgroup/pids.c`。`struct pids_cgroup`（`:41`）第一成员仍是 css：

```c
struct pids_cgroup {
	struct cgroup_subsys_state css;

	/*
	 * 用 64 位，才能把「不限」表示成 PIDS_MAX = PID_MAX_LIMIT + 1。
	 * 文件里这个值打印成字符串 "max"。
	 */
	atomic64_t counter;		/* pids.current，含本节点和孩子 */
	atomic64_t limit;		/* pids.max */
	int64_t watermark;		/* pids.peak；允许轻微竞态，见 :89 的注释 */

	struct cgroup_file events_file;	/* pids.events */
	atomic64_t events_limit;	/* fork 因超限失败的次数；文件里的键是 max */
};
```

| 文件 | 行 | 含义 |
|---|---|---|
| `pids.max` | `:352` | 硬上限；`max` = 不限。非 root。层次上取最严的那个 |
| `pids.current` | `:358` | `counter`。父节点是孩子的超集（文件头注释 `:25`） |
| `pids.peak` | `:363` | `watermark` |
| `pids.events` | `:368` | 一行 `max <次数>`（`:346`） |

组织操作（把已有任务迁进来）**不会**被 pids 拦住，所以可以出现 `pids.current > pids.max`。违反策略的是 **fork**：`pids_can_fork` 失败时 fork 返回 `-EAGAIN`（文件头 `:18`）。

| 符号 | 位置 | 作用 |
|---|---|---|
| `pids_try_charge` | `:158` | 沿父链试加；任一祖先超限就失败并回滚 |
| `pids_charge` | `:138` | 强制沿父链加上（迁移回滚用） |
| `pids_uncharge` | `:121` | 沿父链减去 |
| `pids_cancel` | `:107` | 只减本节点；减成负数会 `WARN` |
| `pids_can_attach` | `:191` | 迁入前先在目标上 charge |
| `pids_cancel_attach` | `:216` | 迁入取消：目标 uncharge，源 charge 回去 |
| `pids_can_fork` | `:238` | fork 前占 1 个名额；失败则 `events_limit++` 并通知 `pids.events` |
| `pids_cancel_fork` | `:262` | fork 失败后退掉刚才占的名额 |
| `pids_release` | `:275` | `ss->release`：任务拆掉时 uncharge |
| `pids_cgrp_subsys` | `:376` | `can_fork` / `cancel_fork` / `release` / `threaded = true` |

这是 **`ss->can_fork` 的例子**：限额卡在创建路径上。名字额在 `cgroup_release` → `pids_release` 时退，不在 `cgroup_exit` 里退。

---

## 7.3 freezer（v2）

v2 不把 `freezer_cgrp_subsys` 暴露成默认层次上的控制器。那个子系统在 `kernel/cgroup/legacy_freezer.c:472`，只有 `legacy_cftypes`，所以进不了 v2 的 `cgroup.controllers`。

v2 的冻住是 core 文件 + `kernel/cgroup/freezer.c`：

| 接口 | 含义 |
|---|---|
| `cgroup.freeze` | 写 `1` 冻、写 `0` 解冻。写入口 `cgroup_freeze_write`（`cgroup.c:3943`）调用 `cgroup_freeze` |
| `cgroup.events` 的 `frozen` | `CGRP_FROZEN` 是否置位（`cgroup.c:3651`） |

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_freeze` | `freezer.c:260` | 记下 `freezer.freeze`，沿子树下推 |
| `cgroup_do_freeze` | `:177` | 给本节点置/清 `CGRP_FREEZE`，再对每个非内核线程调用 `cgroup_freeze_task` |
| `cgroup_freeze_task` | `:155` | 置或清 `JOBCTL_TRAP_FREEZE`（`include/linux/sched/jobctl.h:34`） |
| `cgroup_freezer_migrate_task` | `:222` | 迁移提交时按目标组的 `CGRP_FREEZE` 调整任务，并改两侧的 frozen 计数 |
| `cgroup_update_frozen` | `:52` | 后代和任务都到位时置 `CGRP_FROZEN` |

与进程章衔接：`cgroup_post_fork` 若见 `CGRP_FREEZE`，给新生儿打 `JOBCTL_TRAP_FREEZE`（见 [04](./04-lifecycle-hooks.md)）。用户看到的是「整组停住」。实现走的是 jobctl 陷阱：`kernel/signal.c:2782` 看到 `JOBCTL_TRAP_FREEZE` 就调用 `do_freezer_trap`（`:2592`）。这不是对每个任务发一次 `SIGSTOP` 就结束。

内核线程（`PF_KTHREAD`）在 `cgroup_do_freeze` 里被跳过。

---

## 7.4 其它控制器

登记表见 [01](./01-overview.md)。这里只补文件和一句话。

| 名字 | 源码 | v2 文件 | 一句话 |
|---|---|---|---|
| `hugetlb` | `mm/hugetlb_cgroup.c` | `<hugepage-size>.max`、`.current`、`.events` 等，由 `:725` 起 `snprintf` 拼出来 | 巨页池限额。收费入口 `hugetlb_cgroup_charge_cgroup`（`:298`） |
| `rdma` | `kernel/cgroup/rdma.c` | `rdma.max`（`:549`）、`rdma.current`（`:556`） | RDMA 资源上限和当前用量 |
| `misc` | `kernel/cgroup/misc.c` | `misc.max`、`misc.current`、`misc.capacity`（仅 root）、`misc.events`（`:349`） | 杂项计数。v6.6.160 里资源名只有打开 `CONFIG_KVM_AMD_SEV` 时的 `sev` / `sev_es`（`:21`） |
| `cpuacct` | `kernel/sched/cpuacct.c:358` | 无 v2 文件 | v1 CPU 记账。v2 看 `cpu.stat` |
| `devices` | `security/device_cgroup.c:815` | 无 v2 文件 | v1 设备白名单 |
| `perf_event` | `kernel/events/core.c:14018` | 不出现在 `cgroup.controllers` | `implicit_on_dfl = true`，默认层次上总是开着，给 perf 按 cgroup 过滤 |
| `net_cls` | `net/core/netclassid_cgroup.c:140` | 无 v2 文件 | v1 `net_cls.classid` |
| `net_prio` | `net/core/netprio_cgroup.c:256` | 无 v2 文件 | v1 `net_prio.prioidx` / `ifpriomap` |

`misc_cgrp_subsys`（`misc.c:417`）只有 `css_alloc`、`css_free` 和两张相同的 cftype。收费给别的子系统调用 `misc_cg_try_charge`（`:140`）。

一个名字在不在 `cgroup.controllers` 里，看 Kconfig、它有没有 `dfl_cftypes`、以及有没有被绑到 v1 树。不要用「源码里有这个文件」代替挂载结果。

---

*对照：`block/blk-cgroup.c`、`kernel/cgroup/pids.c`、`freezer.c`、`legacy_freezer.c`、`cgroup_subsys.h`。*
