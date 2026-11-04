# 10. seccomp、能力与 LSM

**结论**：容器在 `execve` 之前做三件收紧。seccomp 过滤器挂在 `task_struct->seccomp` 上，系统调用入口比业务代码先看到它。能力五集合写在 `cred` 里，user ns 决定这些位对谁有效。LSM 在 `pivot_root`、创建 user ns、以及写 `/proc/thread-self/attr/exec` 时各有一个钩子。这里只写这三处容器运行时真正碰到的内核入口。

---

## 10.1 seccomp

```c
/* include/linux/seccomp.h:37 ，CONFIG_SECCOMP */
struct seccomp {
	int mode;
	atomic_t filter_count;
	struct seccomp_filter *filter; /* 只从 current 读，入口不加读锁 */
};
```

它嵌在 `task_struct` 的 `seccomp` 成员（`include/linux/sched.h:1132`）。`mode` 的用户可见值（`include/uapi/linux/seccomp.h:10`）：`SECCOMP_MODE_DISABLED` 0、`SECCOMP_MODE_STRICT` 1、`SECCOMP_MODE_FILTER` 2。内核内部还有 `SECCOMP_MODE_DEAD`（`kernel/seccomp.c:33`，值是 `SECCOMP_MODE_FILTER + 1`），`SECCOMP_RET_KILL_*` 之后用来保证这条任务不能再做系统调用。

过滤器本体：

```c
/* kernel/seccomp.c:226 */
struct seccomp_filter {
	refcount_t refs;
	refcount_t users;
	bool log;
	bool wait_killable_recv;
	struct action_cache cache;
	struct seccomp_filter *prev;   /* 后装的滤在前，形成一条链 */
	struct bpf_prog *prog;
	struct notification *notif;
	struct mutex notify_lock;
	wait_queue_head_t wqh;
};
```

BPF 看到的数据包：

```c
/* include/uapi/linux/seccomp.h:62 */
struct seccomp_data {
	int nr;                      /* 系统调用号 */
	__u32 arch;                  /* AUDIT_ARCH_* */
	__u64 instruction_pointer;
	__u64 args[6];               /* 一律按 64 位存放 */
};
```

系统调用入口在 `syscall_trace_enter`（`kernel/entry/common.c:48`）。ptrace 之后、`trace_sys_enter` 之前，若 `SYSCALL_WORK_SECCOMP` 置位就调用 `__secure_computing(NULL)`（`:75`）。`__secure_computing`（`kernel/seccomp.c:1335`）按 mode 分发：strict 走 `__secure_computing_strict`，filter 走 `__seccomp_filter`（`:1201`）。后者 `populate_seccomp_data`（`:246`）填 `seccomp_data`，再 `seccomp_run_filters`（`:406`）沿着 `prev` 链跑 BPF。`ACTION_ONLY` 更小的返回值留下（`:428`），注释写明最低的 BPF 返回值优先。

返回值的高 16 位是动作（`include/uapi/linux/seccomp.h:38`）：

| 宏 | 值 | 容器里的典型用途 |
|---|---|---|
| `SECCOMP_RET_KILL_PROCESS` | `0x80000000` | 杀掉整个线程组 |
| `SECCOMP_RET_KILL_THREAD` | `0x00000000` | 只杀当前线程 |
| `SECCOMP_RET_TRAP` | `0x00030000` | 变成 `SIGSYS` |
| `SECCOMP_RET_ERRNO` | `0x00050000` | 低 16 位是 errno |
| `SECCOMP_RET_USER_NOTIF` | `0x7fc00000` | 停住，等监听进程回答 |
| `SECCOMP_RET_TRACE` | `0x7ff00000` | 交给 ptracer |
| `SECCOMP_RET_LOG` | `0x7ffc0000` | 放行并记一条 |
| `SECCOMP_RET_ALLOW` | `0x7fff0000` | 放行 |

默认的容器配置是一张「默认 ERRNO 或 KILL，再对允许的调用号 ALLOW」的经典 BPF。`SECCOMP_RET_USER_NOTIF` 把决定权交回用户态监听者，监听进程的协议不在本章。

装过滤器的系统调用是 `seccomp`（`kernel/seccomp.c:2069`）→ `do_seccomp`（`:2044`）。操作数 `SECCOMP_SET_MODE_FILTER` 是 1。flags 里容器会碰到的是 `SECCOMP_FILTER_FLAG_TSYNC`（线程组一起装）、`SECCOMP_FILTER_FLAG_LOG`、`SECCOMP_FILTER_FLAG_NEW_LISTENER`（要 notif 时）、`SECCOMP_FILTER_FLAG_SPEC_ALLOW`、`SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV`。掩码在 `include/linux/seccomp.h:6`。

`NO_NEW_PRIVS` 置位之后，没有 `CAP_SYS_ADMIN` 也能装过滤器，而且 exec 不会再提升能力。runc 因此把「先 set no_new_privs、再在 exec 前装 seccomp」和「还没丢能力时就装」分成两条，见 [09](./09-runc-create-path.md)。过滤器跨 fork 继承，跨 exec 保留。

strict 模式只放行 `read`、`write`、`exit`、`sigreturn` 这一小撮，容器基本不用它。

---

## 10.2 能力

五集合在 `struct cred` 里，字段见 [04](./04-pid-user-uts-ipc-time-ns.md)。`kernel_cap_t` 是一个 `u64`（`include/linux/capability.h:24`）。`CAP_LAST_CAP` 为 40。

容器运行时在 exec 前做的事对应这几个集合：

| 集合 | 容器里怎么收 |
|---|---|
| `cap_bset` | bounding set。runc 的 `ApplyBoundingSet` 先收它。不在 bounding set 里的位，exec 文件的能力也不能再抬上来 |
| `cap_inheritable` | 与文件的 inheritable 取交，决定 exec 后 permitted 里从进程继承的部分 |
| `cap_permitted` | 这一刻真正拥有的位 |
| `cap_effective` | `cap_capable` 实际检查的位 |
| `cap_ambient` | 没有文件能力、也没有 setuid 时仍能跨 exec 留下的位。必须是 permitted 与 inheritable 的子集（`cap_ambient_invariant_ok`，`include/linux/cred.h:167`） |

`capset`（`kernel/capability.c:220`）改的是当前任务。新 user ns 里 `set_cred_user_ns` 把 permitted、effective、bset 置满，inheritable 和 ambient 清空。随后 runc 再按 OCI spec 收成容器作者点名的那几位。`cap_capable` 的父链规则见 [04](./04-pid-user-uts-ipc-time-ns.md)：容器内的 `CAP_NET_ADMIN` 能改这个 net ns，改不了宿主机的。

`PR_SET_NO_NEW_PRIVS` 之后，exec 一个 setuid 或带文件能力的程序不会把 permitted 抬高。seccomp 和 no_new_privs 一起用，是为了让「过滤器还没盖住的调用」也不能借 exec 翻身。

---

## 10.3 LSM 边界上的三个钩子

**建 user ns**。`create_user_ns` 调用 `security_create_user_ns`（`security/security.c:3583`）。发行版可以在这里拒绝非特权用户再往里嵌 user ns。钩子失败时 `create_user_ns` 返回这个错误（`user_namespace.c:117`）。

**换根**。`pivot_root` 在改挂载树之前调用 `security_sb_pivotroot`（`security/security.c:1438`，调用点 `fs/namespace.c:4297`）。AppArmor、SELinux、TOMOYO 都能否决这次 pivot。

**exec 时的标签**。runc v1.5.2 在换根之后写标签，再 exec：

- AppArmor：`libcontainer/apparmor/apparmor_linux.go` 的 `applyProfile`（`:62`）写 `/proc/thread-self/attr/apparmor/exec`（旧内核路径是 `attr/exec`），内容是 `exec <profile>`。内核 `apparmor_setprocattr`（`security/apparmor/lsm.c:690`）看到 name 为 `exec`、命令为 `exec` 时调用 `aa_change_profile(..., AA_CHANGE_ONEXEC)`（`:739`）。标签在下一次 exec 生效。
- SELinux：`SetExecLabel` 写 `attr/exec`。内核 `selinux_setprocattr`（`security/selinux/hooks.c:6625`）对名字 `exec` 检查 `PROCESS__SETEXEC`，把上下文收成下一次 exec 的 SID。

写进 `cred->security` 的具体结构每家 LSM 不同。容器笔记停在「运行时写 proc 属性，内核钩子在 exec 时消费」。策略语言、profile 怎么编译，不在这里。

`copy_process` 里的 `security_task_alloc`（进程章 04 的第 7 步）给孩子复制 LSM 状态。seccomp 的复制在 `copy_seccomp`（`kernel/fork.c:1911`），调用点在 `fork.c:2680`，位于已经拿下 `tasklist_lock` 之后（进程章 04 的第 22 步）。

---

## 10.4 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/seccomp.h` | `struct seccomp` | 37 | mode 与过滤器链头 |
| `kernel/seccomp.c` | `struct seccomp_filter` | 226 | BPF 与 prev 链 |
| `kernel/entry/common.c` | `syscall_trace_enter` | 48 | 系统调用入口上的 seccomp |
| `kernel/seccomp.c` | `__secure_computing` | 1335 | 按 mode 分发 |
| `kernel/seccomp.c` | `__seccomp_filter` | 1201 | 跑 BPF，取最严动作 |
| `kernel/seccomp.c` | `seccomp` | 2069 | 装过滤器的系统调用 |
| `include/uapi/linux/seccomp.h` | `struct seccomp_data` | 62 | BPF 的输入 |
| `kernel/capability.c` | `capset` | 220 | 改五集合 |
| `security/commoncap.c` | `cap_capable` | 66 | 沿 user ns 检查 |
| `security/security.c` | `security_create_user_ns` | 3583 | 能否建 user ns |
| `security/security.c` | `security_sb_pivotroot` | 1438 | 能否换根 |
| `security/apparmor/lsm.c` | `apparmor_setprocattr` | 690 | `exec <profile>` |
| `security/selinux/hooks.c` | `selinux_setprocattr` | 6625 | `attr/exec` |

---

**导航**：[← runc](./09-runc-create-path.md) · [目录](./README.md) · [观测 →](./11-observability.md)
