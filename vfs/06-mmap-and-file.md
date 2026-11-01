# 6. mmap 与文件缺页

**结论**：`mmap` 一个普通文件时，VFS 调用 `file->f_op->mmap`。通用实现 `generic_file_mmap` 只做两件事：要求地址空间有 `read_folio`，然后把 `vma->vm_ops` 设成 `generic_file_vm_ops`。真正读页发生在缺页，`vm_ops->fault` 指向 `filemap_fault`。

> `call_mmap` 在 `include/linux/fs.h:2022`。`generic_file_mmap` 在 `mm/filemap.c:3714`。`filemap_fault` 在 `:3293`。缺页怎么从架构异常进到 `do_fault`，见 [MM §07](../memory-management/07-page-fault.md)。

tmpfs 不走 `filemap_fault`。它的 `mmap` 装的是 `shmem_vm_ops`，`fault` 是 `shmem_fault`。那条路在 [10](./10-tmpfs-walkthrough.md)，页的分配和换出在 [MM §8.4](../memory-management/08-page-cache-and-writeback.md)。

---

## 6.1 `vm_operations_struct`

这张表挂在 VMA 上，不挂在 `struct file` 上。`mmap` 钩子负责把它装上去。字段顺序如下。

```c
/* include/linux/mm.h:563 */
struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);     /* VMA 被复制时，例如 fork */
	void (*close)(struct vm_area_struct *area);    /* VMA 从 mm 摘掉。可睡眠，调用者持有 mmap_lock */
	int (*may_split)(struct vm_area_struct *area, unsigned long addr);
	int (*mremap)(struct vm_area_struct *area);
	int (*mprotect)(struct vm_area_struct *vma, unsigned long start,
			unsigned long end, unsigned long newflags);
	vm_fault_t (*fault)(struct vm_fault *vmf);     /* 这一页不在页表里 */
	vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
	vm_fault_t (*map_pages)(struct vm_fault *vmf,
			pgoff_t start_pgoff, pgoff_t end_pgoff); /* 顺手装周围几页 */
	unsigned long (*pagesize)(struct vm_area_struct *area);

	/* 原先只读的页要变成可写。返回错误会变成 SIGBUS */
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
	/* VM_PFNMAP 或 VM_MIXEDMAP 上的对应物 */
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);

	int (*access)(struct vm_area_struct *vma, unsigned long addr,
		      void *buf, int len, int write); /* get_user_pages 失败时的兜底 */
	const char *(*name)(struct vm_area_struct *vma); /* /proc/pid/maps 的特殊名字 */

#ifdef CONFIG_NUMA
	int (*set_policy)(struct vm_area_struct *vma, struct mempolicy *new);
	struct mempolicy *(*get_policy)(struct vm_area_struct *vma,
					unsigned long addr);
#endif
	struct page *(*find_special_page)(struct vm_area_struct *vma,
					  unsigned long addr);
};
```

普通磁盘文件用的那一份（`mm/filemap.c:3706`，包在 `CONFIG_MMU` 里）：

```c
const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};
```

`filemap_map_pages` 在 `:3596`。缺页处理里 `do_read_fault` 会先试 `map_pages`，装不上当前页再调 `fault`。这个分发在 [MM §07](../memory-management/07-page-fault.md) 的 `do_fault`（`mm/memory.c:4745`）和 `do_fault_around`（`:4571`）。

---

## 6.2 从 `mmap` 到 `f_op->mmap`

x86-64 的系统调用是 `mmap`（`arch/x86/kernel/sys_x86_64.c:86`）。它最终进 `do_mmap`（`mm/mmap.c:1219`）。`do_mmap` 检查长度、溢出、`MAP_FIXED`，再按文件是否可写、是否共享，算出 VMA 标志。文件映射还要过 `mmap` 权限。

真正建 VMA 在 `mmap_region`。文件不为空时（`mm/mmap.c:2796`）：

```text
vma->vm_file = get_file(file)     struct file 引用加一，和 fd 表那份并列
error = mmap_file(file, vma)      mm/internal.h:97
```

`mmap_file`（`mm/internal.h:97`）就是：

```text
err = call_mmap(file, vma)        include/linux/fs.h:2022
  file->f_op->mmap(file, vma)
失败：
  vma->vm_ops = &vma_dummy_vm_ops  不再调用这个 VMA 上的其它钩子
```

`generic_file_mmap`（`mm/filemap.c:3714`）：

```text
mapping->a_ops->read_folio 为空：-ENOEXEC
file_accessed                         更新 atime
vma->vm_ops = &generic_file_vm_ops
返回 0
```

它不分配页，也不建页表。页表要等用户第一次碰这块地址。

没有 `writepage` 的文件系统可以用 `generic_file_readonly_mmap`（`:3728`）。共享且可能写的 VMA 在这里返回 `-EINVAL`，其余转给 `generic_file_mmap`。`CONFIG_MMU` 没打开时，这两个函数都返回 `-ENOSYS`（`:3739`）。

`call_mmap` 是内联函数，只做 `file->f_op->mmap(file, vma)`（`fs.h:2022`）。`do_mmap` 在更早的地方看到 `f_op->mmap` 为空就返回 `-ENODEV`（`mm/mmap.c:1348`），到不了 `call_mmap`。文件系统要支持 `mmap`，得把这个指针填上。

---

## 6.3 `filemap_fault`

`filemap_fault`（`mm/filemap.c:3293`）在缺页路径上被 `vm_ops->fault` 调用。进来时调用者持有 per-VMA 锁或 `mmap_lock`，细节和 `VM_FAULT_RETRY` 的重试约定在 MM §07，下面只列这个函数自己的分支。

```text
index = vmf->pgoff
index 已经 >= DIV_ROUND_UP(i_size, PAGE_SIZE)：VM_FAULT_SIGBUS     :3305

folio = filemap_get_folio(mapping, index)                         :3312
命中：
  还没 FAULT_FLAG_TRIED：do_async_mmap_readahead               :3319
  folio 还不是 uptodate：共享地拿 invalidate_lock
未命中：
  count_vm_event(PGMAJFAULT)，ret |= VM_FAULT_MAJOR             :3326
  do_sync_mmap_readahead
  共享地拿 invalidate_lock
  __filemap_get_folio(FGP_CREAT|FGP_FOR_MMAP)                   :3339
    失败且没有 fpin：VM_FAULT_OOM

lock_folio_maybe_drop_mmap 失败：VM_FAULT_RETRY                 :3350
folio->mapping 已经不是这个文件：解锁，回到 retry_find          :3354
还不是 uptodate：
  没拿 invalidate_lock：放下 folio，回到 retry_find
  拿着锁：page_not_uptodate
    filemap_read_folio → a_ops->read_folio                      :3414
    成功或 AOP_TRUNCATED_PAGE：retry_find
    否则 VM_FAULT_SIGBUS

中途为了 IO 放下了 mmap 锁（fpin 非空）：
  解锁 folio，返回 VM_FAULT_RETRY                               :3385
否则：
  再对一下 i_size，越界则 VM_FAULT_SIGBUS                        :3396
  vmf->page = folio_file_page(folio, index)
  返回 ret | VM_FAULT_LOCKED                                    :3404
```

返回 `VM_FAULT_LOCKED` 表示 folio 还锁着，调用者装进页表后再解锁。`VM_FAULT_MAJOR` 对应 `/proc/vmstat` 里的 `pgmajfault`：这次缓存里没有现成的 uptodate folio，做了同步预读。MM §07 对 `filemap_get_folio`、`filemap_get_entry`（`mm/filemap.c:1844`）和 xarray 的关系写得更细，这里不重推一遍。

`page_not_uptodate` 只同步重读一次（`:3406` 的注释）。`read_folio` 是文件系统的方法，读盘、解压或从网络拉，都在那个函数里。

写映射上第一次写只读页时走 `filemap_page_mkwrite`（`generic_file_vm_ops` 的第三个指针）。它把页变脏，让回写能看见。脏页记账在 MM §8.3 的 `folio_mark_dirty`。

---

## 6.4 和读系统调用的差别

| | `read` | `mmap` 之后的缺页 |
|---|---|---|
| 入口 | `vfs_read` → `read_iter` | `vm_ops->fault` |
| 谁提供偏移 | `kiocb->ki_pos` 或 `f_pos` | `vmf->pgoff`，等于文件内页号 |
| 数据去哪 | `iov_iter` 拷到用户缓冲 | folio 装进用户页表，没有这次拷贝 |
| 缓存未命中 | `filemap_read` → `read_folio` | `filemap_fault` → `read_folio` |
| 预读 | `filemap_get_pages` 里的 readahead | `do_sync_mmap_readahead` / `do_async_mmap_readahead`，另外 `map_pages` 装周围页 |

两条路共用同一个 `address_space`。`read` 已经把页放进缓存后，紧接着的缺页会在 `filemap_get_folio` 命中，不再标 `PGMAJFAULT`。

---

## 6.5 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/mm.h` | `struct vm_operations_struct` | 563 | VMA 的缺页和生命周期钩子 |
| `include/linux/fs.h` | `call_mmap` | 2022 | `file->f_op->mmap` |
| `mm/internal.h` | `mmap_file` | 97 | 调用 `call_mmap`，失败则换成空 `vm_ops` |
| `mm/mmap.c` | `do_mmap` | 1219 | 系统调用侧的检查和标志 |
| `mm/mmap.c` | `mmap_region` 里的 `mmap_file` | 2798 | `get_file` 后装 VMA |
| `arch/x86/kernel/sys_x86_64.c` | `mmap` | 86 | x86-64 系统调用入口 |
| `mm/filemap.c` | `generic_file_vm_ops` | 3706 | `fault` / `map_pages` / `page_mkwrite` |
| `mm/filemap.c` | `generic_file_mmap` | 3714 | 要求有 `read_folio`，装上 `vm_ops` |
| `mm/filemap.c` | `generic_file_readonly_mmap` | 3728 | 拒绝共享可写 |
| `mm/filemap.c` | `filemap_fault` | 3293 | 页缓存缺页 |
| `mm/filemap.c` | `filemap_map_pages` | 3596 | 周围页一次性装入 |
| `mm/memory.c` | `do_fault` | 4745 | 文件缺页分发，见 MM §07 |
| `mm/shmem.c` | `shmem_fault` | 2180 | tmpfs 的 fault，不是 `filemap_fault` |

---

**导航**：[← 读写](./05-read-write.md) · [目录](./README.md) · [挂载 →](./07-mount-and-ns.md)
