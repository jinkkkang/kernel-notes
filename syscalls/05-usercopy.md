# 5. `copy_*_user` 与 SMAP

**结论**：系统调用参数里的用户指针不能直接解引用。x86-64 的 `access_ok` 只接受用户半边地址（符号位为 0）且 `ptr+size` 不溢出；真正搬字节时 `copy_user_generic` 用 `stac()` / `clac()` 包住拷贝。缺页或权限失败走 `_ASM_EXTABLE_UA`，函数返回还没拷完的字节数，调用方把它变成 `-EFAULT`。

> `access_ok` 的 x86-64 实现在 `arch/x86/include/asm/uaccess_64.h:85`。`copy_from_user` 在 `include/linux/uaccess.h:180`，出线的 `_copy_from_user` 在 `lib/usercopy.c:11`（本架构没有把 `INLINE_COPY_FROM_USER` 编进来）。

缺页怎么分配页、SMAP 违例在故障里怎么分类，见 [memory-management/07-page-fault.md](../memory-management/07-page-fault.md)。要按用户地址拿到 `struct page` 并在页表之外持有，走 GUP/pin，见 [07b](../memory-management/07b-gup-and-pin.md)。本章只覆盖「系统调用里搬一段用户缓冲」。

---

## 5.1 为什么不能直接 `*user_ptr`

内核和用户共用一张页表时，一个用户指针可能指向：

- 用户半边里一块有效映射，可以读
- 用户半边里的洞，访问会缺页
- 内核半边，若允许内核随便解引用，系统调用就变成任意内核读

SMAP 在 CPU 的 `flags` 里（本机 `/proc/cpuinfo` 有 `smap`，sampled on 6.12.113+）。AC 标志为 0 时，内核态访问用户页会故障。`syscall_init` 的 `MSR_SYSCALL_MASK` 已经把 AC 清掉（[02](./02-x86-entry.md)），所以一进内核默认不能碰用户页。要碰，必须在这一小段里 `stac()`，完了 `clac()`。

```c
/* arch/x86/include/asm/smap.h:30 */
static __always_inline void clac(void)
{
	alternative("", __ASM_CLAC, X86_FEATURE_SMAP);   /* 0f 01 ca */
}
static __always_inline void stac(void)
{
	alternative("", __ASM_STAC, X86_FEATURE_SMAP);   /* 0f 01 cb */
}
```

没有 SMAP 特性时 `alternative` 换成空指令。`__uaccess_begin` / `__uaccess_end` 就是 `stac()` / `clac()`（`arch/x86/include/asm/uaccess.h:37`）。`get_user` 一类走 `__uaccess_begin_nospec`，里面多一次 `barrier_nospec()`。

---

## 5.2 `access_ok`

通用头 `include/asm-generic/access_ok.h:31` 在没有自己的 `__access_ok` 时，用 `TASK_SIZE_MAX` 做 `size <= limit && addr <= limit - size`。x86-64 在包含它之前就定义了自己的版本，并用 `#define __access_ok __access_ok`（`uaccess_64.h:94`）挡住通用函数。

```c
/* arch/x86/include/asm/uaccess_64.h:57 */
#define valid_user_address(x) ((long)(x) >= 0)

/* :85 */
static inline bool __access_ok(const void __user *ptr, unsigned long size)
{
	if (__builtin_constant_p(size <= PAGE_SIZE) && size <= PAGE_SIZE) {
		return valid_user_address(ptr);
	} else {
		unsigned long sum = size + (unsigned long)ptr;
		return valid_user_address(sum) && sum >= (unsigned long)ptr;
	}
}
```

注释（`:52`）说明：转成有符号数之后，用户指针是正的，内核指针是负的。小的编译期长度只检查起点在用户半边；更长的范围还要求 `ptr+size` 仍是正数并且加法没有回绕。`access_ok(addr, size)` 是 `likely(__access_ok(addr, size))`（`asm-generic/access_ok.h:45`）。

这只是范围检查。用户半边里没有映射的地址（比如 `(void *)1`）也能通过 `access_ok`，真正访问时才缺页，再由异常表把失败交回来。

---

## 5.3 `copy_from_user` 到 `stac`

```c
/* include/linux/uaccess.h:180 */
copy_from_user(to, from, n)
    check_copy_size(to, n, false)     /* thread_info.h:237，目标对象太小就拒绝 */
    _copy_from_user(to, from, n)

/* lib/usercopy.c:11  没有 INLINE_COPY_FROM_USER 时的定义 */
_copy_from_user(...)
    might_fault()
    access_ok(from, n) 失败则 res 保持为 n
    否则 barrier_nospec()
         raw_copy_from_user()          /* uaccess_64.h:125 */
    有剩余字节则把内核目标的尾巴清零
    return res                        /* 0 表示全部拷完 */
```

`raw_copy_from_user` 就是 `copy_user_generic(dst, src, size)`（`uaccess_64.h:125`）。`copy_to_user` 对称，出线版在 `lib/usercopy.c:34`，失败时不清用户缓冲。

```c
/* arch/x86/include/asm/uaccess_64.h:105 */
static __always_inline unsigned long
copy_user_generic(void *to, const void *from, unsigned long len)
{
	stac();
	asm volatile(
		"1:\n\t"
		ALTERNATIVE("rep movsb",
			    "call rep_movs_alternative", ALT_NOT(X86_FEATURE_FSRM))
		"2:\n"
		_ASM_EXTABLE_UA(1b, 2b)
		:"+c" (len), "+D" (to), "+S" (from), ASM_CALL_CONSTRAINT
		: : "memory", "rax");
	clac();
	return len;                    /* rcx 里剩下的字节数 */
}
```

有 `X86_FEATURE_FSRM` 时用 `rep movsb`，调用约定和 `rep_movs_alternative` 一样：`rcx` 是还没拷的字节数（`copy_user_64.S:16` 的注释写明这一点）。没有 FSRM 时 `call rep_movs_alternative`（`:32`）。那条函数对小块逐字节或逐 8 字节拷，对大块在 ERMS 上用 `rep movsb`，否则对齐后 `rep movsq`。

`copy_from_user` 的返回值是剩余字节数。系统调用实现把它当成失败：`put_timespec64` 失败时 `clock_gettime` 返回 `-EFAULT`（`kernel/time/posix-timers.c:1146`）。剩余字节数本身通常不返回给用户。

---

## 5.4 `_ASM_EXTABLE_UA`

```c
/* arch/x86/include/asm/asm.h:229 */
#define _ASM_EXTABLE_UA(from, to)				\
	_ASM_EXTABLE_TYPE(from, to, EX_TYPE_UACCESS)
```

拷贝指令的地址和修复地址写进异常表，类型是 `EX_TYPE_UACCESS`。缺页处理找到这项之后调用 `ex_handler_uaccess`（`arch/x86/mm/extable.c:156`），它在 `#GP` 且地址不合适时 `WARN_ONCE`，然后 `ex_handler_default`（`:32`）把 `regs->ip` 改成 `ex_fixup_addr(e)`。

`copy_user_generic` 的修复点是标签 `2`，也就是 `rep movsb` 之后、`clac()` 之前。`rcx` 仍是没拷完的数量，函数把它返回。`rep_movs_alternative` 里每条可能故障的 mov 都有自己的 `_ASM_EXTABLE_UA`，修复点跳到 `.Lexit` 或 `.Lcopy_user_tail`（`copy_user_64.S:52` 起），同样带着剩余的 `rcx` 返回。

页是谁补上的、写时复制、SMAP 故障为什么直接失败，仍在内存章。这里的合同只是：异常表把「这次用户访问失败」变成「拷贝函数返回非 0」。

---

## 5.5 本机采样

`demos/02-bad-pointer-efault.c`，sampled on 6.12.113+：

```text
uname.release=6.12.113+
write(1, (void *)1, 1) n=-1 errno=14 (Bad address)
read(/dev/zero, (void *)1, 16) n=-1 errno=14 (Bad address)
write(1, (void *)-1, 1) n=-1 errno=14 (Bad address)
write(/dev/null, &stack_byte, 1) n=1 errno=0 (-)
```

`(void *)1` 在用户半边但没有映射。`(void *)-1` 在内核半边，过不了 `valid_user_address`。栈上的一字节写到 `/dev/null` 成功。errno 14 是 `EFAULT`。这是用户态看到的结果；内核里 `copy_*_user` 返回的是剩余字节数，VFS 再把它变成 `-EFAULT`。

---

## 5.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/include/asm/uaccess_64.h` | `__access_ok` | 85 | 用户半边且不回绕 |
| `include/linux/uaccess.h` | `copy_from_user` | 180 | 检查对象大小后调用 `_copy_from_user` |
| `lib/usercopy.c` | `_copy_from_user` | 11 | `access_ok` + `raw_copy_from_user` + 尾巴清零 |
| `arch/x86/include/asm/uaccess_64.h` | `copy_user_generic` | 105 | `stac` / `rep movs` / `clac` |
| `arch/x86/lib/copy_user_64.S` | `rep_movs_alternative` | 32 | 没有 FSRM 时的拷贝 |
| `arch/x86/include/asm/asm.h` | `_ASM_EXTABLE_UA` | 229 | 类型 `EX_TYPE_UACCESS` |
| `arch/x86/mm/extable.c` | `ex_handler_uaccess` | 156 | 把 IP 改到修复点 |
| `arch/x86/include/asm/smap.h` | `stac` / `clac` | 36 / 30 | 按 SMAP 特性插入指令 |

---

**导航**：[← 进入/退出](./04-enter-exit-work.md) · [目录](./README.md) · [vDSO →](./06-vdso-and-vvar.md)
