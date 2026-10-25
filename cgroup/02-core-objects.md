# 2. 核心对象

**结论**：记四件事——**`cgroup`（树上的节点）**、**`cgroup_subsys`（一种控制器）**、**`cgroup_subsys_state` / css（该控制器在某节点上的状态）**、**`css_set`（一个进程当前持有的「各 css 组合」）**。进程不直接挂一长串 cgroup，而是挂一个 `css_set`。

> 定义：`include/linux/cgroup-defs.h`  
> 任务指针：`task_struct->cgroups`（`include/linux/sched.h:1233`，类型 `struct css_set __rcu *`）

---

## 2.1 关系一张图

```text
task_struct.cgroups ──RCU──► css_set
                               │
                               ├─ subsys[cpu]    → css → cgroup（控制器没开到本节点时，可能指祖先）
                               ├─ subsys[memory] → css → cgroup
                               ├─ …
                               └─ dfl_cgrp       → 默认层次上「这个进程所在」的 cgroup

cgroup.self = 一个特殊 css（ss == NULL），指回自己
cgroup.subsys[ssid] → 本节点上该控制器的 css（未启用则可能为空，有效 css 在祖先）
```

**线程组**：默认按**进程**迁移（写 `cgroup.procs`）；v2 另有 thread mode（`cgroup.threads`）。同一线程组通常共享同一套资源视图。

**迁移不带动已有子进程**：把父进程挪走，已经 fork 出来的孩子仍留在原地——这是运维踩坑经典题。文档原文在 `cgroup-v2.rst`「What is cgroup?」：迁移进程不会影响已经存在的子孙进程。

---

## 2.2 `struct cgroup_subsys_state`（css）

`include/linux/cgroup-defs.h:152`。字段从结构体开头按这个顺序排，中间没有别的成员。

```c
struct cgroup_subsys_state {
	struct cgroup *cgroup;			/* 挂在哪个 cgroup 节点上 */
	struct cgroup_subsys *ss;		/* 所属子系统；cgroup.self 的 ss 为 NULL */

	struct percpu_ref refcnt;		/* 引用计数；走 css_get / css_put / css_tryget */

	struct list_head sibling;		/* 挂在父 css 的 children 上 */
	struct list_head children;		/* 子 css 链表 */

	/* 挂到所属 cgroup->rstat_css_list，flush 时顺着这条链调 ss->css_rstat_flush */
	struct list_head rstat_css_node;

	/*
	 * 本子系统里唯一的 id。0 不用，root 的 css 永远是 1。
	 * 可以用 css_from_id() 反查。
	 */
	int id;

	unsigned int flags;			/* CSS_*，见下表 */

	/*
	 * 全局递增序号。内核保证每个 css 的 children 按 serial_nr 升序，
	 * 遍历可以中途停、再续。
	 */
	u64 serial_nr;

	/* 自己和孩子 online 的次数。用来保证父 css 不会比孩子先 offline */
	atomic_t online_cnt;

	struct work_struct destroy_work;	/* percpu_ref 杀引用后的销毁工作 */
	struct rcu_work destroy_rwork;		/* RCU 之后再释放 */

	/*
	 * 父 css。放在结构体末尾，是为了离「包住它的大结构」里紧跟着的热字段近一点。
	 * cgroup.self 的 parent 在非 root 上指向父节点的 self。
	 */
	struct cgroup_subsys_state *parent;
};
```

`flags` 的位（同文件 `:50`）：

| 枚举 | 值 | 含义 |
|---|---|---|
| `CSS_NO_REF` | `1 << 0` | 这个 css 不做引用计数 |
| `CSS_ONLINE` | `1 << 1` | 处在 `css_online()` 和 `css_offline()` 之间 |
| `CSS_RELEASED` | `1 << 2` | 引用已经到 0，进入释放 |
| `CSS_VISIBLE` | `1 << 3` | 用户态看得到（对应接口文件已挂上） |
| `CSS_DYING` | `1 << 4` | 正在死 |

控制器私有结构的第一成员几乎总是 `struct cgroup_subsys_state css`（例如 `struct mem_cgroup`、`struct pids_cgroup`），再用 `container_of` 找回大结构——与 [locks-and-ds/01](../locks-and-ds/01-c-primer.md) 同一套路。

---

## 2.3 `struct css_set`

`include/linux/cgroup-defs.h:209`。创建之后，`subsys[]` 除了启动时的 `init_css_set` 以外基本不再改。

```c
struct css_set {
	/*
	 * 每个子系统一个 css。下标就是 cgroup_subsys_id。
	 * 创建后不可变（boot 时往 init_css_set 里填子系统除外）。
	 */
	struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];

	refcount_t refcount;			/* 多少任务/路径还握着这个 cset */

	/*
	 * domain cgroup 上指向自己。
	 * threaded 时指向最近 domain 祖先对应的那个 cset。
	 * 不绑在某个具体任务上的资源，记到 dom_cset 能摸到的 domain css 上。
	 */
	struct css_set *dom_cset;

	struct cgroup *dfl_cgrp;		/* 默认层次上，这个 cset 所在的 cgroup */

	int nr_tasks;				/* 使用本 cset 的任务数；css_set_lock 保护 */

	/*
	 * tasks：正常挂在这里的任务。
	 * mg_tasks：正在迁出或迁入的任务。
	 * dying_tasks：已经 exit、还没 release 的任务。
	 * 三条链都用 task->cg_list 串起来，css_set_lock 保护。
	 */
	struct list_head tasks;
	struct list_head mg_tasks;
	struct list_head dying_tasks;

	struct list_head task_iters;		/* 正在遍历本 cset 的 css_task_iter */

	/*
	 * 默认层次上，subsys[ssid] 可能指向祖先的 css，而不是本节点的。
	 * 这条链挂在「那个有效 css 所属 cgroup」的 e_csets[ssid] 上，
	 * 方便按控制器把「谁在用我」扫出来。
	 */
	struct list_head e_cset_node[CGROUP_SUBSYS_COUNT];

	struct list_head threaded_csets;	/* dom_cset 指回本 cset 的 threaded cset */
	struct list_head threaded_csets_node;

	struct hlist_node hlist;		/* 全局 hash 槽；内容相同就复用已有 cset */

	struct list_head cgrp_links;		/* 指向各层次上被本 cset 引用的 cgroup */

	/* 正在进行的迁移：源预载、目标预载、正式迁移名单。cgroup_mutex 保护 */
	struct list_head mg_src_preload_node;
	struct list_head mg_dst_preload_node;
	struct list_head mg_node;

	struct cgroup *mg_src_cgrp;		/* 这次迁移的源 cgroup */
	struct cgroup *mg_dst_cgrp;		/* 这次迁移的目标 cgroup */
	struct css_set *mg_dst_cset;		/* 任务最终要落到的目标 cset */

	bool dead;				/* 关联的 cgroup 已死，迁移时直接忽略 */

	struct rcu_head rcu_head;		/* RCU 延迟释放 */
};
```

**直觉**：css_set 是「配置指纹」。大量进程共享同一组合时，内核复用同一个 `css_set`，靠 `refcount` 活着。查找或新建走 `find_css_set()`（`cgroup.c:1191`，`static`）。

辅助内联：

| 符号 | 位置 | 作用 |
|---|---|---|
| `task_css_set` | `include/linux/cgroup.h:421` | 读 `task->cgroups`（再包一层 RCU 检查） |
| `task_css` | 同文件 `:433` | `task_css_set(task)->subsys[id]` |
| `task_dfl_cgroup` | 同文件 `:490` | `task_css_set(task)->dfl_cgrp` |
| `css_get` / `css_put` | `include/linux/cgroup_refcnt.h:8` / `:76` | 经 `cgroup.h` 包含进来的内联；`CSS_NO_REF` 时是空操作 |

---

## 2.4 `struct cgroup`

`include/linux/cgroup-defs.h:389`。下面按源码顺序整段给出，分段只是方便读，不是另一份结构。

### 身份、深度、人口

```c
struct cgroup {
	/* --- 身份、深度、人口 --- */
	struct cgroup_subsys_state self;	/* ss == NULL，cgroup 指回本节点 */

	unsigned long flags;			/* 用 unsigned long，方便 bitops；CGRP_* */

	int level;				/* 深度；root = 0，每下一层 +1 */
	int max_depth;				/* 用户可配的最大深度；cgroup.max.depth */

	/*
	 * nr_descendants：还活着的后代个数。
	 * nr_dying_descendants：用户已经 rmdir、但还有人握引用的后代。
	 * max_descendants：后代个数上限；cgroup.max.descendants。
	 * 读持 cgroup_mutex 或 css_set_lock 之一即可；写要两把都拿。
	 */
	int nr_descendants;
	int nr_dying_descendants;
	int max_descendants;

	/*
	 * 本节点上每个非空 css_set 给 nr_populated_csets +1。
	 * 它为 0 表示本节点没有任务。
	 * 孩子若自己还有人口，再按 domain / threaded 记进后两个计数。
	 */
	int nr_populated_csets;
	int nr_populated_domain_children;
	int nr_populated_threaded_children;

	int nr_threaded_children;		/* 活着的 threaded 子 cgroup 个数 */

	unsigned int kill_seq;			/* cgroup.kill 的序号；css_set_lock */
```

### kernfs 与控制器开关

```c
	/* --- kernfs 与控制器开关 --- */
	struct kernfs_node *kn;			/* 对应 /sys/fs/cgroup/... 这个目录 */
	struct cgroup_file procs_file;		/* cgroup.procs 的文件句柄 */
	struct cgroup_file events_file;		/* cgroup.events 的文件句柄 */

	/* 下标是 enum psi_res：io / memory / cpu，以及打开 IRQ 记账时的 irq */
	struct cgroup_file psi_files[NR_PSI_RESOURCES];

	/*
	 * subtree_control：用户写进 cgroup.subtree_control 的掩码。
	 * subtree_ss_mask：真正生效的掩码，可能因为 depends_on 多开几个。
	 * 控制器旋钮只在 subtree_control 里出现时才对用户可见。
	 * old_* 是改掩码过程中的旧值。四个掩码都是 u16。
	 */
	u16 subtree_control;
	u16 subtree_ss_mask;
	u16 old_subtree_control;
	u16 old_subtree_ss_mask;

	/* 本节点上每个子系统的 css；没启用就是 NULL */
	struct cgroup_subsys_state __rcu *subsys[CGROUP_SUBSYS_COUNT];

	struct cgroup_root *root;		/* 这棵层次的根对象 */

	struct list_head cset_links;		/* 哪些 css_set 的任务在本节点；css_set_lock */

	/* 哪些 css_set 的 subsys[ssid] 指向「本节点的这个 css」 */
	struct list_head e_csets[CGROUP_SUBSYS_COUNT];
```

### domain、统计、其余

```c
	/* --- domain、统计、其余 --- */
	/*
	 * 非 threaded：指向自己。
	 * threaded：指向最近的 domain 祖先。
	 * threaded 子树里，进程粒度和 no-internal-process 都放开。
	 * 不绑任务的 domain 级消耗记到 dom_cgrp。
	 */
	struct cgroup *dom_cgrp;
	struct cgroup *old_dom_cgrp;		/* 打开 threaded 期间暂存旧值 */

	struct cgroup_rstat_cpu __percpu *rstat_cpu; /* 每 CPU 一份递归统计 */
	struct list_head rstat_css_list;	/* 本节点上要参与 flush 的 css */

	struct cgroup_base_stat last_bstat;	/* 上次读到的 CPU 时间快照 */
	struct cgroup_base_stat bstat;		/* 汇总后的 CPU 时间 */
	struct prev_cputime prev_cputime;	/* 打印 user/system 时做调整 */

	struct list_head pidlists;		/* 按 namespace 缓存的 procs/tasks 名单 */
	struct mutex pidlist_mutex;

	wait_queue_head_t offline_waitq;	/* 等 css offline 完成 */

	struct work_struct release_agent_work;	/* v1 release agent */

	struct psi_group *psi;			/* 本节点的压力记账；root 另走全局组 */

	struct cgroup_bpf bpf;			/* cgroup BPF 程序 */

	atomic_t congestion_count;		/* 本 cgroup 上的块层拥塞计数 */

	struct cgroup_freezer_state freezer;	/* v2 freezer 的内部状态 */

#ifdef CONFIG_BPF_SYSCALL
	struct bpf_local_storage __rcu *bpf_cgrp_storage;
#endif

	/*
	 * 含自己在内的祖先数组，柔性数组，root 在 [0]。
	 * cgroup_root 里紧跟在 cgrp 后面的 cgrp_ancestor_storage
	 * 就是 root 这个柔性数组的第 0 格。
	 */
	struct cgroup *ancestors[];
};
```

`flags` 里和 v2 日常相关的位（同文件 `:59`，这是普通枚举，值就是 0、1、2、3，配合 `test_bit`）：

| 枚举 | 含义 |
|---|---|
| `CGRP_NOTIFY_ON_RELEASE` | v1：需要通知用户态 release |
| `CGRP_CPUSET_CLONE_CHILDREN` | 新建 cpuset 孩子时克隆父配置 |
| `CGRP_FREEZE` | 这一支要被冻住 |
| `CGRP_FROZEN` | 已经冻住（`cgroup.events` 的 `frozen`） |

`struct cgroup_freezer_state`（同文件 `:370`）：

```c
struct cgroup_freezer_state {
	bool freeze;			/* 用户希望本节点及后代被冻住 */
	int e_freeze;			/* 算上祖先之后，实际是否该冻 */

	/* 下面两个字段由 css_set_lock 保护 */
	int nr_frozen_descendants;	/* 已冻住的后代 cgroup 数 */
	int nr_frozen_tasks;		/* 算作 frozen 的任务数（含 SIGSTOP、ptrace） */
};
```

### 根：`struct cgroup_root`

`include/linux/cgroup-defs.h:537`。一棵层次的根；v2 默认根就是常见的 `/sys/fs/cgroup`。

```c
struct cgroup_root {
	struct kernfs_root *kf_root;		/* 这棵层次的 kernfs 根 */

	unsigned int subsys_mask;		/* 绑在这棵树上的子系统位图 */
	int hierarchy_id;			/* 层次 id */

	struct list_head root_list;		/* 串起所有活着的层次 */
	struct rcu_head rcu;

	/*
	 * 根 cgroup。释放它时会把整个 cgroup_root 一起销毁。
	 * cgrp->ancestors[0] 会溢出写进紧跟着的 cgrp_ancestor_storage。
	 */
	struct cgroup cgrp;
	struct cgroup *cgrp_ancestor_storage;	/* 必须紧挨 cgrp */

	atomic_t nr_cgrps;			/* 这棵树上的 cgroup 个数；/proc/cgroups */
	unsigned int flags;			/* CGRP_ROOT_* */

	char release_agent_path[PATH_MAX];	/* v1 release agent 路径 */
	char name[MAX_CGROUP_ROOT_NAMELEN];	/* 层次名字，可以空 */
};
```

和挂载选项对应的 `CGRP_ROOT_*`（同文件 `:77`）：

| 标志 | 值 | 挂载选项 |
|---|---|---|
| `CGRP_ROOT_NOPREFIX` | `1 << 1` | v1：子系统名不加前缀 |
| `CGRP_ROOT_XATTR` | `1 << 2` | 支持扩展属性 |
| `CGRP_ROOT_NS_DELEGATE` | `1 << 3` | `nsdelegate` |
| `CGRP_ROOT_FAVOR_DYNMODS` | `1 << 4` | `favordynmods` |
| `CGRP_ROOT_CPUSET_V2_MODE` | `1 << 16` | v1 cpuset 用 v2 行为 |
| `CGRP_ROOT_MEMORY_LOCAL_EVENTS` | `1 << 17` | `memory_localevents` |
| `CGRP_ROOT_MEMORY_RECURSIVE_PROT` | `1 << 18` | `memory_recursiveprot` |

---

## 2.5 `struct cgroup_subsys`

`include/linux/cgroup-defs.h:669`。这是控制器向 core 注册的回调表。函数指针可以是 `NULL`，core 只调用非空的那些。

```c
struct cgroup_subsys {
	struct cgroup_subsys_state *(*css_alloc)(struct cgroup_subsys_state *parent_css);
	int (*css_online)(struct cgroup_subsys_state *css);
	void (*css_offline)(struct cgroup_subsys_state *css);
	void (*css_released)(struct cgroup_subsys_state *css);
	void (*css_free)(struct cgroup_subsys_state *css);
	void (*css_reset)(struct cgroup_subsys_state *css);
	void (*css_rstat_flush)(struct cgroup_subsys_state *css, int cpu);
	int (*css_extra_stat_show)(struct seq_file *seq,
				   struct cgroup_subsys_state *css);
	int (*css_local_stat_show)(struct seq_file *seq,
				   struct cgroup_subsys_state *css);

	int (*can_attach)(struct cgroup_taskset *tset);	/* 迁入前检查，可失败 */
	void (*cancel_attach)(struct cgroup_taskset *tset);
	void (*attach)(struct cgroup_taskset *tset);		/* 迁移已经提交 */
	void (*post_attach)(void);
	int (*can_fork)(struct task_struct *task,
			struct css_set *cset);			/* 创建子进程前检查 */
	void (*cancel_fork)(struct task_struct *task, struct css_set *cset);
	void (*fork)(struct task_struct *task);
	void (*exit)(struct task_struct *task);
	void (*release)(struct task_struct *task);
	void (*bind)(struct cgroup_subsys_state *root_css);	/* 绑到某棵层次 */

	bool early_init:1;		/* 很早的启动阶段就初始化 */

	/*
	 * 为真：默认层次上不出现在 cgroup.controllers / subtree_control，
	 * 对所有节点隐式启用，并且不受 no-internal-process 约束。
	 * 这种控制器必须同时 threaded。
	 */
	bool implicit_on_dfl:1;

	/*
	 * 为真：默认层次上支持 threaded 模式。
	 * threaded 子树里忽略进程粒度和 no-internal-process。
	 */
	bool threaded:1;

	int id;				/* 启动时填，等于 cgroup_subsys_id */
	const char *name;		/* 出现在 cgroup.controllers 里的名字 */
	const char *legacy_name;	/* v1 名字；没填就用 name */

	struct cgroup_root *root;	/* 当前绑着的层次；cgroup_lock() 保护 */
	struct idr css_idr;		/* css->id 的分配器 */

	struct list_head cfts;		/* 已注册的 cftype 数组 */

	struct cftype *dfl_cftypes;	/* v2 接口文件；可以和 legacy 指向同一张表 */
	struct cftype *legacy_cftypes;	/* v1 接口文件 */

	/*
	 * 本控制器启用时，依赖的其它子系统也要一起启用（若它们还在）。
	 * 因依赖被打开的控制器，在用户显式写入 subtree_control 之前对用户不可见。
	 */
	unsigned int depends_on;
};
```

注册表：`include/linux/cgroup_subsys.h` 里的 `SUBSYS(name)`。包含两次——一次生成 `enum cgroup_subsys_id`（`cgroup-defs.h:43`），名字形如 `cpu_cgrp_id`。

每个接口文件用 `struct cftype`（同文件 `:580`）描述：`.name`、`.seq_show` / `.read_u64`、`.write` / `.write_u64`、`.flags`。core 会给控制器文件名加上子系统前缀，所以表里写 `"max"`，用户看到的是 `memory.max` 或 `cpu.max`。

`cftype->flags` 里笔记会碰到的位（`:118`）：

| 标志 | 值 | 含义 |
|---|---|---|
| `CFTYPE_ONLY_ON_ROOT` | `1 << 0` | 只在 root 上创建 |
| `CFTYPE_NOT_ON_ROOT` | `1 << 1` | root 上不创建 |
| `CFTYPE_NS_DELEGATABLE` | `1 << 2` | 委托边界之内仍可写 |
| `CFTYPE_DEBUG` | `1 << 5` | 只在 `cgroup_debug` 时创建 |

---

## 2.6 kernfs / cgroup2 直觉

- cgroup2 是挂在 **kernfs** 上的伪文件系统：`mkdir` → `cgroup_mkdir`（`kernel/cgroup/cgroup.c:5749`），写文件 → 各 `cftype` 的 `.write` / `.seq_show`。
- 用户态「创建一个组」≈ `mkdir /sys/fs/cgroup/foo`；「把进程放进去」≈ `echo $PID > .../cgroup.procs`。
- 本机挂载采样见 [09](./09-observability.md)：`cgroup2 on /sys/fs/cgroup`（sampled on 6.12）。

---

## 2.7 关键函数表（对象层）

| 符号 | 位置 | 作用 |
|---|---|---|
| `task_css_set` | `include/linux/cgroup.h:421` | 取任务当前 css_set |
| `task_dfl_cgroup` | `include/linux/cgroup.h:490` | 取默认层次上的 cgroup |
| `css_get` / `css_tryget` / `css_put` | `include/linux/cgroup_refcnt.h:8` / `:42` / `:76` | css 引用 |
| `find_css_set` | `cgroup.c:1191`（`static`） | 按「旧 cset + 目标 cgroup」查找或创建 cset |
| `cgroup_mkdir` | `cgroup.c:5749` | 建子 cgroup |
| `cgroup_attach_task` | `cgroup.c:2868` | 把任务或整个线程组迁到目标 cgroup |
| `cgroup_migrate_prepare_dst` | `cgroup.c:2771` | 为每个源 cset 准备目标 cset |
| `cgroup_migrate` | `cgroup.c:2838` | 把 leader（或整组）送进迁移执行 |
| `cgroup_migrate_execute` | `cgroup.c:2541`（`static`） | `can_attach` → 改 `css_set` → `attach` |

---

*对照：`include/linux/cgroup-defs.h`、`include/linux/cgroup.h`、`include/linux/cgroup_refcnt.h`、`include/linux/sched.h`。*
