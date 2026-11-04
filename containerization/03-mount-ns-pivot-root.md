# 3. mount 命名空间与 pivot_root

**结论**：`CLONE_NEWNS` 把当前挂载树复制进一个新的 `mnt_namespace`。复制发生在 user ns 边界上时，原来的 shared 挂载变成 slave，避免容器里的 `mount` 传回宿主机。`pivot_root` 在这个新树里把旧根挪到 `put_old`，把新根接到原来的挂载点上，再让仍指着旧根的 `fs_struct` 改指新根。传播规则本身在 [vfs/07](../vfs/07-mount-and-ns.md)，这里只写容器换根用到的那几条。

> `copy_mnt_ns` 与 `pivot_root` 都在 `fs/namespace.c`。`struct mnt_namespace` 在 `fs/mount.h:8`，`struct mount` 在 `fs/mount.h:39`。

---

## 3.1 复制一棵树

`copy_mnt_ns`（`fs/namespace.c:3849`）：没有 `CLONE_NEWNS` 就 `get_mnt_ns` 共享。有的话 `alloc_mnt_ns(user_ns, false)`（`:3868`），然后在 `namespace_lock` 里 `copy_tree`。

```c
/* fs/namespace.c:3874 起，copy_mnt_ns 的第一遍 */
copy_flags = CL_COPY_UNBINDABLE | CL_EXPIRE;
if (user_ns != ns->user_ns)
	copy_flags |= CL_SHARED_TO_SLAVE;
new = copy_tree(old, old->mnt.mnt_root, copy_flags);
```

三个标志在 `fs/pnode.h`：

| 宏 | 值 | 在复制里的作用 |
|---|---|---|
| `CL_EXPIRE` | `0x01` | 复制时带上过期标记的处理（`:1226` 一带） |
| `CL_COPY_UNBINDABLE` | `0x04` | 允许把 unbindable 也复制进去。普通 bind 复制见 vfs/07，遇到 unbindable 会拒绝 |
| `CL_SHARED_TO_SLAVE` | `0x20` | 源是 shared 时，副本变成它的 slave（`namespace.c:1208`） |

容器几乎总是在新 user ns 里做 `CLONE_NEWNS`（runc 先 `unshare` user，再 `unshare` 其余）。`user_ns != ns->user_ns` 成立，宿主机上的 shared 根（systemd 默认把 `/` 做成 shared）进容器后变成 slave。容器里再 `mount`，事件不会沿 slave 关系传回 master。这是「容器挂载不污染宿主机」的内核原因，靠的是这个 flag，靠的是传播链表，见 vfs/07 的 `mnt_slave` / `mnt_master`。

第二遍（`:3898`）把每个新 `mount` 的 `mnt_ns` 设成新 ns，并在 `new_fs` 非空时把 `fs_struct` 的 `root.mnt` / `pwd.mnt` 换成副本上的对应挂载。`unshare` 已经因为 `CLONE_NEWNS` 附带了 `CLONE_FS`（`fork.c:3422`），所以这里改的是一份私有的 `fs_struct`。

`MNT_LOCKED` 出现在 user ns 边界上的挂载：容器里不能把这些挂载 umount 掉。`pivot_root` 看见新根带 `MNT_LOCKED` 会直接失败（`namespace.c:4320`）。

---

## 3.2 `pivot_root` 做什么

系统调用在 `fs/namespace.c:4276`。注释（`:4252`）把语义写死了：把当前进程的根文件系统挪到 `put_old`，把 `new_root` 做成新根，并把「root/cwd 正指着当前根」的进程改到 `new_root`。root/cwd 已经不在根上的进程不动（`:4269`）。

调用链：

```text
pivot_root                         fs/namespace.c:4276
  may_mount                        没有就 -EPERM（:4284）
  user_path_at(new_root)           LOOKUP_FOLLOW | LOOKUP_DIRECTORY
  user_path_at(put_old)
  security_sb_pivotroot            security/security.c:1438   LSM
  get_fs_root                      当前 fs_struct 的根
  lock_mount(&old)
  下面一串 -EINVAL / -EBUSY / -ENOENT 检查
  umount_mnt(new_mnt)              先把新根从旧父上摘下
  unhash_mnt(root_mnt)             摘下旧根，留下它的挂载点
  attach_mnt(root_mnt, old_mnt, old_mp)   旧根挂到 put_old
  attach_mnt(new_mnt, root_parent, root_mp) 新根挂到原来的位置
  touch_mnt_namespace
  chroot_fs_refs(&root, &new)      fs/fs_struct.c:59  改仍指着旧根的 fs_struct
```

检查（`:4307` 起）集中在这些条件，失败码写在旁边：

| 条件 | 返回 |
|---|---|
| 当前根或新根不在本 ns（`check_mnt`） | `-EINVAL` |
| `put_old`、新根的父、旧根的父，任何一个是 shared（`IS_MNT_SHARED`） | `-EINVAL` |
| 新根带 `MNT_LOCKED` | `-EINVAL` |
| 新根 dentry 已 unlink | `-ENOENT` |
| 新根或 `put_old` 就是当前根 | `-EBUSY` |
| 当前根或新根不是挂载点（`path_mounted`），或没有父挂载 | `-EINVAL` |
| 从新根走不到 `put_old`，或从当前根走不到新根（`is_path_reachable`） | `-EINVAL` |

shared 被拒绝，是因为 `attach_mnt` 这里不做 `propagate_mnt`。容器在 `pivot_root` 之前会把将要涉及的挂载改成 slave 或 private。runc 的 `pivotRoot` 在调用之后对旧根做 `MS_SLAVE|MS_REC` 再 `MNT_DETACH`，见 [09](./09-runc-create-path.md)。内核注释还写了 `pivot_root(".", ".")` 的用法（`namespace.c:3137` 附近，以及 `:4271`）：新根先 bind 成挂载点，进程 `chdir` 进去，两个参数都是 `"."`。调用返回后 `"."` 仍指着旧根，接着就可以 `umount` 它。

`chroot_fs_refs`（`fs/fs_struct.c:59`）走任务表，把仍指向旧根路径的 `root` 和 `pwd` 换成新路径。已经 `chdir` 到别处的进程保持原 cwd。

当前根若在初始 rootfs 上，注释（`:4264`）说明这条路径走不通，要换别的办法。容器运行时不在 rootfs 上做这件事：它先有自己的 mnt ns 和一块已经挂上的 rootfs。

---

## 3.3 和传播、idmap 的衔接

vfs/07 的四种传播在换根时只被当成门闩：`pivot_root` 拒绝 shared，不负责把树改成 slave。改类型仍走 `do_change_type`（`namespace.c:2594`）。

6.6 的挂载可以带 `mnt_idmap`（`struct vfsmount` 里的字段，vfs/07 已列）。`can_idmap_mount`（`namespace.c:4388`）要求目标还在匿名 mnt ns 里（`is_anon_ns`），并且文件系统置了 `FS_ALLOW_IDMAP`。这是「挂载级 uid 映射」，和 user ns 的 `uid_map` 文件是两条路径：前者写在 `vfsmount` 上，后者写在 `user_namespace` 上。容器运行时可以用 idmapped mount 让卷里的文件属主看起来是容器内的 uid，而不改 user ns 的整张映射表。runc v1.5.2 的父进程在非 rootless 时会服务这类挂载请求（`libcontainer/process_linux.go` 的 `goCreateMountSources`，`:880` 一带）。映射表本身见 [04](./04-pid-user-uts-ipc-time-ns.md)。

`/proc/sys/fs/mount-max` 限制一个 mnt ns 里的挂载数，sysctl 表在 `namespace.c:5126`。本机读到的值见 [11](./11-observability.md)。

---

## 3.4 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/namespace.c` | `copy_mnt_ns` | 3849 | `CLONE_NEWNS` 复制挂载树 |
| `fs/pnode.h` | `CL_SHARED_TO_SLAVE` | 28 | 跨 user ns 时 shared 降为 slave |
| `fs/pnode.h` | `CL_COPY_UNBINDABLE` | 25 | 复制时保留 unbindable |
| `fs/namespace.c` | `pivot_root` | 4276 | 换根 |
| `security/security.c` | `security_sb_pivotroot` | 1438 | LSM 钩子 |
| `fs/fs_struct.c` | `chroot_fs_refs` | 59 | 改仍指着旧根的 root/pwd |
| `fs/namespace.c` | `can_idmap_mount` | 4388 | idmapped mount 的门闩 |
| `fs/namespace.c` | `mntns_operations` | 5116 | `/proc/<pid>/ns/mnt`，`type = CLONE_NEWNS` |

---

**导航**：[← 命名空间](./02-namespaces.md) · [目录](./README.md) · [pid / user / uts / ipc / time →](./04-pid-user-uts-ipc-time-ns.md)
