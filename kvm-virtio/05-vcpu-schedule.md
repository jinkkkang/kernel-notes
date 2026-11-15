# 5. vCPU 调度与阻塞

**结论**：一个 vCPU 是宿主机上调用 `KVM_RUN` 的那条线程。客户机执行 `HLT` 之后有两条完全不同的去向：有内核 LAPIC 时，线程留在内核里先 halt poll 再 `schedule()`；没有内核 LAPIC 时，ioctl 带着 `KVM_EXIT_HLT` 返回，睡不睡由用户态决定。宿主机调度器眼里的「可运行但没排上」记进 steal time。客户机 HLT 让线程进入 `TASK_INTERRUPTIBLE`，那是睡眠，不是 steal。

---

## 5.1 这条线程是谁

`kvm_vcpu_ioctl` 在 `KVM_RUN` 里如果 `vcpu->pid` 不是当前线程，就 `get_task_pid(current, PIDTYPE_PID)` 换上（`virt/kvm/kvm_main.c:4148`）。QEMU 通常每个 vCPU 一个线程，线程阻塞在 `KVM_RUN` 时，`ps` 里能看见的是这个线程，不是客户机里的 idle 任务。

`struct kvm_vcpu` 里和调度有关的成员（`include/linux/kvm_host.h:323`，片段，中间还有 mmio 和 async pf）：

```c
struct kvm_vcpu {
	struct kvm *kvm;
	int cpu;                         /* 最近一次加载到的宿主 CPU */
	int vcpu_id;                     /* 用户态创建时给的 id */
	int mode;                        /* OUTSIDE / IN / EXITING_GUEST_MODE */
	u64 requests;
	struct mutex mutex;
	struct kvm_run *run;
	struct rcuwait wait;             /* 非架构自定义等待队列时，block 睡在这里 */
	struct pid __rcu *pid;
	int sigset_active;
	sigset_t sigset;
	unsigned int halt_poll_ns;       /* 这个 vCPU 下一次 poll 的上限 */
	bool valid_wakeup;
	bool wants_to_run;
	bool preempted;                  /* 宿主抢占过这条线程 */
	bool ready;
	struct kvm_vcpu_arch arch;       /* x86 的 mp_state 在这里面 */
	/* ... */
};
```

x86 的 `vcpu_block`（`arch/x86/kvm/x86.c:11007`）在 `mp_state == KVM_MP_STATE_HALTED` 时调用 `kvm_vcpu_halt`，其他不可运行状态调用 `kvm_vcpu_block`。醒来之后如果仍然不可运行，返回 1，让 `vcpu_run` 再处理信号和请求，不把 `mp_state` 改回 `RUNNABLE`。变得可运行之后，`HALTED` 和 `AP_RESET_HOLD` 被改回 `KVM_MP_STATE_RUNNABLE`（`:11057`）。

---

## 5.2 HLT 的分叉

`kvm_emulate_halt`（`x86.c:9744`）先 `kvm_skip_emulated_instruction`，再 `kvm_emulate_halt_noskip`。后者进 `__kvm_emulate_halt`（`:9719`）：

```c
++vcpu->stat.halt_exits;
if (lapic_in_kernel(vcpu)) {
	vcpu->arch.mp_state = state;   /* HLT 时是 KVM_MP_STATE_HALTED */
	return 1;                      /* 留在 vcpu_run，下一步 vcpu_block */
} else {
	vcpu->run->exit_reason = reason; /* HLT 时是 KVM_EXIT_HLT */
	return 0;                        /* ioctl 结束 */
}
```

`lapic_in_kernel`（`arch/x86/kvm/lapic.h:187`）在静态键 `kvm_has_noapic_vcpu` 没有打开时直接返回真。创建 vCPU 时如果 `irqchip_in_kernel` 为假，不调用 `kvm_create_lapic`，并对这个静态键 `static_branch_inc`（`x86.c:12020`）。键打开之后，`lapic_in_kernel` 才看 `vcpu->arch.apic` 是否非空。

`irqchip_in_kernel`（`arch/x86/kvm/irq.h:90`）在 `irqchip_mode != KVM_IRQCHIP_NONE` 时为真。三种模式（`asm/kvm_host.h:1143`）：

| 模式 | 怎么来 | HLT |
|---|---|---|
| `KVM_IRQCHIP_NONE` | 默认。demo 停在这里 | 返回 `KVM_EXIT_HLT` |
| `KVM_IRQCHIP_KERNEL` | `KVM_CREATE_IRQCHIP` | 内核 LAPIC，线程在内核里 block |
| `KVM_IRQCHIP_SPLIT` | `KVM_CAP_SPLIT_IRQCHIP` | 仍有内核 LAPIC（`irqchip_in_kernel` 为真），IOAPIC 在用户态 |

所以「客户机执行了 HLT」不能读成「用户态会看到 `KVM_EXIT_HLT`」。QEMU 的普通虚拟机创建 irqchip 之后，HLT 不会把 ioctl 打回来。demo 故意不创建 irqchip，才用 `KVM_EXIT_HLT` 作为结束条件。另外 `KVM_X86_DISABLE_EXITS_HLT`（`include/uapi/linux/kvm.h:837`）会在 `kvm_hlt_in_guest` 为真时从 VMX 的执行控制里拿掉 `CPU_BASED_HLT_EXITING`（`vmx.c:4519`）。demo 不设置这个能力，默认仍拦截 HLT。

`kvm_emulate_halt` 在设置 `KVM_EXIT_HLT` 之前已经 `kvm_skip_emulated_instruction`。用户态看到这个退出时，RIP 指向 HLT 的下一字节。demo 那一页在两条指令之后是 0。再次 `KVM_RUN` 会从那个字节继续取指，而不是停在 HLT 上。demo 在第一次 `KVM_EXIT_HLT` 就返回，避免空转。用户态若要让客户机保持停机，就停在这次 ioctl 返回之后，直到它准备好新的 RIP 或中断模型。

---

## 5.3 halt poll 然后才睡眠

有内核 LAPIC 时，`kvm_vcpu_halt`（`kvm_main.c:3558`）先算本次允许的 poll 时间。全局默认在 x86 是 `KVM_HALT_POLL_NS_DEFAULT` 200000 纳秒（`arch/x86/include/asm/kvm_host.h:59`），模块参数 `halt_poll_ns` 定义在 `kvm_main.c:80`。每个 vCPU 还有自己的 `halt_poll_ns`，由 `grow_halt_poll_ns` / `shrink_halt_poll_ns` 按 `halt_poll_ns_grow` 和 `halt_poll_ns_shrink` 调整（`:3417`、`:3436`）。VM 也可以用 `KVM_CAP_HALT_POLL` 覆盖（`:4701` 一带）。

poll 窗口里线程不睡眠：循环调用 `kvm_vcpu_check_block`（`:3455`），中间 `cpu_relax()`。`kvm_vcpu_check_block` 在 vCPU 又变得可运行、有待处理定时器、当前线程有信号、或有 `KVM_REQ_UNBLOCK` 时返回 `-EINTR`。窗口内变成可运行，这次 poll 算成功，`halt_successful_poll` 增加，线程没有进入调度器的睡眠统计。

窗口用完仍不可运行，才 `kvm_vcpu_block`（`:3480`）：

1. `preempt_disable`，`kvm_arch_vcpu_blocking`，`prepare_to_rcuwait`。
2. 循环里 `set_current_state(TASK_INTERRUPTIBLE)`，再查一次 `kvm_vcpu_check_block`，然后 `schedule()`。
3. 醒来后 `finish_rcuwait`，`kvm_arch_vcpu_unblocking`。

`kvm_vcpu_wake_up`（`:3631`）在 `__kvm_vcpu_wake_up` 成功时把 `vcpu->ready` 设为真，并增加 `halt_wakeup`。`kvm_vcpu_kick`（`:3647`）先试唤醒；线程若已经在客户机里（`IN_GUEST_MODE`），把 mode 改成 `EXITING_GUEST_MODE` 或向所在 CPU 发 IPI，让它退出客户机后再看请求。kick 的是宿主机线程，客户机里的 idle 任务不会因此变成一条宿主机任务。

`kvm_vcpu_check_block` 看到 `signal_pending(current)` 就打破睡眠。用户态因此能用信号把堵在 HLT 里的 `KVM_RUN` 拉回来。`xfer_to_guest_mode_work` 在信号未屏蔽时写 `KVM_EXIT_INTR` 并返回 `-EINTR`（[03](./03-kvm-run.md)）。单次 `SIGALRM` 若在进入 `ioctl` 之前就被处理掉，这次 `KVM_RUN` 不会再看见它。demo 在 `KVM_RUN` 之外用 `sigprocmask` 挡住 `SIGALRM`，再把 8 字节的空 `kvm_signal_mask` 交给 `KVM_SET_SIGNAL_MASK`。`kvm_sigset_activate`（`virt/kvm/kvm_main.c:3394`）在 ioctl 里换上这张空掩码，到期的闹钟保持 pending，到这时才投递。`KVM_CAP_IMMEDIATE_EXIT` 非 0 时，处理函数还把 `kvm_run->immediate_exit` 置 1；这个字节只在下一次进入 `kvm_vcpu_ioctl` 时被读（`:4163`），不能单独打断已经睡在 `schedule` 里的那一次。

---

## 5.4 抢占、超售、steal

宿主机把这条 vCPU 线程换下来，有两种账：

- 线程处于 `TASK_INTERRUPTIBLE`（halt block）或 `TASK_UNINTERRUPTIBLE`：它不在运行队列上。`sched_switch` 里前一个状态是睡眠（`S` 或 `D`），不是「等 CPU」。这段时间客户机认为自己在 HLT，宿主机没有把它算进下面的 steal 增量。
- 线程是 `TASK_RUNNING` 但 CPU 被别的任务占着：调度器把它累加进 `current->sched_info.run_delay`。`record_steal_time`（`x86.c:3535`）在 `KVM_REQ_STEAL_UPDATE` 时读取这个计数。

`record_steal_time` 要求客户机已经用 `MSR_KVM_STEAL_TIME`（`arch/x86/include/uapi/asm/kvm_para.h:55`，值 `0x4b564d03`）启用了结构。GPA 经 `kvm_gfn_to_hva_cache_init` 变成 HVA 之后，函数把 `current->sched_info.run_delay - vcpu->arch.st.last_steal` 加进客户机结构的 `steal` 字段（`x86.c:3618`），并用 `version` 的奇偶做顺序：先写成奇数，`smp_wmb`，更新 `steal`，再写成偶数（`:3609`）。`preempted` 字节同时清掉。结构本身：

```c
/* arch/x86/include/uapi/asm/kvm_para.h:62 */
struct kvm_steal_time {
	__u64 steal;       /* 纳秒级的累加，来源是宿主机 run_delay 的增量 */
	__u32 version;     /* 奇数表示正在写 */
	__u32 flags;
	__u8  preempted;   /* KVM_VCPU_PREEMPTED 或 KVM_VCPU_FLUSH_TLB */
	__u8  u8_pad[3];
	__u32 pad[11];
};
```

客户机内核读这个结构，用来显示自己被宿主机抢走的时间。它和客户机内部的 idle 时间（HLT 或 idle 任务）是两套数。超售时多条 vCPU 线程抢同一宿主 CPU，`run_delay` 变大，steal 增加，同时这些线程仍可能大部分时间停在 `KVM_RUN` 里而不是停在 HLT。把宿主机 `sched_stat_wait` 和客户机 `/proc/stat` 的 idle 当成同一个数，会对错账。

`vcpu->preempted` 是 KVM 自己的标志，表示这条 vCPU 线程被抢占过。定向让步（`kvm_vcpu_on_spin`，`kvm_main.c` 里在 halt 之外的 PAUSE 退出上使用）会看别的 vCPU 是不是 `preempted` 并且不在内核态，再 `yield_to`。那是锁持有者被抢占时的让步，不是 HLT 的睡眠。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← VM entry/exit](./04-vm-entry-exit.md) · [目录](./README.md) · [客户机内存 →](./06-guest-memory.md)
