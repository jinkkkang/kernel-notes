# kernel-notes

对着 Linux 内核真实源码整理的子系统笔记。内核版本：**Linux 6.6 LTS（v6.6.160）**。结构体、函数、行号都对照这个版本；本机观测数字采自 6.12，文中会标明 sampled on 6.12。

源码不放在本仓库，请自行对照：

- [elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
- [git.kernel.org stable](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git)

## 目录

| 笔记 | 说明 |
|------|------|
| [process-management/](./process-management/) | **进程管理（大章）**：总览与 `CLONE_*`、`task_struct`、PID/pidns、fork/clone、内核线程、exec、exit/回收、上下文切换入口、信号、观测清单。调度选人算法（EEVDF）见 scheduler。 |
| [locks-and-ds/](./locks-and-ds/) | **同步原语与内核数据结构（大章）**：读内核够用的 C、`list_head`/hlist、树与映射容器、atomic/refcount、spinlock、mutex/rwsem/seqlock、RCU、lockdep 与踩坑。面向 C 新手补嵌入式链表与宏。 |
| [cgroup/](./cgroup/) | **控制组（大章）**：cgroup v2 core 与对象模型、层次接口、fork/exit 挂钩与 cgroup ns、cpu/cpuset、memory、io/pids/freezer、PSI/rstat、本机观测。完整调度/MM/块层另章。 |
| [scheduler/](./scheduler/) | **调度（大章）**：调度类链、`rq`/`cfs_rq`/`sched_entity`、EEVDF、入队与唤醒、负载均衡、cgroup cpu 衔接、RT/DL 边界、观测与性能分析配方。 |
| [memory-management/](./memory-management/) | **内存管理（大章）**：物理页与 zone、buddy、SLUB、vmalloc、mm/VMA 与页表、缺页、页缓存、THP/hugetlb、回收与 OOM、KVM 二维页表与 mmu_notifier、balloon/FPR/热插拔、本机 `/proc` 观测与症状剧本。cgroup charge 与完整 VFS 另章。 |
| [interrupts/](./interrupts/) | **中断（大章）**：x86-64 IDT 入口、APIC/MSI 向量、genirq 对象与层次域、向量矩阵、注册与流控、分配失败、softirq/tasklet、工作队列与 irq_work、定时器、KVM irqfd/posted interrupt、观测与性能配方。 |
| [vfs/](./vfs/) | **虚拟文件系统（大章）**：`dentry` / `inode` / `file`、路径查找、fd 与 open/close、读写与页缓存交接、mmap、挂载与传播、dcache/icache、文件锁与 inotify/fanotify、tmpfs 走读、proc/sysfs/debugfs、观测与性能配方。页缓存和回写的内存侧见 memory-management。 |
| [block-io/](./block-io/) | **块层与 IO（大章）**：`bio` / `request`、blk-mq、`none` / mq-deadline / kyber / bfq、多队列亲和、ioprio 与 cgroup io、回写与 fsync、Direct IO、io_uring、NVMe 与 virtio-blk 走读、错误与超时、观测与性能配方。页缓存回写的内存侧见 memory-management，cgroup 文件见 cgroup。 |

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
| [demos/](./locks-and-ds/demos/) | 用户态 C 可直接 `gcc` 编译运行（`container_of`、list、hash/hlist、atomic 竞争）；`kmod_*.c` 只提供示例源码，不装载 |

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
| [demos/](./cgroup/demos/) | 用户态 shell 示意，默认 **DRY-RUN**（只打印命令）；不要在共享生产机上对不属于自己的树写限额 |

### 调度子章节

| 文件 | 内容 |
|------|------|
| [README.md](./scheduler/README.md) | 章节目录、阅读顺序、边界 |
| [01-overview.md](./scheduler/01-overview.md) | 总览与调度类链 |
| [02-core-objects.md](./scheduler/02-core-objects.md) | rq / cfs_rq / sched_entity / task_group |
| [03-eevdf.md](./scheduler/03-eevdf.md) | EEVDF 选人 |
| [04-enqueue-wakeup.md](./scheduler/04-enqueue-wakeup.md) | 入队出队与 ttwu |
| [05-load-balance.md](./scheduler/05-load-balance.md) | PELT 与负载均衡 |
| [06-other-classes.md](./scheduler/06-other-classes.md) | RT / DL / idle / stop |
| [07-cgroup-cpu.md](./scheduler/07-cgroup-cpu.md) | 与 cgroup cpu 衔接 |
| [08-observability.md](./scheduler/08-observability.md) | 观测与本机实测 |
| [09-perf-analysis.md](./scheduler/09-perf-analysis.md) | 性能分析配方 |
| [demos/](./scheduler/demos/) | 只读采集脚本、一次 MEASURED 抓取样本（sampled on 6.12）、两个 bpftrace RECIPE（未跑） |

### 内存管理子章节

| 文件 | 内容 |
|------|------|
| [README.md](./memory-management/README.md) | 章节目录、阅读顺序、与 cgroup/调度/VFS 边界 |
| [01-overview.md](./memory-management/01-overview.md) | 物理线与虚拟线、总图 |
| [02-physical-layout.md](./memory-management/02-physical-layout.md) | page / folio / node / zone / buddy / sparsemem / memblock |
| [03-00-page-alloc.md](./memory-management/03-00-page-alloc.md) | 页分配：调用链、GFP/scope、水位、PCP、释放合并、慢路径、高阶与 compaction、mempolicy、CMA。 |
| [04-slab.md](./memory-management/04-slab.md) | SLUB 与 `kmem_cache`、page frag |
| [05-vmalloc-and-kernel-va.md](./memory-management/05-vmalloc-and-kernel-va.md) | vmalloc、highmem、`kmap_local`、VMAP_STACK |
| [06-mm-and-vma.md](./memory-management/06-mm-and-vma.md) | `mm_struct`、VMA、页表、TLB、rmap、overcommit |
| [07-page-fault.md](./memory-management/07-page-fault.md) | 缺页：匿名、文件、COW、userfaultfd、hwpoison |
| [08-page-cache-and-writeback.md](./memory-management/08-page-cache-and-writeback.md) | 页缓存、脏页、shmem |
| [09-thp-and-hugetlb.md](./memory-management/09-thp-and-hugetlb.md) | THP、hugetlb 预留、HVO |
| [10-00-reclaim.md](./memory-management/10-00-reclaim.md) | 回收：`scan_control`、经典 LRU、MGLRU 设计与路径、对照与开关、zswap、OOM、mlock、迁移。 |
| [11-kvm-memory.md](./memory-management/11-kvm-memory.md) | EPT/NPT、memslot、TDP MMU、mmu_notifier、HMM |
| [12-balloon-fpr-and-overcommit.md](./memory-management/12-balloon-fpr-and-overcommit.md) | balloon、FPR、KSM、virtio-mem、内存热插拔 |
| [13-observability.md](./memory-management/13-observability.md) | 本机 `/proc`、pagemap、DAMON、shrinker debugfs |
| [14-perf-and-dev-practice.md](./memory-management/14-perf-and-dev-practice.md) | 按症状排障、page_owner |
| [demos/](./memory-management/demos/) | 用户态 `maps`/`smaps_rollup` 与 minor fault 示例（写笔记时本机编过跑过）；`kmod_slab_cache.c` 只提供源码，不 `insmod` |

### 中断子章节

| 文件 | 内容 |
|------|------|
| [README.md](./interrupts/README.md) | 章节目录、阅读顺序、与调度/锁/内存的边界 |
| [01-overview.md](./interrupts/01-overview.md) | 总览：设备到 softirq 的总图 |
| [02-x86-entry.md](./interrupts/02-x86-entry.md) | IDT、`common_interrupt`、中断栈、`irqentry_enter` |
| [03-apic-and-vectors.md](./interrupts/03-apic-and-vectors.md) | LAPIC、IO-APIC、MSI/MSI-X、256 向量 |
| [04-genirq-objects.md](./interrupts/04-genirq-objects.md) | `irq_desc`、`irq_data`、`irq_chip`、`irq_domain`、`irqaction` |
| [05-irqdomain-hierarchy-and-matrix.md](./interrupts/05-irqdomain-hierarchy-and-matrix.md) | 向量域、中断重映射、`irq_matrix` |
| [06-request-and-flow.md](./interrupts/06-request-and-flow.md) | `request_threaded_irq`、流控、亲和、热插拔 |
| [07-irq-alloc-failure.md](./interrupts/07-irq-alloc-failure.md) | 向量与 IRTE 分配失败 |
| [08-softirq-tasklet.md](./interrupts/08-softirq-tasklet.md) | softirq、`ksoftirqd`、tasklet |
| [09-deferred-work-choice.md](./interrupts/09-deferred-work-choice.md) | 工作队列、中断线程、`irq_work` |
| [10-timers.md](./interrupts/10-timers.md) | 时间轮、hrtimer、NO_HZ、LAPIC 定时器 |
| [11-kvm-irq-virt.md](./interrupts/11-kvm-irq-virt.md) | 客户机 LAPIC、irqfd、posted interrupt |
| [12-observability.md](./interrupts/12-observability.md) | `/proc`、debugfs、tracepoint、irqbalance |
| [13-perf-analysis.md](./interrupts/13-perf-analysis.md) | 风暴、softirq 饱和、亲和与配方 |

### VFS 子章节

| 文件 | 内容 |
|------|------|
| [README.md](./vfs/README.md) | 章节目录、阅读顺序、与内存章 / 块层 / 容器化章的边界 |
| [01-overview.md](./vfs/01-overview.md) | 系统调用到 VFS、具体文件系统、页缓存、块层 |
| [02-core-objects.md](./vfs/02-core-objects.md) | `super_block`、`inode`、`dentry`、`file` 与四张 ops |
| [03-path-lookup.md](./vfs/03-path-lookup.md) | `path_openat`、RCU-walk、dcache、挂载点、符号链接 |
| [04-fd-and-open.md](./vfs/04-fd-and-open.md) | `files_struct`、`fdtable`、open/close、`struct file` 引用 |
| [05-read-write.md](./vfs/05-read-write.md) | `read_iter` / `write_iter`、`kiocb`、缓冲 IO 与 Direct IO |
| [06-mmap-and-file.md](./vfs/06-mmap-and-file.md) | `f_op->mmap`、`vm_operations_struct`、`filemap_fault` |
| [07-mount-and-ns.md](./vfs/07-mount-and-ns.md) | `vfsmount`、新挂载 API、shared/slave/private/unbindable |
| [08-dcache-icache.md](./vfs/08-dcache-icache.md) | dentry/inode LRU、shrinker、负 dentry |
| [09-locks-and-notify.md](./vfs/09-locks-and-notify.md) | flock、POSIX 锁、lease、inotify、fanotify |
| [10-tmpfs-walkthrough.md](./vfs/10-tmpfs-walkthrough.md) | tmpfs 上 open→read→write→fsync |
| [11-pseudo-fs.md](./vfs/11-pseudo-fs.md) | procfs、sysfs/kernfs、debugfs |
| [12-observability.md](./vfs/12-observability.md) | `/proc/sys/fs/*`、slab 名、tracepoint |
| [13-perf-analysis.md](./vfs/13-perf-analysis.md) | 路径查找、dcache、fsync、DIO 的配方 |

### 块层与 IO 子章节

| 文件 | 内容 |
|------|------|
| [README.md](./block-io/README.md) | 章节目录、阅读顺序、与 VFS / 内存 / cgroup / 中断的边界 |
| [01-overview.md](./block-io/01-overview.md) | VFS 到 bio、request、调度器、驱动的总图 |
| [02-bio-and-bvec.md](./block-io/02-bio-and-bvec.md) | `struct bio`、`bio_vec`、拆分合并、`submit_bio` |
| [03-request-and-blk-mq.md](./block-io/03-request-and-blk-mq.md) | `struct request`、`request_queue`、硬件队列、tag、`blk_mq_ops` |
| [04-sched-none.md](./block-io/04-sched-none.md) | `none`：没有 elevator 的派发 |
| [05-sched-mq-deadline.md](./block-io/05-sched-mq-deadline.md) | mq-deadline |
| [06-sched-kyber.md](./block-io/06-sched-kyber.md) | kyber |
| [07-sched-bfq.md](./block-io/07-sched-bfq.md) | BFQ |
| [08-multiqueue-affinity.md](./block-io/08-multiqueue-affinity.md) | 软件/硬件队列、CPU 映射、IRQ 亲和 |
| [09-ioprio-and-cgroup-io.md](./block-io/09-ioprio-and-cgroup-io.md) | ioprio，以及 cgroup io 交到 blk-cgroup |
| [10-writeback-and-fsync.md](./block-io/10-writeback-and-fsync.md) | 回写到 bio，fsync 到设备 flush |
| [11-direct-io.md](./block-io/11-direct-io.md) | `O_DIRECT`：`iov_iter` 到 bio |
| [12-io-uring.md](./block-io/12-io-uring.md) | 环、SQE/CQE、固定缓冲、poll、`uring_cmd`，对照经典 AIO |
| [13-nvme-walkthrough.md](./block-io/13-nvme-walkthrough.md) | NVMe PCI：`nvme_queue_rq` 到完成 IRQ |
| [14-virtio-blk-walkthrough.md](./block-io/14-virtio-blk-walkthrough.md) | virtio-blk：`virtio_queue_rq` 到 `virtblk_done` |
| [15-errors-timeout-retry.md](./block-io/15-errors-timeout-retry.md) | `blk_status_t`、超时、failfast |
| [16-observability.md](./block-io/16-observability.md) | `/sys/block/*/stat`、blktrace、io_uring fdinfo |
| [17-perf-analysis.md](./block-io/17-perf-analysis.md) | 队列深度、延迟、调度器与 io_uring 的配方 |

## 约定

- 讲原理落到文件路径、函数名、结构体；行号对应注明的内核版本。
- 大章要求：关键结构体成员中文注释、每节关键函数表、关键路径中文注释摘录。
- 观测命令只作参考，默认不在生产机上跑。
