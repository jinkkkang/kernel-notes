# 3.8 NUMA 策略与 numactl

**结论**：策略回答「这次页从哪个 node 来」。进程有一份，VMA 可以另有一份。VMA 上的优先。中断不看当前进程的策略（`include/linux/mempolicy.h:27`）。用户态入口是 `set_mempolicy`、`mbind`、`set_mempolicy_home_node`、`migrate_pages`。numactl 是它们的命令行壳。

> 内核版本：**v6.6.160**。策略枚举在 `include/uapi/linux/mempolicy.h:19`。
> 导航：[上一篇 高阶](./03-07-high-order-and-compaction.md) · [本目录](./03-00-page-alloc.md) · [下一篇 CMA](./03-09-cma.md)

---

## `struct mempolicy`（`include/linux/mempolicy.h:44`，`CONFIG_NUMA`）

```c
#ifdef CONFIG_NUMA
struct mempolicy {
	atomic_t refcnt;		/* mpol_put 减到 0 释放。mpol_dup 从 1 起 */
	unsigned short mode;		/* MPOL_*，见下 */
	/*
	 * MPOL_F_STATIC_NODES / RELATIVE_NODES / NUMA_BALANCING，
	 * 以及内部的 MPOL_F_SHARED、MPOL_F_MOF、MPOL_F_MORON。
	 */
	unsigned short flags;
	nodemask_t nodes;		/* bind / interleave / preferred 用的节点集合 */
	int home_node;			/* MPOL_BIND 和 MPOL_PREFERRED_MANY 的 home node */

	union {
		nodemask_t cpuset_mems_allowed;	/* 相对模式时，相对这组 cpuset 允许的节点 */
		nodemask_t user_nodemask;	/* 用户传入的 nodemask */
	} w;
};
#endif
```

没编 `CONFIG_NUMA` 时，`struct mempolicy` 是空结构（`:189`），相关操作变成空的内联函数。

系统默认策略是 `default_policy`（`mm/mempolicy.c:127`）：`mode = MPOL_LOCAL`，引用计数初始为 1，注释写明永不释放。`get_task_policy`（`:166`）在进程还没有自己的策略时，先看 `preferred_node_policy[当前 node]`，那个也没初始化就退回 `default_policy`。

模式（`include/uapi/linux/mempolicy.h:19`，从 0 递增）：

| 值 | 名字 | 行为 |
|---|---|---|
| 0 | `MPOL_DEFAULT` | 删掉这段上的策略。进程上再空，分配走 `default_policy`，也就是 `MPOL_LOCAL` |
| 1 | `MPOL_PREFERRED` | 优先 `nodes` 里的第一个 node。`policy_node` 把它设成起始 nid，nodemask 仍是 NULL，所以失败可以回退 |
| 2 | `MPOL_BIND` | `policy_nodemask` 返回 `nodes`。低 zone 在 `apply_policy_zone` 不通过时不套这张掩码 |
| 3 | `MPOL_INTERLEAVE` | 按页在 `nodes` 里轮转。进程自己的 `il_prev` 不加锁（`mempolicy.h:30-33` 的注释：只有这个进程碰自己的状态） |
| 4 | `MPOL_LOCAL` | 分配发生在哪个 CPU 的 node，就从哪拿。`policy_nodemask` 返回 NULL |
| 5 | `MPOL_PREFERRED_MANY` | 一组优先 node。第一趟只在这组里试，失败再放开 |
| 6 | `MPOL_MAX` | 枚举的上界，不是一种策略 |

`MPOL_F_STATIC_NODES`（`1 << 15`）：用户给的节点号不随 cpuset 重新解释。`MPOL_F_RELATIVE_NODES`（`1 << 14`）：当成相对 cpuset 允许集合的偏移。`sanitize_mpol_flags`（`mempolicy.c:1452`）拒绝这两个同时设。空节点的 `MPOL_PREFERRED` 再带其中任何一个，`mpol_new`（`:266`）在 `:286-290` 返回 `-EINVAL`；节点为空、又不带这两个标志时，模式改成 `MPOL_LOCAL`。`MPOL_F_NUMA_BALANCING`（`1 << 13`）：只允许配在 `MPOL_BIND` 上，否则 `set_mempolicy` / `mbind` 返回 `-EINVAL`（`:1461-1464`）。配上之后内核再把内部的 `MPOL_F_MOF | MPOL_F_MORON` 设上，让 NUMA balancing 可以在缺页时迁移。

`MPOL_F_SHARED`、`MPOL_F_MOF`、`MPOL_F_MORON` 在 `uapi/linux/mempolicy.h:63-65`，是内核内部标志，不从用户态的 mode 参数直接传进来。

共享映射（shmem、某些文件）的策略不挂在单个 VMA 上，挂在区间树里。`struct shared_policy`（`mempolicy.h:122`）和树上的 `struct sp_node`（`:116`）：

```c
struct sp_node {
	struct rb_node nd;		/* 区间树节点 */
	unsigned long start, end;	/* 这段策略覆盖的页偏移 */
	struct mempolicy *policy;	/* 挂在这段上的策略 */
};

struct shared_policy {
	struct rb_root root;		/* 共享策略的区间树 */
	rwlock_t lock;			/* 保护这棵树 */
};
```

```text
set_mempolicy(mode, nmask, maxnode)          # SYSCALL_DEFINE3 mempolicy.c:1584
  → kernel_set_mempolicy                      # :1565
       sanitize_mpol_flags
       do_set_mempolicy                       # 装到 current->mempolicy

set_mempolicy_home_node(start, len, home_node, flags)
                                             # SYSCALL_DEFINE4 :1490
  改已有 VMA 策略上的 home_node。BIND 和 PREFERRED_MANY 用它

mbind(start, len, mode, nmask, maxnode, flags)  # SYSCALL_DEFINE6 :1557
  → kernel_mbind                              # :1469
       → do_mbind                             # :1256
            拆/改 VMA 上的 vm_policy
            MPOL_MF_MOVE / MOVE_ALL 时 migrate_pages 把已有页迁走
            MPOL_MF_STRICT 且迁不动：返回 -EIO

migrate_pages(pid, maxnode, old_nodes, new_nodes)
                                             # SYSCALL_DEFINE4 :1678
  → kernel_migrate_pages                     # :1590
```

`flags` 里 `MPOL_MF_STRICT`（uapi `:47`）要求现有页也符合策略，迁不动返回 `-EIO`。`MPOL_MF_MOVE` 迁本进程的页，`MPOL_MF_MOVE_ALL` 连别人的共享页一起迁，而且要 `CAP_SYS_NICE`（`do_mbind` `:1271`）。`MPOL_MF_LAZY`（`1 << 3`）不把页立刻迁走：`do_mbind` 给新策略加上 `MPOL_F_MOF`（`:1292`），扫描 VMA 时走 `change_prot_numa`（`:695`），等下次缺页再迁。它和立刻迁移的页列表不能同时用，`:1343` 有 `WARN_ON_ONCE`。

numactl 常见对应：`numactl --membind` → `MPOL_BIND`，`--interleave` → `MPOL_INTERLEAVE`，`--preferred` → `MPOL_PREFERRED`，`--localalloc` → `MPOL_LOCAL`。`--physcpubind` 是 CPU 亲和，不进 `struct mempolicy`。

VMA 上的指针是 `vm_policy`（[06](./06-mm-and-vma.md)）。缺页分配匿名页时 `vma_alloc_folio`（`mempolicy.c:2166`）走这份策略。迁移本身见 [10-10-migration.md](./10-10-migration.md) 的 `migrate_pages`。

---

## 分配时策略怎么变成 nid

`alloc_pages`（`:2261`）在进程上下文、并且 gfp 没有 `__GFP_THISNODE` 时用 `get_task_policy(current)`。中断里或带了 `THISNODE`，用 `default_policy`。

| 模式 | 调用 | 传给 `__alloc_pages` 的参数 |
|---|---|---|
| `MPOL_INTERLEAVE` | `alloc_page_interleave`（`:2113`） | nid 来自 `interleave_nodes`（`:1890`，更新 `current->il_prev`），nodemask 是 NULL。命中该 node 时加 `NUMA_INTERLEAVE_HIT` |
| `MPOL_PREFERRED_MANY` | `alloc_pages_preferred_many`（`:2130`） | 第一趟：gfp 去掉 `DIRECT_RECLAIM` 和 `NOFAIL`，加上 `NOWARN`，nodemask 是 `pol->nodes`。失败第二趟：原来的 gfp，nodemask 是 NULL |
| 其它 | 直接 `__alloc_pages` | nid 是 `policy_node`（`:1868`），nodemask 是 `policy_nodemask`（`:1845`） |

`policy_node`：`MPOL_PREFERRED` 取 `first_node(policy->nodes)`。`MPOL_BIND` 和 `MPOL_PREFERRED_MANY` 如果 `home_node != NUMA_NO_NODE`，起始节点改成 home node。`MPOL_BIND` 再带 `__GFP_THISNODE` 会 `WARN_ON_ONCE`，因为这会破坏「只从这组节点拿」的约定。

`policy_nodemask`：`MPOL_BIND` 要同时过 `apply_policy_zone` 和 cpuset 的 mems 检查，才返回 `&policy->nodes`。`MPOL_PREFERRED_MANY` 直接返回这组节点。其它模式返回 NULL，分配器不额外限制节点。

`vma_alloc_folio`（`:2166`）在 INTERLEAVE 和 PREFERRED_MANY 上走同样的两条专门路径，只是结果包成 folio。开了 THP 且 `hugepage` 为真、策略又允许当前（或 preferred）节点时，先用 `__GFP_THISNODE | __GFP_NORETRY` 只在那个节点上试，失败并且原来的 gfp 允许直接回收，再放开到远程节点。slowpath 里看到「贵、`__GFP_NORETRY`、`__GFP_THISNODE`」就不再回收，就是为了这条 THP 路径（`page_alloc.c:4072`）。

---

## 节点性能坐标（numaperf）

`Documentation/admin-guide/mm/numaperf.rst` 讲的是：一个 node 上的内存带宽和延迟可以不一样（本地 DRAM、慢一档的内存）。内核用 `struct access_coordinate`（`include/linux/node.h:30`）装四个数：

```c
struct access_coordinate {
	unsigned int read_bandwidth;	/* 头注释：读带宽，MB/s。numaperf.rst 写 MiB/s */
	unsigned int write_bandwidth;	/* 写带宽 */
	unsigned int read_latency;	/* 读延迟，纳秒 */
	unsigned int write_latency;	/* 写延迟，纳秒 */
};
```

头文件注释（`node.h:25-28`）写的是 MB/s。管理文档写的是 MiB/second，延迟是纳秒。两边单位的说法都在树里，读 sysfs 时以 `numaperf.rst` 的 MiB/s 和纳秒为准。

访问等级（`node.h:43`）：

| 枚举 | 值 | 含义 |
|---|---|---|
| `ACCESS_COORDINATE_LOCAL` | 0 | 到最近 initiator 的坐标。文档里的 access class 0 |
| `ACCESS_COORDINATE_CPU` | 1 | 到最近 CPU node 的坐标。access class 1，只含 CPU |
| `ACCESS_COORDINATE_MAX` | 2 | 上界 |

文档里的例子：

```text
/sys/devices/system/node/nodeX/access0/targets/nodeY
/sys/devices/system/node/nodeY/access0/initiators/nodeX
/sys/devices/system/node/nodeY/access0/initiators/read_bandwidth
/sys/devices/system/node/nodeY/access0/initiators/write_bandwidth
/sys/devices/system/node/nodeY/access0/initiators/read_latency
/sys/devices/system/node/nodeY/access0/initiators/write_latency
```

access class 1 的目录是 `access1`，形状一样，只有 CPU 到内存的数。initiator 是能发起访问的节点（CPU、GPU、网卡），target 是被访问的内存节点。class 0 是到这个 target 最快的那一组 initiator。

`node_set_perf_attrs`（`include/linux/node.h:80`）在 `CONFIG_HMEM_REPORTING` 下把一份 `access_coordinate` 登记到节点上。没编这个选项时它是空的内联函数（`:88`）。这套坐标是给用户态和 HMEM 上报看的，页分配的 `alloc_pages` 不读它。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `SYSCALL_DEFINE3(set_mempolicy)` | `mm/mempolicy.c:1584` | 设进程策略 |
| `kernel_set_mempolicy` | `:1565` | 校验标志和节点掩码，再 `do_set_mempolicy` |
| `SYSCALL_DEFINE6(mbind)` | `:1557` | 设 VMA 策略，可顺带迁移 |
| `kernel_mbind` / `do_mbind` | `:1469` / `:1256` | mbind 的两层实现 |
| `SYSCALL_DEFINE4(set_mempolicy_home_node)` | `:1490` | 给已有 VMA 策略写 `home_node` |
| `SYSCALL_DEFINE4(migrate_pages)` | `:1678` | 把进程的页从一组节点迁到另一组 |
| `get_task_policy` | `:166` | 进程策略，没有则 `default_policy`（`MPOL_LOCAL`） |
| `alloc_pages` | `:2261` | 分配入口里套上策略 |
| `alloc_page_interleave` | `:2113` | `MPOL_INTERLEAVE` |
| `alloc_pages_preferred_many` | `:2130` | `MPOL_PREFERRED_MANY` 的两趟分配 |
| `vma_alloc_folio` | `:2166` | 按 VMA 的 `vm_policy` 分配 folio |
| `policy_nodemask` | `:1845` | BIND / PREFERRED_MANY 的节点掩码 |
| `policy_node` | `:1868` | PREFERRED 和 home node 的起始 nid |
| `interleave_nodes` | `:1890` | 轮转下一个 node，写 `current->il_prev` |
| `node_set_perf_attrs` | `include/linux/node.h:80` | `CONFIG_HMEM_REPORTING` 下登记带宽和延迟 |

---

[上一篇 高阶](./03-07-high-order-and-compaction.md) · [本目录](./03-00-page-alloc.md) · [下一篇 CMA](./03-09-cma.md)
