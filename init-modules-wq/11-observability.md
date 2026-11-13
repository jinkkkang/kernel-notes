# 11. 观测

**结论**：模块在不在，看 `/proc/modules` 和 `/sys/module/<名字>/`；带 `WQ_SYSFS` 的队列在 `/sys/bus/workqueue/devices/<名字>/`；initcall 耗时靠 `initcall_debug` 打进 printk；工作项的排队和执行有四条 `workqueue:*` tracepoint。下面的命令都是 RECIPE，本章没有贴过一次采样输出。

> 格式来自 v6.6.160 源码。真机数字留给 backlog `IMW-KMOD-WQ` 和以后的只读采集。不要在生产机上 `insmod`。

---

## 11.1 `/proc/modules`

`proc_modules_init`（`kernel/module/procfs.c:147`）用 `module_init` 创建名为 `"modules"` 的 proc 文件。内建的 `module_init` 就是 device 级 initcall（[04](./04-initcalls.md)）。

`m_show`（`:74`）对每个非 `UNFORMED` 模块打一行。注释里的格式（`:107`）：

```text
modulename size refcount deps address
```

对应的 `seq_printf`：

| 字段 | 行 | 内容 |
|------|----|------|
| 名字和大小 | 86 | `mod->name`，以及各 `mem[type].size` 之和（`module_total_size`，`:65`） |
| 引用数 | `print_unload_info` `:21` | `module_refcount(mod)`，已经减去 `MODULE_REF_BASE` |
| 依赖 | `:27` | `source_list` 上每个使用者的名字，逗号分隔。没有就印 `-`。有 init 无 exit 再加 `[permanent],` |
| 状态 | 91 | `GOING` → `Unloading`，`COMING` → `Loading`，否则 `Live` |
| 地址 | 96 | `mod->mem[MOD_TEXT].base`，格式 `0x%px`。`kallsyms_show_value` 为假时 `m->private` 非空，地址打成 NULL |

没有 `CONFIG_MODULE_UNLOAD` 时引用和依赖两列是字面量 ` - -`（`:44`）。

**RECIPE**（只读，仍不要在不信任的机器上把地址当秘密贴出去）：

```text
grep kn_demo_wq /proc/modules
```

demo 01 加载成功时，这一行应含名字 `kn_demo_wq` 和 `Live`。卸下之后这一行应消失。本章没有这样一行输出。

---

## 11.2 `/sys/module/`

`param_sysfs_init`（`kernel/params.c:961`，`subsys_initcall`，`:972`）做 `kset_create_and_add("module", ...)`。这就是 `/sys/module`。注释写它必须早于 initramfs，否则 `request_module` 触发的加载会在 `mod_sysfs_init` 里失败。

每个模块的目录由 `mod_sysfs_init`（`kernel/module/sysfs.c:338`）把 kobject 加进这个 kset，名字是 `mod->name`。`mod_sysfs_setup`（`:370`）再建：

| 目录或属性组 | 行 | 内容 |
|--------------|----|------|
| `holders/` | 381 | 依赖本模块的人。链接由 `add_usage_links`（`:395`）按 `source_list` 加上 |
| `parameters/` | `module_param_sysfs_setup` | 路径写在 `params.c:707`：`/sys/module/[mod->name]/parameters/` |
| modinfo 属性 | `module_add_modinfo_attrs` `:391` | `version`、`srcversion` 等 |
| `sections/` | `add_sect_attrs` `:399` | 需要 `CONFIG_KALLSYMS`。每个分配了的 ELF 段一个 bin 属性 |

内建参数没有点号时归到 kobject `"kernel"`（`params.c:840`）。所以 `initcall_debug`（`core_param`，`init/main.c:772`）的节点是 `/sys/module/kernel/parameters/initcall_debug`。

`workqueue.watchdog_thresh` 这个名字写在 `kernel/workqueue.c:6401` 的注释里。`workqueue.c` 编进 vmlinux 时没有定义 `MODULE`，`module_param_cb` 使用的前缀是 `KBUILD_MODNAME "."`（`include/linux/moduleparam.h:15`）。可加载模块正好相反：`#ifdef MODULE` 把前缀定义成空（`:12`），`insmod` 的参数名不带模块名。`param_sysfs_builtin`（`params.c:827`）按点号前面的部分建 kobject，所以这个内建参数落在 `/sys/module/workqueue/parameters/watchdog_thresh`。perm 是 `0644`（`workqueue.c:6593`）。`CONFIG_WQ_WATCHDOG` 关掉时这段参数不存在（`:6602` 起的 `#else`）。

**RECIPE**：

```text
ls /sys/module/kn_demo_wq
cat /sys/module/kn_demo_wq/parameters/msg
```

demo 01 的参数名是 `msg`。加载前这个目录不存在；`rmmod` 之后 `mod_sysfs_teardown` 把它去掉。

---

## 11.3 `initcall_debug`

命令行 `initcall_debug` 把 `init/main.c:771` 的布尔量设成真。`start_kernel:949` 调用 `initcall_debug_enable`（`:1178`），注册 tracepoint，回调用 `KERN_DEBUG` 打印：

```text
calling  %pS @ %i
initcall %pS returned %d after %lld usecs
```

`%i` 是 `task_pid_nr(current)`（`:1161`）。时间是 `ktime_us_delta`（`:1172`）。

启动期的 initcall 要在命令行上打开才赶得上。模块的 init 走同一个 `do_one_initcall`，所以模块加载时只要这个布尔量已经是真，也会打这两行。运行中改 `/sys/module/kernel/parameters/initcall_debug` 来得及影响之后的 `finit_module`，来不及补上已经跑完的启动 initcall。

黑名单是另一条命令行：`initcall_blacklist=函数名`，逗号分隔（[04](./04-initcalls.md)）。它让函数不被调用，不是用来观察的。

**RECIPE**。打开 `initcall_debug` 会在启动日志里为每个 initcall 加两行，日志量跟着内建函数个数涨。需要重启的那次采集是 demo 02，还没写脚本。本章不编一份 dmesg。

---

## 11.4 workqueue 的 sysfs

`wq_subsys` 的总线名是 `"workqueue"`（`kernel/workqueue.c:6235`）。`wq_sysfs_init`（`:6274`，`core_initcall`，`:6290`）调用 `subsys_virtual_register`。`workqueue_sysfs_register` 的注释（`:6303`）写暴露路径是 `/sys/bus/workqueue/devices`。设备名是 `wq->name`（`:6334`）。

每条注册过的队列都有的属性（`wq_sysfs_attrs`，`:6057`）：

| 文件 | 模式 | 行 | 含义 |
|------|------|----|------|
| `per_cpu` | 只读 | 6032 | `!(flags & WQ_UNBOUND)`，印 `1` 或 `0`（`:6030`） |
| `max_active` | 读写 | 6055 | 读的是 `saved_max_active`（`:6039`）。写下去调用 `workqueue_set_max_active` |

`WQ_UNBOUND` 再多四个文件（`wq_sysfs_unbound_attrs`，`:6227`）：`nice`、`cpumask`、`affinity_scope`、`affinity_strict`，都是 `0644`。

`alloc_workqueue` 只在标志含 `WQ_SYSFS` 时注册（`:4813`）。系统的 `events` 队列没有这个标志，不会出现在这个目录里。demo 01 的队列名字是 `kn-demo-wq`，标志含 `WQ_UNBOUND | WQ_SYSFS`，所以 `per_cpu` 应为 `0`，并且应有 `cpumask` 等四个文件。`destroy_workqueue` 第一步就是 `workqueue_sysfs_unregister`（`:4878`）。

注释写的是总线设备目录。`subsys_virtual_register` 还会在虚拟设备树里放一份，demo 脚本因此额外看 `/sys/devices/virtual/workqueue/kn-demo-wq/`，**在则记录，不在不判失败**。本章没有对 `subsys_virtual_register` 的实现文件逐行核对那条虚拟路径的拼法，不以它为验收条件。

**RECIPE**：

```text
ls /sys/bus/workqueue/devices/kn-demo-wq
cat /sys/bus/workqueue/devices/kn-demo-wq/per_cpu
cat /sys/bus/workqueue/devices/kn-demo-wq/max_active
```

`max_active` 在 demo 里传了 0，创建路径会换成 `WQ_DFL_ACTIVE`（256，[10](./10-cmwq-queue-and-flags.md)）。真机读到的数字以文件为准，不要拿这份笔记里的 256 去覆盖一次实际 `cat`。

---

## 11.5 tracepoint

模块（`include/trace/events/module.h`，`TRACE_SYSTEM module`）：

| 事件 | 行 | 打印 |
|------|----|------|
| `module:module_load` | 31 | 名字和 taint 标志。`load_module` 在调用 `do_init_module` 之前 `trace_module_load`（`main.c:2993`） |
| `module:module_free` | 50 | 名字。`free_module` 开头 |
| `module:module_get` / `module_put` | 92、99 | `call_site` 和 `refcnt`。需要 `CONFIG_MODULE_UNLOAD`。`refcnt` 是原子值，含 `MODULE_REF_BASE` |
| `module:module_request` | 107 | 名字、`wait`、调用点。`__request_module` 在 `kmod.c:166` |

taint 字符在 `show_module_flags`（`:24`）：`P` 专有、`O` 树外、`F` 强制、`C` staging/crap、`E` 未签名。

工作队列（`include/trace/events/workqueue.h`）：

| 事件 | 行 | 何时 |
|------|----|------|
| `workqueue:workqueue_queue_work` | 23 | `__queue_work` 决定 pwq 之后（`workqueue.c:1809`）。字段有 `function`、`workqueue` 名字、`req_cpu`、`cpu` |
| `workqueue:workqueue_activate_work` | 59 | 插入 `pool->worklist` 时（`:1821`）。积压在 `inactive_works` 里的项此时还没有这条 |
| `workqueue:workqueue_execute_start` | 82 | 调用 `work->func` 之前（`:2656`） |
| `workqueue:workqueue_execute_end` | 108 | `func` 返回之后（`:2662`） |

`req_cpu` 在 `queue_work` 路径上是 `WORK_CPU_UNBOUND`（数值 `NR_CPUS`）。unbound 池的 `cpu` 字段是负数。这些是格式，不是一次 trace 的摘录。

**RECIPE**。要看事件，用 tracefs 或 `perf`，用完把 enable 写回 0。过滤到 demo 的函数名，避免把系统 `events` 队列的全部工作项引进环。ftrace 的开销见 [tracing/12](../tracing/12-perf-analysis.md)。本章环境没有把这些事件打开过。

---

## 11.6 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `kernel/module/procfs.c` | `m_show` | 74 | `/proc/modules` 一行 |
| `kernel/params.c` | `param_sysfs_init` | 961 | 创建 `module` kset |
| `kernel/module/sysfs.c` | `mod_sysfs_setup` | 370 | 单个模块的目录 |
| `init/main.c` | `initcall_debug_enable` | 1178 | 把 initcall tracepoint 打到 printk |
| `kernel/workqueue.c` | `workqueue_sysfs_register` | 6314 | `/sys/bus/workqueue/devices/<name>` |
| `kernel/workqueue.c` | `per_cpu_show` | 6025 | sysfs `per_cpu` |
| `include/trace/events/workqueue.h` | `workqueue_queue_work` | 23 | 入队事件 |
| `include/trace/events/module.h` | `module_load` | 31 | 装入事件 |

---

**导航**：[← 入队与标志](./10-cmwq-queue-and-flags.md) · [目录](./README.md) · [性能配方 →](./12-perf-analysis.md)
