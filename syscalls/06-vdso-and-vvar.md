# 6. vDSO 与 vvar

**结论**：`clock_gettime` 这类只读内核已经发布出来的时钟，不必每次 `syscall`。`arch_setup_additional_pages` 把一段可执行的 `[vdso]` 和一段只读的 `[vvar]` 映进地址空间。vDSO 里的 `__vdso_clock_gettime` 用顺序锁读 `struct vdso_data`，自己算出 `timespec`。算不了才执行 `syscall` 指令落到内核的 `sys_clock_gettime`。

> 映射在 `arch/x86/entry/vdso/vma.c:223` 的 `map_vdso`。数据页布局在 `include/vdso/datapage.h:90`。算法在 `lib/vdso/gettimeofday.c`，x86 的 `vclock_gettime.c:15` 把它包含进来。

timekeeping 怎样更新 `cycle_last` 和倍率不在本章。time namespace 的偏移怎样写见 [containerization/04](../containerization/04-pid-user-uts-ipc-time-ns.md)。

---

## 6.1 为什么可以不陷入

内核周期性地把「这个时钟在某个周期计数上的基准时间」写进所有 CPU 共享的 vvar 页。用户态读 TSC（或页里标明的别的计数器），用 `mult` / `shift` 换成纳秒，加上 `basetime`。顺序锁 `seq` 保证读到的不是写了一半的快照。

这条路径没有换栈、没有 `swapgs`、没有 `pt_regs`。本机 libc `clock_gettime(CLOCK_MONOTONIC)` 和强制 `syscall` 指令的周期差在 [11](./11-perf-analysis.md)。

---

## 6.2 两段映射

`arch_setup_additional_pages`（`vma.c:367`，`CONFIG_X86_64` 这一份）在 `vdso64_enabled` 非 0 时调用 `map_vdso_randomized(&vdso_image_64)`。`vdso=` 启动参数由 `vdso_setup`（`:416`）写成这个变量，默认是 1（`:44`）。

`map_vdso`（`:223`）在 `mmap_write_lock` 下做两次 `_install_special_mapping`：

| 段 | 权限 | 名字 | 行号 |
|---|---|---|---|
| 代码 | `VM_READ\|VM_EXEC\|VM_MAYREAD\|VM_MAYWRITE\|VM_MAYEXEC` | `[vdso]`（`vdso_mapping`，`:208`） | 245 |
| 数据 | `VM_READ\|VM_MAYREAD\|VM_IO\|VM_DONTDUMP\|VM_PFNMAP` | `[vvar]`（`vvar_mapping`，`:213`） | 257 |

注释（`:242`）说明代码段带 `VM_MAYWRITE` 是为了让 gdb 写时复制并下断点。数据段是 PFN 映射，故障函数 `vvar_fault`（`:128`）按符号偏移插入页帧，不走普通文件缺页。

`vvar_fault` 在偏移等于 `sym_vvar_page` 时调用 `find_timens_vvar_page(vma)`（`:152`）。任务在非根 time namespace 里时，这个偏移映的是命名空间自己的页，真实 vvar 页改映到 `sym_timens_page`。函数声明在 `include/linux/time_namespace.h:47`；`CONFIG_TIME_NS` 关掉时内联返回 `NULL`（`:144`）。换命名空间时 `vdso_join_timens`（`vma.c:111`）把已有的 `[vvar]` VMA `zap_vma_pages`，下次访问再缺页。偏移量本身的语义在容器章。

64 位地址由 `vdso_addr`（`:289`）放在栈顶附近再随机化一截。注释承认分布在一个 PMD 里偏向末尾。

导出符号在 `arch/x86/entry/vdso/vdso.lds.S:17` 的 `LINUX_2.6` 版本脚本里：

```text
clock_gettime / __vdso_clock_gettime
gettimeofday / __vdso_gettimeofday
getcpu       / __vdso_getcpu
time         / __vdso_time
clock_getres / __vdso_clock_getres
__vdso_sgx_enter_enclave     仅 CONFIG_X86_SGX
```

`vclock_gettime.c:41` 的 `__vdso_clock_gettime` 直接 `return __cvdso_clock_gettime(clock, ts)`，并给一个弱别名 `clock_gettime`。

---

## 6.3 `struct vdso_data`

```c
/* include/vdso/datapage.h:54 */
struct vdso_timestamp {
	u64 sec;
	u64 nsec;          /* 高分辨率时钟上，nsec 左移了 cs[x].shift */
};

/* :90  同一页会被 64 位和 compat 代码同时读，改字段要两边一起看 */
struct vdso_data {
	u32 seq;           /* 顺序锁。奇数表示正在更新 */

	s32 clock_mode;    /* 用哪种硬件计数器；TIMENS 时迫使走慢路径 */
	u64 cycle_last;    /* 基准周期 */
	u64 mask;
	u32 mult;
	u32 shift;

	union {
		struct vdso_timestamp basetime[VDSO_BASES]; /* 每个 clock_id 一份基准 */
		struct timens_offset  offset[VDSO_BASES];   /* 命名空间页上改成偏移 */
	};

	s32 tz_minuteswest;
	s32 tz_dsttime;
	u32 hrtimer_res;
	u32 __unused;

	struct arch_vdso_data arch_data;
};
```

`VDSO_BASES` 是 `CLOCK_TAI + 1`（`:28`）。高分辨率时钟的掩码 `VDSO_HRES`（`:29`）包含 `REALTIME`、`MONOTONIC`、`BOOTTIME`、`TAI`。粗时钟是 `REALTIME_COARSE` 和 `MONOTONIC_COARSE`。`MONOTONIC_RAW` 单独用 `CS_RAW` 那一页（`:38`，`CS_BASES` 是 2）。

页上实际有 `_vdso_data[CS_BASES]`（`:121`）。`__cvdso_clock_gettime_common`（`lib/vdso/gettimeofday.c:227`）把 `CLOCK_*` 收成位，高分辨率和 RAW 进 `do_hres`，粗时钟进 `do_coarse`，其余返回 `-1`，外层再 `clock_gettime_fallback`（`:261`）执行真正的 `syscall`。

`do_hres`（`:110`）的读循环：

```text
seq = READ_ONCE(vd->seq)
seq 为奇数:
    clock_mode == VDSO_CLOCKMODE_TIMENS → do_hres_timens
    否则 cpu_relax() 再读
smp_rmb()
读 cycle_last / basetime / mult / shift，加上硬件计数器的增量
vdso_read_retry：再读 seq，和一开始不同就重来
循环外把 ns 折成 tv_sec / tv_nsec
```

注释（`:123`）说明 time namespace 的 vvar 把 `seq` 固定成 1、`clock_mode` 设成 `VDSO_CLOCKMODE_TIMENS`，这样奇数检查会走进 `do_hres_timens`，到真实页上读主机时间再加偏移。不在 time namespace 里的任务只是在自旋等 `seq` 变成偶数时多看一眼 `clock_mode`。

`vdso_read_begin` / `vdso_read_retry` 在 `include/vdso/helpers.h:9` 和 `:20`，粗时钟路径用的是这对助手；`do_hres` 把「奇数则可能是 timens」写开了，所以没有直接调用 `vdso_read_begin`。

---

## 6.4 本机 maps

sampled on 6.12.113+，`grep -E 'vdso|vvar|vsyscall' /proc/self/maps` 的一次输出：

```text
7fa929818000-7fa92981c000 r--p 00000000 00:00 0                          [vvar]
7fa92981c000-7fa92981e000 r-xp 00000000 00:00 0                          [vdso]
ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0                  [vsyscall]
```

`[vvar]` 只读，紧挨在 `[vdso]` 前面，代码段可执行。这两段的起始地址每次启动都变（`vdso_addr` 的随机化）。`[vsyscall]` 固定在 `ffffffffff600000`，和 `VSYSCALL_ADDR` 一致，见下一节。

---

## 6.5 遗留的 `[vsyscall]` 页

`arch/x86/include/uapi/asm/vsyscall.h:11`：

```c
#define VSYSCALL_ADDR (-10UL << 20)    /* 0xffffffffff600000 */
```

早期用户态直接 `call` 这个固定地址。现在这一页默认不作为可执行数据页交给进程随便读。`emulate_vsyscall`（`arch/x86/entry/vsyscall/vsyscall_64.c:115`）在缺页处理里识别「指令获取落在这个地址」：

- 写故障或内核特权故障直接返回 false（`:124`）
- `address != regs->ip` 时不当成取指。`EMULATE` 模式返回 false；否则打一条拒绝读 vsyscall 页的警告（`:132`）
- `vsyscall=none` 时警告并返回 false（`:157`）
- 否则按页内偏移认出 `gettimeofday` / `time` / `getcpu`，改成一次正常系统调用

模式变量 `vsyscall_mode`（`:45`）由 `CONFIG_LEGACY_VSYSCALL_NONE` 或 `CONFIG_LEGACY_VSYSCALL_XONLY` 决定初值，启动参数 `vsyscall=` 可以改成 `emulate` / `xonly` / `none`（`:54`）。本机 maps 上这一页的权限是 `--xp`：可执行、不可读。这和 `xonly` 的形状一致；具体编进这颗 6.12 内核的 Kconfig 没有在 v6.6.160 的源码行号里再对一遍。

---

## 6.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `arch/x86/entry/vdso/vma.c` | `arch_setup_additional_pages` | 367 | exec 时映射 64 位 vDSO |
| `arch/x86/entry/vdso/vma.c` | `map_vdso` | 223 | `[vdso]` + `[vvar]` |
| `arch/x86/entry/vdso/vma.c` | `vvar_fault` | 128 | 按符号插入 vvar / timens 页 |
| `arch/x86/entry/vdso/vdso.lds.S` | `LINUX_2.6` | 17 | 导出符号 |
| `arch/x86/entry/vdso/vclock_gettime.c` | `__vdso_clock_gettime` | 41 | 转到 `__cvdso_clock_gettime` |
| `lib/vdso/gettimeofday.c` | `__cvdso_clock_gettime` | 266 | 选数据页并计算 |
| `lib/vdso/gettimeofday.c` | `do_hres` | 110 | 顺序锁 + 硬件计数器 |
| `include/vdso/datapage.h` | `struct vdso_data` | 90 | vvar 上的快照 |
| `arch/x86/entry/vsyscall/vsyscall_64.c` | `emulate_vsyscall` | 115 | 固定地址上的旧入口 |

---

**导航**：[← usercopy](./05-usercopy.md) · [目录](./README.md) · [compat →](./07-compat.md)
