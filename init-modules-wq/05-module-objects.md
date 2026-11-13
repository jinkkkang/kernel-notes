# 5. 模块对象

**结论**：一个已加载模块是 `struct module`。状态从 `UNFORMED` 到 `COMING` 再到 `LIVE`，卸下时变成 `GOING`。代码和数据按 `mod_mem_type` 分放在 `mem[]` 里，init 三类在 `do_init_module` 成功后被丢掉。

> `include/linux/module.h`。`enum module_state` 在 `:314`，`enum mod_mem_type` 在 `:326`，`struct module_memory` 在 `:364`，`struct module` 在 `:404`。

---

## 5.1 状态

```c
/* include/linux/module.h:314 */
enum module_state {
	MODULE_STATE_LIVE,	/* Normal state. */
	MODULE_STATE_COMING,	/* Full formed, running module_init. */
	MODULE_STATE_GOING,	/* Going away. */
	MODULE_STATE_UNFORMED,	/* Still setting it up. */
};
```

枚举值的数值顺序是 LIVE、COMING、GOING、UNFORMED。生命周期顺序不是这个数值顺序：

```text
add_unformed_module     state = UNFORMED     挂上 modules 链表
complete_formation      state = COMING       内存已设成只读/不可执行的该设的页，可以看见符号
do_init_module 成功     state = LIVE
do_init_module 失败
  或 delete_module      state = GOING
free_module             短暂改回 UNFORMED，然后从链表摘掉并释放
```

`/proc/modules` 不打印 `UNFORMED`（`kernel/module/procfs.c:82` 判断，`:83` `return 0`）。`COMING` 印成 `Loading`，`GOING` 印成 `Unloading`，其余印成 `Live`（`:91`）。

`module_is_live`（`module.h:608`，`return mod->state != MODULE_STATE_GOING`）把还在 `COMING` 的模块也当成活的。`try_module_get` 用它（见 [07](./07-module-unload.md)）。`strong_try_module_get`（`kernel/module/main.c:152`）更严：`UNFORMED` 是 `BUG_ON`，`COMING` 返回 `-EBUSY`。

---

## 5.2 七段内存

```c
/* include/linux/module.h:326 */
enum mod_mem_type {
	MOD_TEXT = 0,
	MOD_DATA,
	MOD_RODATA,
	MOD_RO_AFTER_INIT,
	MOD_INIT_TEXT,
	MOD_INIT_DATA,
	MOD_INIT_RODATA,

	MOD_MEM_NUM_TYPES,
	MOD_INVALID = -1,
};
```

`mod_mem_type_is_init`（`:339`）只认后三种。`mod_mem_type_is_core` 是其余的。`mod_mem_type_is_text`（`:346`）是 `MOD_TEXT` 和 `MOD_INIT_TEXT`。

```c
/* include/linux/module.h:364 */
struct module_memory {
	void *base;
	unsigned int size;

#ifdef CONFIG_MODULES_TREE_LOOKUP
	struct mod_tree_node mtn;
#endif
};
```

`struct module` 里是数组 `mem[MOD_MEM_NUM_TYPES]`（`:462`）。布局时 `layout_sections`（`main.c:1572`）按段标志把 ELF section 归进上表。核心段的映射在 `:1529`：可执行 → `MOD_TEXT`，只读 → `MOD_RODATA`，`SHF_RO_AFTER_INIT` → `MOD_RO_AFTER_INIT`，可写 → `MOD_DATA`。init 段的映射在 `:1536`：没有单独的 init 版 `RO_AFTER_INIT`，那个槽是 `MOD_INVALID`，碰到会 `WARN_ON_ONCE` 并跳过（`:1557`）。

x86-64 上这些页来自 `module_alloc`，窗口在模块映射区，不在普通 vmalloc 那 32TB 里。地址范围见 [memory-management/05](../memory-management/05-vmalloc-and-kernel-va.md)。本章不重复那张表。

---

## 5.3 `struct module`（节选）

`:404`–`:462` 里还有按配置出现的成员：`build_id`（`CONFIG_STACKTRACE_BUILD_ID`）、`kcfi_traps`（`CONFIG_ARCH_USES_CFI_TRAPS`）、`param_lock`（`CONFIG_SYSFS`）、`sig_ok`（`CONFIG_MODULE_SIG`）。下面只留不包在这些 `#ifdef` 里的成员，顺序与头文件一致。`:464`–`:562` 是 kallsyms、tracepoint、ftrace、livepatch 等另一段按配置展开的字段。

```c
/* include/linux/module.h:404 */
struct module {
	enum module_state state;

	/* Member of list of modules */
	struct list_head list;

	/* Unique handle for this module */
	char name[MODULE_NAME_LEN];

	/* ... CONFIG_STACKTRACE_BUILD_ID 时才有 build_id[] ... */

	/* Sysfs stuff. */
	struct module_kobject mkobj;
	struct module_attribute *modinfo_attrs;
	const char *version;
	const char *srcversion;
	struct kobject *holders_dir;

	/* Exported symbols */
	const struct kernel_symbol *syms;
	const s32 *crcs;
	unsigned int num_syms;

	/* Kernel parameters. */
	struct kernel_param *kp;
	unsigned int num_kp;

	/* GPL-only exported symbols. */
	unsigned int num_gpl_syms;
	const struct kernel_symbol *gpl_syms;
	const s32 *gpl_crcs;
	bool using_gplonly_symbols;

	bool async_probe_requested;

	/* Exception table */
	unsigned int num_exentries;
	struct exception_table_entry *extable;

	/* Startup function. */
	int (*init)(void);

	struct module_memory mem[MOD_MEM_NUM_TYPES] __module_memory_align;

	/* :464 起按配置展开，直到卸下相关字段 */
```

`MODULE_NAME_LEN` 是 `MAX_PARAM_PREFIX_LEN`（`module.h:35`）。`init` 由 modpost 写成 `init_module`（[04](./04-initcalls.md)）。`async_probe_requested` 在 `load_module` 里被赋成全局参数 `async_probe`（`main.c:2964`）。

```c
/* include/linux/module.h:563  CONFIG_MODULE_UNLOAD */
	/* What modules depend on me? */
	struct list_head source_list;
	/* What modules do I depend on? */
	struct list_head target_list;

	/* Destruction function. */
	void (*exit)(void);

	atomic_t refcnt;
```

依赖边是单独的 `struct module_use`（`:308`）：`source` 是使用者，`target` 是被用者。`source_list` 挂在被用者上，回答「谁依赖我」；`target_list` 挂在使用者上，回答「我依赖谁」。`delete_module` 看到 `source_list` 非空就拒绝（[07](./07-module-unload.md)）。

`refcnt` 的基数是 `MODULE_REF_BASE`（1，`main.c:542`）。加载时再加一，init 成功后 `module_put` 放掉这一下。`module_refcount()`（`:689`）返回 `atomic_read(&mod->refcnt) - MODULE_REF_BASE`，所以「没有别的引用」时这个函数是 0，原子变量里仍是 1。

---

## 5.4 `__this_module` 从哪来

每个 `.ko` 的 `*.mod.c` 由 `add_header`（`scripts/mod/modpost.c:1833`）生成：

```c
__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,          /* 仅当 mod->has_init */
	.exit = cleanup_module,       /* CONFIG_MODULE_UNLOAD 且 has_cleanup */
	.arch = MODULE_ARCH_INIT,
};
```

装入时 `elf_validity_cache_copy` 检查这个段只有一个、带 `SHF_ALLOC`、大小等于运行内核的 `sizeof(struct module)`（`main.c:1842`–`:1881`）。对不上就拒绝，避免用另一份内核头编译的模块把后面的字段写错位。`THIS_MODULE` 指向这份对象。

`find_module`（`main.c:363`）在 `modules` 链表上按名字比较，默认跳过 `UNFORMED`（`find_module_all` 的 `even_unformed` 传 false，`:355`）。

---

## 5.5 关键函数

| 文件 | 符号 | 行号 | 作用 |
|------|------|------|------|
| `include/linux/module.h` | `enum module_state` | 314 | 四个状态 |
| `include/linux/module.h` | `enum mod_mem_type` | 326 | 七类内存加 `MOD_INVALID` |
| `include/linux/module.h` | `struct module_memory` | 364 | `base` + `size` |
| `include/linux/module.h` | `struct module` | 404 | 模块对象 |
| `include/linux/module.h` | `struct module_use` | 308 | 一条依赖边 |
| `kernel/module/main.c` | `MODULE_REF_BASE` | 542 | 引用计数基数 1 |
| `kernel/module/main.c` | `find_module` | 363 | 按名字找，忽略 UNFORMED |
| `kernel/module/main.c` | `module_refcount` | 689 | 对外的引用数，已减去基数 |
| `scripts/mod/modpost.c` | `add_header` | 1833 | 生成 `__this_module` |

---

**导航**：[← initcall](./04-initcalls.md) · [目录](./README.md) · [装入 →](./06-module-load.md)
