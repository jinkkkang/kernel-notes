# 3.8 NUMA 策略与 numactl

**结论**：策略回答「这次页从哪个 node 来」。进程有一份，VMA 可以另有一份。VMA 上的优先。中断不看当前进程的策略（`include/linux/mempolicy.h:30`）。用户态入口是 `set_mempolicy`、`mbind`、`migrate_pages`，numactl 是它们的命令行壳。

> 导航：[上一篇 高阶](./03-07-high-order-and-compaction.md) · [本目录](./03-00-page-alloc.md) · [下一篇 CMA](./03-09-cma.md)

---

## `struct mempolicy`（`include/linux/mempolicy.h:47`，`CONFIG_NUMA`）

| 成员 | 中文 |
|---|---|
| `atomic_t refcnt` | 引用。`mpol_put` 减到 0 释放。`mpol_dup` 出来的新对象引用从 1 起 |
| `unsigned short mode` | `MPOL_*`，见下 |
| `unsigned short flags` | `MPOL_F_STATIC_NODES` / `RELATIVE_NODES` / `NUMA_BALANCING`，以及内部的 `MPOL_F_SHARED`、`MPOL_F_MOF`、`MPOL_F_MORON` |
| `nodemask_t nodes` | bind / interleave / preferred 用的节点集合 |
| `int home_node` | `MPOL_BIND` 和 `MPOL_PREFERRED_MANY` 的 home node |
| `nodemask_t cpuset_mems_allowed` | 与下一字段同 union。相对模式时，相对这组 cpuset 允许的节点 |
| `nodemask_t user_nodemask` | 同 union。用户传入的 nodemask |
| `struct rcu_head rcu` | 释放走 RCU |

没编 `CONFIG_NUMA` 时，`struct mempolicy` 是空结构（`:188`），`get_task_policy` 返回 NULL。

模式（`include/uapi/linux/mempolicy.h:19`）：

| 值 | 行为 |
|---|---|
| `MPOL_DEFAULT` | 删掉这段上的策略，退回进程默认。进程默认再空，就是本地 node 优先、允许回退 |
| `MPOL_PREFERRED` | 优先一个 node，没有就回退 |
| `MPOL_BIND` | 只从 `nodes` 里拿 |
| `MPOL_INTERLEAVE` | 按页在 `nodes` 里轮转。进程自己的 interleave 游标不加锁（注释 `:34`） |
| `MPOL_LOCAL` | 分配发生在哪个 CPU 的 node，就从哪拿 |
| `MPOL_PREFERRED_MANY` | 一组优先 node，都没有再回退 |
| `MPOL_WEIGHTED_INTERLEAVE` | 按节点带宽加权轮转 |

`MPOL_F_STATIC_NODES`：用户给的节点号不随 cpuset 重新解释。`MPOL_F_RELATIVE_NODES`：当成相对 cpuset 允许集合的偏移。两个不要同时设。`MPOL_F_NUMA_BALANCING`：允许 NUMA balancing 在这个策略上做 migrate-on-fault。

共享映射（shmem、某些文件）的策略不挂在单个 VMA 上，挂在区间树里。`struct shared_policy`（`:113`）两个成员：`struct rb_root root`、`rwlock_t lock`。树上的 `struct sp_node`（`:117`）：`struct rb_node nd`、`pgoff_t start`、`pgoff_t end`、`struct mempolicy *policy`。

```text
set_mempolicy(mode, nmask, maxnode)          # SYSCALL_DEFINE3 mempolicy.c:1857
  → kernel_set_mempolicy                      # :1838
       sanitize_mpol_flags
       装到 current 的进程策略

mbind(start, len, mode, nmask, maxnode, flags)  # SYSCALL_DEFINE6 :1830
  → kernel_mbind                              # :1742
       → do_mbind                             # :1489
            拆/改 VMA 上的 vm_policy
            MPOL_MF_MOVE / MOVE_ALL 时把已有页迁走
            MPOL_MF_STRICT 且迁不动：返回 -EIO

分配
  alloc_pages_noprof                          # :2575
    → alloc_pages_mpol                        # :2441
         policy_nodemask() 算出 nid 和 nodemask
         MPOL_PREFERRED_MANY → alloc_pages_preferred_many
         PMD 阶且策略允许当前 node：先 __GFP_THISNODE|__GFP_NORETRY
         否则 __alloc_frozen_pages_noprof
```

`flags` 里 `MPOL_MF_STRICT`（`:51`）要求现有页也符合策略，`MPOL_MF_MOVE` 迁本进程的页，`MPOL_MF_MOVE_ALL` 连别人的共享页一起迁。`MPOL_MF_LAZY` 注释标明是不支持的标志。

numactl 常见对应：`numactl --membind` → `MPOL_BIND`，`--interleave` → `MPOL_INTERLEAVE`，`--preferred` → `MPOL_PREFERRED`，`--localalloc` → `MPOL_LOCAL`。`--physcpubind` 是 CPU 亲和，不进 `struct mempolicy`。

VMA 上的指针是 `vm_policy`（[06](./06-mm-and-vma.md)）。缺页分配匿名页时走这份策略。迁移本身见 [10-10-migration.md](./10-10-migration.md) 的 `migrate_pages`。

---

## 节点性能坐标（numaperf）

`Documentation/admin-guide/mm/numaperf.rst` 讲的是：一个 node 上的内存带宽和延迟可以不一样（本地 DRAM、慢一档的内存）。内核用 `struct access_coordinate`（`include/linux/node.h:29`）装四个数：

| 成员 | 中文 |
|---|---|
| `unsigned int read_bandwidth` | 读带宽，MB/s |
| `unsigned int write_bandwidth` | 写带宽，MB/s |
| `unsigned int read_latency` | 读延迟，纳秒 |
| `unsigned int write_latency` | 写延迟，纳秒 |

`ACCESS_COORDINATE_LOCAL` 是到最近 initiator 的坐标，`ACCESS_COORDINATE_CPU` 是到最近 CPU node 的坐标（`:42`）。用户态从 `/sys/devices/system/node/nodeN/access0` / `access1` 读，具体文件名以该文档和当前内核的 sysfs 为准。

`mempolicy_set_node_perf`（`mm/mempolicy.c:221`）把坐标收成带宽：`min(read_bandwidth, write_bandwidth)`，写进 `node_bw_table`。`MPOL_WEIGHTED_INTERLEAVE` 的权重在 `struct weighted_interleave_state`（`mempolicy.c:158`）：`bool mode_auto`、柔性数组 `u8 iw_table[]`。auto 模式下按带宽重算权重；手工模式只更新带宽表，不改当前权重（函数注释 `:242-245`）。

---

## 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `SYSCALL_DEFINE3(set_mempolicy)` | `mm/mempolicy.c:1857` | 设进程策略 |
| `SYSCALL_DEFINE6(mbind)` | `mempolicy.c:1830` | 设 VMA 策略，可顺带迁移 |
| `kernel_mbind` / `do_mbind` | `:1742` / `:1489` | mbind 的两层实现 |
| `alloc_pages_mpol` | `:2441` | 按 mempolicy 分配 |
| `alloc_pages_noprof` | `:2575` | 分配入口里套上策略 |
| `mempolicy_set_node_perf` | `:221` | 用带宽坐标更新 weighted interleave |

---

[上一篇 高阶](./03-07-high-order-and-compaction.md) · [本目录](./03-00-page-alloc.md) · [下一篇 CMA](./03-09-cma.md)
