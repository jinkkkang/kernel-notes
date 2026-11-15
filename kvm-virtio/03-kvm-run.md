# 3. KVM_RUN 主循环

**结论**：`KVM_RUN` 在宿主机内核里先处理「上次交给用户态的 IO 还没做完」和 vCPU 请求，然后才有可能把 CPU 送进客户机。循环的返回值 `<= 0` 时 ioctl 结束。返回值 `1` 表示这次退出已经在内核里处理完，循环再试一次进入。信号把 `exit_reason` 设成 `KVM_EXIT_INTR` 并让 ioctl 返回 `-EINTR`，此时客户机指令没有继续执行。

---

## 3.1 从 ioctl 到架构循环

```text
kvm_vcpu_ioctl                         virt/kvm/kvm_main.c:4117
  case KVM_RUN                         :4144
    arg 非 0 → -EINVAL                 :4146
    线程变了就更新 vcpu->pid           :4148
    wants_to_run = !immediate_exit     :4163
    kvm_arch_vcpu_ioctl_run            arch/x86/kvm/x86.c:11227
    wants_to_run = false
    trace_kvm_userspace_exit           kvm_main.c:4167
```

`kvm_vcpu_ioctl` 开头如果 `vcpu->kvm->mm != current->mm` 或 VM 已死，直接 `-EIO`（`:4126`）。然后 `mutex_lock_killable(&vcpu->mutex)`，被信号打断返回 `-EINTR`（`:4140`），这次还没有进 `kvm_arch_vcpu_ioctl_run`。

`kvm_arch_vcpu_ioctl_run`（`x86.c:11227`）的顺序：

1. `vcpu_load`，`kvm_sigset_activate`（若用户态设过 `KVM_SET_SIGNAL_MASK`），把 `kvm_run->flags` 清 0，换上客户机 FPU（`kvm_load_guest_fpu`，`:11213`）。
2. `mp_state == KVM_MP_STATE_UNINITIALIZED` 时不进客户机，调用 `kvm_vcpu_block`（`:11256`）。这是等 SIPI 的 AP，不是已经在跑的 BSP。醒了之后若仍有信号，写 `KVM_EXIT_INTR` 并返回 `-EINTR`（`:11264`）。
3. 若 `kvm_run->kvm_dirty_regs` 置位，`sync_regs` 把用户态刚写的寄存器同步进来（`:11278`）。
4. **没有**内核 LAPIC 时，用 `kvm_run->cr8` 调 `kvm_set_cr8`（`:11285`）。有内核 LAPIC 时这条跳过，TPR 在 LAPIC 模型里。
5. 若 `vcpu->arch.complete_userspace_io` 非空，先调用它并清掉指针（`:11307`）。返回值 `<= 0` 就从这次 ioctl 回去。这是 PIO/MMIO 的后半段，见 3.3。
6. `wants_to_run` 为假（用户态把 `immediate_exit` 设成非 0）时返回 `-EINTR`（`:11318`），不进 `vcpu_run`。
7. `kvm_x86_vcpu_pre_run` 之后进入 `vcpu_run`（`:11326`）。
8. 出口处恢复用户态 FPU，按 `kvm_valid_regs` 把寄存器存回 `kvm_run`，`kvm_sigset_deactivate`，`vcpu_put`。

第 5 步发生在第 7 步之前。所以上一次 `KVM_EXIT_IO` 对应的指令，要到下一次 `KVM_RUN` 的开头才被跳过。用户态只是读了 PIO 缓冲区，并没有完成这条指令。

---

## 3.2 `vcpu_run`：可运行和不可运行

```c
/* arch/x86/kvm/x86.c:11076，判断用的是 mp_state，不是「线程在不在内核」 */
static inline bool kvm_vcpu_running(struct kvm_vcpu *vcpu)
{
	return (vcpu->arch.mp_state == KVM_MP_STATE_RUNNABLE &&
		!vcpu->arch.apf.halted);
}
```

`vcpu_run`（`:11083`）是一个 `for (;;)`：

- `kvm_vcpu_running` 为真：`vcpu_enter_guest`（`:10593`）。
- 为假：`vcpu_block`（`:11007`）。有内核 LAPIC 的 HLT 把 `mp_state` 改成 `KVM_MP_STATE_HALTED` 之后会走这里，见 [05](./05-vcpu-schedule.md)。
- 返回值 `<= 0`：跳出循环，ioctl 结束。
- 返回值大于 0：清 `KVM_REQ_UNBLOCK`，必要时注入 Xen 事件和定时器中断。若用户态请求了中断窗口且现在能注入，写 `KVM_EXIT_IRQ_WINDOW_OPEN` 并跳出（`:11113`）。
- `__xfer_to_guest_mode_work_pending()` 为真时，暂时放下 SRCU，调用 `xfer_to_guest_mode_handle_work`（`:11121`）。

`xfer_to_guest_mode_work`（`kernel/entry/kvm.c:6`）在 `:11` 看到 `_TIF_SIGPENDING` 或 `_TIF_NOTIFY_SIGNAL` 就调用 `kvm_handle_signal_exit` 并返回 `-EINTR`。`kvm_handle_signal_exit`（`include/linux/kvm_host.h:2307`）在 `:2309` 把 `exit_reason` 设成 `KVM_EXIT_INTR`，`signal_exits` 加一。同一函数里 `_TIF_NEED_RESCHED` 只是 `schedule()`，调度完再看标志，不会仅仅因为要重新调度就结束 ioctl。

`vcpu_enter_guest` 在真正执行 `vmx_vcpu_run` 之前，如果 `kvm_request_pending(vcpu)`，会按请求位处理一批事情（`x86.c:10604` 起）。和本章直接有关的几位：

| 请求 | 定义位置 | 在进入客户机之前做什么 |
|---|---|---|
| `KVM_REQ_VM_DEAD` | `kvm_host.h:167` | 返回 `-EIO` |
| `KVM_REQ_CLOCK_UPDATE` | `asm/kvm_host.h:77` | `kvm_guest_time_update` |
| `KVM_REQ_STEAL_UPDATE` | `asm/kvm_host.h:81` | `record_steal_time` |
| `KVM_REQ_EVENT` | `asm/kvm_host.h:79` | `kvm_apic_accept_events`，再尝试注入 |
| `KVM_REQ_TRIPLE_FAULT` | 同文件的架构请求区 | 可以写成 `KVM_EXIT_SHUTDOWN` 并返回 0 |
| `KVM_REQ_REPORT_TPR_ACCESS` | 同上 | 写成 `KVM_EXIT_TPR_ACCESS` 并返回 0 |

这些请求处理完，函数仍可能 `goto out` 返回 0，ioctl 结束，CPU 没有执行客户机指令。`KVM_REQ_EVENT` 处理之后还要 `kvm_mmu_reload`（`:10778`）。失败则取消注入并返回。

关中断之后，函数做 `smp_store_release(&vcpu->mode, IN_GUEST_MODE)`（`:10795`）。紧接着再查一次 `kvm_vcpu_exit_request`（`:2166`）：`mode == EXITING_GUEST_MODE`，或仍有请求，或 `xfer_to_guest_mode_work_pending()`。任一为真就把 mode 改回 `OUTSIDE_GUEST_MODE`，返回 1，让 `vcpu_run` 再转一圈。mode 的三个值在 `include/linux/kvm_host.h:274`：`OUTSIDE_GUEST_MODE`、`IN_GUEST_MODE`、`EXITING_GUEST_MODE`。

通过这次检查之后才调用 `static_call(kvm_x86_vcpu_run)`（`:10883`）。那是 [04](./04-vm-entry-exit.md) 的 VM-entry。

---

## 3.3 用户态 IO 的后半段挂在下一次 RUN

`emulator_pio_in_out` 决定不了端口时，把数据拷进 `pio_data`，填好 `kvm_run->io`，返回 0（`userspace_io` 从 `x86.c:7948` 起，第一行赋值在 `:7949`）。`kvm_fast_pio_out`（`:9178`）在这个失败上**不**立刻跳过指令，而是记下线性 RIP（`:9197`），把 `complete_userspace_io` 设成 `complete_fast_pio_out`（`:9198`）。端口 `0x7e` 且开着 `KVM_X86_QUIRK_OUT_7E_INC_RIP` 是例外：它在退出前就 `kvm_skip_emulated_instruction`（`:9195`）。demo 使用 `0x3f8`，走的是一般路径。

下一次 `KVM_RUN` 进来时，`complete_fast_pio_out`（`:9168`）把 `pio.count` 清 0。线性 RIP 没被用户态改掉的话，调用 `kvm_skip_emulated_instruction`（`:8781`），返回 1，`vcpu_run` 才去执行下一条客户机指令。IN 的对应函数是 `complete_fast_pio_in`（`:9203`）：它从 `pio_data` 读回用户态写下的值，写进 RAX，再跳过指令。OUT 不需要用户态把缓冲区写回去，数据在退出前已经由 `memcpy` 放进 `pio_data`（`:7957`）。

MMIO 的多段访问用 `complete_emulated_mmio`（`:11164`）。写完一段仍可能再次把 `exit_reason` 设成 `KVM_EXIT_MMIO` 并返回 0，所以一次客户机指令可以对应多次 ioctl 返回。

---

## 3.4 返回用户态时日志里有什么

`trace_kvm_userspace_exit` 的字段是 `reason` 和 `errno`（`include/trace/events/kvm.h:22`）。打印时 `errno < 0` 且等于 `-EINTR` 显示 `restart`，其他负值显示 `error`，否则打印 `exit_reason` 的符号。它在 `kvm_arch_vcpu_ioctl_run` 返回之后无条件调用（`kvm_main.c:4167`）。因此这条 trace 表示 ioctl 结束，不表示刚才发生过 VM-exit。VM-exit 的 trace 是 `kvm_exit`，在进入客户机之后才会有，见 [14](./14-observability.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 用户态 API](./02-userspace-api.md) · [目录](./README.md) · [VM entry/exit →](./04-vm-entry-exit.md)
