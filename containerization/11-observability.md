# 11. 观测

**结论**：命名空间看 `lsns` 和 `/proc/<pid>/ns` 的 nsfs inode，cgroup 看 `/proc/<pid>/cgroup` 的路径，user ns 看 `uid_map` 与 `setgroups`，overlay 的开关在 `/sys/module/overlay/parameters`，FUSE 连接在 fusectl 的 `/sys/fs/fuse/connections`。下面带数字的段落都是 **sampled on 6.12**（`uname -r` 为 `6.12.94+`）。结构体和行号仍以 v6.6.160 为准。

---

## 11.1 本机命名空间（sampled on 6.12）

`lsns -n -o NS,TYPE,NPROCS,PID,USER` 当时只有一套，八种类型各一行，进程数都是同一个 shell 所在的会话：

```text
NS         TYPE    NPROCS   PID USER
4026531834 time        28   326 ubuntu
4026531835 cgroup      28   326 ubuntu
4026531837 user        28   326 ubuntu
4026531840 net         28   326 ubuntu
4026532350 mnt         28   326 ubuntu
4026532351 uts         28   326 ubuntu
4026532352 ipc         28   326 ubuntu
4026532353 pid         28   326 ubuntu
```

`NPROCS` 会随采样时刻变化。NS 列是 nsfs 的 inode，和 `readlink /proc/self/ns/<名字>` 方括号里的数字一致：

```text
cgroup -> cgroup:[4026531835]
ipc    -> ipc:[4026532352]
mnt    -> mnt:[4026532350]
net    -> net:[4026531840]
pid    -> pid:[4026532353]
pid_for_children -> pid:[4026532353]
time   -> time:[4026531834]
time_for_children -> time:[4026531834]
user   -> user:[4026531837]
uts    -> uts:[4026532351]
```

`pid` 与 `pid_for_children`、`time` 与 `time_for_children` 两两相同：这个 shell 没有 `unshare` 过 pid 或 time。`stat -L /proc/self/ns/mnt` 的 inode 是 `4026532350`；不加 `-L` 的 `stat` 得到的是 proc 符号链接自己的 inode（当时在 10866 附近），那个数不是命名空间号。`nsfs_show_path` 印的是 nsfs inode（`fs/nsfs.c:252`）。

`stat -f -c '%T %t' /proc/self/ns/mnt` 得到 `nsfs 6e736673`，与 v6.6.160 的 `NSFS_MAGIC`（`include/uapi/linux/magic.h:94`）一致。

`/proc/self/status` 的 `NSpid` 只有一列，说明从当前 pid ns 看只有一层号。`Uid`/`Gid` 四列都是 1000。

```text
CapInh: 000001ffffffffff
CapPrm: 0000000000000000
CapEff: 0000000000000000
CapBnd: 000001ffffffffff
CapAmb: 0000000000000000
NoNewPrivs: 0
Seccomp: 0
```

`CAP_LAST_CAP` 是 40，低 41 位全 1 就是 `0x1ffffffffff`。这个进程 bounding 和 inheritable 是满的，permitted、effective、ambient 是空的，所以 `cap_capable` 在 `init_user_ns` 上会失败。`Seccomp: 0` 对应 `SECCOMP_MODE_DISABLED`。

初始 user ns 的映射是恒等的一整段：

```text
$ cat /proc/self/uid_map
         0          0 4294967295
$ cat /proc/self/setgroups
allow
```

`gid_map` 同形。`4294967295` 是 `2^32-1`，覆盖整个 32 位 id 空间。

---

## 11.2 一次非特权 user ns（sampled on 6.12）

本机没有 `kernel.unprivileged_userns_clone` 这个 sysctl 文件。`/proc/sys/user/max_user_namespaces` 读到 `63994`。下面这条命令当时能跑通：

```text
$ unshare --user --map-root-user sh -c 'id; cat /proc/self/uid_map; cat /proc/self/gid_map; cat /proc/self/setgroups; readlink /proc/self/ns/user /proc/self/ns/mnt'
uid=0(root) gid=0(root) groups=0(root),65534(nogroup)
         0       1000          1
         0       1000          1
deny
user:[4026532210]
mnt:[4026532350]
```

读法：user ns inode 换成了新的 `4026532210`，mnt ns 仍是原来的 `4026532350`（这条命令没有 `CLONE_NEWNS`）。容器内 uid 0 映到父 ns 的 1000，长度 1。这是 `new_idmap_permitted` 的单段自映射（`kernel/user_namespace.c:1172`）。`setgroups` 是 `deny`，否则单 gid 映射不会被允许。`groups=65534` 是 overflow gid，附加组没有被带进新 ns。

---

## 11.3 cgroup（sampled on 6.12）

```text
$ cat /proc/self/cgroup
0::/system.slice/pod-.../cursor-agent/workload
$ findmnt -t cgroup2 -o TARGET,FSTYPE,OPTIONS
/sys/fs/cgroup cgroup2 rw,nosuid,nodev,noexec,relatime,nsdelegate,memory_recursiveprot
$ cat /sys/fs/cgroup/cgroup.controllers
cpuset cpu io memory hugetlb pids
```

只有 `0::` 一行，说明是 cgroup v2 统一层次。`nsdelegate` 对应 `CGRP_ROOT_NS_DELEGATE`，委托边界的行为见 [06](./06-cgroup-in-containers.md) 和 [cgroup/03](../cgroup/03-hierarchy-control.md)。

`/sys/kernel/cgroup/delegate` 的内容与 v6.6.160 源码里带 `CFTYPE_NS_DELEGATABLE` 的名单一致：

```text
cgroup.procs
cgroup.threads
cgroup.subtree_control
memory.oom.group
memory.reclaim
```

`/proc/sys/fs/mount-max` 读到 `100000`，对应 `fs/namespace.c` 的 `mount-max` sysctl。

---

## 11.4 overlay 与 fusectl（sampled on 6.12）

6.6 的 overlay 没有自己的 sysfs 目录，也没有 `debugfs` 注册。能在 sysfs 里看见的是模块参数 `/sys/module/overlay/parameters/`。当时：

```text
check_copy_up=N
index=N
metacopy=N
nfs_export=N
redirect_always_follow=Y
redirect_dir=N
redirect_max=256
xino_auto=N
```

这些是模块加载时的默认值，一次具体挂载还可以用挂载选项覆盖（`fs/overlayfs/params.c` 的 `metacopy`、`redirect_dir` 等）。本机 `findmnt -t overlay` 没有挂载。合并树真正用了哪些选项，看那次挂载的 `mountinfo`，不看模块参数。

FUSE 模块参数 `/sys/module/fuse/parameters/`：

```text
allow_sys_admin_access=N
max_user_bgreq=5224
max_user_congthresh=5224
```

fusectl 挂在 `/sys/fs/fuse/connections`（`findmnt -t fusectl` 的 FSTYPE 是 `fusectl`）。当时有一个连接目录 `41`：

```text
abort                  只写
congestion_threshold   12
max_background         16
waiting                0
```

这四个名字就是 `fuse_ctl_add_conn`（`fs/fuse/control.c:256`）创建的。`waiting` 是还堵在内核里的请求数，`max_background` 对应 `fuse_conn->max_background`。

---

## 11.5 看一个真正的容器时读什么

下面的命令需要目标进程的 pid。本环境没有现成的容器 init，列为读法，数字要在目标机器上现读。

```text
lsns -p <pid>
readlink /proc/<pid>/ns/*
cat /proc/<pid>/cgroup
cat /proc/<pid>/uid_map
cat /proc/<pid>/setgroups
tr '\0' ' ' < /proc/<pid>/environ | head    # 仅确认容器环境，注意里面可能有密钥
grep -E 'overlay|fuse' /proc/<pid>/mountinfo
```

`uid_map` 第二列是父 user ns 里的 id。宿主机从初始 user ns 看，第二列就是全局 uid。容器内 0 映到 100000 这种段，是编排器写进去的，内核只负责 [04](./04-pid-user-uts-ipc-time-ns.md) 的那次写入检查。

`/proc/<pid>/status` 的 `CapEff`、`CapBnd`、`NoNewPrivs`、`Seccomp` 四行对上 [10](./10-seccomp-caps-lsm.md)。`Seccomp` 为 2 表示 `SECCOMP_MODE_FILTER`。`NSpid` 多列时，从左到右是从读者所在的层到任务最深的层，生成处在 `fs/proc/array.c`（进程章 03）。

---

## 11.6 RECIPE

本环境没有 `CAP_SYS_ADMIN`，也没有 `ip` 命令。下面不作为实测。

```bash
# RECIPE：新的 mnt + pid + net，孩子里看 pid 1 和空的网卡表
sudo unshare --mount --pid --net --fork --mount-proc sh -c 'echo pid=$$; ls /sys/class/net'

# RECIPE：pivot_root 的最小形态。目录必须是挂载点，父挂载不能是 shared
# 内核检查见 fs/namespace.c pivot_root
sudo unshare --mount bash -c '
  mount --make-rslave /
  mkdir -p /tmp/newroot/old
  mount --bind /tmp/newroot /tmp/newroot
  cd /tmp/newroot && pivot_root . old
'

# RECIPE：把一张已有网卡移进目标 net ns
sudo ip link set dev eth1 netns <pid>
```

移网卡的内核入口是 `__dev_change_net_namespace`（`net/core/dev.c:11321`）。

---

**导航**：[← seccomp / 能力 / LSM](./10-seccomp-caps-lsm.md) · [目录](./README.md) · [放大与坑 →](./12-perf-and-pitfalls.md)
