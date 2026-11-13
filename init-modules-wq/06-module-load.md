# 6. 模块装入

**结论**：`finit_module` 从调用者给出的文件描述符读镜像，`load_module` 验签、摆好七段内存、做重定位，然后 `do_init_module` 调用 `mod->init`。成功后状态是 `LIVE`，init 段排队到 `init_free_wq` 上释放。

> 系统调用在 `kernel/module/main.c`。`finit_module`：`:3208`。`load_module`：`:2842`。`layout_and_allocate`：`:2372`。`apply_relocations`：`:1447`。`do_init_module`：`:2515`。签名算法不在本章。

---

## 6.1 `finit_module` 与还在的 `init_module`

```c
/* kernel/module/main.c:3208 */
SYSCALL_DEFINE3(finit_module, int, fd, const char __user *, uargs, int, flags)
{
	int err;
	struct fd f;

	err = may_init_module();
	if (err)
		return err;

	pr_debug("finit_module: fd=%d, uargs=%p, flags=%i\n", fd, uargs, flags);

	if (flags & ~(MODULE_INIT_IGNORE_MODVERSIONS
		      |MODULE_INIT_IGNORE_VERMAGIC
		      |MODULE_INIT_COMPRESSED_FILE))
		return -EINVAL;

	f = fdget(fd);
	err = idempotent_init_module(f.file, uargs, flags);
	fdput(f);
	return err;
}
```

`may_init_module`（`:2640`）要求 `CAP_SYS_MODULE`，并且 `modules_disabled == 0`，否则 `-EPERM`。`modules_disabled` 由命令行 `nomodule` 置位（`core_param`，`:128`，perm 0，所以参数本身不进 sysfs）。运行时的开关是 `/proc/sys/kernel/modules_disabled`（`kernel/sysctl.c:1750`，表 `kern_table` 在 `:1627`，`register_sysctl_init("kernel", kern_table)` 在 `:2266`）。置上之后不能再装、也不能再卸。

合法标志在 `include/uapi/linux/module.h`：

| 标志 | 值 | 行 |
|------|----|----|
| `MODULE_INIT_IGNORE_MODVERSIONS` | 1 | 6 |
| `MODULE_INIT_IGNORE_VERMAGIC` | 2 | 7 |
| `MODULE_INIT_COMPRESSED_FILE` | 4 | 8 |

`idempotent_init_module`（`:3189`）用文件的 inode 做一次胜者选举：同一个 inode 已经有装载在进行时，后来的调用 `idempotent_wait_for_completion`，不再并行走第二遍 `load_module`。

旧的 `init_module(2)` 仍在 `:3054`：从用户地址拷贝整段镜像，然后同样 `load_module`（`:3074`）。本章后面的步骤两条系统调用共用。

---

## 6.2 `load_module` 的顺序

`load_module`（`:2842`）失败就按标签往回拆。主路径上的调用：

| 步骤 | 函数 | 行 | 做完时的状态 |
|------|------|----|--------------|
| 验签，并剥掉尾部签名长度 | `module_sig_check` | 2862 | 还没有 `struct module` |
| ELF 头与段 | `elf_validity_cache_copy` | 2871 | 检查 `.gnu.linkonce.this_module` |
| 名字、vermagic、黑名单等 | `early_mod_check` | 2875 | |
| 计算布局并拷到最终地址 | `layout_and_allocate` | 2880 | 返回最终的 `struct module *` |
| 挂上全局链表 | `add_unformed_module` | 2891 | `MODULE_STATE_UNFORMED` |
| per-cpu、引用计数、各段指针 | `percpu_modalloc`、`module_unload_init`、`find_module_sections` | 2902–2917 | `refcnt` 被设成基数再加一 |
| 符号改成最终地址 | `simplify_symbols` | 2929 | |
| 重定位 | `apply_relocations` | 2933 | |
| 异常表、kallsyms、架构收尾 | `post_relocation` | 2937 | |
| 只读 / NX / 可执行 | `complete_formation` | 2956 | `MODULE_STATE_COMING` |
| 通知、ftrace、解析参数、sysfs | `prepare_coming_module`、`parse_args`、`mod_sysfs_setup` | 2960–2979 | |
| 调用 init | `do_init_module` | 2995 | 成功则 `LIVE` |

`module_sig_check` 失败走 `free_copy`，模块不会出现在链表上。验签的密码学实现本章不写。

`module_blacklist` 是 `core_param`（`:2370`，perm `0400`）。`early_mod_check` 用它拒绝名单里的名字。

---

## 6.3 布局与重定位

```c
/* kernel/module/main.c:2372 */
static struct module *layout_and_allocate(struct load_info *info, int flags)
{
	/* module_frob_arch_sections、禁止 W+X 段 */
	/* per-cpu 段去掉 SHF_ALLOC，稍后单独分配 */
	/* .data..ro_after_init 和 __jump_table 标上 SHF_RO_AFTER_INIT */
	layout_sections(info->mod, info);   /* :2415，见 05 的七段映射 */
	layout_symtab(info->mod, info);
	err = move_module(info->mod, info); /* :2419 真正分配并 memcpy */
	mod = (void *)info->sechdrs[info->index.mod].sh_addr;
	return mod;
}
```

`move_module`（`:2231`）之后，`info->mod` 里那份临时对象不再是运行中的模块。返回值是落在 `MOD_*` 内存里的那份。

```c
/* kernel/module/main.c:1447 */
static int apply_relocations(struct module *mod, const struct load_info *info)
{
	for (i = 1; i < info->hdr->e_shnum; i++) {
		/* 跳过非法 sh_info、跳过目标段没有 SHF_ALLOC 的 */
		if (info->sechdrs[i].sh_flags & SHF_RELA_LIVEPATCH)
			err = klp_apply_section_relocs(...);
		else if (info->sechdrs[i].sh_type == SHT_REL)
			err = apply_relocate(...);
		else if (info->sechdrs[i].sh_type == SHT_RELA)
			err = apply_relocate_add(...);
	}
	return err;
}
```

`SHF_RELA_LIVEPATCH` 交给 `klp_apply_section_relocs`。livepatch 自己的状态机不在本章。普通模块走 `apply_relocate` 或 `apply_relocate_add`，架构代码在 `arch/*/kernel/module.c`。

`complete_formation`（`:2735`）在 `module_mutex` 下 `verify_exported_symbols`，然后 `module_enable_ro` / `module_enable_nx` / `module_enable_x`，最后：

```c
/* kernel/module/main.c:2754 */
/*
 * Mark state as coming so strong_try_module_get() ignores us,
 * but kallsyms etc. can see us.
 */
mod->state = MODULE_STATE_COMING;
```

参数解析在这之后（`:2967`），所以 `module_param` 的 set 函数运行时模块已经是 `COMING`。解析用的是 `uargs`，也就是 `finit_module` 的第二参数，形式和命令行 `key=value` 相同。

`mod_sysfs_setup`（`kernel/module/sysfs.c:370`）在 `/sys/module/<name>/` 下建 `holders/`、参数和 modinfo。kset 本身要更早存在，见 [11](./11-observability.md)。

---

## 6.4 `do_init_module`

```c
/* kernel/module/main.c:2515 */
static noinline int do_init_module(struct module *mod)
{
	freeinit = kmalloc(sizeof(*freeinit), GFP_KERNEL);
	/* 记下 MOD_INIT_TEXT / DATA / RODATA 的 base */

	do_mod_ctors(mod);
	if (mod->init != NULL)
		ret = do_one_initcall(mod->init);
	if (ret < 0)
		goto fail_free_freeinit;
	if (ret > 0) {
		pr_warn("%s: '%s'->init suspiciously returned %d, ...",
			__func__, mod->name, ret);
		dump_stack();
	}

	mod->state = MODULE_STATE_LIVE;
	blocking_notifier_call_chain(&module_notify_list,
				     MODULE_STATE_LIVE, mod);
	kobject_uevent(&mod->mkobj.kobj, KOBJ_ADD);

	if (!mod->async_probe_requested)
		async_synchronize_full();

	mutex_lock(&module_mutex);
	module_put(mod);                 /* :2579 放下加载时多拿的那一次引用 */
	/* 清空 mem[] 里的 init 三类，kallsyms 换到 core_kallsyms */
	if (llist_add(&freeinit->node, &init_free_list))
		schedule_work(&init_free_wq);   /* :2611 */
	mutex_unlock(&module_mutex);
	return 0;
}
```

`do_one_initcall` 就是启动时跑内建 initcall 的那个函数（`init/main.c:1205`）。init 返回正数时模块仍然变成 `LIVE`，同时 `pr_warn` 加 `dump_stack`。返回负数则 `state = GOING`（`:2627`），通知链、`free_module`，函数把这个负数返回给系统调用。

init 段不能在 init 函数还没返回时就 `vfree`：注释（`:2597`）写 kallsyms 可能在关抢占的情况下走过这块内存，而 `module_memfree` 不能在中断里调用。所以成功路径把指针挂上 `init_free_list`，用 `schedule_work(&init_free_wq)` 推迟。`do_free_init`（`:2480`）先 `synchronize_rcu`，再 `module_memfree` 三段。`flush_module_init_free_work`（`:2498`）就是 `flush_work(&init_free_wq)`，`mark_readonly` 在释放内核 init 内存之前会调用它（[03](./03-rest-init-and-pid1.md)）。

`schedule_work` 进的是 `system_percpu_wq`（[10](./10-cmwq-queue-and-flags.md)）。

失败路径在 `free_module` 之前会 `module_put`。加载时 `module_unload_init`（`:545`）把 `refcnt` 设成 `MODULE_REF_BASE` 再 `atomic_inc`，这一次 `module_put` 把多拿的引用放掉。

---

## 6.5 关键函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `kernel/module/main.c` | `finit_module` | 3208 | fd + 参数字符串 + 标志 |
| `kernel/module/main.c` | `init_module` | 3054 | 从用户缓冲拷贝镜像的旧系统调用 |
| `kernel/module/main.c` | `may_init_module` | 2640 | `CAP_SYS_MODULE`，且未被 `nomodule` 关掉 |
| `kernel/module/main.c` | `load_module` | 2842 | 验签到 `do_init_module` |
| `kernel/module/main.c` | `layout_and_allocate` | 2372 | 布局并 `move_module` |
| `kernel/module/main.c` | `apply_relocations` | 1447 | `SHT_REL` / `SHT_RELA` |
| `kernel/module/main.c` | `add_unformed_module` | 2714 | 链表上变成 `UNFORMED` |
| `kernel/module/main.c` | `complete_formation` | 2735 | 变成 `COMING` |
| `kernel/module/main.c` | `do_init_module` | 2515 | `do_one_initcall(mod->init)`，成功则 `LIVE` |
| `kernel/module/sysfs.c` | `mod_sysfs_setup` | 370 | `/sys/module/<name>/` |

---

**导航**：[← 模块对象](./05-module-objects.md) · [目录](./README.md) · [卸下 →](./07-module-unload.md)
