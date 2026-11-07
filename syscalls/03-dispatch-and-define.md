# 3. 分派与 `SYSCALL_DEFINE`

**结论**：`do_syscall_64` 先把栈顶随机垫高，再跑进入工作，然后 `do_syscall_x64` 在号小于 `NR_syscalls` 时调用 `x64_sys_call`。那个函数是 `switch (nr)`，每个 case 直接调 `__x64_sys_*`。`sys_call_table[]` 还在同一文件里，热路径不索引它。

> `arch/x86/entry/common.c:36` 的 `do_syscall_x64`，`arch/x86/entry/syscall_64.c:27` 的 `x64_sys_call`。

---

## 3.1 `do_syscall_64`

```c
/* arch/x86/entry/common.c:69 */
__visible noinstr void do_syscall_64(struct pt_regs *regs, int nr)
{
	add_random_kstack_offset();
	nr = syscall_enter_from_user_mode(regs, nr);

	instrumentation_begin();

	if (!do_syscall_x64(regs, nr) && !do_syscall_x32(regs, nr) && nr != -1) {
		/* Invalid system call, but still a system call. */
		regs->ax = __x64_sys_ni_syscall(regs);
	}

	instrumentation_end();
	syscall_exit_to_user_mode(regs);
}
```

`add_random_kstack_offset`（`include/linux/randomize_kstack.h:50`）在 `CONFIG_RANDOMIZE_KSTACK_OFFSET` 下读取 `current->kstack_offset`，用 `__builtin_alloca_uninitialized` 垫出最多 10 位（`KSTACK_OFFSET_MAX` 是 `& 0x3FF`，`:40`）的栈空间，再用一条空 `asm` 把分配留住。静态键关掉时这是空操作。偏移本身在更早一次退出时选定：`arch_exit_to_user_mode_prepare`（`arch/x86/include/asm/entry-common.h:85`）调用 `choose_random_kstack_offset(rdtsc())`。头文件 `:76` 的注释说明为什么放在退出而不是进入：用户态更难预测「内核里会待多久」，新偏移在内核里停留的时间也更短。同一注释写明 x86-64 上编译器 8 字节对齐会再吃掉 3 个低位，剩下 7 位熵。

`syscall_enter_from_user_mode` 可能把号改成 `-1`，表示这次调用应当跳过。`-1` 时两个 `do_syscall_*` 都返回 false，并且 `nr != -1` 不成立，于是不会改 `regs->ax`。入口汇编已经把 `ax` 写成了 `-ENOSYS`，用户看到的就是这个值。号越界但不是 `-1` 时，显式调用 `__x64_sys_ni_syscall`。`SYSCALL_DEFINE0(ni_syscall)`（`common.c:340`）返回 `-ENOSYS`。

`noinstr` 表示这段不能被插桩。`instrumentation_begin/end` 把真正的系统调用实现包在可插桩区间里。

---

## 3.2 `x64_sys_call` 开关，表只给 tracing

```c
/* arch/x86/entry/common.c:36 */
static __always_inline bool do_syscall_x64(struct pt_regs *regs, int nr)
{
	unsigned int unr = nr;          /* 负数变成很大的无符号数，比较时越界 */

	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		regs->ax = x64_sys_call(regs, unr);
		return true;
	}
	return false;
}
```

`NR_syscalls` 是 `arch/x86/include/asm/unistd.h:35` 的 `__NR_syscalls`。这个宏不手写在仓库里的某个 `.h` 里：`arch/x86/entry/syscalls/Makefile` 用 `scripts/syscallhdr.sh --emit-nr --abis common,64` 生成 `unistd_64.h`。脚本在 `:92` 输出 `#define __NR_syscalls $((max + 1))`。`syscall_64.tbl` 里 abi 为 `common` 或 `64` 的最大号是 453（`map_shadow_stack`，tbl `:377`），所以生成结果是 454。号 453 仍然小于 454，进得了开关。

`array_index_nospec`（`include/linux/nospec.h:55`）在边界检查之后把下标夹进 `[0, size)`。注释（`:42`）说明：CPU 若投机地越过了 `unr < NR_syscalls`，这个掩码让后续用下标的访存仍然落在范围内。这里下标只进 `switch`，宏仍然按这个顺序调用。

```c
/* arch/x86/entry/syscall_64.c:14
 * The sys_call_table[] is no longer used for system calls, but
 * kernel/trace/trace_syscalls.c still wants to know the system
 * call address.
 */
const sys_call_ptr_t sys_call_table[] = {
	/* __x64_##sym 按 tbl 展开 */
};

long x64_sys_call(const struct pt_regs *regs, unsigned int nr)
{
	switch (nr) {
	/* case nr: return __x64_##sym(regs); */
	default: return __x64_sys_ni_syscall(regs);
	}
}
```

`default` 在 `nr` 已经小于 `NR_syscalls` 时仍然存在：表里的洞也会落到 `__x64_sys_ni_syscall`。`sys_call_ptr_t` 的 typedef 在 `arch/x86/include/asm/syscall.h:20`，注释写明「只给 `kernel/trace/trace_syscalls.c` 用」。

x32 是紧跟着的第二次尝试（`do_syscall_x32`，`common.c:52`）：`xnr = nr - __X32_SYSCALL_BIT`，`__X32_SYSCALL_BIT` 是 `0x40000000`（`arch/x86/include/uapi/asm/unistd.h:13`）。`CONFIG_X86_X32_ABI` 打开且 `xnr < X32_NR_syscalls` 时调用 `x32_sys_call`（`arch/x86/entry/syscall_x32.c:16`），同样是 `switch`。64 位主线读到这里即可，参数布局仍是 `r10` 那一套，见 [07](./07-compat.md) 的边界。

---

## 3.3 tbl 长什么样

`arch/x86/entry/syscalls/syscall_64.tbl` 开头注释（`:4`）写明列是：

```text
<number> <abi> <name> <entry point>
```

abi 取 `common`、`64` 或 `x32`。`__x64_sys_*` 桩在编译时从 `sys_*` 生成，tbl 里写的是 `sys_read` 这种名字。同一文件 `:7` 的注释就是这句话。

| 号 | abi | 名字 | 入口列 | tbl 行 |
|---|---|---|---|---|
| 0 | common | `read` | `sys_read` | 11 |
| 39 | common | `getpid` | `sys_getpid` | 50 |
| 186 | common | `gettid` | `sys_gettid` | 197 |
| 219 | common | `restart_syscall` | `sys_restart_syscall` | 230 |
| 228 | common | `clock_gettime` | `sys_clock_gettime` | 239 |

本机安装的 `/usr/include/x86_64-linux-gnu/asm/unistd_64.h` 里这五个号相同（`__NR_getpid` 39、`__NR_clock_gettime` 228）。那是用户态头，行号不要当成 v6.6.160 的内核行号。内核侧以 tbl 为准。

---

## 3.4 从 `__x64_sys_*` 到 `__do_sys_*`

x86 盖过了通用的 `__SYSCALL_DEFINEx`。`arch/x86/include/asm/syscall_wrapper.h:22` 的注释把寄存器说死了：64 位是 `RDI, RSI, RDX, R10, R8, R9`，32 位是 `EBX, ECX, EDX, ESI, EDI, EBP`。

```c
/* syscall_wrapper.h:56 */
#define SC_X86_64_REGS_TO_ARGS(x, ...)					\
	__MAP(x,__SC_ARGS						\
		,,regs->di,,regs->si,,regs->dx				\
		,,regs->r10,,regs->r8,,regs->r9)
```

第四个参数是 `regs->r10`。C 的调用约定本来用 `rcx` 当第四个参数，但 `syscall` 已经把用户 `RIP` 放进了 `rcx`，所以 ABI 把第四个参数挪到 `r10`。桩在调用 `__se_sys_*` 之前再按 C ABI 放回 `rcx`。头文件 `:32` 的示例注释写的就是 `mov 0x38(%rdi), %rcx` 从 `regs->r10` 解出第四个参数。

展开后的形状（`syscall_wrapper.h:223`，零参数用 `:244` 的 `SYSCALL_DEFINE0`）：

```text
__x64_sys_<name>(const struct pt_regs *regs)   入口桩，从 pt_regs 解参数
    return __se_sys_<name>(解出来的参数);       符号扩展
        return __do_sys_<name>(...);            真正的函数体
```

零参数没有什么可解的，`__x64_sys_<name>` 直接别名到 `__do_sys_<name>`（`__SYS_STUB0`，`:85`）。桩仍然接收 `pt_regs *`，因为所有 case 的函数类型必须一样。`__ia32_sys_*` 在 `CONFIG_IA32_EMULATION` 或 32 位内核里按另一套寄存器再生成一份。

通用头 `include/linux/syscalls.h:241` 的 `__SYSCALL_DEFINEx` 被 x86 这份替换掉。`SYSCALL_DEFINE1`…`SYSCALL_DEFINE6`（`syscalls.h:220`）仍负责挂上 `SYSCALL_METADATA`：tracepoint 的事件名 `sys_enter_<name>` / `sys_exit_<name>` 从这里来。

---

## 3.5 走一遍 `getpid`

tbl `:50`：`39 common getpid sys_getpid`。

```c
/* kernel/sys.c:958 */
SYSCALL_DEFINE0(getpid)
{
	return task_tgid_vnr(current);
}
```

上面的注释（`:950`）写明：尽管名字是 getpid，返回的是 tgid。同一线程组里 `CLONE_THREAD` 出来的线程 tgid 相同，各自的 pid 不同。`gettid` 在 `:964`，返回 `task_pid_vnr(current)`。

调用链就是：

```text
syscall                rax = 39
entry_SYSCALL_64       orig_ax = 39，ax = -ENOSYS
do_syscall_64
  syscall_enter_from_user_mode
  do_syscall_x64         39 < NR_syscalls
    x64_sys_call         case 39: return __x64_sys_getpid(regs)
      __do_sys_getpid    task_tgid_vnr(current)
  regs->ax = 那个 tgid
syscall_exit_to_user_mode
sysretq                用户 rax = tgid
```

`getpid` 不碰用户指针。要搬用户缓冲的调用在桩之后进入 `copy_from_user` / `copy_to_user`，见 [05](./05-usercopy.md)。`clock_gettime` 的内核实现是 `kernel/time/posix-timers.c:1133` 的 `SYSCALL_DEFINE2(clock_gettime, ...)`（`CONFIG_POSIX_TIMERS=y` 时编这个文件，`kernel/time/Makefile` 的 `ifeq`）。它通过 `put_timespec64` 把结果写回用户指针；libc 平时不走这里，而走 vDSO。

---

## 3.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/entry/common.c` | `do_syscall_64` | 69 | 垫栈、进入、分派、退出 |
| `arch/x86/entry/common.c` | `do_syscall_x64` | 36 | 边界检查后调用开关 |
| `arch/x86/entry/syscall_64.c` | `x64_sys_call` | 27 | `switch (nr)` |
| `arch/x86/entry/syscall_64.c` | `sys_call_table` | 20 | 给 tracing 的函数地址表 |
| `include/linux/nospec.h` | `array_index_nospec` | 55 | 把下标夹进范围内 |
| `include/linux/randomize_kstack.h` | `add_random_kstack_offset` | 50 | 用上一次留下的偏移垫栈 |
| `arch/x86/include/asm/syscall_wrapper.h` | `SC_X86_64_REGS_TO_ARGS` | 56 | 第四个参数取 `regs->r10` |
| `kernel/sys.c` | `getpid` | 958 | `task_tgid_vnr(current)` |
| `arch/x86/entry/common.c` | `ni_syscall` | 340 | 返回 `-ENOSYS` |

---

**导航**：[← 入口](./02-x86-entry.md) · [目录](./README.md) · [进入/退出工作 →](./04-enter-exit-work.md)
