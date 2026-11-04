# 5b. NUMA balancing 闭环

**结论**：CPU 在节点 A 上跑、页在节点 B 上时，远程访问变慢。[05](./05-load-balance.md) 的 `load_balance` 搬的是**任务**。本篇搬的是**页**：周期扫描把 PTE 改成 `PROT_NONE` hint，下次访问在缺页里记账，值得搬再调用 `migrate_misplaced_page`，记账多了再把任务的 `numa_preferred_nid` 指到那一侧。`MEMORY_TIERING` 只点模式位。

> 内核版本：**v6.6.160**。行号对照 `raw.githubusercontent.com/gregkh/linux/v6.6.160`。
> 本机数字 **sampled on 6.12**（`uname -r` 为 `6.12.94+`）。这台机器 `online` 节点只有 `0`。
> 导航：[← 负载均衡](./05-load-balance.md) · [目录](./README.md) · [观测 →](./08-observability.md)

sched 和 mm 的交界头是 `include/linux/sched/numa_balancing.h`：`task_numa_fault`、`should_numa_migrate_memory`、`TNF_*`。缺页怎么分发在 [memory-management/07](../memory-management/07-page-fault.md)，`migrate_pages` 的批处理在 [10-10](../memory-management/10-10-migration.md)，mempolicy 模式在 [03-08](../memory-management/03-08-numa-policy.md)。`task_struct` 上的 NUMA 字段已在 [process/02](../process-management/02-task-struct.md) 注解，这里不重复整段结构。

```text
task_tick_fair                         fair.c:12707
  static key sched_numa_balancing 开着
  task_tick_numa                       :3558
    运行时间超过 numa_scan_period
    task_work_add(numa_work, TWA_RESUME)
         回到用户态时跑 task_numa_work   :3260
           change_prot_numa             mempolicy.c:631
             change_protection(MM_CP_PROT_NUMA)
               newprot = PAGE_NONE
               x86：清 _PAGE_PRESENT，置 _PAGE_PROTNONE

用户再碰这一页 → #PF
  handle_pte_fault                     memory.c:5052
    pte_protnone 且 VMA 可访问
    do_numa_page                       :4806
      numa_migrate_prep                :4789
        mpol_misplaced                 mempolicy.c:2557
          should_numa_migrate_memory   fair.c:1871
      migrate_misplaced_page           migrate.c:2565
        migrate_pages(..., MIGRATE_ASYNC, MR_NUMA_MISPLACED)
      task_numa_fault                  fair.c:3122
        到点再 task_numa_placement     :2838
        numa_migrate_preferred         :2584
          task_numa_migrate            :2448   点到：任务跟着走
```

---

## 5b.1 CPU 在 A、页在 B

两条路分工：

| 谁 | 搬什么 | 落在哪 |
|---|---|---|
| `load_balance` / `select_task_rq_fair` | 任务 | [05](./05-load-balance.md)。PELT 和域扫描不在本篇重写 |
| 本篇 | 页。记账够了再改 `numa_preferred_nid`，让任务有机会跟过去 | `fair.c` 的 NUMA 段 + mm 的 hint fault |

页上记下上次碰到它的 CPU/pid。`struct page` 的 `_last_cpupid` 在 [mm/02](../memory-management/02-physical-layout.md)。私有访问是「这次 fault 的 pid 和页上记的 pid 相同」。共享访问才会考虑把任务收进同一个 `numa_group`。

`rq` 上已有三个计数（[02](./02-core-objects.md) 的 `struct rq`）：`nr_numa_running`、`nr_preferred_running`、`numa_migrate_on`。入队时 `account_numa_enqueue`（`fair.c:1538`）按 `numa_preferred_nid != NUMA_NO_NODE` 给第一项加一，首选节点等于任务当前节点时再给第二项加一。出队对称相减（`:1544`）。`account_entity_enqueue`（`:3649`）在任务实体入队时调用它们。

---

## 5b.2 开关与模式

`include/linux/sched/sysctl.h:22`：

```c
#define NUMA_BALANCING_DISABLED		0x0
#define NUMA_BALANCING_NORMAL		0x1
#define NUMA_BALANCING_MEMORY_TIERING	0x2
```

`sysctl_numa_balancing_mode`（`:27`，没编 `CONFIG_NUMA_BALANCING` 时宏是 0）。文档在 `Documentation/admin-guide/sysctl/kernel.rst:663`：写进去的值是这几位的或。`kernel.numa_balancing` 的处理函数是 `sysctl_numa_balancing`（`kernel/sched/core.c:4589`），表项在 `:4718`，`extra1` / `extra2` 是 `SYSCTL_ZERO` / `SYSCTL_FOUR`，proc 接受 0–4。非 0 就 `static_branch_enable(&sched_numa_balancing)`（`:4560`）。静态键定义在 `:4554`，默认 false。

| 值 | 位 | 本篇怎么用 |
|---|---|---|
| 0 | `NUMA_BALANCING_DISABLED` | 静态键关掉。`task_tick_fair`（`fair.c:12707`）在 `:12717` 直接跳过 `task_tick_numa` |
| 1 | `NUMA_BALANCING_NORMAL` | 节点之间把页往访问它的 CPU 一侧搬。下面的闭环写的是这一支 |
| 2 | `NUMA_BALANCING_MEMORY_TIERING` | 按内存层级提升热页。只点这个位：`should_numa_migrate_memory`（`:1882`）和 `change_pte_range`（`mm/mprotect.c:155`）有分支。不写 CXL，不写水位算法 |
| 3 | `NORMAL \| MEMORY_TIERING` | 两位同时置上。proc 上限允许 |

启动：`check_numabalancing_enable`（`mm/mempolicy.c:2847`）。`numa_balancing=enable|disable` 由 `setup_numabalancing`（`:2865`）记下，多节点且没有这条覆盖时，按 `CONFIG_NUMA_BALANCING_DEFAULT_ENABLED` 调 `set_numabalancing_state`（`core.c:4568`）。那个函数只把模式设成 `NORMAL` 或 `DISABLED`。层级位要事后写 sysctl；从非层级切到带 `MEMORY_TIERING` 时，`reset_memory_tiering`（`:4578`）把每个 `pgdat` 的提升阈值清掉。

扫描节流的用户接口在 debugfs（`kernel/sched/debug.c:366`，要 `CONFIG_SCHED_DEBUG` 和 `CONFIG_NUMA_BALANCING`）：`/sys/kernel/debug/sched/numa_balancing/`。变量名带 `sysctl_`，挂的是 debugfs 文件：

| 文件 | 变量 | 默认 |
|---|---|---|
| `scan_period_min_ms` | `sysctl_numa_balancing_scan_period_min` `fair.c:1403` | 1000 |
| `scan_period_max_ms` | `..._scan_period_max` `:1404` | 60000 |
| `scan_size_mb` | `..._scan_size` `:1407` | 256 |
| `scan_delay_ms` | `..._scan_delay` `:1410` | 1000 |
| `hot_threshold_ms` | `..._hot_threshold` `:1413` | `MSEC_PER_SEC`（1000）。只给层级那一支用 |

另有 proc `kernel.numa_balancing_promote_rate_limit_MBps`（`fair.c:167`），默认 65536（`:143`）。它限制每个目标节点的提升吞吐，仍属层级。本篇只留这个名字。

`task_scan_min` / `task_scan_max`（`:1476`、`:1512`）按 RSS 把 min/max 除成「扫完一遍地址空间要多少个窗口」。窗口数来自 `task_nr_scan_windows`（`:1454`）：RSS 除以 `scan_size` 兆字节对应的页数。本地访问比例高时，`update_task_scan_period`（`:2647`）把 `numa_scan_period` 拉长。目标是大约 70% 本地（`NUMA_PERIOD_THRESHOLD` 7 / `NUMA_PERIOD_SLOTS` 10，`:2638`）。

---

## 5b.3 扫描：`task_numa_work` → `change_prot_numa`

`task_tick_fair` 在静态键打开时调用 `task_tick_numa`（`:3558`）。内核线程、正在退出、没有 `mm`、或者 `numa_work` 已经挂在队列上（`work->next != work`），直接返回。用的是 `se.sum_exec_runtime`，不是墙上时间：任务得真的跑过一段，才值得扫描。越过 `numa_scan_period` 毫秒的运行时间，并且 `mm->numa_next_scan` 到点，就 `task_work_add(curr, work, TWA_RESUME)`（`include/linux/task_work.h:18`）。工作在回到用户态时跑，不占 tick。

`task_numa_work`（`:3260`）拿 `mmap_read_trylock`。这次预算是 `scan_size` 兆字节的已映射页，虚拟范围放宽到 8 倍，用来跳过空洞。一个 VMA 要同时过这些门才改 PTE（`:3344` 起）：

| 跳过 | trace 原因（`numa_balancing.h:18`） |
|---|---|
| 不可迁移、`vma_policy_mof` 为假、hugetlb、`VM_MIXEDMAP` | `NUMAB_SKIP_UNSUITABLE` |
| 只读文件映射（有 `VM_READ` 无 `VM_WRITE`）或 vdso 这类没有 `vm_mm` 的 | `NUMAB_SKIP_SHARED_RO` |
| `vma_is_accessible` 为假。真 `PROT_NONE` 和 hint 分不清 | `NUMAB_SKIP_INACCESSIBLE` |
| 新建 VMA 还在 `scan_delay` 里 | `NUMAB_SKIP_SCAN_DELAY` |
| 本 pid 最近没在这个 VMA 上 fault 过，除非没有别的 VMA 可扫 | `NUMAB_SKIP_PID_INACTIVE` |
| 同一轮 `numa_scan_seq` 已经扫完 | `NUMAB_SKIP_SEQ_COMPLETED` |

`vma_policy_mof`（`mempolicy.c:1799`）看策略上有没有 `MPOL_F_MOF`。没编策略的普通任务走 `get_task_policy`（`:166`），落到 `preferred_node_policy[当前 node]`（`:2907`），那份带 `MPOL_F_MOF | MPOL_F_MORON`。所以默认策略的匿名 VMA 会被扫。显式 `MPOL_BIND` 要用户带上 `MPOL_F_NUMA_BALANCING` 才有 `MOF`，见 5b.8。

每个被扫过的 VMA 挂一块状态。`vm_area_struct->numab_state` 的指针在 [mm/06](../memory-management/06-mm-and-vma.md)。结构本身：

```c
/* include/linux/mm_types.h:588。CONFIG_NUMA_BALANCING */
struct vma_numab_state {
	unsigned long next_scan;          /* 这个 VMA 允许开扫的 jiffies。新建时推迟 scan_delay */
	unsigned long pids_active_reset;  /* 下次滑动 pid 窗口。周期 VMA_PID_RESET_PERIOD，fair.c:3254 */
	unsigned long pids_active[2];     /* [0] 上一窗 [1] 本窗。按 pid 哈希置位，会撞 */
	int start_scan_seq;               /* VMA 创建后第一次扫时的 mm->numa_scan_seq */
	int prev_scan_seq;                /* 上次完整扫完时的序号。等于当前序号就跳过 */
};
```

`mm->numa_next_scan`、`numa_scan_offset`、`numa_scan_seq` 在 `mm_struct` 里，同样见 mm/06。扫到地址空间末尾，`reset_ptenuma_scan`（`fair.c:3203`）把 `numa_scan_seq` 加一、偏移归零。`task_numa_placement` 用这个序号决定要不要重算首选节点。

改保护位：

```c
/* mm/mempolicy.c:631。CONFIG_NUMA_BALANCING 才有这个定义；否则是 :648 的空 stub */
unsigned long change_prot_numa(struct vm_area_struct *vma,
			unsigned long addr, unsigned long end)
{
	/* change_protection(..., MM_CP_PROT_NUMA)
	 * nr_updated > 0 时 count_vm_numa_events(NUMA_PTE_UPDATES, nr_updated)
	 */
}
```

`MM_CP_PROT_NUMA` 是 `include/linux/mm.h:2570` 的 `1UL << 1`。`change_protection`（`mm/mprotect.c:513`）看见它就把 `newprot` 设成 `PAGE_NONE`（`:529`）。x86 的 `PAGE_NONE`（`arch/x86/include/asm/pgtable_types.h:206`）是 `_PAGE_ACCESSED | _PAGE_GLOBAL`，没有 `_PAGE_PRESENT`。present 位清掉时，`_PAGE_BIT_PROTNONE` 和 `_PAGE_GLOBAL` 是同一位（`:49`，上面注释写 `pte_present` 仍为真）。CPU 因此 `#PF`。内核 `pte_present`（`arch/x86/include/asm/pgtable.h:950`）看的是 `_PAGE_PRESENT | _PAGE_PROTNONE`，hint PTE 对内核仍算 present。`pte_protnone`（`:991`）要求 PROTNONE 置上且 PRESENT 清掉，用来和空 PTE 分开。通用头 `include/linux/pgtable.h:1446` 把同一句话写在 `#ifndef CONFIG_NUMA_BALANCING` 的 stub 上：可访问的 VMA 里，`pte_protnone` 表示 NUMA hint；真 `PROT_NONE` 的 VMA 要靠 `vma_is_accessible` 排除。扫描和缺页两边都是这么做的。

`change_pte_range`（`mprotect.c:83`）还会少改几类页：已经是 protnone 的、KSM、zone device、COW 且 `page_count != 1` 的、脏文件页。单线程（`mm_users == 1`）私有 VMA 里，页已经在当前节点上的也不改。模式位：`NUMA_BALANCING_NORMAL` 没置上时跳过 toptier（`:155`）；`MEMORY_TIERING` 置上且页不在 toptier 时，用 `xchg_page_access_time` 记下访问时刻（`:158`）。后者到此为止。

---

## 5b.4 缺页：`do_numa_page` / `numa_migrate_prep`

hint fault 的 PTE 里已经有页。用户态一碰，硬件 `#PF`，内核把它从 protnone 恢复成 VMA 的正常保护，并顺手决定要不要迁。分发在 `handle_pte_fault`（`memory.c:5052`）：`pte_protnone` 且 `vma_is_accessible` 就进 `do_numa_page`（`:4806`）。整条缺页链留在 [07](../memory-management/07-page-fault.md)。PMD 透明大页走 `do_huge_pmd_numa_page`（`mm/huge_memory.c:1519`），同样先 `numa_migrate_prep`，再 `migrate_misplaced_page`（`:1574`），记账页数是 `HPAGE_PMD_NR`。

`do_numa_page` 干路：

1. 拿 PTL，`pte_same` 对不上就返回 0，让缺页重来。
2. `pte_modify(old, vma->vm_page_prot)` 还原正常保护。此刻能判断 PTE 是否可写。
3. `vm_normal_page`。没有页、zone device、或者 `PageCompound`（注释写着 PTE 映射的 THP 还没做）→ `out_map`：把 PTE 装回去，不迁移。
4. 不可写则 `TNF_NO_GROUP`。`page_mapcount > 1` 且 `VM_SHARED` 则 `TNF_SHARED`。
5. 层级模式且页不在 toptier 时，`last_cpupid` 用缺省值（慢页上的 cpupid 被拿去记访问时间）。否则 `page_cpupid_last`。
6. `numa_migrate_prep`（`:4789`）`get_page`，`vma_set_access_pid_bit` 把当前 pid 记进 `numab_state->pids_active`，`NUMA_HINT_FAULTS` 加一；页节点等于 `numa_node_id()` 时再加 `NUMA_HINT_FAULTS_LOCAL` 并置 `TNF_FAULT_LOCAL`。返回值是 `mpol_misplaced`。
7. 返回 `NUMA_NO_NODE`：`put_page`，`out_map`，页留在原地。
8. 否则放下 PTL，`migrate_misplaced_page`。成功则 `TNF_MIGRATED`，`task_numa_fault` 后返回 0（PTE 已由迁移路径换成目标页）。失败置 `TNF_MIGRATE_FAIL`，重新锁 PTE，装回原保护，再 `task_numa_fault`。

`TNF_*` 在 `include/linux/sched/numa_balancing.h:12`：

| 宏 | 值 | 含义 |
|---|---|---|
| `TNF_MIGRATED` | `0x01` | 这次把页迁走了 |
| `TNF_NO_GROUP` | `0x02` | 不要为这次 fault 加入 `numa_group` |
| `TNF_SHARED` | `0x04` | 多个地址空间共享 |
| `TNF_FAULT_LOCAL` | `0x08` | 页本来就在 fault 的 CPU 所在节点 |
| `TNF_MIGRATE_FAIL` | `0x10` | 决定迁、但没迁成 |

GUP 主动撞上 hint 时要 `FOLL_HONOR_NUMA_FAULT`。旗标和 KVM 慢路径在 [07b](../memory-management/07b-gup-and-pin.md)，本篇不重写 pin。

---

## 5b.5 迁页：`should_numa_migrate_memory` → `migrate_misplaced_page`

`mpol_misplaced`（`mempolicy.c:2557`）是唯一调用点（经 `numa_migrate_prep`）。策略没有 `MPOL_F_MOF` 就返回 `NUMA_NO_NODE`。带 `MPOL_F_MORON` 时，目标节点改成 fault CPU 的节点，再问 `should_numa_migrate_memory(current, page, curnid, thiscpu)`（`:2621`）。当前节点已经等于目标则仍返回 `NUMA_NO_NODE`。

`should_numa_migrate_memory`（`fair.c:1871`）在 `NORMAL` 下的过滤（`dst_nid` 是 fault CPU 的节点）：

1. `last_cpupid` 用 `page_cpupid_xchg_last` 换成当前 CPU/pid。源页不在 toptier、且旧 cpupid 无效、又没开层级位：返回 false（`:1912`）。
2. 任务还没有 `numa_preferred_nid`，或 `numa_scan_seq <= 4`，并且这次是私有 fault（pid 还没写上，或 pid 对得上）：立刻允许迁（`:1922`）。注释写 4 是为了等两轮节点选择。
3. 页上一个 cpupid 的节点不是 `dst_nid`：返回 false。连续两次采样才承认这段 task↔page 关系。
4. pid 对得上：私有，允许迁。
5. 还没有 `numa_group`：允许迁。
6. 组在目标节点上的 CPU fault 超过源节点的 `ACTIVE_NODE_FRACTION`（3，`:1639`）倍：允许迁。
7. 否则用 3/4 滞回：`faults_cpu(dst) / faults_mem(dst) * 3 > faults_cpu(src) / faults_mem(src) * 4`（`:1971`）。减少页来回搬。

模式带 `NUMA_BALANCING_MEMORY_TIERING` 且源节点不是 toptier 时，函数在 `:1882` 改走热度 / 速率那一支，本篇不展开。

页真的搬：

`migrate_misplaced_page`（`mm/migrate.c:2565`）要求调用者已经持有引用，函数返回前放下。先放弃两类文件页：`mapcount != 1` 且可执行（`:2579`，多半是共享库），或者文件页是脏的（`:2587`，`MIGRATE_ASYNC` 下不是每个文件系统都能搬）。然后 `numamigrate_isolate_page`（`:2512`）。目标节点不够空就放弃；多映射的 THP 也放弃。隔离成功后：

```text
migrate_pages(&migratepages, alloc_misplaced_dst_folio,
              NULL, node, MIGRATE_ASYNC, MR_NUMA_MISPLACED, &nr_succeeded)
              # :2595
```

`MR_NUMA_MISPLACED` 在 `include/linux/migrate_mode.h:28`。`MIGRATE_ASYNC` 不睡眠。失败的页 `putback_lru_page`。成功则 `count_vm_numa_events(NUMA_PAGE_MIGRATE, nr_succeeded)`（`:2608`），返回非 0。批处理、重试、拷贝在 [10-10](../memory-management/10-10-migration.md)，这里只写「hint fault 何时调到这一页」。

---

## 5b.6 记账与偏好：`task_numa_fault` / `numa_group` / `numa_preferred_nid`

`task_numa_fault`（`fair.c:3122`）在缺页末尾调用。静态键关着，或 `current->mm` 为空（例如 ksmd 在用户 mm 里 fault），直接返回。慢节点在层级模式、或 cpupid 无效时，也不记账（`:3142`）。

第一次进来分配 `p->numa_faults`。四段的布局在 `include/linux/sched.h:1287` 的注释里，枚举在 `kernel/sched/sched.h:1771`：

```c
enum numa_faults_stats {
	NUMA_MEM = 0,   /* 衰减后的内存 fault，放置看它 */
	NUMA_CPU,       /* fault 发生时任务跑在哪个节点，按运行时间归一 */
	NUMA_MEMBUF,    /* 本扫描窗的内存 fault */
	NUMA_CPUBUF     /* 本扫描窗的 CPU fault */
};
```

下标是 `task_faults_idx`（`fair.c:1579`）：每种统计按节点再分私有 / 共享两格（`NR_NUMA_HINT_FAULT_TYPES` 是 2，`:1551`）。缓冲段在一次 `task_numa_placement` 时折半衰减进长期段，缓冲清零。`numa_faults_locality[3]`：`[0]` 远程、`[1]` 本地、`[2]` 迁移失败的页数。`numa_pages_migrated` 累加 `TNF_MIGRATED` 的页数。

私有还是共享：cpupid 还是缺省值就算私有；否则 `cpupid_match_pid`。对不上、且没有 `TNF_NO_GROUP`，就 `task_numa_group`（`:2974`）。人数少的并进人数多的；人数相同则指针更大的组留下（`:3025`、`:3031`）。通过这道比较之后，同 `mm` 或带 `TNF_SHARED` 才合并，否则这次仍记成私有（`*priv = !join`，`:3043`）。任务还没有组时，先给自己建一组，`gid` 是自己的 pid。

```c
/* kernel/sched/fair.c:1415。faults[] 是柔性数组，长度按节点数分配 */
struct numa_group {
	refcount_t refcount;
	spinlock_t lock;              /* 保护 nr_tasks 和 faults */
	int nr_tasks;
	pid_t gid;
	int active_nodes;             /* CPU fault 超过最大值 1/3 的节点数 */
	struct rcu_head rcu;
	unsigned long total_faults;
	unsigned long max_faults_cpu;
	unsigned long faults[];       /* 与任务的 NUMA_MEM / NUMA_CPU 同样下标 */
};
```

`p->numa_group` 是 `RCU` 指针。读侧规则写在 `task_struct` 注释里（`sched.h:1277`）：syscall / 缺页里只有 current 改它。`task_numa_group_id`（`fair.c:1559`）给 `/proc/<pid>/sched` 和 trace 用。

`jiffies` 越过 `numa_migrate_retry` 时，先 `task_numa_placement`（`:2838`），再 `numa_migrate_preferred`（`:2584`）。放置看 `mm->numa_scan_seq`：和任务自己的 `numa_scan_seq` 一样就返回，避免一轮扫描里重复算。它按节点累加衰减后的 memory fault（在组里则看组的合计），选出 `max_nid`。没有 CPU 的节点改成距离最近、有 CPU 的节点。组里的 `max_nid` 再经过 `preferred_group_nid`（`fair.c:2755`），按节点距离把一组 fault 收成一个节点。`max_nid` 变了就 `sched_setnuma`（`core.c:9425`）：持 `rq` 锁，把任务临时 dequeue / put_prev，改 `numa_preferred_nid`，再 enqueue，这样 `nr_numa_running` 跟着重算。

`init_numa_balancing`（`fair.c:3508`）在 `__sched_fork`（`core.c:4546`）里调用。新地址空间（没有 `CLONE_VM`）把 `numa_preferred_nid` 设成 `NUMA_NO_NODE`。带 `CLONE_VM` 的新线程沿用拷来的首选节点，只把 `node_stamp` 错开，避免同一 mm 的线程同一时刻开扫。

---

## 5b.7 任务侧跟随（点到）

`numa_migrate_preferred` 在已经跑在首选节点上时返回。否则 `task_numa_migrate`（`fair.c:2448`）。这个函数在首选节点上找 CPU，组跨多个活跃节点时再看别的节点，然后 `migrate_task_to`（`core.c:9404`，经 `stop_one_cpu`）或 `migrate_swap`（`core.c:3477`）。找不到更好的 CPU 就 `trace_sched_stick_numa` 并返回 `-EAGAIN`。cpuset 把任务困在不跨 NUMA 的域里时，`per_cpu(sd_numa)` 为空，直接 `sched_setnuma` 到当前节点并返回 `-EINVAL`（`fair.c:2491`）。选 CPU 的比较、`imbalance_pct`、交换对象，都不在这里展开。

负载均衡侧只留一个钩子。`can_migrate_task`（`fair.c:9038`）把 `migrate_degrades_locality`（`fair.c:8907`）的返回值当成热度：返回 1 表示搬走会让局部性变差，均衡失败次数还没超过 `cache_nice_tries` 就不选这任务；返回 0 表示搬到首选节点更好，允许选；返回 -1 表示局部性不管，退回 `task_hot`。`find_busiest_group` 的其余步骤仍在 [05](./05-load-balance.md)。

---

## 5b.8 与 mempolicy

`MPOL_F_NUMA_BALANCING`（`include/uapi/linux/mempolicy.h:32`，`1 << 13`）只允许配在 `MPOL_BIND` 上。`sanitize_mpol_flags`（`mempolicy.c:1461`）否则返回 `-EINVAL`，通过则或上内部的 `MPOL_F_MOF | MPOL_F_MORON`（`uapi` `:64-65`）。模式表和 `mbind` 在 [03-08](../memory-management/03-08-numa-policy.md)。

接到闭环上就是两处：

- 扫描：`task_numa_work` 要求 `vma_policy_mof`。没有 `MOF` 的 BIND VMA 不会被改成 hint。
- 缺页：`mpol_misplaced` 的 `MPOL_BIND` + `MORON`（`:2591`）。CPU 所在节点在 bind 集合里，才把目标收成这个 CPU 的节点并调用 `should_numa_migrate_memory`。CPU 不在集合里就 `goto out`，页留在策略允许的节点上。

`MPOL_MF_LAZY` 的 `mbind` 也会给新策略或上 `MOF`（`:1292`），并在 `queue_pages_test_walk`（`:691`）里对可访问、非 hugetlb、非 `VM_MIXEDMAP` 的 VMA 调用 `change_prot_numa`。那是一次用户发起的 hint，不是 tick 扫描。

---

## 5b.9 观测

| 接口 | 位置 | 看什么 |
|---|---|---|
| `/proc/<pid>/numa_maps` | `fs/proc/base.c:3313`，`CONFIG_NUMA`。打印 `show_numa_map` `fs/proc/task_mmu.c:1949` | 每个 VMA 一行：策略名、`anon` / `dirty` / `mapped` / `mapmax`、`N<node>=页数`、`kernelpagesize_kB` |
| `/proc/vmstat` 的 `numa_pte_updates` 等 | `mm/vmstat.c:1307-1311`，对应 `include/linux/vm_event_item.h:64-68` | `NUMA_PTE_UPDATES`、`NUMA_HUGE_PTE_UPDATES`、`NUMA_HINT_FAULTS`、`NUMA_HINT_FAULTS_LOCAL`、`NUMA_PAGE_MIGRATE`（文本是 `numa_pages_migrated`） |
| `pgmigrate_success` / `pgmigrate_fail` | 同文件，`CONFIG_MIGRATION` | `migrate_pages` 的总账，含压缩、mbind、本篇。见 10-10 |
| `numa_hit` / `numa_miss` / `numa_local` / `numa_other` / `numa_interleave` | 分配计数，[mm/13](../memory-management/13-observability.md) | 页从哪个节点分到的。和 hint fault 不是同一组 |
| `/proc/<pid>/sched` | `CONFIG_SCHED_DEBUG`，`proc_sched_show_task` `debug.c:989` | `sched_show_numa`（`:974`）：`mm->numa_scan_seq`、`numa_pages_migrated`、`numa_preferred_nid`、`total_numa_faults`、`numa_group_id` |
| debugfs `sched/numa_balancing/` | `debug.c:366` | 5b.2 的扫描节流 |
| tracepoint | `include/trace/events/sched.h` | `sched_move_numa` `:576`、`sched_stick_numa` `:648`、`sched_swap_numa` `:656`、`sched_skip_vma_numa` `:688` |

`count_vm_numa_event` 在没编 `CONFIG_NUMA_BALANCING` 时是空宏（`include/linux/vmstat.h:113`）。编了之后就是普通 `count_vm_event`，所以 `/proc/vmstat` 里这五项和别的 vmstat 一样累加。

### RECIPE

```bash
cat /proc/sys/kernel/numa_balancing
cat /proc/sys/kernel/numa_balancing_promote_rate_limit_MBps
cat /sys/devices/system/node/online
grep -E 'numa_|pgmigrate_' /proc/vmstat
# 换一个活着的 pid。下面这行读的是命令自己的映射
head -n 8 /proc/self/numa_maps
```

多节点、并且允许改机器时，再打开闭环然后看计数是否从 0 往上走：

```bash
sysctl kernel.numa_balancing=1
# 跑一段会碰匿名内存的负载之后
grep -E 'numa_pte_updates|numa_hint_faults|numa_pages_migrated' /proc/vmstat
```

有 `perf` 或 `bpftrace`、并且 tracefs 可读时，事件名用上面四个 `sched_*numa*`。`sched_skip_vma_numa` 的 `reason` 对应 `NUMAB_SKIP_*`。作者这边没有拿它们抓过输出。

### sampled on 6.12

`uname -r`：`6.12.94+`。`/sys/devices/system/node/online` 是 `0`。`/proc/self/status` 的 `Mems_allowed_list` 是 `0`。

| 读数 | 值 |
|---|---|
| `kernel.numa_balancing` | `0` |
| `kernel.numa_balancing_promote_rate_limit_MBps` | `65536`（和 v6.6.160 `fair.c:143` 的初值相同；仍是 6.12 上的读数） |
| `numa_pte_updates` / `numa_huge_pte_updates` | `0` / `0` |
| `numa_hint_faults` / `numa_hint_faults_local` | `0` / `0` |
| `numa_pages_migrated` | `0` |
| `pgmigrate_success` / `pgmigrate_fail` | `0` / `0` |
| `numa_hit` / `numa_miss` / `numa_local` / `numa_other` | `3360998` / `0` / `3360998` / `0` |
| `numa_interleave` | `1328` |

`numa_hit` 这一组是分配命中，单节点上 miss 为 0 是预期。[13](../memory-management/13-observability.md) 里的 `numa_hit` 是更早一次抓取，数字对不上。hint 五项全 0，和 sysctl 读到 0、只有一个节点一致：扫描静态键没有被启动逻辑打开（`check_numabalancing_enable` 只在 `num_online_nodes() > 1` 时自动设）。

`head -n 8 /proc/self/numa_maps` 的前几行（进程是 `head` 自己）：

```text
555e07b88000 default file=/usr/bin/head mapped=2 N0=2 kernelpagesize_kB=4
555e07b92000 default file=/usr/bin/head anon=1 dirty=1 active=0 N0=1 kernelpagesize_kB=4
555e376b9000 default heap anon=4 dirty=4 active=0 N0=4 kernelpagesize_kB=4
7fa101c00000 default file=/usr/lib/x86_64-linux-gnu/libc.so.6 mapped=39 mapmax=26 N0=39 kernelpagesize_kB=4
```

策略列是 `default`，页都在 `N0`。`show_numa_map` 没有 VMA 自己的策略时，打印 `hold_task_mempolicy`（`task_mmu.c:105`）存下的 `get_task_policy`。`mpol_to_str`（`mempolicy.c:3112`）把 `default_policy` 和整段 `preferred_node_policy[]` 都印成 `default`（`:3120-3125`）。样本里的 `default` 因此盖住了 5b.3 说的那份带 `MOF|MORON` 的每节点 preferred。用户设过的策略会印成 `bind`、`prefer`、`interleave`，带 `MPOL_F_NUMA_BALANCING` 时后缀是 `=balancing`（`:3156`）。`/proc/self/sched` 这次不存在（`No such file or directory`），`numa_preferred_nid` 没有采到。`/sys/kernel/debug/sched` 是 `Permission denied`，扫描节流文件没有读。

### 单节点：demo 跳过

跨节点迁页要至少两个 `online` 节点，并且 `kernel.numa_balancing` 非 0。这台机器只有 node0，sysctl 是 0，hint 计数停在 0。不在这里跑「分配 → 扫描 → 迁页」的 demo：现象不会出现，硬做只会把单节点上的 0 误读成闭环坏了。机制仍在 v6.6.160 的代码里；要看页从 `N1` 挪到 `N0`，换一台多节点机器，用上面的 RECIPE 把 sysctl 打开后再读 `numa_maps` 的 `N*` 和 `numa_pages_migrated`。

---

## 5b.10 边界

| 本篇写了 | 留在别处 |
|---|---|
| 扫描 → hint → `do_numa_page` → 迁一页 → `numa_preferred_nid` | `load_balance` / PELT / `find_busiest_group`（[05](./05-load-balance.md)） |
| 何时调用 `do_numa_page`、`migrate_misplaced_page` | 缺页分发全文（[07](../memory-management/07-page-fault.md)）；`migrate_pages` 批处理（[10-10](../memory-management/10-10-migration.md)） |
| `MPOL_F_NUMA_BALANCING` 怎样放行扫描和 `MORON` | mempolicy 模式教程（[03-08](../memory-management/03-08-numa-policy.md)） |
| `NUMA_BALANCING_MEMORY_TIERING` 这个位，以及它让两条函数改走另一支 | 层级、CXL、提升阈值和 `promote_rate_limit` 的调节过程 |
| `migrate_degrades_locality` 的三个返回值 | `can_migrate_task` 的其余条件（05） |
| `FOLL_HONOR_NUMA_FAULT` 一句 | GUP / pin（[07b](../memory-management/07b-gup-and-pin.md)） |

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `NUMA_BALANCING_*` | `include/linux/sched/sysctl.h:22` | 模式位 |
| `sysctl_numa_balancing` | `kernel/sched/core.c:4589` | `kernel.numa_balancing` |
| `sched_numa_balancing` | `core.c:4554` | 总开关静态键 |
| `sysctl_numa_balancing_scan_*` | `fair.c:1403` 起 | 周期 / 大小 / 延迟。debugfs 可写 |
| `task_tick_numa` | `fair.c:3558` | tick 上到点就挂 task_work |
| `task_numa_work` | `:3260` | 扫 VMA，调用 `change_prot_numa` |
| `change_prot_numa` | `mm/mempolicy.c:631` | `MM_CP_PROT_NUMA` |
| `change_protection` | `mm/mprotect.c:513` | hint 时 `newprot = PAGE_NONE` |
| `do_numa_page` | `mm/memory.c:4806` | PTE hint fault |
| `numa_migrate_prep` | `memory.c:4789` | 记账并 `mpol_misplaced` |
| `do_huge_pmd_numa_page` | `mm/huge_memory.c:1519` | PMD THP 的同一条路 |
| `mpol_misplaced` | `mm/mempolicy.c:2557` | 该不该迁、迁到哪 |
| `should_numa_migrate_memory` | `fair.c:1871` | 私有 / 共享 / 组的过滤 |
| `migrate_misplaced_page` | `mm/migrate.c:2565` | `MR_NUMA_MISPLACED` 迁一页 |
| `task_numa_fault` | `fair.c:3122` | 写入 `numa_faults` 缓冲 |
| `struct numa_group` | `fair.c:1415` | 共享 fault 的一组任务 |
| `task_numa_placement` | `:2838` | 选出 `numa_preferred_nid` |
| `sched_setnuma` | `core.c:9425` | 改首选节点并重算 rq 计数 |
| `task_numa_migrate` | `fair.c:2448` | 点到：把任务迁向首选节点 |
| `numa_migrate_preferred` | `:2584` | 还没在首选节点上才调用上一行 |
| `migrate_degrades_locality` | `:8907` | 给 `can_migrate_task` 的局部性答案 |
| `struct vma_numab_state` | `include/linux/mm_types.h:588` | 每个 VMA 的扫描游标 |
| `show_numa_map` | `fs/proc/task_mmu.c:1949` | `/proc/<pid>/numa_maps` |

---

**导航**：[← 负载均衡](./05-load-balance.md) · [章节目录](./README.md) · [观测 →](./08-observability.md)
