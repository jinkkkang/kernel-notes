# 7. 模块卸下

**结论**：`delete_module` 在 `source_list` 为空、状态是 `LIVE`、并且引用计数能降到基数时，把状态改成 `GOING`，调用 `mod->exit`，再 `free_module`。有 init 却没有 exit 的模块默认卸不掉。

> 系统调用：`kernel/module/main.c:698`。引用计数：`module_unload_init`（`:545`）、`try_module_get`（`:831`）、`try_release_module_ref`（`:652`）。下面的链表和 `exit` 都在 `CONFIG_MODULE_UNLOAD` 下。

---

## 7.1 `delete_module`

```c
/* kernel/module/main.c:698 */
SYSCALL_DEFINE2(delete_module, const char __user *, name_user,
		unsigned int, flags)
{
	if (!capable(CAP_SYS_MODULE) || modules_disabled)
		return -EPERM;

	/* strncpy_from_user 到 name[MODULE_NAME_LEN] */

	mutex_lock_interruptible(&module_mutex);
	mod = find_module(name);                 /* :720 */
	if (!list_empty(&mod->source_list))      /* :726 还有别人依赖我 */
		return -EWOULDBLOCK;
	if (mod->state != MODULE_STATE_LIVE)     /* :733 */
		return -EBUSY;
	if (mod->init && !mod->exit) {           /* :741 有 init 无 exit */
		forced = try_force_unload(flags);
		if (!forced)
			return -EBUSY;
	}
	ret = try_stop_module(mod, flags, &forced);
	mutex_unlock(&module_mutex);

	if (mod->exit != NULL)
		mod->exit();                     /* :757 */
	blocking_notifier_call_chain(..., MODULE_STATE_GOING, mod);
	klp_module_going(mod);
	ftrace_release_mod(mod);
	async_synchronize_full();
	free_module(mod);                        /* :769 */
	wake_up_all(&module_wq);
	return 0;
}
```

`find_module` 找不到是 `-ENOENT`（`:722`）。依赖者还在是 `-EWOULDBLOCK`，注释写要先卸掉它们。正在 `COMING` 或已经 `GOING` 是 `-EBUSY`。

`try_force_unload`（`:637`）只在 `CONFIG_MODULE_FORCE_UNLOAD` 下看 `O_TRUNC`。置上这个标志会 `add_taint(TAINT_FORCED_RMMOD, ...)` 并允许继续。没有这个配置时函数恒返回 0，强制卸下走不到。

`try_stop_module`（`:666`）调用 `try_release_module_ref`。失败且不能强制时返回 `-EWOULDBLOCK`。成功则 `mod->state = MODULE_STATE_GOING`（`:676`）。`exit` 在放下 `module_mutex` 之后调用，所以 exit 函数里可以睡觉，不要再假设自己还持有 `module_mutex`。

`free_module`（`:1243`）先 `trace_module_free` 和 `mod_sysfs_teardown`，再在锁里把状态改成 `UNFORMED`（`:1254`），让并发的查找不要再把它当成形的模块。然后 `module_unload_free`、拆参数、从链表 `list_del_rcu`、释放 `mem[]`。

---

## 7.2 引用计数

```c
/* kernel/module/main.c:545 */
static int module_unload_init(struct module *mod)
{
	atomic_set(&mod->refcnt, MODULE_REF_BASE);   /* 1 */
	INIT_LIST_HEAD(&mod->source_list);
	INIT_LIST_HEAD(&mod->target_list);
	atomic_inc(&mod->refcnt);                    /* 加载过程自己持有 */
	return 0;
}
```

`do_init_module` 成功时 `module_put`（`:2579`）把这一次 inc 还回去。此后 `module_refcount()` 为 0 表示没有其它持有者，原子值仍是 `MODULE_REF_BASE`。

```c
/* kernel/module/main.c:652 */
static int try_release_module_ref(struct module *mod)
{
	ret = atomic_sub_return(MODULE_REF_BASE, &mod->refcnt);
	BUG_ON(ret < 0);
	if (ret)
		/* 有人还持有，把基数加回去 */
		ret = atomic_add_unless(&mod->refcnt, MODULE_REF_BASE, 0);
	return ret;
}
```

返回 0 才表示减掉基数后没有剩余引用。有剩余就用 `atomic_add_unless` 把基数加回，除非此时已经变成 0（别人刚好把它放完）。

```c
/* kernel/module/main.c:831 */
bool try_module_get(struct module *module)
{
	if (module) {
		if (likely(module_is_live(module) &&
			   atomic_inc_not_zero(&module->refcnt) != 0))
			trace_module_get(module, _RET_IP_);
		else
			ret = false;
	}
	return ret;
}
```

`module_is_live` 只排除 `GOING`。`GOING` 上 `try_module_get` 失败。`module_put`（`:847`）是 `atomic_dec_if_positive`，减完小于 0 会 `WARN`。`__module_get`（`:822`）是无条件 `atomic_inc`，调用方得自己保证模块不会在这一瞬间消失。

`strong_try_module_get`（`:152`）给「我要依赖这个模块」用：`COMING` 返回 `-EBUSY`，`try_module_get` 失败返回 `-ENOENT`。`ref_module`（`:599`）先 `strong_try_module_get(b)`，再 `add_module_usage`。

---

## 7.3 `source_list` 与 `target_list`

```c
/* include/linux/module.h:308 */
struct module_use {
	struct list_head source_list;
	struct list_head target_list;
	struct module *source, *target;
};
```

```c
/* kernel/module/main.c:582 */
static int add_module_usage(struct module *a, struct module *b)
{
	use->source = a;   /* a 使用 b */
	use->target = b;
	list_add(&use->source_list, &b->source_list);  /* 挂在 b 上：谁用我 */
	list_add(&use->target_list, &a->target_list);  /* 挂在 a 上：我用谁 */
}
```

`/proc/modules` 的依赖列走的是 `source_list`（`procfs.c:27`）：打印每个 `use->source->name`。有 init 无 exit 时再附加 `[permanent],`（`:32`）。`module_refcount(mod)` 印在大小和依赖列表之间（`:21`）。

卸下时 `module_unload_free`（`:620`）沿着本模块的 `target_list` 走，对每个 `use->target` `module_put`，再把两条链表都摘掉。它放下的是「我依赖别人」的那些引用，不负责把依赖我的人清掉——那一步被 `delete_module` 对 `source_list` 的非空检查提前拒绝了。

---

## 7.4 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `kernel/module/main.c` | `delete_module` | 698 | 卸下系统调用 |
| `kernel/module/main.c` | `try_stop_module` | 666 | 降引用，状态改 `GOING` |
| `kernel/module/main.c` | `try_release_module_ref` | 652 | 减去 `MODULE_REF_BASE` |
| `kernel/module/main.c` | `try_module_get` | 831 | 活着才 `inc_not_zero` |
| `kernel/module/main.c` | `module_put` | 847 | `dec_if_positive` |
| `kernel/module/main.c` | `ref_module` | 599 | 依赖边 + 强引用 |
| `kernel/module/main.c` | `add_module_usage` | 582 | 把 `module_use` 挂上两条链表 |
| `kernel/module/main.c` | `module_unload_free` | 620 | 卸下时放下我对别人的引用 |
| `kernel/module/main.c` | `free_module` | 1243 | 拆 sysfs、改回 `UNFORMED`、释放内存 |
| `kernel/module/procfs.c` | `m_show` | 74 | `/proc/modules` 的一行 |

---

**导航**：[← 装入](./06-module-load.md) · [目录](./README.md) · [request_module →](./08-kmod-request.md)
