# 8. `request_module`

**结论**：内核里要一个还没加载的模块时调用 `request_module`，它同步跑用户态 `modprobe`。`modprobe` 再走 `finit_module`。成功返回只说明 modprobe 进程退出码是 0，调用方还得自己确认那个服务现在真的在。

> `include/linux/kmod.h`。`__request_module`：`kernel/module/kmod.c:132`。`call_modprobe`：`:72`。`CONFIG_MODULES` 关掉时 `request_module` 是直接返回 `-ENOSYS` 的内联函数（`kmod.h:30`）。

---

## 8.1 三个宏

```c
/* include/linux/kmod.h:25 */
#define request_module(mod...) __request_module(true, mod)
#define request_module_nowait(mod...) __request_module(false, mod)
#define try_then_request_module(x, mod...) \
	((x) ?: (__request_module(true, mod), (x)))
```

`wait == true` 时 `call_modprobe` 使用 `UMH_WAIT_PROC`，等 modprobe 退出。`false` 时用 `UMH_WAIT_EXEC`，exec 出去就返回。`try_then_request_module` 先看表达式 `x`，为假才去加载，然后再求一次 `x`。

`modprobe_path` 初值是 `CONFIG_MODPROBE_PATH`（`kmod.c:64`）。sysctl 项 `/proc/sys/kernel/modprobe` 指向这块缓冲（`kernel/sysctl.c:1743`，`proc_dostring`，mode `0644`）。字符串空时 `__request_module` 直接 `-ENOENT`（`kmod.c:146`）。

---

## 8.2 `__request_module`

```c
/* kernel/module/kmod.c:132 */
int __request_module(bool wait, const char *fmt, ...)
{
	WARN_ON_ONCE(wait && current_is_async());

	if (!modprobe_path[0])
		return -ENOENT;

	/* vsnprintf 到 module_name[MODULE_NAME_LEN]，超长 -ENAMETOOLONG */

	ret = security_kernel_module_request(module_name);
	if (ret)
		return ret;

	ret = down_timeout(&kmod_concurrent_max,
			   MAX_KMOD_ALL_BUSY_TIMEOUT * HZ);
	if (ret) {
		pr_warn_ratelimited("request_module: modprobe %s cannot be processed, ...");
		return ret;
	}

	trace_module_request(module_name, wait, _RET_IP_);

	if (kmod_dup_request_exists_wait(module_name, wait, &dup_ret)) {
		ret = dup_ret;
		goto out;
	}

	ret = call_modprobe(module_name, wait ? UMH_WAIT_PROC : UMH_WAIT_EXEC);
out:
	up(&kmod_concurrent_max);
	return ret;
}
```

`WARN_ON_ONCE(wait && current_is_async())` 对应注释（`:139`）：模块 init 里可能 `async_synchronize_full()`，而异步工作者自己又在等这次加载，会锁死。`do_init_module` 里指向同一问题的注释在 `main.c:2569`。

并发上限是 `MAX_KMOD_CONCURRENT`（50，`:46`），信号量在 `:47`。等锁超过 `MAX_KMOD_ALL_BUSY_TIMEOUT`（5 秒，`:59`）就失败。注释认为 50 个 modprobe 同时不返回，多半是模块依赖绕成了环。

`kmod_dup_request_exists_wait` 把同一名字上已经在进行的请求合并掉，避免重复 `vmalloc` 镜像。重复请求的统计在 `kernel/module/dups.c`，本章不展开。

返回值：0，或者负的 errno，或者 modprobe 的正退出码（函数注释 `:122`）。注释写明：modprobe 成功退出之后模块仍可能在自己的 init 里失败并卸掉，调用方不能假设符号已经可用。

---

## 8.3 `call_modprobe`

```c
/* kernel/module/kmod.c:72 */
static int call_modprobe(char *orig_module_name, int wait)
{
	static char *envp[] = {
		"HOME=/",
		"TERM=linux",
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		NULL
	};

	argv[0] = modprobe_path;
	argv[1] = "-q";
	argv[2] = "--";
	argv[3] = module_name;    /* kstrdup 出来的一份 */
	argv[4] = NULL;

	info = call_usermodehelper_setup(modprobe_path, argv, envp, GFP_KERNEL,
					 NULL, free_modprobe_argv, NULL);
	ret = call_usermodehelper_exec(info, wait | UMH_KILLABLE);
	return ret;
}
```

argv 是 `modprobe -q -- <name>`。`-q` 让 modprobe 安静，`--` 结束选项。`free_modprobe_argv`（`:66`）释放 `argv[3]` 和 argv 数组。`UMH_KILLABLE` 和 `wait` 按位或在一起。usermodehelper 怎样 `kernel_thread` 出一份没有 `PF_KTHREAD` 的任务，见 [process-management/05](../process-management/05-kernel-threads.md) 里 `user_mode_thread` 的另一个调用点（`kernel/umh.c`）。

modprobe 找到 `.ko` 之后调用的是 `finit_module`（[06](./06-module-load.md)）。内核这边不解析 modules.alias；别名是用户态的事。块层 `request_module("%s-iosched", name)` 能不能自动把 kyber 拉起来，取决于发行版的 alias，内核树里能核对的范围见 [block-io/04](../block-io/04-sched-none.md)。本章不另写一份。

---

## 8.4 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `include/linux/kmod.h` | `request_module` | 25 | `__request_module(true, ...)` |
| `include/linux/kmod.h` | `request_module_nowait` | 26 | 不等进程退出 |
| `kernel/module/kmod.c` | `__request_module` | 132 | 限流、去重、调 modprobe |
| `kernel/module/kmod.c` | `call_modprobe` | 72 | `modprobe -q -- <name>` |
| `kernel/module/kmod.c` | `modprobe_path` | 64 | 默认 `CONFIG_MODPROBE_PATH` |
| `kernel/sysctl.c` | `modprobe` 项 | 1743 | `/proc/sys/kernel/modprobe` |

---

**导航**：[← 卸下](./07-module-unload.md) · [目录](./README.md) · [cmwq 对象 →](./09-cmwq-objects.md)
