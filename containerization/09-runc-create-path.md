# 9. runc 创建路径

**结论**：runc v1.5.2 的容器 init 不是一次 `clone` 带上全部 `CLONE_NEW*`。`nsexec` 在 Go 运行时起来之前用 `clone(CLONE_PARENT|SIGCHLD)` 做出 stage-1，stage-1 先 `unshare` user ns、等父进程写完 uid/gid 映射，再 `unshare` 其余命名空间，然后再 `clone` 出真正的 init（stage-2）。cgroup 在 stage-0 继续 clone 之前就 `Apply` 到 stage-0 上，孩子继承这个组。`pivot_root(".", ".")` 发生在 stage-2 回到 Go 之后的 `prepareRootfs` 里。能力、seccomp 和 `execve` 在换根之后。

> 仓库：[opencontainers/runc](https://github.com/opencontainers/runc) tag **v1.5.2**，提交 `29dd3dc2b13b4123162e5fe132504bb4b15569f1`。下面的文件路径和行号属于这一版，内核符号仍是 v6.6.160。

---

## 9.1 谁在 Go 运行时之前跑

`libcontainer/nsenter/nsenter.go` 用 C 构造函数调用 `nsexec()`：

```c
/* libcontainer/nsenter/nsenter.go */
void __attribute__((constructor)) init(void) {
	nsexec();
}
```

构造函数早于 Go 运行时。`nsexec`（`libcontainer/nsenter/nsexec.c:725`）读环境变量 `_LIBCONTAINER_INITPIPE`。没有这个变量就直接返回，普通的 `runc` 命令不受影响。有的话，这条进程是容器 init，先 `nl_parse` 读父进程送来的 netlink 配置，再 `PR_SET_DUMPABLE, 0`（`:783`），然后 `clone_parent`。

`clone_parent`（`:315`）的标志只有 `CLONE_PARENT | SIGCHLD`：

```c
return clone(child_func, ca.stack_ptr, CLONE_PARENT | SIGCHLD, &ca);
```

`child_func`（`:308`）立刻 `longjmp` 回 `nsexec` 的 `switch`，用 `jmpval` 区分 stage。命名空间标志不放进这次 `clone`。`CLONE_PARENT` 让新任务的父进程是调用者的父进程，也就是外面的 runc 监视进程，而不是 stage-0。stage-0 因此不能自己 `wait` stage-1，它把两个 pid 用 JSON 写回管道，让 runc 去收 stage-1 的僵尸（`:927` 的注释）。

---

## 9.2 三阶段

`initProcess.start`（`libcontainer/process_linux.go:779`）先 `cmd.Start()`。此时子进程已经在跑，并堵在 `nsexec` 读 init 管道上。接着：

```text
manager.Apply(pid)          process_linux.go:827
                            写 cgroup.procs，内核走 cgroup_attach_task
                            注释要求在和孩子同步之前做完，避免孩子逃出组
把 bootstrap 写进 init 管道  :849
                            nsexec 的 nl_parse 解除阻塞
```

stage-0（`nsexec.c:855`，进程名 `runc:[0:PARENT]`）`clone_parent` 出 stage-1，然后在同步循环里做三件父命名空间才做得了的事：

| 孩子请求 | stage-0 动作 | 行号 |
|---|---|---|
| `SYNC_USERMAP_PLS` | rootless 单映射时先 `update_setgroups(..., DENY)`，再 `update_uidmap` / `update_gidmap` | 890 |
| `SYNC_TIMEOFFSETS_PLS` | `update_timens_offsets` 写 `/proc/<stage1>/timens_offsets` | 944，函数在 `:694` |
| `SYNC_RECVPID_PLS` | 把 stage-1 和 stage-2 的 pid 写成 JSON 送回 runc | 914 |

写映射的是 stage-0，它还在父 user ns 里。stage-1 已经 `unshare` 进了新 user ns，自己没有权限写这张图。内核侧就是 [04](./04-pid-user-uts-ipc-time-ns.md) 的 `map_write`。time 偏移要在有任务把偏移冻住之前写，所以也由 stage-0 写 stage-1 的 proc 文件。

stage-1（`:1010`，`runc:[1:CHILD]`）：

```text
join_namespaces              :1032    先 setns 进配置里点名的已有 ns
                                       顺序是非 user、user、再剩余（join_namespaces :667）
unshare(CLONE_NEWUSER)       :1054    单独一次
等 stage-0 写完 uid/gid map
setresuid(0, 0, 0)           :1093    映射生效后成为这个 user ns 里的 root
unshare(其余 cloneflags)     :1107    mnt、pid、net、ipc、uts、cgroup、time 一次做完
若有 timens 偏移，请 stage-0 去写
clone_parent 出 stage-2      :1132
把自己的 pid 交给 stage-0
exit(0)                      :1159
```

`try_unshare`（`:675`）就是 `unshare(2)`，进 `ksys_unshare`。pid ns 的 `unshare` 不改变调用者自己的 pid，所以 stage-1 必须再 clone 一次，stage-2 才是新 pid ns 里的 1 号。这和 `copy_pid_ns` 只改 `pid_ns_for_children` 是同一条规则。

stage-2（`:1169`，`runc:[2:INIT]`）等 `SYNC_GRANDCHILD`，然后 `setsid`、`setuid(0)`、`setgid(0)`，需要的话 `setgroups(0, NULL)`，通知 stage-0，`return` 回 Go。stage-0 在两边都结束后 `exit(0)`（`:997`）。此后容器里活着的 init 是 stage-2。它从 stage-0 fork 出来，cgroup 成员资格跟着 fork 走，所以 `Apply` 不必再对 stage-2 写一次。

Go 侧 `linuxStandardInit.Init`（`libcontainer/standard_init_linux.go:51`）从这里接上。

---

## 9.3 换根

`Init` 里和文件系统有关的顺序：

```text
setupNetwork / setupRoute          standard_init_linux.go:81
prepareRootfs                      rootfs_linux.go:170
  按配置逐个 setupAndMountToRootfs :192   proc、sysfs、bind、可选的 FUSE 卷
  跑 CreateContainer 钩子          :229   此时还在旧根上
  NoPivotRoot：msMoveRoot          :235   MS_MOVE 再 chroot
  有 NEWNS：pivotRoot              :237
  否则 chroot                      :239
finalizeRootfs                     有 NEWNS 时，standard_init_linux.go:116
sethostname / setdomainname        :121   写 uts ns
apparmor.ApplyProfile              :131
写 sysctl、只读路径、mask 路径
```

`pivotRoot`（`rootfs_linux.go:1146`）做的就是内核注释里的 `pivot_root(".", ".")`：

```text
open("/") 得到旧根的 dirfd
fchdir(新根)
pivot_root(".", ".")               unix.PivotRoot，内核 fs/namespace.c:4276
fchdir(旧根 fd)
mount("", ".", "", MS_SLAVE|MS_REC)   旧根不再把 umount 传回宿主机
unmount(".", MNT_DETACH)
chdir("/")
```

`MS_SLAVE|MS_REC` 对应 [03](./03-mount-ns-pivot-root.md) 里 `pivot_root` 拒绝 shared 父挂载的那条。复制 mnt ns 时如果已经因跨 user ns 带了 `CL_SHARED_TO_SLAVE`，这里再收一次旧根。`MNT_DETACH` 让旧根从容器的树里消失，即使还有人拿着引用。

没有新 mnt ns 时不能 pivot，退到 `chroot`（`:239`）。`NoPivotRoot` 走 `msMoveRoot`（`:1197`）：先把宿主机上完整的 proc/sysfs 挂载卸掉或用 tmpfs 盖住，再 `MS_MOVE` 把 rootfs 挪到 `/`。这是同一函数里的另一条路，默认创建走 `pivotRoot`。

---

## 9.4 cgroup、能力、exec

创建路径的 cgroup 是 `cgroups.Manager.Apply`，在 v2 上最终写 `cgroup.procs`。`CLONE_INTO_CGROUP` 出现在 **进已有容器再 exec** 的 `setnsProcess.prepareCgroupFD`（`process_linux.go:372`），不在这条 init 路径上。两条路的内核差别见 [06](./06-cgroup-in-containers.md)。

`Init` 在换根之后、exec 之前：

```text
PR_SET_NO_NEW_PRIVS                 若配置要求，standard_init_linux.go:153
                                    内核 prctl，之后 exec 不能再获得能力
seccomp（还没设 no_new_privs 时）    :191  先装，因为这时还要有能力
finalizeNamespace                   init_linux.go:300
  ApplyBoundingSet                  :341  收紧 cap_bset
  设 keep caps，setupUser 改 uid
  ClearKeepCaps，ApplyCaps          :361  留下 spec 里的 permitted/effective/ambient
seccomp（设了 no_new_privs 时）      standard_init_linux.go:239
                                    尽量贴近 exec，少留必须放行的系统调用
linux.Exec                          :303
```

seccomp 的内核入口见 [10](./10-seccomp-caps-lsm.md)。runc 把过滤器编成 BPF 后通过 `seccomp(2)` 装上，父进程还要拿一个 listener fd 时走 `syncParentSeccomp`。

`createNetworkInterfaces`（`process_linux.go:888`）在 init 换根之前由父进程把接口送进容器的 net ns。那是把现成的 `net_device` 移进去，veth 对端怎么转发不在这里展开。

---

## 9.5 和内核符号的对照

| runc v1.5.2 | 内核 v6.6.160 |
|---|---|
| `nsexec` 的 `unshare` | `ksys_unshare`（`kernel/fork.c:3394`） |
| `clone_parent` 的 `CLONE_PARENT` | `copy_process` 里父进程取调用者的父（进程章 04） |
| stage-1 再 clone 才进入新 pid ns | `copy_pid_ns` 只写 `pid_ns_for_children`（`kernel/pid_namespace.c:145`） |
| `update_uidmap` | `proc_uid_map_write` → `map_write`（`kernel/user_namespace.c:1108`） |
| `update_timens_offsets` | `timens_offsets_write`（`fs/proc/base.c:1618`） |
| `pivot_root(".", ".")` | `pivot_root`（`fs/namespace.c:4276`） |
| `manager.Apply` | `cgroup_procs_write`（`kernel/cgroup/cgroup.c:5206`） |
| `prepareCgroupFD`（exec 路径） | `CLONE_INTO_CGROUP`，`cgroup_css_set_fork`（`kernel/cgroup/cgroup.c:6435`） |
| `linux.Exec` | `do_execveat_common`（进程章 06） |

---

**导航**：[← FUSE](./08-fuse.md) · [目录](./README.md) · [seccomp / 能力 / LSM →](./10-seccomp-caps-lsm.md)
