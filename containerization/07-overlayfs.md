# 7. overlayfs

**结论**：合并目录是一张查找结果，文件本体在 lower 或 upper。第一次以写方式打开还在 lower 上的文件时，`ovl_maybe_copy_up` 把父目录链和文件内容抄到 upper，之后的读写都落在 upper。删除 lower 上的名字靠 upper 里的 whiteout（rdev 为 0 的字符设备）。盖住 lower 目录靠 opaque 的 xattr。6.6 的这些行为在 `fs/overlayfs/`。

> VFS 章只在 `dentry_operations.d_real` 上点到层叠钩子。copy-up、whiteout、opaque 在这里。

---

## 7.1 四层目录和挂载对象

挂载选项在 `fs/overlayfs/params.c:147`：`lowerdir`、`lowerdir+`、`datadir+`、`upperdir`、`workdir`，以及 `redirect_dir`、`index`、`metacopy`、`volatile`、`userxattr` 等。用户看见的合并树是 overlay 自己的超级块；lower/upper/work 是别的文件系统上的目录，各自有 `vfsmount`。

```c
/* fs/overlayfs/ovl_entry.h:8 */
struct ovl_config {
	char *upperdir;
	char *workdir;
	char **lowerdirs;
	bool default_permissions;
	int redirect_mode;
	int verity_mode;
	bool index;
	int uuid;
	bool nfs_export;
	int xino;
	bool metacopy;
	bool userxattr;
	bool ovl_volatile;
};

/* :33 。layers[0] 是 upper */
struct ovl_layer {
	struct vfsmount *mnt;     /* ovl_free_fs 依赖它是第一个成员 */
	struct inode *trap;
	struct ovl_sb *fs;
	int idx;                   /* upper 的 idx 是 0 */
	int fsid;
};

/* :50 。一个合并 dentry 下面的 lower 栈，在 ovl_inode 里 */
struct ovl_entry {
	unsigned int __numlower;
	struct ovl_path __lowerstack[];
};

/* :56 */
struct ovl_fs {
	unsigned int numlayer;
	unsigned int numfs;
	unsigned int numdatalayer;     /* 只做数据、不参与元数据合并的 lower */
	const struct ovl_layer *layers;
	struct ovl_sb *fs;
	struct dentry *workbasedir;    /* 挂载参数 workdir= */
	struct dentry *workdir;        /* workbasedir 下的 work 目录 */
	struct dentry *indexdir;
	long namelen;
	struct ovl_config config;
	const struct cred *creator_cred;
	bool tmpfile;
	bool noxattr;
	bool nofh;
	bool upperdir_locked;
	bool workdir_locked;
	struct inode *workbasedir_trap;
	struct inode *workdir_trap;
	struct inode *indexdir_trap;
	int xino_mode;                 /* -1 关闭；0 同一文件系统；1..32 借用的 ino 位数 */
	atomic_long_t last_ino;
	struct dentry *whiteout;       /* 共享 whiteout 缓存 */
	bool no_shared_whiteout;
	errseq_t errseq;               /* volatile 挂载上对 upper 的只读错误序号 */
};
```

`ovl_upper_mnt`（`ovl_entry.h:102`）就是 `ofs->layers[0].mnt`。没有 upper 的挂载是只读合并：没有 copy-up 的地方。

`ovl_fill_super`（`fs/overlayfs/super.c:1337`）里 `ovl_get_upper`（`:539`）和 `ovl_get_layers`（`:1037`）把这些目录挂上。`ovl_make_workdir`（`:701`）在 upper 的挂载上创建 `work` 目录。注释写明 workdir 和 upper 在同一文件系统（`:727`），这样 whiteout 和 copy-up 的 rename 才是同一文件系统内的改名，不会变成复制。upper 不支持 `d_type` 时 whiteout 会在目录遍历里露出来，`:738` 对此 `pr_warn`。

合并 inode 是 `ovl_inode`（`ovl_entry.h:163`），`vfs_inode` 嵌在里面，`__upperdentry` 指向已经 copy-up 的 upper dentry，`oe` 指向 lower 栈。`OVL_I(inode)` 用 `container_of` 从 `vfs_inode` 找回它（`:179`）。

---

## 7.2 查找：merged 上看见谁

`ovl_lookup` 声明在 `overlayfs.h:645`，实现在 `fs/overlayfs/namei.c`。一个名字先看 upper。upper 上是 whiteout 就停，合并结果里这个名字不存在。upper 上是目录并且带 opaque，就不再往 lower 合并这个目录的内容。否则按 lower 从高到低找同名项，叠进 `ovl_entry` 的 lower 栈。

多个 lower 的顺序是挂载参数里的顺序：前面的盖住后面的。`datadir+` 加的是 `numdatalayer` 里那种只提供数据的层，元数据合并不走它们。

---

## 7.3 whiteout 与 opaque

内核通用 whiteout 是 rdev 为 0 的字符设备：

```c
/* include/linux/fs.h:214 */
#define WHITEOUT_MODE 0
#define WHITEOUT_DEV 0

/* :2161 */
#define IS_WHITEOUT(inode) (S_ISCHR(inode->i_mode) && \
                            (inode)->i_rdev == WHITEOUT_DEV)
```

`ovl_is_whiteout`（`fs/overlayfs/util.c:576`）就是 `inode && IS_WHITEOUT(inode)`。overlay 在 upper 上造 whiteout 走 `ovl_do_whiteout`（`overlayfs.h:364`），里面是 `vfs_whiteout`。查找见到它就当作「这个名字已被删除」（`fs/overlayfs/namei.c:254`）。

在合并树里再 `mknod` 一个同样的字符设备会被拒绝：`ovl_mknod`（`fs/overlayfs/dir.c:666`）看到 `S_ISCHR(mode) && rdev == WHITEOUT_DEV` 返回 `-EPERM`。whiteout 只由 overlay 自己造。

合并 dentry 上另有 `ovl_dentry_is_whiteout`（`fs/overlayfs/util.c:433`）：这个 dentry 没有 inode，并且标了 `OVL_E_OPAQUE`。upper 里的字符设备由上面的 `ovl_is_whiteout` 识别。

opaque 盖住整个 lower 目录。xattr 名由 `ovl_xattr_table` 拼出来（`util.c:773`）：

```c
/* fs/overlayfs/overlayfs.h:29 */
#define OVL_XATTR_NAMESPACE "overlay."
#define OVL_XATTR_TRUSTED_PREFIX XATTR_TRUSTED_PREFIX OVL_XATTR_NAMESPACE
#define OVL_XATTR_USER_PREFIX    XATTR_USER_PREFIX    OVL_XATTR_NAMESPACE
```

`XATTR_TRUSTED_PREFIX` 是 `"trusted."`，`XATTR_USER_PREFIX` 是 `"user."`（`include/uapi/linux/xattr.h:44`）。拼上后缀 `opaque`（`util.c:759`）之后，`userxattr` 为假时名字是 `trusted.overlay.opaque`，为真时是 `user.overlay.opaque`。判定函数读到 1 字节且值是 `'y'` 才算 opaque（`util.c:753`）。合并 dentry 上的对应标志是 `OVL_E_OPAQUE`（`overlayfs.h:60`），`ovl_dentry_is_opaque`（`util.c:428`）读它。真正写入 `"y"` 的是 `ovl_set_opaque_xerr`（`fs/overlayfs/dir.c:234`）。`ovl_make_workdir` 里对 work 目录写的 `overlay.opaque` 值是 `"0"`（`super.c:762`），用来探测 upper 能不能设 xattr，成功后立刻删掉（`:794`）。

同表里还有 `redirect`、`origin`、`impure`、`nlink`、`upper`、`uuid`、`metacopy`、`protattr`。`origin` 在 index 打开时记下文件从哪个 lower inode 抄上来。`redirect` 在目录改名后让查找跟着新名字走，由 `redirect_dir` 选项控制。

---

## 7.4 copy-up

写打开或 `O_TRUNC` 才需要把数据抄上来。`ovl_open_flags_need_copy_up`（`overlayfs.h:392`）在 flags 为 0 时返回假；否则看 `OPEN_FMODE(flags) & FMODE_WRITE` 或 `O_TRUNC`。

```text
ovl_open                         fs/overlayfs/file.c:154
  ovl_maybe_copy_up                fs/overlayfs/copy_up.c:1179
    ovl_open_need_copy_up          :1164
    ovl_copy_up_flags              :1111
      沿父目录走到第一个已有 upper 的祖先   :1144
      ovl_copy_up_one              :1043    从那一层往下逐个抄
        ovl_do_copy_up             :868
          ovl_copy_up_workdir      :706     普通路径：先在 workdir 做临时文件
          或 ovl_copy_up_tmpfile   :792     upper 支持 tmpfile 时
          ovl_copy_up_data         :591     非 metacopy 时拷数据
          再 rename 到 upper 的目标名
```

`ovl_copy_up_flags` 用 `ovl_override_creds` 换成挂载时记下的 `creator_cred` 再抄，避免调用者在 user ns 里的权限不够写 upper。抄完 `revert_creds`。

父目录还在 lower 上时，文件不能单独出现在 upper：循环会先 copy-up 祖先目录。所以一次写打开的成本是「路径上每个还没有 upper 的目录，加上文件全文」，见 [12](./12-perf-and-pitfalls.md)。

`metacopy` 选项允许只抄元数据、数据仍留在 lower，upper inode 上记 `trusted.overlay.metacopy`（或 user 前缀）。`ovl_need_meta_copy_up`（`fs/overlayfs/copy_up.c:941`）在选项打开、对象是普通文件、并且 flags 里没有写打开也没有 `O_TRUNC` 时返回真。写打开仍然整文件拷数据。`ovl_copy_up_meta_inode_data`（`:995`）在后来真的要数据时再补。`verity_mode == OVL_VERITY_REQUIRE` 且 lower 没有 fsverity 时，这次退回整文件拷贝（`:956`）。

index 打开时，非目录先抄到 index 目录再硬链接到 upper 的目标名（`ovl_do_copy_up` 注释，`:875`）。`volatile` 挂载不做 upper 的 sync（`ovl_should_sync`，`ovl_entry.h:122`）。

copy-up 之后 `ovl_inode->__upperdentry` 指向新文件，`ovl_already_copied_up`（`util.c:627`）为真，后续写直接打在 upper 上。lower 原文件保持不动。

---

## 7.5 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/overlayfs/ovl_entry.h` | `struct ovl_fs` | 56 | 一次挂载的层次 |
| `fs/overlayfs/ovl_entry.h` | `struct ovl_inode` | 163 | 合并 inode，含 upper dentry |
| `fs/overlayfs/params.c` | 选项表 | 147 | lower/upper/work 与 metacopy 等 |
| `fs/overlayfs/super.c` | `ovl_fill_super` | 1337 | 挂上各层 |
| `fs/overlayfs/super.c` | `ovl_make_workdir` | 701 | 建 work，要求与 upper 同文件系统 |
| `fs/overlayfs/util.c` | `ovl_is_whiteout` | 576 | upper 上的字符设备 whiteout |
| `fs/overlayfs/dir.c` | `ovl_mknod` | 666 | 禁止用户造 whiteout |
| `fs/overlayfs/util.c` | `ovl_xattr_table` | 773 | opaque、origin、metacopy 等名字 |
| `fs/overlayfs/file.c` | `ovl_open` | 154 | 打开前尝试 copy-up |
| `fs/overlayfs/copy_up.c` | `ovl_copy_up_flags` | 1111 | 先祖先后自己 |
| `fs/overlayfs/copy_up.c` | `ovl_do_copy_up` | 868 | workdir 或 tmpfile，再落到 upper |
| `fs/overlayfs/copy_up.c` | `ovl_need_meta_copy_up` | 941 | metacopy 的判定 |
| `include/linux/fs.h` | `IS_WHITEOUT` | 2161 | rdev 0 的字符设备 |

---

**导航**：[← cgroup](./06-cgroup-in-containers.md) · [目录](./README.md) · [FUSE →](./08-fuse.md)
