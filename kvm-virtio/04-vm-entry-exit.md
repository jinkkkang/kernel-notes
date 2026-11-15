# 4. VM entry 与 VM exit

**结论**：x86 上一次真正执行客户机指令，是 `vcpu_enter_guest` 里那次 `static_call(kvm_x86_vcpu_run)`。Intel 的实现 `vmx_vcpu_run` 把客户机 RIP/RSP 写入 VMCS，再进 `vmx_vcpu_enter_exit`。VM-exit 回到这个调用之后，`kvm_x86_handle_exit` 的返回值决定是再进客户机还是结束 `KVM_RUN`。外部中断和补好的 EPT violation 通常返回 1。需要用户态看的 IO，以及没有内核 LAPIC 时的 HLT，返回 0。

> AMD 用 VMCB 和 `VMRUN`，退出码不同，但 PIO 和 HLT 最后仍进 `kvm_fast_pio` 与 `kvm_emulate_halt`。EPT 怎样填表见 [memory-management/11](../memory-management/11-kvm-memory.md)，这里只写退出处理函数把 GPA 交给谁。

---

## 4.1 Intel：从 VMCS 到 VMLAUNCH/VMRESUME

`vmx_vcpu_run`（`arch/x86/kvm/vmx/vmx.c:7328`）在进入之前：

- 客户机状态被标成必须模拟（`emulation_required`）时，不执行 VM-entry。它合成 `EXIT_REASON_INVALID_STATE`，`failed_vmentry = 1`，返回 `EXIT_FASTPATH_NONE`（`:7344`）。后面的退出处理会看见一次失败的进入。
- `trace_kvm_entry`（`:7356`）。事件字段是 `vcpu_id`、`rip`、`immediate_exit`（`arch/x86/kvm/trace.h:17`）。
- RIP 或 RSP 在 `regs_dirty` 里时写 `GUEST_RIP` / `GUEST_RSP`，然后清掉 dirty 位（`:7369`）。
- 需要时刷新 `HOST_CR3` 和 `HOST_CR4`（`:7388`）。这是宿主机进入客户机时硬件要恢复的宿主状态，不是客户机页表。
- 注释写明真正的 VMENTER/EXIT 在 `.noinstr.text`（`:7423`）。调用是 `vmx_vcpu_enter_exit`（`:7274`），里面 `vmx->fail = __vmx_vcpu_run(...)`（`:7298`）。

`vcpu_enter_guest` 在这次调用返回后把 `vcpu->mode` 写成 `OUTSIDE_GUEST_MODE`（`x86.c:10927`），做 `kvm_x86_handle_exit_irqoff`，打开中断，把 `stat.exits` 加一（`:10961`），再 `static_call(kvm_x86_handle_exit)`（`:10993`）。`handle_exit` 的返回值就是 `vcpu_enter_guest` 的返回值。

快速路径 `exit_fastpath == EXIT_FASTPATH_REENTER_GUEST` 时，`vcpu_enter_guest` 的内层循环不走到 `handle_exit`，直接再调 `kvm_x86_vcpu_run`（`x86.c:10883`）。注释说明走慢路径的退出才在后面计入 `stat.exits`（`:10897`）。所以「发生过 VM-exit」和「`KVM_RUN` 返回」之间还隔着一条可以立刻再进入的路径。

---

## 4.2 退出处理：返回 1 还是返回 0

`vmx.c:6106` 的注释写明约定：处理函数返回 1，退出已经处理完，客户机可以继续；否则设置 `kvm_run`，返回 0，让用户态接着做。`__vmx_handle_exit` 在快速路径不是 `EXIT_FASTPATH_NONE` 时直接返回 1（`:6574`）。否则按 `exit_reason.basic` 查 `kvm_vmx_exit_handlers[]`（`:6111`）。不认识的原因写成 `KVM_EXIT_INTERNAL_ERROR` / `KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON`（`:6605`）。

和本章主线有关的几项：

| VMX 原因 | 处理函数 | 返回值意味着什么 |
|---|---|---|
| `EXIT_REASON_EXTERNAL_INTERRUPT` | `handle_external_interrupt`（`:5347`） | 只增加 `irq_exits`，返回 1。宿主机开中断时会消化这只 IRQ，然后再次 VM-entry。QEMU 看不到这次退出 |
| `EXIT_REASON_EPT_VIOLATION` | `handle_ept_violation`（`:5745`） | 从 VMCS 读 GPA，`trace_kvm_page_fault`，返回 `kvm_mmu_page_fault(...)`（`:5796`）。缺的是第二级翻译时，填好 SPTE 后返回 1，客户机重试同一条指令 |
| `EXIT_REASON_IO_INSTRUCTION` | `handle_io`（`:5360`） | 字符串 IO 走 `kvm_emulate_instruction`。单次 IO 取 qualification 里的端口、长度、方向，调用 `kvm_fast_pio`（`:5378`） |
| `EXIT_REASON_HLT` | 表项直接是 `kvm_emulate_halt`（`:6123`） | 见 [05](./05-vcpu-schedule.md)。有内核 LAPIC 时返回 1 并改 `mp_state`；没有时返回 0 并写 `KVM_EXIT_HLT` |
| `EXIT_REASON_TRIPLE_FAULT` | `handle_triple_fault`（`:5353`） | 写 `KVM_EXIT_SHUTDOWN`，返回 0 |

`handle_io` 从 exit qualification 取出 `string`、`port`、`size`、`in`（`:5366`）。`size` 按 `(qualification & 7) + 1` 计算（`:5375`）。x86 的端口指令用 1、2 或 4 字节；demo 只接受这三档，并且这次客户机指令是 1 字节。字符串指令（`INS`/`OUTS`）不走 `kvm_fast_pio`。

`kvm_fast_pio`（`x86.c:9245`）在 `emulator_pio_*` 返回非 0（内核总线已经完成这次端口访问）时再 `kvm_skip_emulated_instruction`，整次退出对用户态不可见。返回 0 时，`kvm_run->exit_reason` 已经是 `KVM_EXIT_IO`，处理函数把 0 传回 `vcpu_run`，ioctl 结束。用户态看到的是这次返回，不是 VM-exit 指令本身。

EPT misconfig 和其他必须模拟的指令也在同一张表里。它们返回 1 的时候，客户机侧的时间是停的，宿主机线程仍在同一次 `KVM_RUN` 里。把这类退出算进「QEMU 的退出次数」会对不上 `kvm_userspace_exit`。

---

## 4.3 AMD SVM 上不同的那几处

`svm_vcpu_run`（`arch/x86/kvm/svm/svm.c:4260`）同样先 `trace_kvm_entry`，然后把 RAX/RSP/RIP 写进 VMCB 的 save 区（`:4268`）。真正的 `VMRUN` 在 `svm_vcpu_enter_exit`（`:4230`）里的 `__svm_vcpu_run`（`:4253`）。SEV-ES 走 `__svm_sev_es_vcpu_run`，本章不展开加密保存区。

退出分发表里，`SVM_EXIT_HLT` 的处理函数也是 `kvm_emulate_halt`（`:3394`），`SVM_EXIT_IOIO` 是 `io_interception`（`:3397`）。`io_interception`（`:2233`）从 `exit_info_1` 取方向、大小和端口，字符串 IO 在非 SEV-ES 时 `kvm_emulate_instruction`，否则把 `next_rip` 记下来再 `kvm_fast_pio`（`:2253`）。因此用户态看见的 `kvm_run->io` 布局不因 VMX/SVM 而换成另一套结构；差别在退出原因码和 qualification 的位置。

`vcpu_block` 里有一句架构差异（`x86.c:11042`）：嵌套时 AMD 没有 VMCS12 那种 activity state，VM-exit 会把 HLT 里的 L2 弄醒。这只影响嵌套客户机的 HLT 记录，不改变 L0 上 `kvm_emulate_halt` 对 `lapic_in_kernel` 的分支。

SVM 的加速中断是 AVIC，不是 VMX 的 posted-interrupt PIR。相关 trace 从 `kvm_avic_incomplete_ipi`（`arch/x86/kvm/trace.h:1426`）起。注入条件放在 [07](./07-irq-clock.md)，这里不把 AVIC 的状态机展开。

---

## 4.4 和 demo 对上的两次退出

demo 的客户机字节是 `OUT DX, AL`（`0xEE`）然后 `HLT`（`0xF4`），端口 `0x3f8`。没有注册 in-kernel 设备时，`kvm_io_bus_write` 失败，`handle_io` → `kvm_fast_pio_out` 写 `KVM_EXIT_IO` 并返回 0。用户态读到方向 `KVM_EXIT_IO_OUT`（数值 1）、`size == 1`、`count == 1`。它再次 `KVM_RUN` 时，`complete_fast_pio_out` 跳过这条 OUT，客户机执行 HLT。demo 不创建 irqchip，HLT 把 `KVM_EXIT_HLT` 交回用户态。若这台机器的 VMX 因客户机状态不合法而失败进入，用户态会看到 `KVM_EXIT_FAIL_ENTRY` 或 `KVM_EXIT_INTERNAL_ERROR`，demo 把这种情况当错误，不打印 `result=ok`。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← KVM_RUN](./03-kvm-run.md) · [目录](./README.md) · [vCPU 调度 →](./05-vcpu-schedule.md)
