# 2. KVM 用户态 API

**结论**：`/dev/kvm` 上的 ioctl 只做三件和本章有关的事：核对 API 版本、检查能力、创建一个 VM fd。VM fd 再创建 vCPU fd 并登记内存。vCPU fd 的 `mmap` 得到 `struct kvm_run`，`KVM_RUN` 本身不携带结构体指针，退出原因写在这块映射里。

> `KVM_API_VERSION` 在 v6.6.160 是 12（`include/uapi/linux/kvm.h:18`）。`KVM_GET_API_VERSION` 在参数非 0 时返回 `-EINVAL`（`virt/kvm/kvm_main.c:5130`）。

---

## 2.1 三个文件描述符

`kvm_dev_ioctl`（`kvm_main.c:5124`）处理打开 `/dev/kvm` 之后的命令：

| ioctl | 定义 | 返回 |
|---|---|---|
| `KVM_GET_API_VERSION` | `kvm.h:926`，`_IO(KVMIO, 0x00)` | `KVM_API_VERSION`（12） |
| `KVM_CREATE_VM` | `kvm.h:927`，`_IO(KVMIO, 0x01)` | VM fd。x86 上第三参是 VM 类型，demo 传 0 |
| `KVM_CHECK_EXTENSION` | `kvm.h:935` | 能力存在时非 0。`KVM_CAP_USER_MEMORY` 是 3（`kvm.h:953`） |
| `KVM_GET_VCPU_MMAP_SIZE` | `kvm.h:939` | 映射 vCPU 所需的字节数 |

`KVM_GET_VCPU_MMAP_SIZE` 的实现（`kvm_main.c:5141`）从一页 `struct kvm_run` 起算。x86 再加一页 PIO 数据（`CONFIG_X86`）。开了 coalesced MMIO 再加一页。所以 x86 上这个返回值大于 `sizeof(struct kvm_run)`。PIO 的字节不在结构体内部，见 2.4。

VM fd 的 `kvm_vm_ioctl_create_vcpu`（`kvm_main.c:3945`）创建 vCPU，`create_vcpu_fd`（`:3903`）用 `anon_inode_getfd` 得到 vCPU fd，文件操作是 `kvm_vcpu_fops`（`:3892`）：`unlocked_ioctl` 走 `kvm_vcpu_ioctl`，`mmap` 走 `kvm_vcpu_mmap`。

---

## 2.2 登记一段客户机物理内存

用户态把自己 `mmap` 的匿名页登记成 GPA。参数是完整的小结构：

```c
/* include/uapi/linux/kvm.h:91 */
struct kvm_userspace_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;   /* GPA 起点，按页对齐 */
	__u64 memory_size;        /* 字节。0 表示删除这个 slot */
	__u64 userspace_addr;     /* 对应的 HVA */
};
```

`KVM_SET_USER_MEMORY_REGION`（`kvm.h:1473`）进 `kvm_vm_ioctl_set_memory_region`（`kvm_main.c:2065`）。flag 的 bit 0 到 bit 15 给用户态（`kvm.h:100`）：`KVM_MEM_LOG_DIRTY_PAGES`（bit 0）和 `KVM_MEM_READONLY`（bit 1）。bit 16 起是内核自己的，例如 slot 正在修改时的 `KVM_MEMSLOT_INVALID`。slot 怎样挂进 GFN 树、代际怎样换，内存章 [11.1](../memory-management/11-kvm-memory.md) 已经按同一版本写过。本章只用这个结构说明：客户机指令所在的 GPA 必须落在某个 slot 的 `[guest_phys_addr, +memory_size)` 里，`userspace_addr` 是宿主机进程里同一段字节的地址。

---

## 2.3 寄存器

x86 的通用寄存器和段寄存器是两支 ioctl。`KVM_SET_REGS` / `KVM_GET_REGS` 的布局：

```c
/* arch/x86/include/uapi/asm/kvm.h:111
 * 这是完整结构。SET 时由用户态填，GET 时由内核填。
 */
struct kvm_regs {
	__u64 rax, rbx, rcx, rdx;
	__u64 rsi, rdi, rsp, rbp;
	__u64 r8,  r9,  r10, r11;
	__u64 r12, r13, r14, r15;
	__u64 rip, rflags;
};
```

`__set_regs`（`arch/x86/kvm/x86.c:11385`）把它们写进 `vcpu->arch`，并执行 `kvm_set_rflags(vcpu, regs->rflags | X86_EFLAGS_FIXED)`。固定位（bit 1）即使用户态传 0 也会被或上。它还会清掉待注入异常，并 `kvm_make_request(KVM_REQ_EVENT, vcpu)`。

段和系统寄存器在 `struct kvm_sregs`（`asm/kvm.h:144`）。和实模式 demo 有关的片段：

```c
/* arch/x86/include/uapi/asm/kvm.h:126 */
struct kvm_segment {
	__u64 base;          /* 实模式下线性地址 = base + offset */
	__u32 limit;
	__u16 selector;
	__u8  type;
	__u8  present, dpl, db, s, l, g, avl;
	__u8  unusable;
	__u8  padding;
};

/* :144，只列出本章要改的成员。中间还有 ds/es/fs/gs/ss/tr/ldt、gdt/idt。 */
struct kvm_sregs {
	struct kvm_segment cs, ds, es, fs, gs, ss;
	struct kvm_segment tr, ldt;
	struct kvm_dtable gdt, idt;
	__u64 cr0, cr2, cr3, cr4, cr8;
	__u64 efer;
	__u64 apic_base;
	__u64 interrupt_bitmap[(KVM_NR_INTERRUPTS + 63) / 64];
};
```

`kvm_vcpu_reset`（`x86.c:12146`）在创建 vCPU 时把 RIP 写成 `0xfff0`（`:12249`），CR0 置上 ET，并在非 INIT 时再置 NW 和 CD（`:12259`）。VMX 的 `vmx_vcpu_reset` 把 CS selector 设成 `0xf000`（`vmx/vmx.c:4888`）、CS base 设成 `0xffff0000`（`:4889`），其余数据段 base 为 0、limit 为 `0xffff`（`seg_setup`，`:3938`）。用户态要在 GPA 0 执行自己的字节，就要 `KVM_GET_SREGS` 之后把 CS selector 和 CS base 改成 0，清掉 CR0.PE（bit 0）和 CR0.PG（bit 31），再 `KVM_SET_SREGS`。`__set_sregs_common` 会调用 `kvm_x86_set_cr0`（`x86.c:11667`）。

Intel 在 `enable_unrestricted_guest == 0` 时，实模式还要一块 3 页的 TSS。`KVM_SET_TSS_ADDR`（`kvm.h:1475`）进 `kvm_vm_ioctl_set_tss_addr`（`x86.c:6137`）。地址落在最后 3 页会 `-EINVAL`。`vmx_set_tss_addr`（`vmx.c:5092`）在 unrestricted guest 打开时直接返回 0；否则用内部 memslot 铺 3 页。SVM 的 `set_tss_addr` 是 `KVM_X86_OP_OPTIONAL_RET0`（`arch/x86/include/asm/kvm-x86-ops.h:91`），静态调用返回 0。能力位 `KVM_CAP_SET_TSS_ADDR` 在 x86 的检查里和一批能力一起返回 1（`x86.c:4470`），所以「能力为 1」不能区分现在是不是 unrestricted guest。

---

## 2.4 `mmap` 出来的 `kvm_run`

`KVM_RUN` 的定义是 `_IO(KVMIO, 0x80)`（`kvm.h:1578`），第三参必须是 0，否则 `kvm_vcpu_ioctl` 返回 `-EINVAL`（`kvm_main.c:4146`）。退出信息在映射里。结构体开头是输入和输出共用的头，IO 退出是联合体里的一支。联合体后面还有 MMIO、异常等成员，这里只留 IO 这一支：

```c
/* include/uapi/linux/kvm.h:283 起，片段 */
struct kvm_run {
	/* in */
	__u8 request_interrupt_window;
	__u8 immediate_exit;          /* 非 0 时本次 KVM_RUN 不想进客户机 */
	__u8 padding1[6];

	/* out */
	__u32 exit_reason;
	__u8 ready_for_interrupt_injection;
	__u8 if_flag;
	__u16 flags;

	/* in (pre_kvm_run), out (post_kvm_run) */
	__u64 cr8;
	__u64 apic_base;

	union {
		/* KVM_EXIT_IO，结构从 kvm.h:320 起；上一行是注释 */
		struct {
			__u8 direction;       /* KVM_EXIT_IO_IN=0，KVM_EXIT_IO_OUT=1 */
			__u8 size;            /* 字节 */
			__u16 port;
			__u32 count;
			__u64 data_offset;    /* 相对这次 mmap 的起点，不是相对本结构体末尾 */
		} io;
		/* 其余 exit 成员略 */
	};
};
```

方向宏在 `kvm.h:321`：`KVM_EXIT_IO_IN` 是 0，`KVM_EXIT_IO_OUT` 是 1。和「0 表示读、1 表示写」的习惯相反的是 IN/OUT 的命名：OUT 是客户机写端口，数值是 1。

x86 把 `data_offset` 设成 `KVM_PIO_PAGE_OFFSET * PAGE_SIZE`（`x86.c:7962`）。`KVM_PIO_PAGE_OFFSET` 是 1（`arch/x86/include/uapi/asm/kvm.h:14`）。缺页处理 `kvm_vcpu_fault`（`kvm_main.c:3840`）在 `pgoff == 0` 时映射 `vcpu->run`，在 `pgoff == KVM_PIO_PAGE_OFFSET` 时映射 `vcpu->arch.pio_data`。这页在 `kvm_arch_vcpu_create` 里 `alloc_page`（`x86.c:12025`）。用户态要用 `KVM_GET_VCPU_MMAP_SIZE` 返回的长度去做 `mmap`，再检查 `data_offset + size * count` 落在映射内。只按 `sizeof(struct kvm_run)` 映射，会在访问 PIO 字节时缺到 `kvm_arch_vcpu_fault`，那条路径返回 `VM_FAULT_SIGBUS`（`x86.c:6132`）。

`kvm_vcpu_mmap`（`kvm_main.c:3870`）只对 dirty ring 那些页要求 `VM_SHARED`。`struct kvm_run` 和 PIO 页本身由 fault 处理函数提供。demo 仍用 `MAP_SHARED` 映射 vCPU fd，匿名客户机内存用 `MAP_PRIVATE|MAP_ANONYMOUS`。

---

## 2.5 和 demo 的对应

[demos/01-minimal-kvm.c](./demos/01-minimal-kvm.c) 按这一节的顺序调用：打开设备、核对版本 12、确认 `KVM_CAP_USER_MEMORY` 非 0、`KVM_CREATE_VM`、`KVM_SET_TSS_ADDR`、按页对齐的匿名页、`KVM_SET_USER_MEMORY_REGION`、`KVM_CREATE_VCPU`、按 `KVM_GET_VCPU_MMAP_SIZE` 映射（至少两页，才能盖住 PIO 页）、`KVM_GET_SREGS` / `KVM_SET_SREGS`（CS/DS/ES/FS/GS/SS 的 base 与 selector 都是 0，limit 为 `0xffff`）、`KVM_SET_REGS`。进入 `KVM_RUN` 前它挡住 `SIGALRM`，并用 8 字节空掩码调用 `KVM_SET_SIGNAL_MASK`；`KVM_CAP_IMMEDIATE_EXIT` 非 0 时，闹钟处理函数才写 `immediate_exit`。它不调用 `KVM_CREATE_IRQCHIP`。这个省略对 HLT 的含义在 [05](./05-vcpu-schedule.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 全景](./01-overview.md) · [目录](./README.md) · [KVM_RUN →](./03-kvm-run.md)
