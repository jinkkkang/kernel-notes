# 7. 缺页

**结论**：用户访问一个还没有 PTE，或者 PTE 权限不够的地址，进 `handle_mm_fault()`。匿名页向 buddy 要 folio；文件页先找页缓存；写 COW 页会复制一份。hugetlb 在入口就分叉，不走普通的 PMD/PTE。

> 主文件：`mm/memory.c`。文件缺页的页缓存半边在 `mm/filemap.c`。
> 导航：[上一节 mm/VMA](./06-mm-and-vma.md) · [目录](./README.md) · [下一节 页缓存](./08-page-cache-and-writeback.md)

调用者进来之前已经拿着 VMA 锁或 `mmap_lock`（`FAULT_FLAG_VMA_LOCK`）。返回 `VM_FAULT_RETRY` 时锁可能已经放下，**不能再碰 `vma`**。这是 `handle_mm_fault` 注释写明的（`:6834`）。

---

## 7.1 从架构异常到 `handle_pte_fault`

x86 的 `exc_page_fault` 在 `arch/x86/mm/fault.c`，把错误码整理成 `FAULT_FLAG_*` 后进 `handle_mm_fault`。本节从架构无关的入口写。

```text
handle_mm_fault                         # memory.c:6841
  sanitize_fault_flags
  arch_vma_access_permitted             # pkey / 权限，失败 SIGSEGV
  用户态缺页：mem_cgroup_enter_user_fault
  lru_gen_enter_fault                   # MGLRU 记这次访问
  is_vm_hugetlb_page ?
       hugetlb_fault                    # hugetlb.c:6013
     : __handle_mm_fault                # memory.c:6604
  lru_gen_exit_fault
  用户态：mem_cgroup_exit_user_fault    # OOM 如果发生在缺页里，在这里处理

__handle_mm_fault
  pgd_offset → p4d_alloc → pud_alloc
  pud 空且允许 PUD 大页：create_huge_pud
  pud 已是大页：写保护则 wp_huge_pud，否则置 young
  pmd_alloc
  pmd 空且允许 PMD THP：create_huge_pmd # memory.c:6379
       失败带 VM_FAULT_FALLBACK 才降级成 PTE
  pmd 是 swap / migration / device：对应的 huge 处理
  否则 handle_pte_fault                 # :6514
```

`__handle_mm_fault` 在下到 PTE 之前先尝试大页。THP 的策略（`madvise`、`always`、`never`）在 `create_huge_pmd` 里通过 `thp_vma_allowable_order` 过滤。细节见 [09](./09-thp-and-hugetlb.md)。

---

## 7.2 `handle_pte_fault`（`:6514`）

```text
pmd 还是空：
  先不建 PTE 页。vm_ops->fault 可能要装大页，
  页表一旦露出来就很难收回
  vmf->pte = NULL
pmd 已有：
  pte_offset_map_rw_nolock
  pte_none → 当成缺失
!pte        → do_pte_missing             # :4695
!pte_present → do_swap_page              # :4881
pte_protnone：
  uffd-wp 的 RWP → do_uffd_rwp
  否则 NUMA hint → do_numa_page
拿 ptl，pte 没被别人改掉：
  写或 unshare，且 PTE 不可写 → do_wp_page  # :4380
  否则置 dirty / young，更新 TLB
```

`do_pte_missing` 只有一个判断：

```c
if (vma_is_anonymous(vmf->vma))
    return do_anonymous_page(vmf);   /* memory.c:5427 */
else
    return do_fault(vmf);            /* :6104 */
```

---

## 7.3 匿名、文件、COW

### 匿名

`do_anonymous_page`：分配一个零页 folio（`vma_alloc_zeroed_movable_folio` 或 THP 失败后的 order-0），挂上 `anon_vma`，charge 到缺页时的 memcg，装一条可写或只读的 PTE。

只读的第一次访问可以映射全局零页（`ZERO_PAGE`），不分配。一写就走 `do_wp_page` 变成私有页。这是匿名页 RSS 看起来「读过但 RssAnon 仍很小」的原因。

用户态示例 [demos/02_fault.c](./demos/02_fault.c) 逐页写入，`ru_minflt` 会上升。那是 minor fault：页不在内存里，但也不需要磁盘。

### 文件

`do_fault`（`:6104`）：

| 条件 | 去哪 |
|---|---|
| `vm_ops->fault` 为空 | 把 PTE 再读一遍。仍是 none 就 `SIGBUS`。有的驱动只在 `mmap` 时一次装好 |
| 不是写 | `do_read_fault`（`:5980`）→ 通常 `filemap_fault`（`filemap.c:3555`） |
| 写且不是 `VM_SHARED` | `do_cow_fault`（`:6012`）。私有文件映射：读进来再复制，之后变成匿名 |
| 写且共享 | `do_shared_fault`（`:6054`）。脏页写回文件 |

`filemap_fault` 在 `address_space->i_pages`（xarray）里找 folio。没有就读盘，major fault。`/proc/vmstat` 的 `pgmajfault` 是这类。本机快照 `pgfault` 约 2259 万，`pgmajfault` 8519，绝大多数是匿名或页缓存命中。

### COW

`do_wp_page`：PTE 在，但是只读，这次是写。

- 是匿名且引用计数为 1、没有别的映射：直接把 PTE 改成可写。fork 之后如果对端已经退出，经常走这里，不复制。
- 否则分配新 folio，拷贝，把旧页的 mapcount 减掉，装新 PTE。这是 fork + 写入的成本。

`VM_FAULT_RETRY` 出现在 folio 锁拿不到、或者需要等待回写的时候。调用者要重新查找 VMA，因为 `munmap` 可能已经把旧 VMA 释放了。

---

## 7.4 userfaultfd（点到为止）

缺页可以不在内核里填页，而把事件交给用户态。`userfaultfd` 系统调用在 `mm/userfaultfd.c:4841`。`userfaultfd_register`（`:3718`）把范围记到 `vma->vm_userfaultfd_ctx`。

两种常见模式：

- **missing**：PTE 还没有。qemu postcopy、Criu 用它把缺页转成用户态协议。
- **wp（write-protect）**：页在，但写成只读。`handle_pte_fault` 里 `userfaultfd_pte_rwp` 为真时进 `do_uffd_rwp`，而不是 `do_numa_page`（`:6569`）。

内核填页的 ioctl 全在 `userfaultfd_ioctl`（`mm/userfaultfd.c:4685`）。`UFFDIO_API` 之外的命令要求上下文已经初始化。分发：

| ioctl | 处理函数 | 做什么 |
|---|---|---|
| `UFFDIO_API` | `userfaultfd_api` | 协商特性 |
| `UFFDIO_REGISTER` / `UNREGISTER` | `userfaultfd_register`（`:3718`）/ `userfaultfd_unregister` | 把范围记进或摘出 `vma->vm_userfaultfd_ctx` |
| `UFFDIO_WAKE` | `userfaultfd_wake` | 叫醒等在这个范围上的缺页 |
| `UFFDIO_COPY` | `userfaultfd_copy` | 从用户缓冲拷进缺页的地址 |
| `UFFDIO_ZEROPAGE` | `userfaultfd_zeropage` | 用零页填 |
| `UFFDIO_MOVE` | `userfaultfd_move` | 把已有页移进来 |
| `UFFDIO_WRITEPROTECT` | `userfaultfd_writeprotect` | 装上/拿掉写保护 |
| `UFFDIO_CONTINUE` | `userfaultfd_continue` | minor fault：页已经在页缓存，继续往下装 PTE |
| `UFFDIO_POISON` | `userfaultfd_poison` | 把这段标成 hwpoison |
| `UFFDIO_RWPROTECT` | `userfaultfd_rwprotect` | 读写保护 |
| `UFFDIO_SET_MODE` | `userfaultfd_set_mode` | 切模式 |

参数布局在 `include/uapi/linux/userfaultfd.h`，这里不抄结构体。权威说明是 `Documentation/admin-guide/mm/userfaultfd.rst`。

和 KVM 的关系：postcopy 迁移时，guest 碰尚未从源端拉来的页，宿主缺页进 uffd，用户态再 `UFFDIO_COPY` 把页填上。EPT 那一层见 [11](./11-kvm-memory.md)。

---

## 7.5 返回值

`vm_fault_t` 是位掩码，不是枚举。常见的：

| 位 | 含义 |
|---|---|
| `VM_FAULT_OOM` | 分配失败，交给 memcg 或全局 OOM |
| `VM_FAULT_SIGBUS` | 没有 `vm_ops->fault`、或文件截断、或 hwpoison |
| `VM_FAULT_SIGSEGV` | 权限 / pkey |
| `VM_FAULT_RETRY` | 锁已放，重来 |
| `VM_FAULT_MAJOR` | 这次碰了磁盘 |
| `VM_FAULT_FALLBACK` | 大页没装成，调用者改走小页 |
| `VM_FAULT_NOPAGE` | 驱动自己装了 PTE，或者什么都不用做 |

`VM_DROPPABLE` 的 VMA 在内存压力下可以被丢掉且不 SIGBUS。`handle_mm_fault` 开头读了这个标志。

---

## 7.6 hwpoison

**结论**：硬件报告某一页损坏时，`memory_failure` 尽量把这一页从用户映射里摘掉，该杀的进程发 SIGBUS，而不是让后面的读把坏数据当好数据用。恢复关着（`sysctl_memory_failure_recovery == 0`）就直接 panic。

入口 `memory_failure(pfn, flags)`（`mm/memory-failure.c:2386`）。返回值注释在函数上方：`-ENXIO` 内核不管理这块，`-EOPNOTSUPP` 被 `hwpoison_filter` 滤掉，`-EHWPOISON` 这页已经毒过（可能已经杀进程），其它负数是失败。

```text
sysctl_memory_failure_recovery == 0 → panic
mutex_lock(&mf_mutex)
pfn_to_online_page 失败：
  arch_memory_failure，或 pfn 无效时 memory_failure_pfn
get_hwpoison_page                         # :1550，先把这页的引用拿稳
  0 且在 buddy 上：take_page_off_buddy，记 MF_RECOVERED
  -ENOTRECOVERABLE：PG_reserved / slab / 页表 / large-kmalloc，MF_IGNORED
hwpoison_filter 命中：清 PG_hwpoison，返回 -EOPNOTSUPP
大 folio：folio_set_has_hwpoisoned，try_to_split_thp_page
  拆不开：kill_procs_now，MF_MSG_UNSPLIT_THP
shake_folio，然后 hwpoison_user_mappings   # :1624
```

`hwpoison_user_mappings` 先 `collect_procs`（`:1654`）。匿名、文件、ksm 各有一份收集（`collect_procs` 本体在 `:664`）。必须在 `try_to_unmap` 之前做，因为 unmap 会把 rmap 拆掉。真正拆映射的是 `unmap_poisoned_folio`（`:1568`）：`TTU_IGNORE_MLOCK | TTU_SYNC | TTU_HWPOISON`，把 PTE 换成 hwpoison 项，而不是普通 swap 项。干净页可以丢掉不杀；脏页要 SIGBUS，因为用户可能已经写过、数据丢了。`kill_procs` 在 `:462`，单个进程走 `kill_proc`（`:286`）。

`/proc/meminfo` 的 `HardwareCorrupted` 对应各 node `memory_failure_stats` 的合计，[02](./02-physical-layout.md) 的 `pg_data_t` 里有这个字段。软下线（还没出错、提前把页迁走）是另一条，入口不在 `memory_failure` 里，不要把两者的计数加在一起解读。

---

## 7.7 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `handle_mm_fault` | `mm/memory.c:6841` | 缺页入口 |
| `__handle_mm_fault` | `:6604` | 分配 pud/pmd，尝试大页 |
| `create_huge_pmd` | `:6379` | PMD THP |
| `handle_pte_fault` | `:6514` | PTE 级分发 |
| `do_pte_missing` | `:4695` | 匿名还是文件 |
| `do_anonymous_page` | `:5427` | 匿名缺页 |
| `do_fault` | `:6104` | 文件缺页分发 |
| `do_read_fault` | `:5980` | 读文件 |
| `do_cow_fault` | `:6012` | 私有文件映射的写 |
| `do_shared_fault` | `:6054` | 共享文件映射的写 |
| `do_wp_page` | `:4380` | COW / 把只读 PTE 变成可写 |
| `do_swap_page` | `:4881` | swap 项、migration 项 |
| `filemap_fault` | `mm/filemap.c:3555` | 页缓存缺页 |
| `hugetlb_fault` | `mm/hugetlb.c:6013` | 大页 |
| `userfaultfd` 系统调用 | `mm/userfaultfd.c:4841` | 创建 uffd |
| `userfaultfd_register` | `mm/userfaultfd.c:3718` | 注册范围 |
| `userfaultfd_ioctl` | `mm/userfaultfd.c:4685` | COPY / ZEROPAGE / MOVE / POISON 等 |
| `memory_failure` | `mm/memory-failure.c:2386` | 硬件坏页 |
| `hwpoison_user_mappings` | `:1624` | 摘用户映射并发 SIGBUS |
| `unmap_poisoned_folio` | `:1568` | `TTU_HWPOISON` 拆 PTE |
| `get_hwpoison_page` | `:1550` | 为处理坏页拿引用 |

---

[上一节 mm/VMA](./06-mm-and-vma.md) · [目录](./README.md) · [下一节 页缓存](./08-page-cache-and-writeback.md)
