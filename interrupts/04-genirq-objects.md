# 4. genirq 对象

**结论**：一个 Linux IRQ 号对应一个 `irq_desc`；描述符里嵌着共享数据 `irq_common_data` 和第一份 `irq_data`；芯片操作是 `irq_chip` 函数表；`irq_domain` 把硬件号翻译成这个 IRQ 号；驱动挂上的是 `irqaction` 链表。

> 定义：`include/linux/irq.h`、`include/linux/irqdesc.h`、`include/linux/irqdomain.h`、`include/linux/interrupt.h`。字段顺序与 v6.6.160 一致。

C 新手先看嵌套：`irq_desc` **包含** `irq_data`（不是指针）。`irq_data.common` **指向** 同一描述符里的 `irq_common_data`。层次域里，子 `irq_data.parent_data` 指向父域那一份 `irq_data`，父份不在这个 `irq_desc` 里面，是另分配的。

---

## 4.1 关系

```text
irq_desc                         一个 Linux IRQ
  ├─ irq_common_data             所有芯片层共享：亲和、MSI 描述符
  ├─ irq_data                    最顶层芯片的那一份
  │     ├─ chip -> irq_chip      mask / unmask / eoi / set_affinity
  │     ├─ domain -> irq_domain
  │     └─ parent_data -> ...    一直指到向量域那一层
  ├─ handle_irq                  流控：handle_fasteoi_irq 等
  └─ action -> irqaction         驱动注册的处理函数，共享时串成链表
        └─ next
```

`irq_data_to_desc`（`include/linux/irqdesc.h:125`）用 `container_of`：从 `irq_data.common` 反推外层的 `irq_desc`。只有描述符里那份顶层 `irq_data` 能这么做；层次中间的 `irq_data` 是单独分配的。

---

## 4.2 `irq_common_data` 与 `irq_data`

```c
/* include/linux/irq.h:147 */
struct irq_common_data {
	unsigned int		__private state_use_accessors; /* IRQD_* 位，用 irqd_*() 访问 */
#ifdef CONFIG_NUMA
	unsigned int		node;              /* 分配描述符时选的 NUMA 节点 */
#endif
	void			*handler_data;     /* 芯片或流控的私有指针 */
	struct msi_desc		*msi_desc;         /* MSI/MSI-X 这一条对应的描述符 */
#ifdef CONFIG_SMP
	cpumask_var_t		affinity;          /* 希望投到哪些 CPU */
#endif
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	cpumask_var_t		effective_affinity;/* 芯片真正接受的子集 */
#endif
#ifdef CONFIG_GENERIC_IRQ_IPI
	unsigned int		ipi_offset;        /* IPI 目标在 affinity 里的起点 */
#endif
};
```

`state_use_accessors` 带 `__private`，别直接改。位定义在 `irq.h:227`：

| 位 | 掩码 | 含义 |
|---|---|---|
| 低 4 位 | `IRQD_TRIGGER_MASK` (`0xf`) | 边沿/电平 |
| 8 | `IRQD_SETAFFINITY_PENDING` | 亲和改动还没落到硬件 |
| 9 | `IRQD_ACTIVATED` | 已经 activate |
| 12 | `IRQD_AFFINITY_SET` | 有人显式设过亲和 |
| 16 | `IRQD_IRQ_DISABLED` | 逻辑上 disable |
| 17 | `IRQD_IRQ_MASKED` | 硬件上 mask |
| 18 | `IRQD_IRQ_INPROGRESS` | 处理函数正在跑 |
| 20 | `IRQD_FORWARDED_TO_VCPU` | 转发给客户机 vCPU |
| 21 | `IRQD_AFFINITY_MANAGED` | 内核托管亲和（多队列设备） |
| 23 | `IRQD_MANAGED_SHUTDOWN` | 托管中断因掩码里没有在线 CPU 而关掉 |
| 24 | `IRQD_SINGLE_TARGET` | 只允许一个目标 CPU |
| 26 | `IRQD_CAN_RESERVE` | 可以先占位、等到 `request_irq` 再拿真向量 |
| 28 | `IRQD_AFFINITY_ON_ACTIVATE` | 去激活期间不要调 `irq_set_affinity` |

x86 向量域在分配时置上 `IRQD_SINGLE_TARGET`、`IRQD_HANDLE_ENFORCE_IRQCTX`、`IRQD_AFFINITY_ON_ACTIVATE`（`vector.c:576` 起）。

```c
/* include/linux/irq.h:179 */
struct irq_data {
	u32			mask;          /* 访问芯片寄存器时预计算的位掩码 */
	unsigned int		irq;           /* Linux IRQ 号 */
	unsigned long		hwirq;         /* 本 domain 内的硬件号 */
	struct irq_common_data	*common;       /* 指回描述符里的共享块 */
	struct irq_chip		*chip;         /* 这一层的芯片操作 */
	struct irq_domain	*domain;       /* 这一层属于哪个域 */
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	struct irq_data		*parent_data;  /* 父域的 irq_data；向量域是链尾，为 NULL */
#endif
	void			*chip_data;    /* 这一层的私有数据，x86 向量域放 apic_chip_data */
};
```

同一条中断在 MSI 层、重映射层、向量层各有一份 `irq_data`，`hwirq` 各算各的：MSI 层可能是表项索引，向量层在 x86 上被设成 Linux IRQ 号本身（`x86_vector_alloc_irqs` 里 `irqd->hwirq = virq + i`，`vector.c:575`）。

---

## 4.3 `irq_desc`

```c
/* include/linux/irqdesc.h:55 */
struct irq_desc {
	struct irq_common_data	irq_common_data;   /* 嵌在里面，不是指针 */
	struct irq_data		irq_data;          /* 顶层 irq_data，同样嵌在里面 */
	unsigned int __percpu	*kstat_irqs;       /* 每 CPU 计数，/proc/interrupts 用它 */
	irq_flow_handler_t	handle_irq;        /* 流控函数指针 */
	struct irqaction	*action;           /* 驱动处理函数链表头 */
	unsigned int		status_use_accessors; /* 给 irq_settings_* 用的 _IRQ_* */
	unsigned int		core_internal_state__do_not_mess_with_it; /* IRQS_* ，别直接改 */
	unsigned int		depth;             /* disable 嵌套深度，0 才真的打开 */
	unsigned int		wake_depth;        /* 唤醒源嵌套深度 */
	unsigned int		tot_count;         /* 非每 CPU 的累计 */
	unsigned int		irq_count;         /* 用来发现卡住的中断 */
	unsigned long		last_unhandled;    /* 未处理计数的老化时间 */
	unsigned int		irqs_unhandled;    /* 可疑的无人认领次数 */
	atomic_t		threads_handled;   /* 线程处理过的次数 */
	int			threads_handled_last;
	raw_spinlock_t		lock;              /* 保护这条描述符，硬中断里拿 */
	struct cpumask		*percpu_enabled;
	const struct cpumask	*percpu_affinity;
#ifdef CONFIG_SMP
	const struct cpumask	*affinity_hint;    /* 给用户态的提示，不自动生效 */
	struct irq_affinity_notify *affinity_notify;
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_var_t		pending_mask;      /* 还没搬完的目标掩码 */
#endif
#endif
	unsigned long		threads_oneshot;   /* 共享 oneshot：哪些线程还没跑完 */
	atomic_t		threads_active;    /* 正在跑的中断线程数 */
	wait_queue_head_t	wait_for_threads;  /* synchronize_irq 在这等 */
#ifdef CONFIG_PM_SLEEP
	unsigned int		nr_actions;
	unsigned int		no_suspend_depth;
	unsigned int		cond_suspend_depth;
	unsigned int		force_resume_depth;
#endif
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*dir;            /* /proc/irq/N/ */
#endif
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
	struct dentry		*debugfs_file;   /* /sys/kernel/debug/irq/irqs/N */
	const char		*dev_name;
#endif
#ifdef CONFIG_SPARSE_IRQ
	struct rcu_head		rcu;
	struct kobject		kobj;            /* sysfs */
#endif
	struct mutex		request_mutex;   /* 串行 request/free，可睡眠 */
	int			parent_irq;
	struct module		*owner;
	const char		*name;             /* 流控名字，/proc/interrupts 里那一列 */
#ifdef CONFIG_HARDIRQS_SW_RESEND
	struct hlist_node	resend_node;
#endif
} ____cacheline_internodealigned_in_smp;
```

`desc_set_defaults`（`kernel/irq/irqdesc.c:102`）把新描述符设成：芯片先是 `no_irq_chip`，`handle_irq` 是 `handle_bad_irq`，`depth = 1`（先关着），并置 `IRQD_IRQ_DISABLED` 和 `IRQD_IRQ_MASKED`。

打开 `CONFIG_SPARSE_IRQ` 时，描述符不放在大数组里，而放进一棵 maple tree（`irqdesc.c:133` 的 `sparse_irqs`）。找空号的 `irq_find_free_area`（`:139`）在 `MAX_SPARSE_IRQS` 里找不到就返回 `-ENOSPC`（`:144`）。`CONFIG_SPARSE_IRQ` 下 `MAX_SPARSE_IRQS` 是 `INT_MAX`（`kernel/irq/internals.h:15`），否则是 `NR_IRQS`（`:17`）。

全局 `nr_irqs` 初值是 `NR_IRQS`（`irqdesc.c:129`）。它是「目前准备好的 IRQ 号空间」，还会被架构代码改小，见 [07](./07-irq-alloc-failure.md)。

---

## 4.4 `irq_chip`

芯片结构是一张函数表。某一项可以是 `NULL`，核心会用默认动作（例如 `irq_enable` 为空就走 `unmask`）。字段顺序如下。

```c
/* include/linux/irq.h:501 */
struct irq_chip {
	const char	*name;                 /* /proc/interrupts 里的芯片名 */
	unsigned int	(*irq_startup)(struct irq_data *data);
	void		(*irq_shutdown)(struct irq_data *data);
	void		(*irq_enable)(struct irq_data *data);
	void		(*irq_disable)(struct irq_data *data);
	void		(*irq_ack)(struct irq_data *data);
	void		(*irq_mask)(struct irq_data *data);
	void		(*irq_mask_ack)(struct irq_data *data);
	void		(*irq_unmask)(struct irq_data *data);
	void		(*irq_eoi)(struct irq_data *data);          /* 告诉 APIC：这条处理完了 */
	int		(*irq_set_affinity)(struct irq_data *data, const struct cpumask *dest, bool force);
	int		(*irq_retrigger)(struct irq_data *data);
	int		(*irq_set_type)(struct irq_data *data, unsigned int flow_type);
	int		(*irq_set_wake)(struct irq_data *data, unsigned int on);
	void		(*irq_bus_lock)(struct irq_data *data);     /* 慢总线，可睡眠，在进程上下文 */
	void		(*irq_bus_sync_unlock)(struct irq_data *data);
#ifdef CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE
	void		(*irq_cpu_online)(struct irq_data *data);
	void		(*irq_cpu_offline)(struct irq_data *data);
#endif
	void		(*irq_suspend)(struct irq_data *data);
	void		(*irq_resume)(struct irq_data *data);
	void		(*irq_pm_shutdown)(struct irq_data *data);
	void		(*irq_calc_mask)(struct irq_data *data);
	void		(*irq_print_chip)(struct irq_data *data, struct seq_file *p);
	int		(*irq_request_resources)(struct irq_data *data);
	void		(*irq_release_resources)(struct irq_data *data);
	void		(*irq_compose_msi_msg)(struct irq_data *data, struct msi_msg *msg);
	void		(*irq_write_msi_msg)(struct irq_data *data, struct msi_msg *msg);
	int		(*irq_get_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool *state);
	int		(*irq_set_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool state);
	int		(*irq_set_vcpu_affinity)(struct irq_data *data, void *vcpu_info);
	void		(*ipi_send_single)(struct irq_data *data, unsigned int cpu);
	void		(*ipi_send_mask)(struct irq_data *data, const struct cpumask *dest);
	int		(*irq_nmi_setup)(struct irq_data *data);
	void		(*irq_nmi_teardown)(struct irq_data *data);
	unsigned long	flags;                 /* IRQCHIP_* */
};
```

`flags` 的位在 `irq.h:571`。和 x86 MSI 相关的有 `IRQCHIP_AFFINITY_PRE_STARTUP`（`:582`，启动前先更新亲和）和 `IRQCHIP_SKIP_SET_WAKE`（`:576`）。x86 向量 MSI 父域在 `x86_init_dev_msi_info` 里把这两位置上（`arch/x86/kernel/apic/msi.c:253`）。

`irq_set_affinity` 的返回值约定（`irq.h:123`）：`IRQ_SET_MASK_OK`（0）表示核心去拷贝亲和掩码；`IRQ_SET_MASK_OK_NOCOPY` 表示芯片自己已经改了 `affinity`；`IRQ_SET_MASK_OK_DONE` 表示层次域到此为止，不要再往下传。

---

## 4.5 `irq_domain`

```c
/* include/linux/irqdomain.h:151 */
struct irq_domain {
	struct list_head		link;       /* 挂在全局域链表上 */
	const char			*name;      /* debugfs 目录名，向量域是 "VECTOR" */
	const struct irq_domain_ops	*ops;       /* alloc/free/activate/... */
	void				*host_data; /* 主人的私有指针 */
	unsigned int			flags;      /* IRQ_DOMAIN_FLAG_* */
	unsigned int			mapcount;   /* 已映射条数 */
	struct mutex			mutex;      /* 层次域用根域这把锁 */
	struct irq_domain		*root;      /* 层次的根；非层次时指向包含者 */

	/* --- 可选 --- */
	struct fwnode_handle		*fwnode;
	enum irq_domain_bus_token	bus_token;  /* PCI MSI、DMAR 等用它区分 */
	struct irq_domain_chip_generic	*gc;
	struct device			*dev;
	struct device			*pm_dev;
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	struct irq_domain		*parent;    /* 父域。向量域没有父 */
#endif
#ifdef CONFIG_GENERIC_MSI_IRQ
	const struct msi_parent_ops	*msi_parent_ops;
#endif
	void				(*exit)(struct irq_domain *d);

	/* --- 反向映射：hwirq → irq_data --- */
	irq_hw_number_t			hwirq_max;
	unsigned int			revmap_size; /* 线性表长度；0 表示纯基数树 */
	struct radix_tree_root		revmap_tree;
	struct irq_data __rcu		*revmap[];   /* 柔性数组，线性表贴在结构体尾巴上 */
};
```

`ops`（`irqdomain.h:88`）里层次域真正用的是 `alloc` / `free` / `activate` / `deactivate`（`:100` 起，要 `CONFIG_IRQ_DOMAIN_HIERARCHY`）。`map` 是旧的平面域接口。

域标志（`:183`）里本章会碰到：`IRQ_DOMAIN_FLAG_HIERARCHY`、`IRQ_DOMAIN_FLAG_MSI`、`IRQ_DOMAIN_FLAG_MSI_PARENT`（`:208`）、`IRQ_DOMAIN_FLAG_MSI_DEVICE`（`:211`）。

x86 向量域在 `arch_early_irq_init`（`vector.c:793`）里用名字 `"VECTOR"` 创建，并 `irq_set_default_host`（`:802`）。

---

## 4.6 `irqaction`

```c
/* include/linux/interrupt.h:118 */
struct irqaction {
	irq_handler_t		handler;         /* 硬中断里调用的函数 */
	void			*dev_id;         /* 驱动自己的指针，共享中断用来区分 */
	void __percpu		*percpu_dev_id;  /* 每 CPU 设备中断用 */
	struct irqaction	*next;           /* 共享：下一家 */
	irq_handler_t		thread_fn;       /* 线程里跑的函数，可为 NULL */
	struct task_struct	*thread;         /* 内核线程 */
	struct irqaction	*secondary;      /* 强制线程化时的第二份 action */
	unsigned int		irq;
	unsigned int		flags;           /* IRQF_* */
	unsigned long		thread_flags;
	unsigned long		thread_mask;     /* oneshot 共享：这一家占哪一位 */
	const char		*name;           /* /proc/interrupts 最后一列 */
	struct proc_dir_entry	*dir;
} ____cacheline_internodealigned_in_smp;
```

`handler` 的类型是 `irqreturn_t (*)(int, void *)`（`:100`）。返回值里和线程有关的是 `IRQ_WAKE_THREAD`：`__handle_irq_event_percpu`（`kernel/irq/handle.c:139`）看到它就 `wake_up_process(action->thread)`（`:176`）。

注册标志 `IRQF_*` 在 `interrupt.h:71`。常用的：

| 宏 | 值 | 作用 |
|---|---|---|
| `IRQF_SHARED` | `0x00000080` | 允许多个 `irqaction` 串在一起 |
| `IRQF_PERCPU` | `0x00000400` | 每 CPU 中断，不参与均衡 |
| `IRQF_NOBALANCING` | `0x00000800` | 排除出 irqbalance 会写的那类均衡 |
| `IRQF_ONESHOT` | `0x00002000` | 主处理函数返回后先别 unmask，等线程跑完 |
| `IRQF_NO_THREAD` | `0x00010000` | 不能被强制线程化 |
| `IRQF_NO_AUTOEN` | `0x00080000` | `request_irq` 成功后仍保持关闭 |

`IRQF_TIMER`（`:86`）是 `__IRQF_TIMER | IRQF_NO_SUSPEND | IRQF_NO_THREAD`。

共享中断必须带非空的 `dev_id`，否则 `request_threaded_irq` 直接 `-EINVAL`（`manage.c:2169`）。摘链时靠 `dev_id` 认出是哪一家。

---

## 4.7 描述符从哪来（函数索引）

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `kernel/irq/irqdesc.c` | `desc_set_defaults` | 102 | 新描述符的初值 |
| | `irq_find_free_area` | 139 | 在 maple tree 里找一段空号 |
| | `early_irq_init` | 549 | 打印 `NR_IRQS / nr_irqs / preallocated` |
| | `__irq_alloc_descs` | 814 | 分配一段 IRQ 号并建描述符 |
| `kernel/irq/irqdomain.c` | `irq_domain_alloc_descs` | 1144 | 域代码调用的包装 |
| | `irq_domain_alloc_irq_data` | 1373 | 为层次中的每一层分配 `irq_data` |
| `include/linux/irqdesc.h` | `generic_handle_irq_desc` | 159 | `desc->handle_irq(desc)` |

分配失败的 `-ENOMEM` / `-ENOSPC` 落点见 [07](./07-irq-alloc-failure.md)。

---

**导航**：[← 向量](./03-apic-and-vectors.md) · [目录](./README.md) · [域与矩阵 →](./05-irqdomain-hierarchy-and-matrix.md)
