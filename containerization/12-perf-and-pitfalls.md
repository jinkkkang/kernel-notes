# 12. 放大与坑

**结论**：容器里最容易被看成「内核忽然变慢」的四件事，都有明确的代码位置。overlay 的第一次写打开会把整个文件和中间目录抄到 upper。user ns 的映射写错一次就再也改不了，而且映不进父 ns 的 uid 0。cgroup v1 与 v2 的挂法不同，混在一个运行时配置里会挂到不同的树上。FUSE 的每次未命中都要等守护进程从 `/dev/fuse` 读走请求再写回。下面的采集命令是 RECIPE，本环境没有对着 v6.6.160 跑出过延迟数字。

---

## 12.1 overlay copy-up 把小写放大成整文件

`ovl_open`（`fs/overlayfs/file.c:154`）在 `FMODE_WRITE` 或 `O_TRUNC` 时调用 `ovl_maybe_copy_up`。`ovl_copy_up_flags`（`copy_up.c:1111`）从文件走到最近一个已经有 upper 的祖先，把中间每一层目录和这个文件都抄上去。数据拷贝在 `ovl_copy_up_data`（`:591`），走 workdir 里的临时文件再 rename 到 upper。

因此：

- 改 lower 上一个大文件的一个字节，upper 上会出现整份拷贝，外加路径上还没有 upper 的目录。
- workdir 和 upper 必须在同一文件系统（`ovl_make_workdir` 的注释，`super.c:727`）。workdir 所在的盘满了，copy-up 失败，写打开失败。
- `metacopy` 只跳过「非写打开」的数据拷贝（`ovl_need_meta_copy_up`，`copy_up.c:941`）。写打开和 `O_TRUNC` 仍然整文件拷。它减轻的是只改权限、不改内容的那次，减轻不了第一次 `write`。
- 多个进程同时第一次写同一个 lower 文件时，`ovl_copy_up_start`（`fs/overlayfs/util.c:652`）拿 `ovl_inode->lock` 串行。后到的在锁里看见 `ovl_already_copied_up` 就返回。时间花在拷贝上。

```bash
# RECIPE：看 copy-up 是否占住了写延迟。事件名以 fs/overlayfs 里的 tracepoint 为准
# 6.6 的 include/trace/events/ 下若没有 overlay 事件，就对 ovl_copy_up_file 做 kprobe
perf probe -a ovl_copy_up_file
perf record -e probe:ovl_copy_up_file -a -- sleep 5
perf script
```

upper 是普通文件系统之后，脏页和回写回到内存章与块层，不再经过 overlay 的 copy-up。

---

## 12.2 user ns 映射

`map_write` 成功一次之后 `nr_extents != 0`，再写返回 `-EPERM`（`kernel/user_namespace.c:968`）。映射写错只能丢掉这个 user ns 重建，没有原地修改。

非特权创建者的 `parent_could_setfcap` 为假（`create_user_ns` 在 `:126` 从当时的有效能力取样）。`verify_root_map`（`:878`）因此拒绝把父 ns 的 uid 0 写进自己的 `uid_map`。能写的是 [11](./11-observability.md) 那种「容器内 0 → 自己的 uid，长度 1」。需要把宿主机上某个服务的 uid 0 文件能力带进容器时，这张图必须由父 ns 里持有 `CAP_SETFCAP` 的进程来写。

`setgroups` 先写成 `deny` 才能做非特权的单 gid 映射（`new_idmap_permitted` `:1181`）。`deny` 之后不能再 `allow`（`proc_setgroups_write` `:1258`）。先写了 `gid_map` 就不能再 `deny`（`:1264`）。顺序反了，两条都会 `-EPERM`，容器里的进程要么带进父 ns 的附加组，要么根本没有 gid 映射。

gid 映射缺失时，VFS 上的组权限检查走 `from_kgid` 的失败路径，文件看起来全部不可按组访问。先看 `/proc/<pid>/gid_map` 是空的还是只有一行，再看 `setgroups`。

嵌套 user ns 的 id 要一层层 `map_id_down`。`/proc/<pid>/uid_map` 的第二列是父层里的 id。父层就是初始 user ns 时，这一列才是全局 id。

---

## 12.3 cgroup v1 与 v2 混用

v2 是一棵树，`/proc/<pid>/cgroup` 只有 `0::` 一行。v1 是每个控制器一棵，同一行会变成 `name=...` 或控制器名加路径。runc v1.5.2 用 `cgroups.IsCgroup2UnifiedMode()` 决定 `Apply` 走 v2 还是 v1，`prepareCgroupFD` 在 v1 上直接返回、不使用 `CLONE_INTO_CGROUP`（`process_linux.go:375`）。

混用时的现象：

- 进程的 cpu 在 v1 的 `cpu` 树，memory 在 v2 树。两套限额都生效，但 `cgroup.controllers` 里看不到已经在 v1 上的控制器。
- `nsdelegate` 只约束 v2 的 cgroup namespace 边界。v1 的委托是目录权限，没有这个挂载选项。
- 容器里再 `mkdir` 子组时，no-internal-process 由 v2 的 `cgroup_migrate_vet_dst` 执行（`kernel/cgroup/cgroup.c:2648`）。同一份 unit 文件在 v1 宿主上可以把进程放在中间节点；在 v2 上若该节点 `subtree_control` 非空，写 `cgroup.procs` 得到 `-EBUSY`。
- 混合挂载（v2 统一树旁边仍挂着某个 v1 控制器）时，`/proc/self/cgroup` 会同时出现 `0::` 和其它行。只读第一行会漏掉还在 v1 上的限额。

cgroup 章默认讲 v2。排障时先 `findmnt -t cgroup,cgroup2` 和 `cat /proc/<pid>/cgroup`，确认进程到底在哪几棵树上，再读对应的 `memory.max` 或 v1 的 `memory.limit_in_bytes`。

---

## 12.4 FUSE 延迟

`fuse_simple_request`（`fs/fuse/dev.c:485`）把调用线程放在 `fuse_req->waitq` 上，直到守护进程 `read` `/dev/fuse` 再写回回答。延迟堆在这三处：

- 守护进程没被调度到。它若和容器业务抢同一颗 CPU，或者被自己的 cgroup 限住，`waiting` 会涨。fusectl 的 `waiting` 就是这个数（[11](./11-observability.md)）。
- 单线程守护进程的队列。`pending` 上的请求一次读走一个（`fuse_dev_do_read`）。`FUSE_DEV_IOC_CLONE` 可以多开设备 fd，让多个线程并行读。
- 后台请求打满 `max_background`。写回走 `fuse_simple_background`，超过上限后调用方把拥塞转嫁给写路径。`congestion_threshold` 是更早的水位。

连接被 abort 或守护进程退出后，堵着的请求以 `req->out.h.error` 失败返回，挂载点上的读写变成 I/O 错误。容器 memory 限额若先杀掉卷的守护进程，业务进程在挂载点上看到的是这批 I/O 错误。

```bash
# RECIPE
cat /sys/fs/fuse/connections/<id>/waiting
cat /sys/fs/fuse/connections/<id>/max_background
# 谁在读 /dev/fuse
ls -l /proc/<daemon-pid>/fd | grep fuse
```

镜像用 FUSE 做惰性 lower 时，第一次读冷块付这笔往返。copy-up 到 upper 之后的写走 upper 的普通文件系统，不再进 FUSE 队列。

---

## 12.5 创建路径上容易看错的顺序

runc v1.5.2 的 stage-1 在 `unshare(CLONE_NEWPID)` 之后，`getpid()` 仍是旧 ns 的号。新 pid ns 里的 1 号是接下来 `clone` 出来的 stage-2。见 [09](./09-runc-create-path.md) 和 `copy_pid_ns`（`kernel/pid_namespace.c:145`）。

`pivot_root` 在父挂载仍是 shared 时返回 `-EINVAL`（`fs/namespace.c:4316`）。复制 mnt ns 时若没有跨 user ns，`copy_mnt_ns` 不会自动加 `CL_SHARED_TO_SLAVE`。调用前要自己把相关挂载改成 slave 或 private。runc 在 `pivot_root(".", ".")` 之后对旧根再做一次 `MS_SLAVE|MS_REC`。

`CLONE_NEWNS` 与 `CLONE_FS` 同时出现，`copy_process` 返回 `-EINVAL`（`kernel/fork.c:2269`）。`unshare` 会自动补上 `CLONE_FS`（`fork.c:3422`），自己的 `clone` 标志要避开这个组合。

---

**导航**：[← 观测](./11-observability.md) · [目录](./README.md)
