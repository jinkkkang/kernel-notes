# 真机补测清单（REAL-MACHINE-TEST-BACKLOG）

> 给后续 **code agent / 真机实践** 用的工作单：列出仓库里仍标 RECIPE、未贴实测、或 demos 未跑通的条目，以及**补完后应改哪些文件**。
>
> - 源码钉：**Linux v6.6.160**（结构体 / 调用链 / 行号只对这个版本）。
> - 本机数字标：**sampled on <uname -r>**（现有多为 6.12.x；cgroup 写操作 demos 曾标 6.8）。**禁止**把采样内核的行号写进源码链。
> - 约定：能跑出输出 → 把 stdout / 关键数字贴进对应节，并把该条从「RECIPE」改成「实测」；跑不了 → 保留 RECIPE，写明缺什么（工具 / CAP / 硬件）。
> - **不要**在生产机、共享机上改 IRQ 亲和、换 elevator、`pivot_root`、打满内存、长时间 `perf sched record -a`，也不要为了补测去打开调试分配器。
> - 本清单是 backlog，不是教程正文；正文仍在各章 `*-observability.md` / `*-perf-analysis.md` / `demos/`。
>
> 对照记录（2026-11-09，对应当时的 `main`）：下面各 ID 的路径都在树里。节号按当前标题改过（见各行「落到哪里」）。仓库里没有命令面的 `IRQ-KVM` 标成 `blocked`。章 README / demos README 里草案漏掉的条目补了 ID：`LDS-WAIT-KPROBE`、`SCH-NUMA`、`MM-KMEMLEAK`。没有新编 RECIPE 编号。

**怎么用（给 code agent）**

1. 按「优先级」或按章领取一条 `ID`。
2. 读「落到哪里」里的文件，确认现有 RECIPE / 空栏文案。节号以文件里的标题为准。
3. 在具备前提的机器上执行「建议动作」；区分 MEASURED vs 仍 RECIPE。
4. 更新正文：贴输出摘录、标 `sampled on …`、改 README「实测 vs RECIPE」小节；若 demos 脚本有开关（如 `TRACE_RUN_GRAPH=1`、`CG_WRITE=1`、`CG_DEMO_WRITE=1`），在 demos README 记一次成功跑法。
5. 回写本文件：该 `ID` 状态改为 `done`，填「完成记录」一行（日期、内核、PR）。跑不了则改为 `blocked`，写清 blocker。
6. 一次 PR 可覆盖多条，但提交说明里列 `ID` 列表。

**状态取值**：`open` | `blocked`（写清 blocker）| `done`

---

## 0. 全局前提（跨章）

| 类别 | 常见需求 |
|------|----------|
| 权限 | root，或 `CAP_SYS_ADMIN`；网络实验另需 `CAP_NET_ADMIN` |
| 工具 | `bpftrace`、`bpftool`、`perf`、`trace-cmd`、`strace`、`ip`/`ethtool`/`ss`/`tc`、`blktrace`/`blkparse`/`iostat`、可选 `systemd-cgls`/`cgtop` |
| 环境 | cgroup v2；可写委托目录；块设备；多队列 virtio 或物理 NIC；宿主机可看 `vhost` 时才能填 networking V2/V4 的宿主机一侧；`/dev/kvm` 才做 MM/KVM；out-of-tree kmod 要 `/lib/modules/$(uname -r)/build` |
| 版本 | 数字旁永远写 `sampled on …`；与 v6.6.160 源码引用分开 |

---

## 1. 优先级总表（先做这些）

| 优先级 | ID | 一句话 |
|--------|-----|--------|
| P0 | NET-V1 … NET-V5 | networking 云上 MQ / vhost / RPS 五栏（实测栏仍是「待补」） |
| P0 | EBPF-XDP-VETH | XDP 挂到 veth（非 `BPF_PROG_TEST_RUN`） |
| P0 | EBPF-TP-BT + EBPF-BPFTOOL | sched_switch bpftrace + bpftool 枚举 |
| P0 | TR-GRAPH | tracing `04-function-graph.sh` / RECIPE-G1 |
| P1 | SCH-BT + SCH-PERF | scheduler 两个 `.bt` + `perf sched` |
| P1 | MM-KVM-BALLOON | 嵌套 KVM / EPT + balloon 操作 |
| P1 | BIO-BLKTRACE | block-io blktrace 或 `block_rq_*` + 换 scheduler |
| P1 | IRQ-STORM | interrupts softirq / 亲和 / `ethtool -L` |
| P2 | SYS-TRACE | syscalls strace / perf trace / raw_syscalls |
| P2 | CTR-PIVOT | containerization pivot_root + netns |
| P2 | LDS-FUTEX-KMOD | locks `futex_enter.bt` + 三件 kmod + §8.7 wait kprobe |
| P2 | SCH-NUMA | 多节点才做 NUMA balancing；单节点标 blocked |
| P2 | MM-KMEMLEAK | kmemleak；未编 `CONFIG_DEBUG_KMEMLEAK` 则 blocked |
| P2 | 其余各章 observability 尾巴 | 见分章。`IRQ-KVM` 已 blocked，不要领 |

---

## 2. 分章明细

### 2.1 process-management/

**已有**：无 `demos/`。`10-observability.md` §10.6 是只读 `/proc` 采样（sleep/tini/node 等，sampled on 6.12）。章 README 没有「实测 vs RECIPE」小节，补测只改 `10-observability.md`。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| PM-BPF-FORK | open | bpftrace 看 `sched_process_fork` / `sched_process_exit` | `process-management/10-observability.md` §10.2 | 按节内已有 `bpftrace -e` 触发 `clone`/`exit`，贴 10～20 行。字段名以 `bpftrace -lv` 为准 | 节内有 **实测** 摘录 + `sampled on`；不再只写 RECIPE |
| PM-PERF-FG | open | `function_graph` 或 `perf probe` 看 `kernel_clone` | 同文件 §10.3 | 按节内命令：`set_graph_function` 收成 `kernel_clone` 再切 `function_graph`，或 `perf probe -a kernel_clone` 后短时 `perf record`。做完把 `current_tracer` 设回 `nop`。不要改成无过滤的 `perf record -g` | 有 graph 片段或 probe 计数，并注明开销；tracer 已恢复 |
| PM-SIGNAL | open | `/proc/<pid>/status` 的 `SigQ` / `SigPnd` / `ShdPnd` / `SigBlk` / `SigIgn` / `SigCgt` | 同文件 §10.4 | 按节内 RECIPE 对一个活着的 pid 读掩码并贴数 | 实测替换 RECIPE |

---

### 2.2 locks-and-ds/

**已有**：用户态 `01`–`04` 作者机已跑过（2026-10-22 的样本），`05_pthread_mutex_futex` 标明 sampled on 6.12。不要把前四份改写成「全是 6.12」。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| LDS-FUTEX-BT | open | 跑通 `futex_enter.bt` | `locks-and-ds/demos/futex_enter.bt`；`locks-and-ds/09-futex.md`；`locks-and-ds/demos/README.md` | 安装 bpftrace；用 `locks-and-ds/demos/05_pthread_mutex_futex` 触发 futex；贴按 `args->op` 的计数 | demos README 标 MEASURED；`09-futex.md` 改实测 |
| LDS-WAIT-KPROBE | open | `kprobe:__wake_up` / `kprobe:wait_for_completion` | `locks-and-ds/08-wait-queue-and-completion.md` §8.7 | 按节内已注释的两条 bpftrace 短跑。需要能挂 kprobe。不要和下面的 kmod `insmod` 混成一次 | 节内有实测摘录，或写明缺 bpftrace / 权限 |
| LDS-KMOD-LIST | open | out-of-tree `kmod_list_example` 加载 | `locks-and-ds/demos/kmod_list_example.c`、`locks-and-ds/demos/Makefile.example`；`locks-and-ds/02-list.md` | **非生产**机：`make -f Makefile.example` → `insmod` → 读 dmesg → `rmmod`。注明 **未** 在 v6.6.160 模块树内，宿主为 sampled 内核 | 贴 dmesg / 成功加载记录；强调勿生产 |
| LDS-KMOD-IDR | open | `kmod_idr_example` | `locks-and-ds/demos/kmod_idr_example.c`、同一份 `locks-and-ds/demos/Makefile.example`；`locks-and-ds/03-trees-maps.md` | 同 LDS-KMOD-LIST | 同上 |
| LDS-KMOD-COMPLETION | open | `kmod_completion_timeout` | `locks-and-ds/demos/kmod_completion_timeout.c`、同一份 `Makefile.example`；`locks-and-ds/08-wait-queue-and-completion.md`（模块段，在 §8.7 的 kprobe 之后） | 同 LDS-KMOD-LIST。期望第二次空等打印 `left=0` | 同上 |
| LDS-LOCK-STAT | open | `/proc/lock_stat` 与 perf/bpftrace 锁点名 | `locks-and-ds/08-observability.md` §8.2、§8.3 | 确认 `lock_stat` 开启；采一段争用；可选 `perf lock`。文件不存在就写 blocker（未开 `CONFIG_LOCK_STAT`） | 有数字与读法；无则写 blocker |

---

### 2.3 cgroup/

**已有**：`02_tree_walk` / `03_stat_vs_meminfo` 只读。`04_memory_pids_limit` / `05_cpu_throttle` 在 `CG_WRITE=1` 下测过，stdout 标 sampled on 6.8。`09-observability.md` §9.3 另有 sampled on 6.12 的只读树。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| CG-OBS-RO | open | §9.2 只读体检与 service 叶子 | `cgroup/09-observability.md` §9.2。§9.3 已是 sampled on 6.12，不要改成「没采过」 | `cat` 关键 controllers、跟一个 user service 叶子；有 `systemd-cgls` / `cgtop` 就贴一小段 | 贴本机树片段 + sampled on |
| CG-WRITE-RERUN | open | 在 **当前** 宿主内核重跑写限额，并补 `01` 的写路径 | `cgroup/demos/README.md`、`cgroup/README.md` 的 demo 段；`cgroup/demos/01_cg_smoke.sh`、`04_memory_pids_limit.sh`、`05_cpu_throttle.sh` | `04`/`05` 用 `CG_WRITE=1`，只碰新建的 `cg-demo-*`。`01` 的写开关是 **`CG_DEMO_WRITE=1`**，还要 `CG_DEMO_ROOT` 指向可写目录，不要套用 `CG_WRITE`。demos README 贴出的 stdout 只有 04/05；章 README 写「全部实测过」，但 `01` 的写路径没有贴输出。重跑后把 6.8 改成当次 `uname -r` | README 输出与内核版本一致；`01` 要么有写路径摘录，要么写明仍是 DRY-RUN |

---

### 2.4 scheduler/

**已有**：`box-sched-obs.txt` 是 MEASURED（sampled on 6.12.94+）。demos README 写明这份样本是 **改 `capture-sched.sh` 之前** 抓的。`08-observability` 有 `/proc` / PSI 样本。两个 `.bt` 未跑。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| SCH-WAKE-BT | open | `wakeup_latency.bt` | `scheduler/demos/wakeup_latency.bt`；`scheduler/09-perf-analysis.md` §9.4 | bpftrace 短跑；贴直方图 | demos README MEASURED |
| SCH-SWITCH-BT | open | `sched_switch_hist.bt` | `scheduler/demos/sched_switch_hist.bt` | 同上。tracing 的 RECIPE-P3 指向这一支，做完顺手对一下 TR-BPF-P3 | 同上 |
| SCH-PERF-SCHED | open | `perf sched` / `perf trace` 与场景 A–D | `scheduler/09-perf-analysis.md` §9.2、§9.3、§9.6 | **短时** record。§9.6 的 A 尾延迟、B 单核、C 迁移、D cgroup 节流各留一组数。禁止长时间 `perf sched record -a` | 各场景有实测或明确 blocked |
| SCH-OBS-REFRESH | open | 用改过的脚本重抓 08 的本机数字 | `scheduler/08-observability.md`；`scheduler/demos/capture-sched.sh`；`scheduler/demos/box-sched-obs.txt` | 重跑 capture 脚本。旧 txt 不要当成与当前脚本逐行一致 | 版本标签更新，并写明这是脚本更新后的样本 |
| SCH-NUMA | open | 多节点上打开 NUMA balancing 后看 hint / 迁页 | `scheduler/05b-numa-balancing.md` §5b.9 | 只读 sysctl / vmstat / `numa_maps` **已经**有 sampled on 6.12.94+：单节点，`online` 为 `0`，hint 五项为 0。本 ID 只在多于一个 online 节点的机器上做：按节内 RECIPE 打开 `kernel.numa_balancing`，跑一段匿名内存负载，再读 `numa_pte_updates`、`numa_hint_faults`、`numa_pages_migrated` 和 `numa_maps` 的 `N*`。单节点把本 ID 标 blocked | 多节点有前后计数，或 blocked（单节点）。不要把已有的 0 再贴一遍 |

---

### 2.5 memory-management/

**已有**：`01_maps_smaps.c` / `02_fault.c` 编过跑过。meminfo / vmstat / buddy 等只读在 `13-observability.md`。`/proc/pagetypeinfo` 与 `/proc/slabinfo` 当时 Permission denied。章 README 的 RECIPE 名单是 perf、bpftrace、KVM nested、balloon 操作、kmemleak。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| MM-SLAB-KMOD | open | `kmod_slab_cache.c`（文档写不 insmod） | `memory-management/demos/kmod_slab_cache.c`、`memory-management/04-slab.md` §4.6、`memory-management/14-perf-and-dev-practice.md` §14.1 | 非生产加载一次，或维持 RECIPE 并写清原因（没有对应内核构建树） | 要么 MEASURED，要么 blocked+理由 |
| MM-RECLAIM-TRACE | open | ftrace/bpftrace 直接回收路径 | `memory-management/14-perf-and-dev-practice.md` §14.2（`memory-management/13-observability.md` 只列事件名，命令在 14） | 轻压 + 过滤相关函数；勿打满内存 | 有短 trace 摘录 |
| MM-PERF-MEM | open | `perf mem` / 抽样 | `memory-management/14-perf-and-dev-practice.md` §14.2 | 短 workload | 有读法与数字 |
| MM-OOM-THP | open | dmesg OOM；THP sysfs 与 §14.2 的 vm sysctl | `memory-management/14-perf-and-dev-practice.md` §14.2、§14.3、§14.4 | 优先只读。§14.4 已记下当次 6.12 样本的 `thp_*` 全 0，没有毛刺可编。若改 `enabled` / `defrag` 或 vm sysctl，必须还原 | 记录前后值；无 THP 活动就写明计数仍为 0 |
| MM-KMEMLEAK | open | `/sys/kernel/debug/kmemleak` | `memory-management/14-perf-and-dev-practice.md` §14.1；`memory-management/README.md`「实测 vs RECIPE」 | 先看 debugfs 里有没有 `kmemleak`。没有就是没编 `CONFIG_DEBUG_KMEMLEAK`，标 blocked。有的话按节内注释做一次 `scan` 再读。不要在共享机上为了本 ID 打开调试分配器 | 有一段报告摘录，或 blocked（未编） |
| MM-BALLOON | open | virtio-balloon 的 inflate/deflate **操作** | `memory-management/12-balloon-fpr-and-overcommit.md` §12.8；`memory-management/14-perf-and-dev-practice.md` §14.5 | `balloon_inflate` / `balloon_deflate` 的 vmstat 累计已经在 §14.5 贴过（sampled on 6.12）。本 ID 补的是 qemu 监视器 `info balloon` 和一次可逆的 inflate/deflate。没有 balloon 设备就 blocked | 操作前后的 `/proc` 变化，或 blocked（无 balloon） |
| MM-KVM-EPT | open | 嵌套 KVM / EPT 大页 | `memory-management/11-kvm-memory.md` §11.6（EPT 大页）、§11.7（嵌套） | 需 `/dev/kvm` + 嵌套。无则 blocked。看 `trace_kvm_page_fault`，并确认 L2 的 `mmu->page_fault` 不是 `kvm_tdp_page_fault` | 有 EPT/大页相关读数或明确无法做 |

---

### 2.6 interrupts/

**已有**：无 demos。`12-observability.md` 与 `13-perf-analysis.md` 的命令默认 RECIPE。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| IRQ-DEBUGFS | open | debugfs `irq/domains` / `irqs` | `interrupts/12-observability.md` §12.4 | 挂上 debugfs；贴 `domains/VECTOR` 与一条 `irqs/<n>`。需要 `CONFIG_GENERIC_IRQ_DEBUGFS` | 实测；没有目录就 blocked |
| IRQ-PERF-HANDLER | open | `perf` `irq:irq_handler_*` | `interrupts/12-observability.md` §12.5；`interrupts/13-perf-analysis.md` §13.2 | 短时采样（节内是 `perf record -e irq:irq_handler_entry,irq:irq_handler_exit -a -- sleep 2`） | 有事件计数读法 |
| IRQ-SOFTIRQ | open | softirq / `ksoftirqd` 饱和观察 | `interrupts/13-perf-analysis.md` §13.3 | 结合 `/proc/softirqs`、可选 trace。**勿**乱改生产亲和 | 一组前后对比 |
| IRQ-AFFINITY-ETHTOOL | open | 非托管 IRQ `smp_affinity_list` + `ethtool -L` | `interrupts/13-perf-analysis.md` §13.4 | 仅实验网卡/队列；改前备份；改后还原 | 文档写清设备名与还原步骤 + 数字 |
| IRQ-KVM | blocked | （无）`11-kvm-irq-virt.md` 没有操作面 | `interrupts/11-kvm-irq-virt.md` | 不跑 | blocker：该文件没有 RECIPE、没有 shell、没有「操作面」。§11.6 只是宿主机与客户机的边界表。不要为了本 ID 新写观测步骤，也不要硬编数字 |

---

### 2.7 syscalls/

**已有**：`syscalls/demos/01-vdso-vs-syscall.c` / `02-bad-pointer-efault.c` / `03-restart-eintr.c` + maps 采样（sampled on 6.12.113+）。章 README 点名的 RECIPE 是 `strace -c`、`perf trace`、`raw_syscalls:sys_enter`、PTI 开/关。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| SYS-STRACE | open | `strace -c` 对照；顺手可补 §10.2 的 `ausyscall` | `syscalls/10-observability.md` §10.2、`syscalls/11-perf-analysis.md` §11.3 | 对已知 demo 或 `getpid` 循环。`ausyscall` 只在装了该工具时贴一张号对照，没有就在 §10.2 保持 RECIPE | 贴摘要表 |
| SYS-PERF-TRACE | open | `perf trace` | `syscalls/11-perf-analysis.md` §11.3 | 短时 | 实测 |
| SYS-RAW-BPF | open | bpftrace `raw_syscalls:sys_enter` | `syscalls/10-observability.md` §10.4、`syscalls/11-perf-analysis.md` §11.3；`syscalls/demos/README.md` | 过滤 pid；短跑 | 实测 |
| SYS-VDSO-UPROBE | open | uprobe `__vdso_clock_gettime` | `syscalls/11-perf-analysis.md` §11.3 | 按节内 RECIPE。用户态计时已经在 §11.2 测过，本 ID 只补 uprobe | 实测或 blocked |
| SYS-PTI | open | PTI 开/关对比 | `syscalls/11-perf-analysis.md` §11.4 | 仅当 CPU 受 Meltdown 影响且可安全对比。笔记作者机的 `meltdown` 是 **Not affected**，这种机器标 blocked，不要编一份 PTI 成本 | 有对比，或注明 Not affected 无法填数 |

---

### 2.8 vfs/

**已有**：`01_splice_sendfile_copy.c` 在 `/dev/shm` 上跑过。`12-observability.md` 的 `/proc/sys/fs` 只读已采样（sampled on 6.12.94+）。`/proc/slabinfo` 与 tracefs 当时 Permission denied。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| VFS-SPLICE-BT | open | `splice_kprobe.bt`（`kprobe:do_splice`） | `vfs/demos/splice_kprobe.bt`；`vfs/05b-splice-and-zerocopy.md`；`vfs/13-perf-analysis.md`；`vfs/demos/README.md` | 跑 splice demo 的同时 bpftrace | MEASURED |
| VFS-PATH-DCACHE | open | 路径查找 / dcache 配方 | `vfs/13-perf-analysis.md` §13.1、§13.2 | perf/bpftrace/ftrace 按节选一条跑通 | 实测 |
| VFS-FSYNC-DIO | open | `vfs_fsync` / DIO vs 缓冲 | `vfs/13-perf-analysis.md` §13.3、§13.4 | 在 `/dev/shm` 或测试文件；勿生产盘高压 | 实测 |

---

### 2.9 ipc/

**已有**：epoll LT/ET、pipe、eventfd demos 的 stdout 已贴（sampled on 6.12.113+）。`09-observability.md` 的 epoll/pipe sysctl 已采样。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| IPC-PERF-STAT | open | `perf stat` 对着 demos | `ipc/10-perf-analysis.md` §10.5 | 对 `ipc/demos/01-epoll-lt-vs-et` 跑节内那条 `perf stat`。同节末尾的「自写环回间隔」没有现成脚本，不要另起压测；两条已写明的命令跑通即可 | 有计数 |
| IPC-EP-CB-BT | open | `bpftrace` `kprobe:ep_poll_callback` | `ipc/10-perf-analysis.md` §10.5 | **短时**、低连接，对着 `ipc/demos/02-pipe-wakeup`。注明探针成本 | 有计数 + 警告保留 |

---

### 2.10 networking/（P0）

**已有**：`networking/demos/01-sock-stats.c` / `02-unix-scm-fd.c` / `03-veth-ping.sh`。`networking/16-observability.md` §16.3 有单队列 virtio（`enp0s3`，Combined 1）的读法，那是 sampled on 6.12 的已有数字，**不能**拿去填 RECIPE-V 的「实测（待补）」。V1..V5 的空栏分布如下，改的时候每处栏都要碰到：

| ID | 命令写在哪 | 只是指向、没有自己的命令 |
|----|------------|--------------------------|
| V1 | `networking/15-mq-rps-xps-affinity.md` §15.1，`networking/16-observability.md` §16.3 | `networking/17-perf-analysis.md` §17.5 |
| V2 | `15` §15.2，`16` §16.4 | `17` §17.7；`networking/14-virtio-net-host-vhost.md` 只点到 16 |
| V3 | `15` §15.4 | `17` §17.5。`16` **没有** RECIPE-V3 |
| V4 | `16` §16.5，`17` §17.7 | — |
| V5 | `15` §15.3 | `17` §17.1。`16` **没有** RECIPE-V5 |

上表里没写目录的 `15` / `16` / `17` 都在 `networking/` 下，文件名是 `15-mq-rps-xps-affinity.md`、`16-observability.md`、`17-perf-analysis.md`。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| NET-V1 | open | `ethtool -l/-k/-S` + `nproc` 基线 | 上表的 V1 三处，外加 `networking/README.md` 的 RECIPE-V 段 | 在 **多队列** guest 或物理 NIC 上采集。单队列机器保持「待补」，并写明 Combined 仍为 1 | 三处「实测（待补）」换成带 sampled on 的摘录，或写明仍是单队列 |
| NET-V2 | open | 宿主机 `vhost` 线程 + virtio IRQ 亲和 | 上表的 V2。数字写进 `15` §15.2 和 `16` §16.4，`17` 的栏改成指向这次实测 | **要宿主机权限**。列 vhost 线程的 `psr`、客户机 input IRQ。改亲和前备份，测完还原 | V2 实测或 blocked（无宿主机） |
| NET-V3 | open | `ethtool -L combined N` + 绑 IRQ | `15` §15.4 与 `17` §17.5。不要在 `16` 新开一节 V3 | 实验 NIC；改前备份，改后还原；贴前后 softnet / IRQ / 每队列 kicks | 两处 V3 栏有实测 |
| NET-V4 | open | guest 侧 `netif_receive_skb` 与 host 侧 `handle_rx` / `handle_tx` | `16` §16.5（`perf stat` + `trace-cmd`，事件是 `net:netif_receive_skb` 与 `net:net_dev_start_xmit`）；`17` §17.7（guest 抓 `netif_receive_skb`，宿主机数 `handle_rx` / `handle_tx`） | 短 trace。有宿主机时两侧时间窗对齐；只有客户机时只填 16 的栏，17 标 blocked | 两处栏都有交代 |
| NET-V5 | open | 改 `rps_cpus` 前后 `softnet_stat` | `15` §15.3 与 `17` §17.1。不要在 `16` 新开一节 V5 | 备份 mask；改 → 一小股 UDP → 还原；贴 `/proc/net/softnet_stat` | 两处 V5 栏有前后对比 |
| NET-QDISC-REXMIT | open | qdisc 积压 / TCP 重传补数 | `networking/17-perf-analysis.md` §17.2、§17.3。这两节 **没有** RECIPE-V 编号，也 **没有**「实测（待补）」空栏 | 按节内已写的「看什么」选可逆、非生产场景采一组数 | 至少一则贴进对应节。不要新编 RECIPE-V6 |

---

### 2.11 ebpf/（P0）

**已有**：`demos/` 下 `04-map-user`、`03-xdp-testrun`（`BPF_PROG_TEST_RUN`，**未挂网卡**）、`01-kprobe-count` 已用 root 跑过，stdout 在 demos README（sampled on 6.12.113+）。`02-tp-sched-switch.bt` 未跑。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| EBPF-TP-BT | open | `02-tp-sched-switch.bt` | `ebpf/demos/02-tp-sched-switch.bt`、`ebpf/demos/README.md`、`ebpf/13-perf-analysis.md` §13.5 | 装 bpftrace；短跑；贴按 `prev_state` 的计数 | MEASURED |
| EBPF-BPFTOOL | open | `bpftool prog/map/link` 枚举 | `ebpf/12-observability.md` §12.4、`ebpf/11-tooling.md`、`ebpf/13-perf-analysis.md` | 在有加载 prog 时 `bpftool prog show` 等。非 root 列 `/sys/fs/bpf` 会 Permission denied，那次不算本 ID | 贴命令+输出 |
| EBPF-XDP-VETH | open | **`ip link … xdpgeneric`（或 drv）挂 veth**，对端发包验 map | 命令在 `ebpf/13-perf-analysis.md` §13.3 和 `ebpf/demos/README.md`。`ebpf/07-xdp.md` 只说明 veth 可以挂 `xdp_prog`，**没有** `ip link` 步骤，不要在 07 另写一套 | 按 §13.3 建 veth 对；`xdpgeneric` 挂上；对端发包；读 map；`xdpgeneric off` 并删掉 veth。`ebpf/demos/03-xdp-testrun` 的 `retval=1` 是 test_run，不能充当本 ID | 明确「非 test_run」实测；记录 generic vs drv |
| EBPF-PROG-PROFILE | open | `bpftool prog profile` / perf 采 `bpf_prog_run` | `ebpf/13-perf-analysis.md` §13.5（`run_time_ns` 的读法在 §13.4） | 对已挂的 XDP 或 kprobe prog。本机 `bpf_stats_enabled` 为 0 时 fdinfo 的 `run_time_ns` 一直是 0，不要拿它当耗时 | 有开销数字 |

---

### 2.12 tracing/（P0/P1）

**已有**：instance `kn-demo` 上 `01-function-filter` / `02-sched-switch-event` / `03-kprobe-events` 已跑（sampled on 6.12.113+）。`04-function-graph.sh` 默认直接退出。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| TR-GRAPH | open | `04-function-graph.sh`（`TRACE_RUN_GRAPH=1`），对应 RECIPE-G1 | `tracing/demos/04-function-graph.sh`、`tracing/demos/README.md`、`tracing/12-perf-analysis.md` §12.2 | 在 `tracing/demos/` 下 `sudo TRACE_RUN_GRAPH=1 ./04-function-graph.sh`。脚本只跟踪 `do_sys_openat2`，结束时把顶层 tracer 设回 `nop` 并清空 `set_graph_function`。不设变量的那次输出只是 RECIPE 提示，不能标 MEASURED | MEASURED，或仍 RECIPE 并写 blocker |
| TR-PERF-P1 | open | RECIPE-P1：`perf stat` 对 `sched:sched_switch` | `tracing/12-perf-analysis.md` §12.6 | 短时。期望 ftrace 环的 `entries` 不因为这次 perf 而涨 | 实测 |
| TR-IRQSOFF-P2 | open | RECIPE-P2：irqsoff / preemptoff | `tracing/12-perf-analysis.md` §12.6；`tracing/README.md`「实测 vs RECIPE」 | 先看 `available_tracers`。章 README 记下的 6.12.113+ 样本是 `function_graph function nop`，没有这两项。那种机器把本 ID 标 **blocked**。列表里真有 `irqsoff` 时才短开、读 `tracing_max_latency`、设回 `nop` | 不编造有 tracer |
| TR-BPF-P3 | open | RECIPE-P3：指回 scheduler 的 `sched_switch_hist.bt` | `tracing/12-perf-analysis.md` §12.6；脚本是 `scheduler/demos/sched_switch_hist.bt` | 与 **SCH-SWITCH-BT** 一起更新链接与状态。`wakeup_latency.bt` 是 SCH-WAKE-BT，不是 P3 | 交叉引用一致 |

---

### 2.13 block-io/（P1）

**已有**：**无 demos**。章 README 写明不编 `/sys/block/*/stat` 或 `iostat` 的数字。命令默认 RECIPE。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| BIO-SYSFS-IOSTAT | open | `/sys/block/*/stat`、scheduler、`iostat -xz` | `block-io/16-observability.md` §16.2、§16.3 | 选测试盘（非生产关键盘）。列名以 v6.6.160 的 `part_stat_show` 为准，6.12 的数字旁写 sampled on | 贴 stat/iostat |
| BIO-BLKTRACE | open | `blktrace`/`blkparse` 或 `perf` `block_*` | `block-io/16-observability.md` §16.4；`block-io/17-perf-analysis.md` §17.3 | 短时；注意开销与磁盘 | 一段 parse 摘录 |
| BIO-SCHED-SWEEP | open | 换 `none` / mq-deadline / kyber / bfq 对照 | `block-io/04-sched-none.md` … `block-io/07-sched-bfq.md`；`block-io/17-perf-analysis.md` §17.4 | **会冻队列**。维护窗口。改前读当前 elevator，改后还原 | 每调度器一组延迟/深度数 |
| BIO-URING | open | io_uring fdinfo / `io_uring:*` | `block-io/16-observability.md` §16.5；`block-io/17-perf-analysis.md` §17.5；`block-io/12-io-uring.md` | 小 ring 自测或现成工具。6.6 的实现在 `io_uring/`，不要按旧的 `fs/io_uring.c` 找符号 | 实测 |
| BIO-MQ-CPU | open | mq `nr_requests` / cpu_list | `block-io/08-multiqueue-affinity.md`；`block-io/17-perf-analysis.md` §17.2、§17.6 | 只读为主；慎写 | 读数进文 |

---

### 2.14 containerization/（P2）

**已有**：无 demos。`11-observability.md` §11.1–§11.4 已有 sampled on 6.12 的 `lsns`、一次 `unshare --user --map-root-user`、cgroup、overlay 参数、fusectl。那些不要重做。

| ID | 状态 | 补什么 | 落到哪里 | 建议动作 | 验收 |
|----|------|--------|----------|----------|------|
| CTR-UNSHARE | open | `unshare --mount --pid --net` | `containerization/11-observability.md` §11.6 第一条 | 一次性实验 ns；退出即毁。这不是 §11.2 已经测过的 `--user --map-root-user` | 贴 `lsns` 或孩子里的 `pid` / 网卡表 |
| CTR-PIVOT | open | `pivot_root` 最小演练 | `containerization/11-observability.md` §11.6 第二条；`containerization/03-mount-ns-pivot-root.md`；`containerization/12-perf-and-pitfalls.md` §12.5 | **禁止共享机**。用临时 rootfs。父挂载仍是 shared 时内核返回 `-EINVAL` | 成功步骤 + 警告 |
| CTR-NETNS-MOVE | open | `ip link set … netns` | `containerization/11-observability.md` §11.6 第三条；`containerization/05-net-ns.md` | 自建 veth；还原。没有 `ip` 就 blocked | 实测或 blocked |
| CTR-OVL-FUSE | open | `perf probe -a ovl_copy_up_file`；fusectl `waiting` | `containerization/12-perf-and-pitfalls.md` §12.1、§12.4 | overlay 上触发一次写打开的 copy-up。有 FUSE 连接再读 `/sys/fs/fuse/connections/<id>/waiting`。§11.4 的只读参数不要重贴成新实测 | 实测或 blocked |

---

## 3. 完成后如何改仓库（统一模板）

对每个 `done` 的 ID，code agent 应至少：

1. **正文**：RECIPE → 实测；插入 fenced 输出（可截断）；`sampled on <ver>`；保留「如何复现」命令。同一 ID 在多处有「实测（待补）」时，每处都改，或改成指向已贴数字的那一节。
2. **章 README**：更新「实测 vs RECIPE」段落。该章没有这一小节时（process-management、locks-and-ds、cgroup、scheduler、interrupts），只改正文和 demos README，不要新开一节大纲。
3. **demos/README**（若有）：脚本名旁标 MEASURED / RECIPE，并写所需 env（`CG_WRITE`、`CG_DEMO_WRITE`、`TRACE_RUN_GRAPH`、root）。
4. **本文件**：状态 `done` + 完成记录。
5. **不要**：为了补测去改 v6.6.160 源码行号；不要删 RECIPE 标题却不留复现命令；不要提交内核树；不要新编 RECIPE-V6 或别的编号。

### 完成记录（追加）

| 日期 | ID | 内核（sampled on） | PR / commit | 备注 |
|------|-----|-------------------|-------------|------|
| （空） |  |  |  |  |

---

## 4. 维护

- 新章若引入 RECIPE，在本文件对应节加 `ID`。
- 某条永久不可做（无硬件、无对应文件）：状态 `blocked`，正文保留已有 RECIPE 并写 blocker。`IRQ-KVM` 属于「文件在，但没有可填的命令」。
- 与大纲冲突时以各章 README「边界」为准；本清单不授权改厚薄结构。

*生成说明：条目来自 2026-11-09 对 `main`（含 tracing）全库 RECIPE/demos 审计，并在落地时按当前树核对过路径与节号。*
