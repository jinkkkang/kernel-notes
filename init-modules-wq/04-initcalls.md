# 4. initcall

**结论**：内建初始化函数被链接进 `.initcallearly.init` 和 `.initcall0` … `.initcall7`。`do_pre_smp_initcalls` 在打开次级 CPU 之前跑 early；`do_initcalls` 按 pure、core、postcore、arch、subsys、fs、device、late 跑剩下的级别。可加载模块里的 `module_init` 不进这些段，装入时由 `do_init_module` 调用同一个 `do_one_initcall`。

> 级别宏：`include/linux/init.h:290` 起。循环：`init/main.c` 的 `do_pre_smp_initcalls`（`:1315`）、`do_initcalls`（`:1280`）、`do_one_initcall`（`:1205`）。链接顺序：`include/asm-generic/vmlinux.lds.h:894` 的 `INIT_CALLS`（每一级的宏在 `:889`）。

---

## 4.1 级别

内建代码（没有定义 `MODULE`）用 `__define_initcall`（`init.h:283`）把函数指针放进 `.initcall##id`。

| 宏 | 行 | 段 id | `initcall_level_names[]` |
|----|----|-------|--------------------------|
| `early_initcall` | `init.h:290` | `early` | 不在那个数组里。段是 `.initcallearly.init` |
| `pure_initcall` | `:299` | `0` | `"pure"`（`main.c:1249`） |
| `core_initcall` / `_sync` | `:301` | `1` / `1s` | `"core"` |
| `postcore_initcall` / `_sync` | `:303` | `2` / `2s` | `"postcore"` |
| `arch_initcall` / `_sync` | `:305` | `3` / `3s` | `"arch"` |
| `subsys_initcall` / `_sync` | `:307` | `4` / `4s` | `"subsys"` |
| `fs_initcall` / `_sync` | `:309` | `5` / `5s` | `"fs"` |
| `rootfs_initcall` | `:311` | `rootfs` | 没有单独的名字。链接在 level 5 和 level 6 之间 |
| `device_initcall` / `_sync` | `:312` | `6` / `6s` | `"device"` |
| `late_initcall` / `_sync` | `:314` | `7` / `7s` | `"late"` |
| `__initcall` | `:317` | 同 `device_initcall` | — |

`_sync` 放在同一级的 `.initcallNs.init`。`INIT_CALLS_LEVEL`（`vmlinux.lds.h:889`）先 `KEEP` 普通段，再 `KEEP` `s` 段，所以同步变体排在同一级的后半。

```c
/* include/asm-generic/vmlinux.lds.h:894 */
#define INIT_CALLS							\
		__initcall_start = .;					\
		KEEP(*(.initcallearly.init))				\
		INIT_CALLS_LEVEL(0)					\
		INIT_CALLS_LEVEL(1)					\
		INIT_CALLS_LEVEL(2)					\
		INIT_CALLS_LEVEL(3)					\
		INIT_CALLS_LEVEL(4)					\
		INIT_CALLS_LEVEL(5)					\
		INIT_CALLS_LEVEL(rootfs)				\
		INIT_CALLS_LEVEL(6)					\
		INIT_CALLS_LEVEL(7)					\
		__initcall_end = .;
```

`initcall_levels[]`（`main.c:1235`）只有 `__initcall0_start` … `__initcall7_start` 和 `__initcall_end`，没有 `__initcallrootfs_start`。`do_initcall_level(5)` 从 `__initcall5_start` 走到 `__initcall6_start` 之前，因此链接器放在这两符号之间的 rootfs 调用会在名字仍是 `"fs"` 的那一轮里被执行。`trace_initcall_level` 打出来的字符串来自 `initcall_level_names`，不会单独出现 `rootfs`。

`early_initcall` 的注释写明只给内建代码，在初始化 SMP 之前跑（`init.h:286`）。`pure_initcall` 的注释写它不依赖别的子系统，只给不能静态初始化的变量（`:293`）。

可加载模块把上述宏全部定义成 `module_init`（`include/linux/module.h:111`）。模块里的 `core_initcall` 不会在启动的 core 阶段跑。

---

## 4.2 谁在什么时候扫这些段

early，在 `smp_init` 之前，由 pid 1 调用：

```c
/* init/main.c:1315 */
static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}
```

`pure` 到 `late`：

```c
/* init/main.c:1265 */
static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}
```

`parse_args` 的 level 参数让这一级的 `module_param` 在函数被调用前先吃到命令行。`do_initcalls`（`:1280`）把 `saved_command_line` 复制一份，每一级开始前再 `strcpy` 回去，因为解析会改缓冲区。分配失败直接 `panic`。

`initcall_from_entry`（`init.h:125`）在 `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS` 下把表项当成相对位移；否则表项就是函数指针。

`__init`（`init.h:52`）把函数放进 `.init.text`。`kernel_init` 在 initcall 全部结束之后 `free_initmem`（[03](./03-rest-init-and-pid1.md)），所以 initcall 函数和 `__initdata` 不能留到用户态还被调用。模块的 init 段是另一套 `MOD_INIT_*`，由 `do_init_module` 释放，见 [05](./05-module-objects.md)。

---

## 4.3 `do_one_initcall`

```c
/* init/main.c:1205 */
int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;
	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);
	add_latent_entropy();
	return ret;
}
```

返回值原样交回去。`do_init_module` 对 `< 0` 判失败，对 `> 0` 只警告并继续把模块标成 `LIVE`（[06](./06-module-load.md)）。抢占计数被改过会先恢复再 `WARN`。返回时中断仍关着会被重新打开。

黑名单：`CONFIG_KALLSYMS` 下 `initcall_blacklist=`（`__setup` 在 `:1154`）把逗号分隔的函数名放进 `blacklisted_initcalls`（`:1087`）。`initcall_blacklisted`（`:1115`）用 `sprint_symbol_no_offset` 得到名字，在空格处截断（去掉 ` [module]`），命中则 `do_one_initcall` 返回 `-EPERM`，函数本身不跑。没有 `CONFIG_KALLSYMS` 时 `initcall_blacklist`（`:1143`）只打一条警告，检查函数恒返回 false（`:1149`）。

---

## 4.4 `initcall_debug`

```c
/* init/main.c:771 */
bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);
```

`core_param`（`include/linux/moduleparam.h:327`）不加 `KBUILD_MODNAME` 前缀，命令行就是 `initcall_debug`。`params.c:840` 把没有点号的参数归到名为 `kernel` 的 kobject，sysfs 上是 `/sys/module/kernel/parameters/initcall_debug`。perm `0644`，运行中还能改，但启动期的 initcall 已经跑完，改它只影响之后还要进 `do_one_initcall` 的路径（模块加载）。

`start_kernel:949` 在 `trace_init` 之后，若布尔值为真就 `initcall_debug_enable`（`:1178`）：注册 `initcall_start` / `initcall_finish` 两个 tracepoint，回调把 `KERN_DEBUG` 打到 printk。格式串是：

```c
/* init/main.c:1161 与 :1171 */
"calling  %pS @ %i\n"
"initcall %pS returned %d after %lld usecs\n"
```

没有 `TRACEPOINTS_ENABLED` 时，`do_trace_initcall_start`（`:1191`）在 `initcall_debug` 为真时直接调用同一个回调。本章没有贴过一次启动日志。打开方式和注意点在 [11](./11-observability.md)，标 RECIPE。

---

## 4.5 内建 `module_init` 和可加载 `module_init`

| | 内建（未定义 `MODULE`） | 可加载模块 |
|--|--------------------------|------------|
| `module_init(fn)` | `module.h:88`，即 `__initcall(fn)`，落在 device 级 | `module.h:130`，把 `fn` 别名成 `init_module` |
| `module_exit(fn)` | `module.h:100`，`__exitcall`。注释写静态编译时没有效果 | `module.h:138`，别名成 `cleanup_module` |
| 谁调用 | `do_initcalls` | `do_init_module` 里的 `do_one_initcall(mod->init)` |

modpost 在 `scripts/mod/modpost.c` 的 `add_header`（`:1833`）里生成 `__this_module`。有 init 时写出 `.init = init_module`（`:1862`）；有 exit 且 `CONFIG_MODULE_UNLOAD` 时写出 `.exit = cleanup_module`（`:1864`）。这段 C 放进 `.gnu.linkonce.this_module`，装入后就是那份 `struct module`。

---

## 4.6 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `include/linux/init.h` | `early_initcall` … `late_initcall` | 290–315 | 把函数放进对应 initcall 段 |
| `include/asm-generic/vmlinux.lds.h` | `INIT_CALLS` | 894 | 段的链接顺序，含 rootfs |
| `init/main.c` | `do_pre_smp_initcalls` | 1315 | early |
| `init/main.c` | `do_initcalls` | 1280 | level 0–7 |
| `init/main.c` | `do_initcall_level` | 1265 | 先 `parse_args` 再调用 |
| `init/main.c` | `do_one_initcall` | 1205 | 黑名单、trace、抢占与中断检查 |
| `init/main.c` | `initcall_blacklist` | 1087 | `CONFIG_KALLSYMS` 下的 `initcall_blacklist=` |
| `scripts/mod/modpost.c` | `add_header` | 1833 | 生成 `__this_module.init` |

---

**导航**：[← rest_init 与 pid 1](./03-rest-init-and-pid1.md) · [目录](./README.md) · [模块对象 →](./05-module-objects.md)
