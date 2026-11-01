# 11. procfs、sysfs、debugfs

**结论**：这三种文件系统都不把目录树放在磁盘上。proc 的 `/proc/<pid>` 是查找时按名字里的数字找到 `task_struct`，再按一张静态表生成子项。sysfs 的目录树是 kernfs 节点。debugfs 用 `debugfs_create_file` 往一棵简单的内存树里挂 dentry。

> proc 根在 `fs/proc/root.c`，pid 目录在 `fs/proc/base.c`。kernfs 在 `fs/kernfs/`，sysfs 在 `fs/sysfs/mount.c`。debugfs 在 `fs/debugfs/inode.c`。

它们都走普通的 VFS 查找：`lookup` 函数指针被调用，返回一个 dentry。差别是这个函数不读目录块，而是问任务、kernfs 节点或 debugfs 的 dentry 缓存。

---

## 11.1 proc 的根

```c
/* fs/proc/root.c:277 */
static struct file_system_type proc_fs_type = {
	.name			= "proc",
	.init_fs_context	= proc_init_fs_context,
	.parameters		= proc_fs_parameters,
	.kill_sb		= proc_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT | FS_DISALLOW_NOTIFY_PERM,
};
```

`FS_DISALLOW_NOTIFY_PERM` 让 fanotify 的权限事件不能加在 proc 上，见 [09](./09-locks-and-notify.md)。

根目录的 `lookup` 是 `proc_root_lookup`（`fs/proc/root.c:323`）：

```text
proc_pid_lookup(dentry, flags)     名字是数字就建成 pid 目录，返回 NULL 表示已处理
没处理：
  proc_lookup                      静态项：cpuinfo、sys、fs …
```

`proc_root_readdir`（`:331`）先列静态项，位置到了 `FIRST_PROCESS_ENTRY` 再 `proc_pid_readdir`。所以 `readdir /proc` 里数字目录是现场从 pid 命名空间扫出来的，不是事先建好的一树 dentry。

`proc_root_init`（`:285`）还会 `proc_symlink("mounts", NULL, "self/mounts")`。`/proc/mounts` 是指向 `self/mounts` 的符号链接。

每个 pid 命名空间可以有自己的 proc 实例。`proc_pid_lookup` 用的命名空间来自超级块上的 `proc_fs_info->pid_ns`（`base.c:3488`），不是调用者随便传的。pid 命名空间本身见 [process-management/03](../process-management/03-pid-namespaces.md)。

---

## 11.2 `/proc/<pid>` 怎么来

`proc_pid_lookup`（`fs/proc/base.c:3476`）：

```text
tgid = name_to_int(&dentry->d_name)
  不是整数：返回 -ENOENT
rcu_read_lock
task = find_task_by_pid_ns(tgid, ns)
拿到 task 就 get_task_struct
hide_pid == HIDEPID_NOT_PTRACEABLE 时再查 has_pid_permissions
proc_pid_instantiate(dentry, task, NULL)     :3455
put_task_struct
```

`proc_pid_instantiate`：

```text
inode = proc_pid_make_base_inode(...)        :1988 被 :3460 调用
  失败：-ENOENT
inode->i_op = &proc_tgid_base_inode_operations
inode->i_fop = &proc_tgid_base_operations
inode->i_flags |= S_IMMUTABLE
set_nlink(inode, nlink_tgid)
pid_update_inode
d_set_d_op(dentry, &pid_dentry_operations)
d_splice_alias(inode, dentry)
```

`S_IMMUTABLE` 让这个目录不能被 `unlink` / `rename`。任务退出后，dentry 上的操作会发现任务没了，而不是目录还留在盘上。

子项不放在哈希里等着，而放在编译期的数组 `tgid_base_stuff[]`（`:3281`）。`proc_tgid_base_inode_operations` 的 `lookup` 调 `proc_pident_lookup`（`:2700`），在这张数组里按名字找。数组开头几项：

| 名字 | 种类 | 源码 |
|---|---|---|
| `task` | 目录 | `:3282` |
| `fd` | 目录 | `:3283` |
| `map_files` | 目录 | `:3284` |
| `fdinfo` | 目录 | `:3285` |
| `ns` | 目录 | `:3286` |
| `environ` / `auxv` | 文件 | `:3290` / `:3291` |
| `status` / `limits` | 读一次就生成的文件 | `:3292` / `:3294` |
| `comm` | 可读写 | `:3304` |
| `cmdline` | 文件 | `:3308` |
| `stat` / `statm` | 读一次就生成 | `:3309` / `:3310` |
| `maps` | 文件 | `:3311` |
| `cwd` / `root` / `exe` | 符号链接 | `:3316` 起 |
| `mounts` / `mountinfo` | 文件 | `:3319` / `:3320` |

`ONE(...)` 是打开后读一次、内容由回调填进 seq。`REG(...)` 有自己的 `file_operations`。`LNK(...)` 的 `get_link` 按当时的 `task_struct` 找路径。`fd` 目录再按文件描述符号生成，不在 `tgid_base_stuff` 里把每个 fd 写死。

线程目录是 `/proc/<pid>/task/<tid>`，走另一张表。`/proc/self` 是一个魔术符号链接，指向当前任务的 tgid。`proc_self_init` 在 `proc_root_init` 里调用（`root.c:289`）。

读 `/proc/<pid>/status` 时，文件的 `read` 回调拿着当时还能找到的 task，把字段格式化进缓冲。下一次读会再格式化一次。中间任务退出，后续读返回错误或空，而不是一份冻住的快照文件。

---

## 11.3 sysfs 与 kernfs

sysfs 的类型（`fs/sysfs/mount.c:90`）：

```c
static struct file_system_type sysfs_fs_type = {
	.name			= "sysfs",
	.init_fs_context	= sysfs_init_fs_context,
	.kill_sb		= sysfs_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT,
};
```

`sysfs_init`（`:97`）先 `kernfs_create_root`，再 `register_filesystem`。用户看到的 `/sys` 是这棵 kernfs 树的一个挂载。

kernfs 的目录项是 `struct kernfs_node`，不靠磁盘目录。VFS 的 `lookup` 是 `kernfs_iop_lookup`（`fs/kernfs/dir.c:1189`）：

```text
parent = dir->i_private          父 inode 上挂着 kernfs_node
down_read(&root->kernfs_rwsem)
需要命名空间时取出 ns
kn = kernfs_find_ns(parent, dentry->d_name.name, ns)
没找到：留下负 dentry 的可能，函数后半处理
找到但 kernfs_active(kn) 为假：
  注释写明对 VFS 不可见，不要建负项，返回 NULL     :1207
找到且活跃：
  kernfs_get_inode               做成 VFS inode，i_private 指回 kn
```

所以 sysfs 的「文件」是设备或子系统事先用 `__kernfs_create_file`（`include/linux/kernfs.h:425`）或 `sysfs_create_file` 挂上的节点。读它调用节点上的 `ops->read` 或 `seq_show`，当场从内核对象取值。写它调用 `store`。没有页缓存里的旧副本要回写。

cgroup 文件系统也建在 kernfs 上。cgroup 的层次和控制文件见 [cgroup/](../cgroup/)，本章只说明查找入口是同一个 `kernfs_iop_lookup`。

---

## 11.4 debugfs

```c
/* fs/debugfs/inode.c:304 */
static struct file_system_type debug_fs_type = {
	.owner =	THIS_MODULE,
	.name =		"debugfs",
	.mount =	debug_mount,
	.kill_sb =	kill_litter_super,
};
```

`debugfs_init`（`:899`）在 `core_initcall` 里注册。它仍用旧的 `mount` 回调，不是 `init_fs_context`。

内核代码创建文件用 `debugfs_create_file`（`:483`），内部是 `__debugfs_create_file`（`:410`）。它在 debugfs 的 dentry 树里加一个名字，`i_fop` 用调用者给的 `file_operations`，私有指针放进 `i_private` 或 dentry。`debugfs_create_file_unsafe`（`:522`）同样建文件，注释说明差别是 fops 的用法，不是另一套文件系统。

debugfs 没有一张全局的「所有调试文件」注册表给路径查找去扫。查找就是普通 dcache：创建时 dentry 已经链在父目录下。默认挂载点是 `/sys/kernel/debug`。没挂上时 `debugfs_create_file` 会失败，`debugfs_initialized`（`:879`）用来问注册做完了没有。

debugfs 里的文件同样可以是只读的当场生成文本。回收、shrinker 的 debugfs 接口写在 [MM 观测章](../memory-management/13-observability.md)，本章不重复那些文件名。

---

## 11.5 和 tmpfs 的差别

| | tmpfs | proc / sysfs / debugfs |
|---|---|---|
| 目录项从哪来 | `create` / `mkdir` 时分配 inode | 查找或创建函数按内核对象现做 |
| 文件字节 | `shmem_get_folio` 的页 | `read` 回调里格式化 |
| `write` | 改页缓存 | `store` 改内核对象 |
| 卸载之后 | 页和 inode 丢掉 | 内核对象还在，只是这棵挂载没了 |

---

## 11.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/proc/root.c` | `proc_fs_type` | 277 | 名字 `"proc"` |
| `fs/proc/root.c` | `proc_root_lookup` | 323 | 先试 pid，再试静态项 |
| `fs/proc/root.c` | `proc_root_readdir` | 331 | 静态项然后 pid 扫描 |
| `fs/proc/root.c` | `proc_root_init` | 285 | `self`、`thread-self`、`mounts` 链接 |
| `fs/proc/base.c` | `proc_pid_lookup` | 3476 | 名字转 tgid，找 task |
| `fs/proc/base.c` | `proc_pid_instantiate` | 3455 | 建 pid 目录 inode |
| `fs/proc/base.c` | `proc_pid_make_base_inode` | 1988 | 分配那个 inode |
| `fs/proc/base.c` | `tgid_base_stuff` | 3281 | `status`、`fd`、`maps` 等子项表 |
| `fs/proc/base.c` | `proc_pident_lookup` | 2700 | 在 `pid_entry` 数组里按名字找 |
| `fs/sysfs/mount.c` | `sysfs_fs_type` | 90 | 名字 `"sysfs"` |
| `fs/sysfs/mount.c` | `sysfs_init` | 97 | 建 kernfs 根并注册 |
| `fs/kernfs/dir.c` | `kernfs_iop_lookup` | 1189 | VFS lookup 进 kernfs |
| `fs/debugfs/inode.c` | `debug_fs_type` | 304 | 名字 `"debugfs"` |
| `fs/debugfs/inode.c` | `debugfs_create_file` | 483 | 挂一个调试文件 |
| `fs/debugfs/inode.c` | `debugfs_init` | 899 | `core_initcall` |

---

**导航**：[← tmpfs](./10-tmpfs-walkthrough.md) · [目录](./README.md) · [观测 →](./12-observability.md)
