# 2. x86-64 入口

**结论**：设备中断不走异常的 `error_entry` 以外的特殊门，而是「短桩把向量号当成 error code 压栈 → `asm_common_interrupt` 建好 `pt_regs` → `common_interrupt` 在中断栈上查 `vector_irq[]`」。`irqentry_enter` 负责 RCU/lockdep，`irq_enter_rcu` 负责硬中断嵌套计数。`syscall` 指令不进 IDT，入口是 `entry_SYSCALL_64`，见 [syscalls/02](../syscalls/02-x86-entry.md)。

> 汇编桩写在头文件里，由 `arch/x86/entry/entry_64.S:603` 的 `#include <asm/idtentry.h>` 编进同一段 `.irqentry.text`。C 入口在 `arch/x86/kernel/irq.c:249`。

---

## 2.1 IDT 里有什么

每颗 CPU 有自己的 IDT（`arch/x86/include/asm/irq_vectors.h:24` 的注释写明 64 位是 per CPU）。一项是一个门，CPU 用向量号当索引跳进去。门在 `arch/x86/kernel/idt.c` 填：

| 阶段 | 函数 | 行号 | 填什么 |
|---|---|---|---|
| 早期 | `idt_setup_early_handler` | 310 | 0..31 先指向早期处理桩 |
| 陷阱 | `idt_setup_from_table(..., def_idts, ...)` | 186 | 除法错、缺页、NMI 等 |
| APIC 门 | `idt_setup_apic_and_irq_gates` | 274 | 系统向量 + 其余外部向量 |

`idt_setup_apic_and_irq_gates`（`idt.c:274`）做两截：

```c
/* arch/x86/kernel/idt.c:274 */
void __init idt_setup_apic_and_irq_gates(void)
{
	int i = FIRST_EXTERNAL_VECTOR;          /* 0x20，外部中断从这里起 */
	void *entry;

	idt_setup_from_table(idt_table, apic_idts, ARRAY_SIZE(apic_idts), true);
	/* 上面 true：这些向量同时 set_bit 进 system_vectors */

	/* 还没被标成系统向量、且 < FIRST_SYSTEM_VECTOR 的，全部指向设备中断桩 */
	for_each_clear_bit_from(i, system_vectors, FIRST_SYSTEM_VECTOR) {
		entry = irq_entries_start + IDT_ALIGN * (i - FIRST_EXTERNAL_VECTOR);
		set_intr_gate(i, entry);
	}
	/* FIRST_SYSTEM_VECTOR 及以上、还没占用的，指向伪中断桩，不写进 system_vectors */
	...
}
```

`apic_idts[]`（`idt.c:129`）把本地定时器、重调度 IPI、函数调用 IPI、伪中断、错误中断等直接绑到各自的 `asm_sysvec_*`。这些号都 ≥ `LOCAL_TIMER_VECTOR`（`0xec`），所以它们不占用下面的设备向量窗口。完整表见 [03](./03-apic-and-vectors.md)。

---

## 2.2 设备向量桩

桩的正文在 `arch/x86/include/asm/idtentry.h:536`，汇编器从 `entry_64.S` 包含进来。每个桩只占 `IDT_ALIGN` 字节，所以 IDT 可以用「基址 + 向量差 × 对齐」算出入口。

```text
vector = FIRST_EXTERNAL_VECTOR          /* 0x20 */
重复 NR_EXTERNAL_VECTORS 次:
    ENDBR
    .byte 0x6a, vector                  /* pushq 一个 8 位立即数：向量号 */
    jmp   asm_common_interrupt
    用 0xcc 填满到 IDT_ALIGN
    vector++
```

用 `.byte 0x6a` 而不是 `pushq $vector`，是因为向量号大于 `0x7f` 时，编译器会把立即数扩成 5 字节，桩就对不齐了（注释在 `idtentry.h:525`）。`push` 会把高位按符号扩展，所以 C 里要把 error code 收成 `u8` 再看成 `u32`。

`asm_common_interrupt` 由 `idtentry` 宏生成（`entry_64.S:419` 的 `idtentry_irq`，展开成 `idtentry ... has_error_code=1`）。`idtentry_body`（`entry_64.S:331`）做的事可以记成四步：

1. `call error_entry`：若从用户态进来，切到内核线程栈，并处理 GS。
2. `pt_regs` 指针放进 `%rdi`（C 的第一个参数）。
3. 栈上的 `ORIG_RAX`（刚才压进去的向量号）放进 `%rsi`（第二个参数），再把 `ORIG_RAX` 改成 `-1`，表示这不是一条要重启的系统调用。
4. `call common_interrupt`，返回后 `jmp error_return`（`entry_64.S:359`）。

`error_return`（`:1134`）看 `CS` 低两位：内核态跳 `restore_regs_and_return_to_kernel`；用户态跳 `swapgs_restore_regs_and_return_to_usermode`。后一个是 `common_interrupt_return`（`:610`）的内层标签，里面在 `CONFIG_DEBUG_ENTRY` 下用 `ud2` 断言 CS 表示用户态，再把 IRET 帧拷到蹦床栈。读入口时先别陷进 IRET 细节。

---

## 2.3 `DEFINE_IDTENTRY_IRQ`

C 侧不是手写 `common_interrupt` 的函数头，而是宏（`idtentry.h:206`）：

```c
/* arch/x86/include/asm/idtentry.h:206 展开后的形状 */
__visible noinstr void common_interrupt(struct pt_regs *regs,
					unsigned long error_code)
{
	irqentry_state_t state = irqentry_enter(regs);   /* RCU / lockdep */
	u32 vector = (u32)(u8)error_code;                /* 丢掉符号扩展的高位 */

	kvm_set_cpu_l1tf_flush_l1d();
	instrumentation_begin();                         /* 从此允许 ftrace */
	run_irq_on_irqstack_cond(__common_interrupt, regs, vector);
	instrumentation_end();
	irqentry_exit(regs, state);
}
```

`noinstr` 表示这段不能被插桩。真正查描述符的是 `__common_interrupt`，在 `arch/x86/kernel/irq.c:249`：

```c
DEFINE_IDTENTRY_IRQ(common_interrupt)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	struct irq_desc *desc;

	RCU_LOCKDEP_WARN(!rcu_is_watching(), "IRQ failed to wake up RCU");

	desc = __this_cpu_read(vector_irq[vector]);      /* 本 CPU 的向量 → 描述符 */
	if (likely(!IS_ERR_OR_NULL(desc))) {
		handle_irq(desc, regs);                  /* x86-64：generic_handle_irq_desc */
	} else {
		apic_eoi();                              /* 没人认领也要结束 APIC 周期 */
		if (desc == VECTOR_UNUSED) {
			pr_emerg_ratelimited("%s: %d.%u No irq handler for vector\n",
					     __func__, smp_processor_id(), vector);
		} else {
			__this_cpu_write(vector_irq[vector], VECTOR_UNUSED);
		}
	}
	set_irq_regs(old_regs);
}
```

`handle_irq`（`irq.c:236`）在 `CONFIG_X86_64` 下直接 `generic_handle_irq_desc(desc)`，也就是调用 `desc->handle_irq`（`include/linux/irqdesc.h:159`）。32 位才走 `__handle_irq`。

系统向量用另一套宏 `DEFINE_IDTENTRY_SYSVEC`（`idtentry.h:248`）。它们不经过 `common_interrupt`，例如本地定时器是 `sysvec_apic_timer_interrupt`（`arch/x86/kernel/apic/apic.c:1088` 一带的 `DEFINE_IDTENTRY_SYSVEC`）。

---

## 2.4 中断栈

x86-64 的硬中断栈不是线程栈。`struct irq_stack`（`arch/x86/include/asm/processor.h:392`）只有一块 `stack[IRQ_STACK_SIZE]`。大小由宏决定，不要记死一个字节数：

```c
/* arch/x86/include/asm/page_64_types.h:21 */
#define IRQ_STACK_ORDER (2 + KASAN_STACK_ORDER)          /* 没开 KASAN 时 ORDER 是 2 */
#define IRQ_STACK_SIZE (PAGE_SIZE << IRQ_STACK_ORDER)    /* ORDER 2 → 4 个页 */
```

`map_irq_stack`（`arch/x86/kernel/irq_64.c:35`，要 `CONFIG_VMAP_STACK`）把这几页 `vmap` 出来，并把栈顶指针减 8 后写进 `pcpu_hot.hardirq_stack_ptr`（`:53`）。没开 `VMAP_STACK` 时用同一块 per-CPU 后备存储、没有保护页（`:61`）。

何时换栈由 `run_irq_on_irqstack_cond`（`arch/x86/include/asm/irq_stack.h:196`）决定：

| 条件 | 行为 |
|---|---|
| `user_mode(regs)` 为真 | 线程栈是空的，不换栈。直接 `irq_enter_rcu()` → 处理函数 → `irq_exit_rcu()` |
| 已经在中断栈上（`hardirq_stack_inuse`） | 嵌套中断，也不再换 |
| 其余（典型：从内核线程栈进来的第一层硬中断） | 置 `hardirq_stack_inuse`，`call_on_stack` 切到 `hardirq_stack_ptr` |

切栈的那条汇编里，`irq_enter_rcu` / `irq_exit_rcu` 包住处理函数（`irq_stack.h:189` 的 `ASM_CALL_IRQ`）。所以「记账」发生在中断栈上，而 `irqentry_enter` 还在原来的栈上，两者不是同一个函数。

从进程上下文推迟的 softirq 若开了 `CONFIG_SOFTIRQ_ON_OWN_STACK`，`do_softirq_own_stack`（`irq_stack.h:213`）会借用同一块中断栈跑 `__do_softirq`。此时中断栈不可能正被硬中断占用。

---

## 2.5 `irqentry_enter` / `irqentry_exit`

```c
/* include/linux/entry-common.h:371
 * 调用方应把它当成不透明对象，原样传回 exit。
 */
typedef struct irqentry_state {
	union {
		bool	exit_rcu;   /* 普通中断：退出时要不要 ct_irq_exit() */
		bool	lockdep;    /* 只给 NMI 那对 enter/exit 用 */
	};
} irqentry_state_t;
```

`irqentry_enter`（`kernel/entry/common.c:323`）分三支：

1. **用户态进来**：`irqentry_enter_from_user_mode`（`:310`）→ `__enter_from_user_mode`。`exit_rcu` 保持 false。
2. **打在 idle 任务上**（且不是 `CONFIG_TINY_RCU`）：无条件 `ct_irq_enter()`，`exit_rcu = true`。注释写明不能先看 `rcu_is_watching()`，否则嵌套进来的时钟中断会误判自己是第一层，提前结束 RCU 宽限期（`:334`）。
3. **其余内核态**：`rcu_irq_enter_check_tick()`，给 NO_HZ 看要不要把 tick 拉回来。

`irqentry_exit`（`:415`）在中断仍然关闭时：若回到用户态，走 `irqentry_exit_to_user_mode`；若 `state.exit_rcu`，配对 `ct_irq_exit()`；否则在可抢占内核上可能 `raw_irqentry_exit_cond_resched`（`:390`），里面看 `need_resched()` 再 `preempt_schedule_irq()`。系统调用回到用户态走 `syscall_exit_to_user_mode`，在同一 TIF 循环之前多做 audit / ptrace / tracepoint，见 [syscalls/04](../syscalls/04-enter-exit-work.md)。

---

## 2.6 `irq_enter_rcu` 与 `irq_exit_rcu`

这两个在 `kernel/softirq.c`，和上一节不是一层：

```c
/* kernel/softirq.c:618 */
void irq_enter_rcu(void)
{
	__irq_enter_raw();                 /* 抢占计数加上 HARDIRQ_OFFSET，标上硬中断 */
	if (tick_nohz_full_cpu(smp_processor_id()) ||
	    (is_idle_task(current) && (irq_count() == HARDIRQ_OFFSET)))
		tick_irq_enter();          /* NO_HZ 或 idle 上的第一层硬中断：记账 jiffies */
	account_hardirq_enter(current);
}
```

`irq_exit_rcu`（`:671`）走到 `__irq_exit_rcu`（`:651`）：减去硬中断计数，若没有嵌套、且本 CPU 有 pending softirq、且没有关掉 softirq，就 `invoke_softirq()`。非 RT 的 `invoke_softirq`（`:443`）在中断还没开的时候直接调 `__do_softirq`，或者把活留给 `ksoftirqd`。softirq 循环见 [08](./08-softirq-tasklet.md)。

还有一个 `irq_enter` / `irq_exit`（`:632` / `:683`），会额外包一层 `rcu_irq_enter` / `rcu_irq_exit`。x86 设备中断这条热路径用的是 `_rcu` 后缀这一对，因为 `irqentry_enter` 已经处理过 RCU。

---

## 2.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/kernel/idt.c` | `idt_setup_apic_and_irq_gates` | 274 | 安装 APIC 门和设备中断桩 |
| `arch/x86/include/asm/idtentry.h` | `irq_entries_start` | 536 | 外部向量短桩 |
| `arch/x86/include/asm/idtentry.h` | `DEFINE_IDTENTRY_IRQ` | 206 | 设备中断 C 入口外壳 |
| `arch/x86/entry/entry_64.S` | `idtentry_body` | 331 | 建 `pt_regs` 并调用 C |
| `arch/x86/entry/entry_64.S` | `idtentry_irq` | 419 | 设备中断桩对齐到 cache line |
| `arch/x86/entry/entry_64.S` | `common_interrupt_return` | 610 | 返回用户态的公共尾 |
| `arch/x86/kernel/irq.c` | `common_interrupt` | 249 | 查 `vector_irq[]` |
| `arch/x86/include/asm/irq_stack.h` | `run_irq_on_irqstack_cond` | 196 | 按需换中断栈 |
| `arch/x86/kernel/irq_64.c` | `map_irq_stack` | 35 | 建立 per-CPU 中断栈 |
| `kernel/entry/common.c` | `irqentry_enter` | 323 | RCU / lockdep / idle |
| `kernel/entry/common.c` | `irqentry_exit` | 415 | 配对退出，可能抢占 |
| `kernel/softirq.c` | `irq_enter_rcu` | 618 | 硬中断计数、tick |
| `kernel/softirq.c` | `irq_exit_rcu` | 671 | 可能启动 softirq |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [向量布局 →](./03-apic-and-vectors.md)
