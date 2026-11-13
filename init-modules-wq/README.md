# 内核初始化、模块与工作队列

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)

**一句话**：`start_kernel` 把调度、中断和 workqueue 干起来，`rest_init` 造出 pid 1 与 kthreadd，内建 initcall 跑完后 `kernel_execve` 交给用户态 init；运行时再装模块走 `finit_module` → `load_module` → `do_init_module`；可睡眠的延迟活走 cmwq 的 pool / pwq / worker。

## 阅读顺序

```text
01 总览
  → 02 start_kernel
  → 03 rest_init 与 pid 1
  → 04 initcall
  → 05 模块对象
  → 06 装入
  → 07 卸下
  → 08 request_module
  → 09 cmwq 对象
  → 10 入队与标志
  → 11 观测
  → 12 性能配方
```

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 三条线：启动、装模块、cmwq；和中断 / 进程 / 调度的边界 | 中 |
| [02-start-kernel.md](./02-start-kernel.md) | `start_kernel`：`sched_init`、`workqueue_init_early`、开中断、`arch_call_rest_init` | **厚** |
| [03-rest-init-and-pid1.md](./03-rest-init-and-pid1.md) | `rest_init` 造 pid 1 与 kthreadd；`kernel_init_freeable` 里 `workqueue_init` / `workqueue_init_topology`；`kernel_execve` | **厚** |
| [04-initcalls.md](./04-initcalls.md) | `early` 与 `pure`…`late`；`do_one_initcall`；`initcall_debug` | 中偏厚 |
| [05-module-objects.md](./05-module-objects.md) | `struct module`、`module_state`、`mod_mem_type`、`module_memory` | **厚** |
| [06-module-load.md](./06-module-load.md) | `finit_module` → `load_module` → 布局 / 重定位 → `do_init_module` | **厚** |
| [07-module-unload.md](./07-module-unload.md) | `delete_module`、`refcnt`、`source_list` / `target_list` | 中 |
| [08-kmod-request.md](./08-kmod-request.md) | `__request_module` 与 `call_modprobe` | 较薄 |
| [09-cmwq-objects.md](./09-cmwq-objects.md) | `work_struct`、`worker_pool`、`pool_workqueue`、`workqueue_struct`、`worker` | **厚** |
| [10-cmwq-queue-and-flags.md](./10-cmwq-queue-and-flags.md) | `alloc_workqueue`、`__queue_work`、`process_one_work`、`WQ_*`；`schedule_work` → `system_percpu_wq` | **厚** |
| [11-observability.md](./11-observability.md) | `/proc/modules`、`/sys/module/`、`initcall_debug`、`WQ_SYSFS`、tracepoint | **厚** |
| [12-perf-analysis.md](./12-perf-analysis.md) | 装载失败、积压、rescuer、watchdog。RECIPE | **厚** |
| [demos/](./demos/) | demo 01：可加载模块排队到自建 unbound workqueue。**待真机验证** | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| 启动顺序里调度、中断、workqueue 被叫起来的位置 | 选谁跑见 [scheduler/](../scheduler/README.md)。IDT / softirq 见 [interrupts/](../interrupts/README.md) |
| pid 1 与 kthreadd 是在 `rest_init` 里用哪两个入口造出来的 | `user_mode_thread` / `kernel_thread` 的 `PF_KTHREAD` 差别见 [process-management/05](../process-management/05-kernel-threads.md)。`kernel_execve` 进 `bprm_execve` 之后的 ELF 装载见 [process-management/06](../process-management/06-exec.md) |
| cmwq 的 pool、pwq、入队、标志、rescuer | 硬中断 / softirq / 中断线程 / 工作队列 / `irq_work` 怎么选，表在 [interrupts/09](../interrupts/09-deferred-work-choice.md)。本章不重写那张表 |
| `finit_module` 到 `do_init_module`，以及 `delete_module` 的引用计数 | 模块签名的密码学、livepatch 的补丁状态机不写。`module_sig_check` 只作为 `load_module` 的第一步点到 |
| initcall 与模块 init 都进 `do_one_initcall` | ftrace 环和 `TRACE_EVENT` 宏见 [tracing/](../tracing/README.md)。本章只列出 module / workqueue 的事件名 |

## 源码地图

| 路径 | 角色 |
|------|------|
| `init/main.c` | `start_kernel`、`rest_init`、`kernel_init`、initcall 循环 |
| `include/linux/init.h` | `__init`、`pure_initcall` … `late_initcall` |
| `include/asm-generic/vmlinux.lds.h` | `INIT_CALLS` 把 early / 0…7 / rootfs 排进镜像 |
| `include/linux/module.h` | `struct module`、状态、内存类型 |
| `kernel/module/main.c` | `finit_module`、`load_module`、`delete_module` |
| `kernel/module/kmod.c` | `__request_module`、`call_modprobe` |
| `scripts/mod/modpost.c` | 生成 `__this_module`，填 `init` / `exit` |
| `kernel/params.c` | `/sys/module/` 这个 kset，以及内建参数 |
| `include/linux/workqueue.h` | `work_struct`、`WQ_*`、`schedule_work` |
| `kernel/workqueue.c` | pool、pwq、入队、worker、系统队列、watchdog |
| `kernel/workqueue_internal.h` | `struct worker` |
| `include/trace/events/module.h`、`workqueue.h` | 装载与执行的 tracepoint |
| `fs/exec.c` | `kernel_execve` |

## 实测 vs RECIPE

- **实测**：本章没有本机数字。写作时没有 `insmod` / `rmmod`，也没有贴 `/proc/modules`、dmesg 或 sysfs 的采样。
- **RECIPE / 待真机验证**：[demos/01-kmod-wq.c](./demos/01-kmod-wq.c) 源码写完，默认脚本不加载。验收标准在 [demos/README.md](./demos/README.md)。backlog ID 是 `IMW-KMOD-WQ`。[11](./11-observability.md) 和 [12](./12-perf-analysis.md) 里的命令同样是 RECIPE。

## 未覆盖

- systemd 怎样作为 pid 1 拉起 unit
- livepatch 的补丁对象与一致性模型
- 模块签名的摘要、密钥与验签实现（`module_sig_check` 只出现在装载顺序里）
- kexec / kdump
- 模块压缩解压（`MODULE_INIT_COMPRESSED_FILE` 只说明标志位）
- `irq_work`、softirq、中断线程的实现

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
