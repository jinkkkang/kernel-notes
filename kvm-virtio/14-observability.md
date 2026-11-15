# 14. 观测与性能

**结论**：一次 virtio 读的等待可以落在四层里，层与层的事件不一样。VM-exit 用 `kvm_exit`；ioctl 是否返回用 `kvm_userspace_exit`；vCPU 线程睡觉用 `sched_switch` 的状态和 `kvm_vcpu_wakeup`；队列通知是 ioeventfd / kick，完成是 call fd 和客户机回调。把 `kvm_exit` 的次数当成 QEMU 的退出次数，或把客户机 idle 当成宿主机的 `sched_stat_wait`，会把账记错。下面的事件名和字段按 v6.6.160 的 `TRACE_EVENT` 核对。命令没有执行。写作环境（`sampled on 6.12.113+`，`systemd-detect-virt` 为 kvm）没有 `perf` 二进制；`/sys/kernel/tracing/events/kvm` 对 uid 1000 是 Permission denied。这只说明这次没读到跟踪文件，不说明 v6.6.160 没有这些事件。

---

## 14.1 四层分别看什么

| 层 | 问题 | 看哪个事件 | 不要把它读成 |
|---|---|---|---|
| 客户机指令退出 | CPU 是否离开 non-root | `kvm:kvm_exit` 再配 `kvm:kvm_entry` | `kvm:kvm_userspace_exit`。后者才是 ioctl 返回 |
| vCPU 线程 | 线程是在跑、在 poll，还是在睡 | `sched:sched_switch` 的 `prev_state`；`kvm:kvm_vcpu_wakeup`；`kvm:kvm_halt_poll_ns` | 客户机 `/proc/stat` 的 idle |
| 队列通知 | kick 有没有发生 | 客户机侧是一次 PIO/MMIO（`kvm:kvm_pio` / `kvm:kvm_mmio`）。ioeventfd 命中时往往没有 `kvm_userspace_exit` | eventfd 计数本身不是描述符内容。v6.6.160 的 `drivers/virtio` 与 `drivers/vhost` 里没有 virtqueue kick 的 `TRACE_EVENT` |
| 后端等待 | worker 或用户态后端卡在哪 | vhost worker 的 `sched_switch`；网络后端再接到 [networking/14](../networking/14-virtio-net-host-vhost.md) 的 tap | vCPU 线程的睡眠。后端是另一条线程 |

`kvm_entry`（`arch/x86/kvm/trace.h:17`）在 `vmx_vcpu_run` / `svm_vcpu_run` 的开头。字段：`vcpu_id`、`rip`、`immediate_exit`。打印是 `vcpu %u, rip 0x%lx`，立即退出时带 `[immediate exit]`。

`kvm_exit` 由 `TRACE_EVENT_KVM_EXIT` 生成（`trace.h:297` 与 `:336`）。字段：`exit_reason`、`guest_rip`、`isa`、`info1`、`info2`、`intr_info`、`error_code`、`vcpu_id`。`exit_reason` 经 `kvm_x86_get_exit_info` 填充，VMX 上是 VMCS 的退出原因，不是 `kvm_run->exit_reason` 那套 `KVM_EXIT_*` 编号。IO 退出在这里是硬件原因，在 `kvm_pio` 里才是端口号。

`kvm_userspace_exit`（`include/trace/events/kvm.h:22`）字段是 `reason` 和 `errno`。`errno < 0` 且为 `-EINTR` 时打印 `restart`，其他负值打印 `error`，否则打印 `reason` 的符号。它在 `kvm_vcpu_ioctl` 从 `kvm_arch_vcpu_ioctl_run` 返回之后触发（`virt/kvm/kvm_main.c:4167`）。一次 `kvm_exit` 若处理函数返回 1，不会伴随这条事件。

`kvm_pio`（`trace.h:161`）字段：`rw`、`port`、`size`、`count`、`val`。`KVM_PIO_OUT` 是 1，打印成 `pio_write`；`KVM_PIO_IN` 是 0，打印成 `pio_read`（`:158`）。`val` 只采样第一笔数据。`count > 1` 时打印带 `(...)`。

`kvm_mmio`（`include/trace/events/kvm.h:213`）在通用头里。写观测脚本前用 `perf list` 或调试文件系统确认本机编进了它；本章不把没打开的 trace 文件内容写出来。

---

## 14.2 halt poll 和调度

`kvm_halt_poll_ns`（`include/trace/events/kvm.h:347`）字段：`grow`、`vcpu_id`、`new`、`old`。打印是 `vcpu %u: halt_poll_ns %u (grow|shrink %u)`。`grow` 为真表示窗口变大。它在 `grow_halt_poll_ns` / `shrink_halt_poll_ns` 里触发，不表示这次 HLT 睡了多久。

`kvm_vcpu_wakeup`（`:43`）字段：`ns`、`waited`、`valid`。打印把 `waited` 为真写成 `wait`，为假写成 `poll`，`valid` 写成 `valid` 或 `invalid`。`waited == false` 是 poll 窗口内变得可运行，线程没有 `schedule()`。`waited == true` 才是从 `kvm_vcpu_block` 醒来。`ns` 是这段等待或 poll 的纳秒，不是 steal。

`sched_switch`（`include/trace/events/sched.h:222`）字段：`prev_comm`、`prev_pid`、`prev_prio`、`prev_state`、`next_comm`、`next_pid`、`next_prio`。`prev_state` 在被抢占时带 `+` 且状态打成 `R` 的那条特殊值（`TASK_REPORT_MAX`，`:204`）。睡眠的 `kvm_vcpu_block` 把线程设成 `TASK_INTERRUPTIBLE`，打印里是 `S`。`S` 是客人机 HLT（有内核 LAPIC）把 vCPU 线程送进睡眠；`R+` 是线程仍可运行时被换下，对应 steal 用的 `run_delay`，不是 HLT。

`sched_wakeup` / `sched_waking`（`:170`、`:178`）字段：`comm`、`pid`、`prio`、`target_cpu`。irqfd 或定时器把 vCPU 线程叫醒时，可以看到对那条 `pid` 的 wakeup。`vcpu->pid` 才是这条线程，不要拿客户机里的 pid 去对。

`sched_stat_wait`、`sched_stat_sleep`、`sched_stat_iowait`、`sched_stat_blocked` 包在 `CONFIG_SCHEDSTATS` 里（`sched.h:424`）。没开这个选项时它们是 `DEFINE_EVENT_NOP`，跟踪文件系统里不会出现。打开时字段是 `comm`、`pid`、`delay`（纳秒）。源码注释：`sched_stat_wait` 是可运行但没排上 CPU（`:460`），`sched_stat_sleep` 是不可运行（`:467`）。HLT block 落在 sleep，超售落在 wait。

---

## 14.3 通知和后端

客户机 `vp_notify` 的 `iowrite16` 产生 MMIO 退出。若 ioeventfd 认领，`kvm_exit` 仍然有，`kvm_userspace_exit` 没有，vhost 或 QEMU 的另一条线程在 kick fd 上从阻塞变成可运行。这条线程的 `comm` 对内核 vhost 是 `vhost_worker_create` 起的 `vhost-<pid>`（`drivers/vhost/vhost.c:688`）。

完成侧看 call eventfd 的对端。irqfd 注入时可以有 `kvm:kvm_set_irq`（`include/trace/events/kvm.h:66`，包在 `CONFIG_HAVE_KVM_IRQFD` 里）。字段：`gsi`、`level`、`irq_source_id`。边沿 irqfd 的工作项会对同一 GSI 打出 level 1 和 level 0 两条（`virt/kvm/eventfd.c:50`）。APICv 是否真的避免了 VM-exit，要看随后有没有对应向量的退出，以及 `kvm_apicv_inhibit_changed`（`arch/x86/kvm/trace.h:1378`）里的抑制位。posted interrupt 更新 IRTE 的事件是 `kvm_pi_irte_update`（`:1079`）。IRTE 字段的含义留在中断章。

后端自己的等待不在上述 KVM 事件里。vhost-net 对 tap 的 `sendmsg` 堵住时，卡住的是 vhost worker，vCPU 线程可能已经回到客户机或停在下一次 kick 之前。网络章的 RECIPE 覆盖 tap 之后的 qdisc；本章不重写那条 skb 路径。

---

## 14.4 命令（RECIPE，未执行）

写作环境没有 `perf`，也没有读到 `events/kvm` 目录。下面的命令是配方，不附带输出。事件名以上一节的源码为准。跑的时候把采样标成 `sampled on <uname -r>`，并且和 v6.6.160 的行号分开。不要在生产机上长时间 `perf record -a`。

退出分层，短睡 1 秒，只为确认事件能打开：

```text
perf record -e kvm:kvm_entry,kvm:kvm_exit,kvm:kvm_userspace_exit,kvm:kvm_pio -a -- sleep 1
perf script
```

读法：`kvm_exit` 的 `reason` 是硬件退出原因。同一时间附近若没有 `kvm_userspace_exit`，这次退出留在内核。`kvm_pio` 的 `port`/`size`/`rw` 对上客户机的 OUT/IN。`kvm_userspace_exit` 的 `reason` 才是 `KVM_EXIT_*`。

halt poll 和睡眠：

```text
perf record -e kvm:kvm_halt_poll_ns,kvm:kvm_vcpu_wakeup,sched:sched_switch -a -- sleep 1
perf script
```

读法：`kvm_vcpu_wakeup` 里 `poll` 表示没进调度器。`wait` 再在 `sched_switch` 里找同一 pid 的 `prev_state=S`。`prev_state` 带 `+` 且为 `R` 的切换是抢占。`kvm_halt_poll_ns` 的 `grow`/`shrink` 只说明窗口变了，不说明客户机做了多少次 HLT。`stat.halt_exits` 在 debugfs 的 KVM 统计里，和这条 trace 不是同一个计数。

调度统计只有 `CONFIG_SCHEDSTATS` 打开时才有事件：

```text
perf record -e sched:sched_stat_wait,sched:sched_stat_sleep -a -- sleep 1
```

`delay` 的单位是纳秒。wait 对超售，sleep 对 HLT block。客户机里的 steal 是这些 wait 增量写进 `struct kvm_steal_time` 之后的结果，见 [05](./05-vcpu-schedule.md)，不要把 `delay` 直接抄成客户机的 steal 计数。

vhost 与 vCPU 是两条线程。先用 `ps` 找到 `vhost-` 前缀和 QEMU 的 vCPU 线程，再按 pid 过滤 `sched_switch`。没有 vhost 线程时不要编一个 `vhost-` 的 comm。virtio-blk 的用户态后端没有这个内核线程名。

`perf kvm stat report` 这类子命令能按退出原因汇总。本环境没有 `perf`，没有对 `perf kvm --help` 做过核对，所以不把子命令的旗标写死。要用它时先在目标机上看帮助，再记录实际印出的表头。

---

## 14.5 和 demo 的距离

demo 01 只有两次用户态退出：`KVM_EXIT_IO` 和 `KVM_EXIT_HLT`。它没有 irqchip，所以看不到 halt poll，也没有 virtqueue。用它的 stdout 不能验证本节的 trace 字段。backlog `KVM-MINIMAL-RUN` 只验收 demo 的 IO/HLT 行。本节的命令仍是 RECIPE，直到有一次真实的 `perf script` 原文贴回来。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← reset](./13-reset-migration.md) · [目录](./README.md) · [demos](./demos/README.md)
