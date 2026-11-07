# 2. `entry_SYSCALL_64` 与返回

**结论**：`syscall_init` 把 `MSR_LSTAR` 写成 `entry_SYSCALL_64`。这条入口做 `swapgs`，把用户 `RSP` 暂存在 `cpu_tss_rw.x86_tss.sp2`，再从 `pcpu_hot.top_of_stack` 取出线程内核栈，在上面搭 `pt_regs`，然后 `call do_syscall_64`。返回时能用 `SYSRET` 就走蹦床栈上的 `sysretq`，资格检查失败就并进 IRET 那条公共尾。

> 汇编在 `arch/x86/entry/entry_64.S:87`。MSR 初始化在 `arch/x86/kernel/cpu/common.c:2199`。栈顶偏移 `X86_top_of_stack` 由 `arch/x86/kernel/asm-offsets.c:117` 定义成 `pcpu_hot.top_of_stack`。

设备中断走 IDT，见 [interrupts/02](../interrupts/02-x86-entry.md)。本章只写 `syscall` 指令这条路。

---

## 2.1 `syscall_init` 写哪些 MSR

每颗 CPU 在标识初始化时调用 `syscall_init`（`common.c:2387` 附近的调用点）。函数本身从 `:2199` 开始：

```c
/* arch/x86/kernel/cpu/common.c:2199 */
void syscall_init(void)
{
	wrmsr(MSR_STAR, 0, (__USER32_CS << 16) | __KERNEL_CS);
	wrmsrl(MSR_LSTAR, (unsigned long)entry_SYSCALL_64);
	/* CONFIG_IA32_EMULATION 时 CSTAR = entry_SYSCALL_compat，
	 * 并写 SYSENTER 的 CS/ESP/EIP。否则 CSTAR = entry_SYSCALL32_ignore。
	 */
	wrmsrl(MSR_SYSCALL_MASK,
	       X86_EFLAGS_CF|X86_EFLAGS_PF|X86_EFLAGS_AF|
	       X86_EFLAGS_ZF|X86_EFLAGS_SF|X86_EFLAGS_TF|
	       X86_EFLAGS_IF|X86_EFLAGS_DF|X86_EFLAGS_OF|
	       X86_EFLAGS_IOPL|X86_EFLAGS_NT|X86_EFLAGS_RF|
	       X86_EFLAGS_AC|X86_EFLAGS_ID);
}
```

`wrmsr(MSR_STAR, 0, ...)` 的低 32 位是 0，高 32 位是 `(__USER32_CS << 16) | __KERNEL_CS`。64 位选择子在 `arch/x86/include/asm/segment.h:169` 起：

| 宏 | 计算 | 选择子 |
|---|---|---|
| `__KERNEL_CS` | `GDT_ENTRY_KERNEL_CS(2) * 8` | `0x10` |
| `__USER32_CS` | `GDT_ENTRY_DEFAULT_USER32_CS(4) * 8 + 3` | `0x23` |
| `__USER_DS` | `GDT_ENTRY_DEFAULT_USER_DS(5) * 8 + 3` | `0x2b` |
| `__USER_CS` | `GDT_ENTRY_DEFAULT_USER_CS(6) * 8 + 3` | `0x33` |

`segment.h:177` 的注释说明 SYSRET 把这些选择子焊死了：回到 32 位用户态时 `cs = STAR.SYSRET_CS`；回到 64 位用户态时 `cs = STAR.SYSRET_CS+16`；两种情况下 `ss = STAR.SYSRET_CS+8`。高半字是 `__USER32_CS`（`0x23`），所以 64 位 SYSRET 的 CS 是 `0x33`（`__USER_CS`），SS 是 `0x2b`（`__USER_DS`）。入口后面检查的就是这两个常数。

`MSR_SYSCALL_MASK` 里被清掉的标志包括 `IF` 和 `AC`。注释（`entry_64.S:64`）因此说进入时不必再 `CLD` / `CLAC`：方向标志和 SMAP 的 AC 已经被掩码清掉。`IF` 清掉表示 `do_syscall_64` 被调用时中断是关的，C 代码在进入工作里再打开。

`MSR_CSTAR` 只在厂商不是 Intel 时才写（`wrmsrl_cstar`，`:2187`）。注释写明 Intel 不支持 32 位 `SYSCALL`，写 `MSR_CSTAR` 会被忽略，但在 TDX 客户机里会触发 `#VE`。`CONFIG_IA32_EMULATION` 打开时目标是 `entry_SYSCALL_compat`，否则是 `entry_SYSCALL32_ignore`（`entry_64.S:1523`，直接把 `-ENOSYS` 放进 `eax` 再 `sysretl`）。SYSENTER 三件套同一函数里用 `wrmsrl_safe` 写，细节留在 [07](./07-compat.md)。

---

## 2.2 `pcpu_hot` 与 TSS 暂存

用户 `RSP` 不能直接扔掉：切到内核栈之后还要把它写进 `pt_regs->sp`。硬件 TSS 的 `sp2` 在 Linux 里空着，入口拿来当便签。

```c
/* arch/x86/include/asm/processor.h:286  64 位 x86_hw_tss */
struct x86_hw_tss {
	u32 reserved1;
	u64 sp0;          /* 返回用户态用的蹦床栈顶，见下文 TSS_sp0 */
	u64 sp1;
	/*
	 * Linux 不用 ring 2。entry_SYSCALL_64 把用户 RSP 暂存在这里。
	 * 注释原文在 processor.h:292。
	 */
	u64 sp2;
	u64 reserved2;
	u64 ist[7];
	u32 reserved3;
	u32 reserved4;
	u16 reserved5;
	u16 io_bitmap_base;
} __attribute__((packed));
```

`struct tss_struct`（`:378`）里先放 `x86_hw_tss x86_tss`，再放 `x86_io_bitmap`。`cpu_tss_rw` 是 `DECLARE_PER_CPU_PAGE_ALIGNED(struct tss_struct, cpu_tss_rw)`（`:389`）。汇编里的 `TSS_sp2` 是 `asm-offsets.c:116` 的 `OFFSET(TSS_sp2, tss_struct, x86_tss.sp2)`。

内核栈顶不在 TSS 里，在每 CPU 的 `pcpu_hot`：

```c
/* arch/x86/include/asm/current.h:14
 * static_assert 要求整个对象是 64 字节。
 */
struct pcpu_hot {
	union {
		struct {
			struct task_struct	*current_task;
			int			preempt_count;
			int			cpu_number;
#ifdef CONFIG_CALL_DEPTH_TRACKING
			u64			call_depth;
#endif
			unsigned long		top_of_stack;      /* 线程内核栈顶 */
			void			*hardirq_stack_ptr; /* 中断栈，interrupts/02 用这一字段 */
			u16			softirq_pending;
#ifdef CONFIG_X86_64
			bool			hardirq_stack_inuse;
#else
			void			*softirq_stack_ptr;
#endif
		};
		u8	pad[64];
	};
};
```

`DEFINE_PER_CPU_ALIGNED(struct pcpu_hot, pcpu_hot)` 在 `common.c:2175`，初值把 `current_task` 指到 `init_task`，`top_of_stack` 是 `TOP_OF_INIT_STACK`。`current` 宏就是 `this_cpu_read_stable(pcpu_hot.current_task)`（`current.h:39`）。

和中断章共用这一对象：系统调用换的是 `top_of_stack`（线程内核栈）；硬中断换的是 `hardirq_stack_ptr`（[interrupts/02](../interrupts/02-x86-entry.md) §2.4）。系统调用入口不切中断栈。

---

## 2.3 逐步看 `entry_SYSCALL_64`

```text
entry_SYSCALL_64                         entry_64.S:87
  ENDBR
  swapgs                                 用户 GSBASE ↔ 内核 GSBASE
  movq %rsp, cpu_tss_rw + TSS_sp2        用户 RSP 先寄存在 sp2
  SWITCH_TO_KERNEL_CR3 scratch_reg=%rsp  PTI：CR3 拨到内核页表；此时 %rsp 只是临时寄存器
  movq pcpu_hot + X86_top_of_stack, %rsp 从此是线程内核栈

  push __USER_DS                         pt_regs->ss
  push TSS_sp2                           pt_regs->sp = 刚存下的用户 RSP
  push %r11                              pt_regs->flags
  push __USER_CS                         pt_regs->cs
  push %rcx                              pt_regs->ip
  push %rax                              pt_regs->orig_ax = 系统调用号
  PUSH_AND_CLEAR_REGS rax=$-ENOSYS       其余寄存器；ax 槽写成 -ENOSYS，再 xor 清掉投机残留

  movq %rsp, %rdi                        第一个参数：pt_regs *
  movslq %eax, %rsi                      第二个参数：把号按 32 位符号扩展
  IBRS_ENTER / UNTRAIN_RET / CLEAR_BRANCH_HISTORY
  call do_syscall_64                     返回时中断已关
```

`SWITCH_TO_KERNEL_CR3`（`calling.h:169`，包在 `CONFIG_PAGE_TABLE_ISOLATION` 里）是：

```text
ALTERNATIVE "jmp .Lend", "", X86_FEATURE_PTI
mov %cr3, scratch
ADJUST_KERNEL_CR3 scratch          /* 清掉用户 PGD 位和用户 PCID */
mov scratch, %cr3
```

CPU 没有 `X86_FEATURE_PTI` 时，`ALTERNATIVE` 把这条宏换成 `jmp` 到结尾，CR3 不动。页表两半怎么排见内存章；这里只看到入口宏。没编 `CONFIG_PAGE_TABLE_ISOLATION` 时，同一文件 `:276` 把这组宏定义成空。

`PUSH_AND_CLEAR_REGS`（`calling.h:122`）先 `PUSH_REGS` 再 `CLEAR_REGS`。`rax=$-ENOSYS` 使压进 `pt_regs->ax` 的是 `-ENOSYS`，而真正的号已经在前面的 `orig_ax` 槽里。`CLEAR_REGS`（`:99`）把 `esi`…`r15d` 异或成 0，注释写明是为了不把用户寄存器里的值留给投机执行。`rdi` 不清：它马上要装 `pt_regs` 指针。

`IBRS_ENTER`（`calling.h:301`）在 `CONFIG_CPU_IBRS_ENTRY` 下对 `X86_FEATURE_KERNEL_IBRS` 做 `ALTERNATIVE`，向 `MSR_IA32_SPEC_CTRL` 写入带 IBRS 的 `x86_spec_ctrl_current`。`UNTRAIN_RET`（`arch/x86/include/asm/nospec-branch.h:356`）和 `CLEAR_BRANCH_HISTORY`（`:408`，`ALTERNATIVE` 成 `call clear_bhb_loop`）同样是特性位打开才有指令。返回前的 `CLEAR_CPU_BUFFERS`（`nospec-branch.h:401`）是 `verw`，注释写明用来清受影响 CPU 上的内部缓冲。

`do_syscall_64` 的注释（`entry_64.S:121`）写明它返回时 IRQ 已经关掉。后面的资格检查不能再被中断插进来改 `pt_regs`。

---

## 2.4 什么时候可以 `SYSRET`

`entry_64.S:82` 的注释：用户能改 `pt_regs` 里的字段时必须走 IRET，因为非规范地址在 AMD 和 Intel 上都会让 SYSRET 出问题。检查清单如下，任何一条失败都 `jmp swapgs_restore_regs_and_return_to_usermode`（`:611`，IRET 公共尾，[interrupts/02](../interrupts/02-x86-entry.md) 同一标签）。

| 检查 | 行号 | 失败的含义 |
|---|---|---|
| Xen PV | 130 | `X86_FEATURE_XENPV` 时直接去 IRET |
| `RCX(%rsp) == RIP(%rsp)` | 136 | SYSRET 从 `rcx` 取返回地址，必须等于要回到的 `ip` |
| 规范地址 | 150–160 | 按 48 位或 LA57 的 57 位把 `rcx` 做一次符号扩展，变了就说明原来不是规范地址 |
| `CS == __USER_CS` | 162 | SYSRET 只会装入固定的用户 CS |
| `R11 == EFLAGS` | 165 | 保存的 `r11` 必须仍等于 `pt_regs->flags` |
| `RF` 和 `TF` 都是 0 | 174 | SYSRET 恢复不了 `RF`；恢复 `TF` 会在指令后立刻陷入 |
| `SS == __USER_DS` | 179 | SYSRET 只会装入固定的用户 SS |

通过之后的标签 `syscall_return_via_sysret`（`:186`）注释写明没有人跳到这里，只是为了在 perf 画像里好看。

```text
syscall_return_via_sysret
  IBRS_EXIT
  POP_REGS pop_rdi=0                 rdi 还留在栈上，其余寄存器还原
  movq %rsp, %rdi                    记住线程栈
  movq cpu_tss_rw + TSS_sp0, %rsp    换到蹦床栈
  push 用户 RSP
  push 用户 RDI
  STACKLEAK_ERASE_NOCLOBBER          CONFIG_GCC_PLUGIN_STACKLEAK 才 call stackleak_erase
  SWITCH_TO_USER_CR3_STACK
  pop rdi / pop rsp
  swapgs
  CLEAR_CPU_BUFFERS
  sysretq
```

蹦床栈顶是 `TSS_sp0`（`asm-offsets.c:114` → `tss_struct.x86_tss.sp0`）。在线程栈上执行 `sysretq` 会有窗口：指令退休前 `RSP` 已经是用户栈，NMI 会落到用户栈上。先把最后两格（用户 RSP、用户 RDI）拷到 `sp0` 指向的栈，再 `pop` + `sysretq`，NMI 落在内核的蹦床栈上。`STACKLEAK_ERASE_NOCLOBBER`（`calling.h:366`）在这个窗口之前把线程栈上的残留清掉，仅当编译进了 stackleak 插件。

`SWITCH_TO_USER_CR3_STACK`（`calling.h:214`）在 PTI 打开时把 CR3 拨回用户半边，并按 PCID 决定要不要 `NOFLUSH`。

---

## 2.5 和设备中断入口对照

两边最后都放在 `struct pt_regs` 里，搭的方式不同。

| | `syscall` | 设备中断 `idtentry_irq` |
|---|---|---|
| 入口 | `entry_SYSCALL_64`（`entry_64.S:87`） | `irq_entries_start` 短桩 → `asm_common_interrupt`（[interrupts/02](../interrupts/02-x86-entry.md) §2.2） |
| 栈从哪来 | 自己从 `pcpu_hot.top_of_stack` 装 `RSP` | 硬件用当前栈；从内核进来的第一层再换 `hardirq_stack_ptr` |
| `orig_ax` | 系统调用号 | 先是向量号，随后改成 `-1` |
| `ax` 初值 | `-ENOSYS` | 用户/内核当时的 `rax` |
| C 入口 | `do_syscall_64` | `common_interrupt` |
| 回到用户态 | `syscall_exit_to_user_mode` | `irqentry_exit` → `irqentry_exit_to_user_mode` |
| 快返回 | 资格通过则 `sysretq` | `iretq` |

`irqentry_exit_to_user_mode`（`kernel/entry/common.c:315`）只有 `exit_to_user_mode_prepare` + `__exit_to_user_mode`。系统调用多一层 `syscall_exit_work`（audit / ptrace / tracepoint），见 [04](./04-enter-exit-work.md)。

---

## 2.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/kernel/cpu/common.c` | `syscall_init` | 2199 | 写 STAR / LSTAR / CSTAR / SYSCALL_MASK |
| `arch/x86/include/asm/processor.h` | `cpu_tss_rw` | 389 | 每 CPU TSS，`sp2` 暂存用户 RSP |
| `arch/x86/include/asm/current.h` | `struct pcpu_hot` | 14 | `top_of_stack` 与 `hardirq_stack_ptr` |
| `arch/x86/kernel/asm-offsets.c` | `X86_top_of_stack` | 117 | 汇编用的 `top_of_stack` 偏移 |
| `arch/x86/entry/entry_64.S` | `entry_SYSCALL_64` | 87 | 换栈并 `call do_syscall_64` |
| `arch/x86/entry/entry_64.S` | `syscall_return_via_sysret` | 186 | SYSRET 快路径 |
| `arch/x86/entry/calling.h` | `SWITCH_TO_KERNEL_CR3` | 169 | PTI 下切内核 CR3 |
| `arch/x86/entry/calling.h` | `PUSH_AND_CLEAR_REGS` | 122 | 压寄存器并清掉投机残留 |
| `arch/x86/include/asm/entry-common.h` | `arch_exit_to_user_mode_prepare` | 47 | 出口选下一次栈随机偏移，见 [03](./03-dispatch-and-define.md) |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [分派 →](./03-dispatch-and-define.md)
