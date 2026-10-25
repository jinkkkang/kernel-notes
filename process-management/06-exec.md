# 6. 地址空间交接：exec

**结论**：exec 不给任务换一个新的 `task_struct`。用户看到的进程号（tgid）保持不变。一旦 `begin_new_exec` 把 `point_of_no_return` 置上，失败就不能再干净地回到原来的用户程序。

## 6.1 `struct linux_binprm`

装载一个新程序时，参数、新 mm、新凭证都先堆在这个结构里。定义在 `include/linux/binfmts.h:18`。

```c
struct linux_binprm {
#ifdef CONFIG_MMU
	struct vm_area_struct *vma;	/* 给参数字符串用的临时栈 VMA */
	unsigned long vma_pages;	/* 这个 VMA 占多少页 */
#else
# define MAX_ARG_PAGES	32
	struct page *page[MAX_ARG_PAGES]; /* 没有 MMU 时，参数直接放在这些页里 */
#endif
	struct mm_struct *mm;		/* 正在搭建的新地址空间 */
	unsigned long p;		/* 用户栈当前顶 */
	unsigned long argmin;		/* copy_strings() 用的 rlimit 标记 */
	unsigned int
		have_execfd:1,		/* 要把可执行文件的 fd 交回用户态 */
		execfd_creds:1,		/* 用脚本（binfmt_misc）自己的凭证 */
		secureexec:1,		/* 这次 exec 提升了权限，用来设 AT_SECURE */
		point_of_no_return:1,	/* 已经不能把错误返回给旧的用户程序 */
		comm_from_dentry:1;	/* 短名必须从 dentry 取，不能从文件名字符串取 */
	struct file *executable;	/* 交给解释器的那个可执行文件 */
	struct file *interpreter;	/* 解释器文件。脚本、binfmt_misc 会填它 */
	struct file *file;		/* 当前要装载的文件 */
	struct cred *cred;		/* 新凭证。setuid 就体现在这里 */
	int unsafe;			/* LSM_UNSAFE_*，这次 exec 有多不安全 */
	unsigned int per_clear;	/* 要从 personality 里清掉的位 */
	int argc, envc;			/* 参数个数、环境变量个数 */
	const char *filename;		/* procps 看到的名字 */
	const char *interp;		/* 真正执行的路径。脚本时和 filename 不同 */
	const char *fdpath;		/* execveat 生成的路径名 */
	unsigned interp_flags;		/* BINPRM_FLAGS_* */
	int execfd;			/* 交回用户态的那个可执行文件 fd */
	unsigned long loader, exec;	/* 装载器和入口在用户栈上的位置 */
	struct rlimit rlim_stack;	/* 装载期间保存的栈 rlimit */
	char buf[BINPRM_BUF_SIZE];	/* 文件头魔数 */
} __randomize_layout;
```

旁边的 `struct linux_binfmt`（`:84`）是一种二进制格式的操作表：`load_binary`、`load_shlib`，打开 coredump 时还有 `core_dump`。ELF 的 `load_binary` 就是 `load_elf_binary`。

## 6.2 调用链

```
sys_execve                         fs/exec.c:2162
  do_execve                        :2085
    do_execveat_common             :1941
sys_execveat                       :2170
  do_execveat                      :2094
    do_execveat_common             :1941
      alloc_bprm、数 argc/envc、把字符串拷进新栈
      bprm_execve                  :1867   调用点在 :2017
        exec_binprm                :1819
          search_binary_handler    :1772
            每种 fmt->load_binary
              load_elf_binary      fs/binfmt_elf.c:849
                begin_new_exec     fs/exec.c:1256   ELF 在 binfmt_elf.c:1027 调用
                setup_new_exec     fs/exec.c:1481
```

`exec_binprm` 允许解释器再套解释器。循环从 `:1831` 起，`depth > 5`（`:1833`）就返回 `-ELOOP`。`search_binary_handler` 一旦看见 `point_of_no_return`，或者返回值不是 `-ENOEXEC`，就不再试下一个格式（`:1798`）。

## 6.3 `begin_new_exec` 按什么顺序做

位置：`fs/exec.c:1256`。函数头上的注释写着：从这里开始的失败，用户态旧程序看不到。要么已经在被致命信号杀掉（`de_thread` 或 coredump），要么 `bprm_execve` 在 `:1931` 补一个 `SIGSEGV`。

顺序就是函数体的顺序：

1. `bprm_creds_from_file`：把 setuid 等算进 `bprm->cred`。
2. **`bprm->point_of_no_return = true`**（`:1269`）。这一步在换 mm **之前**。
3. `de_thread`（`:1043`）：同组其他线程被杀掉。如果当前线程不是组长，它会用 `exchange_tids` / `transfer_pid` 接过组长的 pid，所以组外看到的进程号还是原来的 tgid。
4. `posixtimer_exec`：卸掉 POSIX CPU 定时器，避免后面换了 tid 还留着旧定时器。
5. `current->fs->in_exec = 0`。fork 期间 `copy_fs` 看见这个标志会 `-EAGAIN`，这里放开。
6. `io_uring_task_cancel`：exec 跨不过去的 io_uring 活动取消掉。
7. `unshare_files`：文件表如果和别人共享，先变成自己独占的。
8. `do_close_on_exec`：关掉带 close-on-exec 的 fd。
9. `set_mm_exe_file`，然后 `would_dump`：不可读的二进制强制不可 dump。
10. `exec_mmap(bprm->mm)`（`:978`）：切到新 mm，旧 mm 交给 `mmput`。这之后旧用户程序的页已经不在了。
11. `exec_task_namespaces`，再 `unshare_sighand`：处理函数表如果和别人共享，先拷一份，免得 `flush_signal_handlers` 改到别人。
12. 清掉 `PF_FORKNOEXEC`、`PF_RANDOMIZE`、`PF_NOFREEZE`、`PF_NO_SETAFFINITY`，然后 `flush_thread`。
13. `__set_task_comm` 更新短名（从 `:1389` 起）。`comm_from_dentry` 时从 dentry 取，否则取 `filename` 的最后一段。
14. `self_exec_id` 加一（`:1408`）。`do_notify_parent` 用它发现父亲已经 exec 过、安全域变了。
15. `flush_signal_handlers(me, 0)`：不是 `SIG_IGN` 的处理函数回到默认。
16. `commit_creds(bprm->cred)`：新凭证从这里生效。

`setup_new_exec`（`:1481`）在这之后才做：按 personality 选 mmap 布局，放开 `exec_update_lock` 和 `cred_guard_mutex`。

`point_of_no_return` 之前的失败（比如还在 `bprm_creds_from_file`）仍能把 errno 返回系统调用。置位之后，`bprm_execve` 不会把控制权交回旧程序。

## 6.4 exec 前后资源

| 资源 | 行为 |
|---|---|
| 进程号 tgid | 不变。非组长线程 exec 时会接过组长的 pid，组外看到的还是原来的号 |
| `task_struct` 本身 | 不换。换的是它指着的 mm、cred、comm |
| mm | 整份换成 `bprm->mm` |
| files | 先独占，再关 close-on-exec，其余 fd 还在 |
| fs | cwd / root 还在。`in_exec` 被清 0 |
| sighand | 若原先共享就先拆开，再把处理函数恢复成默认（`SIG_IGN` 保留） |
| cred | `commit_creds`。setuid / setgid / LSM 都在这条路上生效 |
| `PF_FORKNOEXEC` | 清掉 |
| `self_exec_id` | 加一 |

## 6.5 关键函数

| 文件 | 函数 | 行号 | 作用 |
|---|---|---|---|
| `fs/exec.c` | `sys_execve` / `sys_execveat` | 2162 / 2170 | 系统调用 |
| | `do_execve` / `do_execveat` | 2085 / 2094 | 转到公共函数 |
| | `do_execveat_common` | 1941 | 分配 `linux_binprm`、拷参数 |
| | `bprm_execve` | 1867 | 打开文件、检查不安全状态、调用装载 |
| | `exec_binprm` | 1819 | 处理解释器套娃，成功后打 exec tracepoint |
| | `search_binary_handler` | 1772 | 遍历已注册的 `linux_binfmt` |
| | `de_thread` | 1043 | 只留当前线程，必要时接过组长 pid |
| | `exec_mmap` | 978 | 换成新 mm |
| | `begin_new_exec` | 1256 | 不可回退点 |
| | `setup_new_exec` | 1481 | mmap 布局，放开 exec 锁 |
| | `flush_signal_handlers` 的调用点 | 1409 | 把处理函数打回默认 |
| `fs/binfmt_elf.c` | `load_elf_binary` | 849 | ELF 装载，`:1027` 调用 `begin_new_exec` |
| `include/linux/binfmts.h` | `struct linux_binprm` | 18 | 装载参数 |
| | `struct linux_binfmt` | 84 | 一种格式的操作表 |

---

**导航**：[章节目录](./README.md) · [← 内核线程与特殊任务](./05-kernel-threads.md) · [退出、僵尸与回收 →](./07-exit-reap.md)
