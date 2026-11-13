# 1. 总览

**结论**：内核从 `start_kernel` 走到用户态 init 是一条启动链；运行中再装代码走 `finit_module`；可以睡觉的推迟工作走 cmwq。三条链在 `do_one_initcall` 和 worker 线程上接上，选型表不在本章。

> 启动：`init/main.c`。模块：`kernel/module/main.c`。cmwq：`kernel/workqueue.c`、`include/linux/workqueue.h`。

---

## 1.1 三条线

```text
start_kernel                         init/main.c:853
  sched_init                         :923
  workqueue_init_early               :942     可以创建队列、可以排队，还没有 kworker
  local_irq_enable                   :978
  arch_call_rest_init                :1051
      rest_init                      :664
        user_mode_thread(kernel_init)  → pid 1
        kernel_thread(kthreadd)        → pid 2
        kernel_init
          kernel_init_freeable       :1501
            workqueue_init           :1515    造出第一批 kworker
            do_pre_smp_initcalls     :1520    early
            smp_init / sched_init_smp
            workqueue_init_topology  :1526
            do_basic_setup → do_initcalls     pure … late，rootfs 夹在 fs 与 device 之间
          free_initmem
          system_state = SYSTEM_RUNNING
          kernel_execve              用户态 init

运行时：
  finit_module → load_module → do_init_module → do_one_initcall(mod->init)

可睡眠的推迟工作：
  alloc_workqueue / schedule_work
    → __queue_work → pool_workqueue → worker_pool->worklist
    → worker_thread → process_one_work → work->func
```

`workqueue_init_early`（`kernel/workqueue.c:6645`）的注释写明：早期就可以创建 workqueue、排队和取消，工作项真正执行要等 kthread 能被创建，也就是 `workqueue_init()` 之后。`schedule_work`（`include/linux/workqueue.h:626`）把工作项放进 `system_percpu_wq`，不是旁边那个注释写着将要删掉的 `system_wq`。

---

## 1.2 和已经写过的章怎么接

| 问题 | 去哪 |
|------|------|
| 硬中断里能不能 `schedule_work`，要不要改用中断线程或 softirq | [interrupts/09](../interrupts/09-deferred-work-choice.md) 的选择表。本章从「已经决定用 workqueue」往下写 pool |
| pid 1 为什么没有 `PF_KTHREAD`，kthreadd 为什么有 | [process-management/05](../process-management/05-kernel-threads.md)。`rest_init` 的两次调用在 [03](./03-rest-init-and-pid1.md) |
| `kernel_execve` 成功之后地址空间怎么换 | [process-management/06](../process-management/06-exec.md)。本章停在 `bprm_execve` |
| `__schedule` 选哪个 kworker | [scheduler/](../scheduler/README.md) |
| 用 ftrace 看 initcall 或 `workqueue_execute_start` 的环 | [tracing/](../tracing/README.md)。事件名在 [11](./11-observability.md) |

---

## 1.3 本章写到哪

写：`start_kernel` 到 `SYSTEM_RUNNING` 的顺序、initcall 级别、`struct module` 与装入 / 卸下、`__request_module` 交到 modprobe、cmwq 的四个对象和入队标志、观测点和 watchdog 的配方。

不写：systemd 的 unit、livepatch 状态机、模块签名算法、kexec/kdump、中断线程和 `irq_work` 的实现。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[目录](./README.md) · [start_kernel →](./02-start-kernel.md)
