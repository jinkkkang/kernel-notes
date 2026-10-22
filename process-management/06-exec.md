# 6. 地址空间交接：exec

**结论**：exec **不**改 PID；过了 `begin_new_exec` 就无法把错误返回给旧用户态（`point_of_no_return`），地址空间整体替换。

## 6.1 `struct linux_binprm`

`include/linux/binfmts.h:28`：

| 成员 | 含义 |
|---|---|
| `vma` / `vma_pages` / `argmin` | 临时栈 VMA（`CONFIG_MMU`） |
| `mm` / `old_mm` | 新 mm / 待释放旧 mm |
| `user_ns` | exec 时的 user ns |
| `p` | 用户栈当前 top |
| `have_execfd` / `execfd_creds` / `secureexec` | 解释器 fd / 凭据 / AT_SECURE |
| `point_of_no_return` | 已无法失败回用户态 |
| `file` / `executable` / `interpreter` / `loader` | 相关文件 |
| `cred` | 新凭证 |
| `argc` / `envc` | 参数计数 |
| `filename` / `interp` / `fdpath` | 路径名 |
| `rlim_stack` | 保存的栈 rlimit |
| `buf[BINPRM_BUF_SIZE]` | 文件头魔术缓冲 |

## 6.2 调用链

```
sys_execve / sys_execveat
  → do_execveat_common(fd, filename, argv, envp, flags)   # fs/exec.c:1897
      → 准备 linux_binprm、打开文件、copy 字符串
      → bprm_execve(bprm)                                  # :1843
          → exec_binprm
              → search_binary_handler(bprm)                # :1760
                  → 各 binfmt->load_binary
                      → load_elf_binary                    # fs/binfmt_elf.c:829
                          → begin_new_exec(bprm)           # fs/exec.c:1136  ★不可回退点
                          → 装段、装解释器、setup_stack …
```

`load_elf_binary` 在约 `:1010` 调用 `begin_new_exec`。

## 6.3 `begin_new_exec` 做什么（要点）

位置：`fs/exec.c:1136`，由 `load_elf_binary` 等在装载关键点调用（ELF 约 `:1010`）。

主要动作（按源码意图归纳，细节以函数体为准）：

1. **线程组只留当前线程**：其它线程被干掉（与历史 `de_thread` 同一目标），保证地址空间切换时没有同伴仍跑在旧 mm 上。
2. **拿 `exec_update_lock` / 处理凭证**：`bprm->cred` 安装到任务；setuid 等在此生效边界内完成。
3. **`exec_mmap`**：切到 `bprm->mm`，旧 mm 转入释放；此后旧用户映像不可用。
4. **标记不可回退**：`bprm->point_of_no_return = 1`；再失败不能把 errno 干净地返回原用户态。
5. **更新 `comm`、`self_exec_id`**：后者让 `do_notify_parent` 能发现父已 exec、安全域变了。
6. 清 `PF_FORKNOEXEC` 等与「尚未 exec」相关的状态。

若在 `point_of_no_return` 之前失败，仍可能释放 `bprm` 并把错误返回 syscall；之后则走向致命路径。


## 6.4 exec 前后资源

| 资源 | 行为 |
|---|---|
| PID / TGID | **不变** |
| mm | 整表替换 |
| files | 默认保留；`close-on-exec` 的 fd 关掉 |
| fs | 保留 cwd/root（除非特殊） |
| sighand | 重置为默认（除 `SIG_IGN` 等保留规则） |
| cred | 可能因 setuid/setgid/安全模块改变 |
| `PF_FORKNOEXEC` | 清除 |

## 6.5 关键函数表

| 文件 | 函数 | 作用 |
|---|---|---|
| `fs/exec.c` | `do_execveat_common` | exec 系统调用公共体 |
| | `bprm_execve` | 准备后执行 |
| | `search_binary_handler` | 遍历 binfmt |
| | `begin_new_exec` | 不可回退点 |
| `fs/binfmt_elf.c` | `load_elf_binary` | ELF 装载 |
| `include/linux/binfmts.h` | `struct linux_binprm` / `linux_binfmt` | 数据结构 |

---

---

**导航**：[章节目录](./README.md) · [← 内核线程与特殊任务](./05-kernel-threads.md) · [退出、僵尸与回收 →](./07-exit-reap.md)
