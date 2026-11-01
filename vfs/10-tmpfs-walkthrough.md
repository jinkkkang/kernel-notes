# 10. tmpfs：从 open 到 fsync

**结论**：tmpfs 是一种具体文件系统，类型名 `"tmpfs"`，函数表在 `mm/shmem.c`。`open` 仍走 VFS 的 `do_filp_open`；`read` / `write` 进 `shmem_file_read_iter` / `shmem_file_write_iter`，页由 `shmem_get_folio` 放进这个 inode 的地址空间；`fsync` 调用 `noop_fsync`，直接返回 0。没有 bio，也没有 ext4。

> 页怎么分配、怎么换成 swap、`shmem_inode_info` 的字段，在 [MM §8.4](../memory-management/08-page-cache-and-writeback.md)。这一节只把 VFS 调用链接到那些函数，不重推换出。

下面按 `CONFIG_TMPFS` 和 `CONFIG_SHMEM` 都打开来写。这两个选项关掉时，`shmem_file_operations` 会变成 `ramfs_file_operations`（`mm/shmem.c:4777`），`read_iter` 不再是 `shmem_file_read_iter`。

---

## 10.1 注册

```c
/* mm/shmem.c:4607，包在 CONFIG_SHMEM 里 */
static struct file_system_type shmem_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "tmpfs",
	.init_fs_context	= shmem_init_fs_context, /* :4590 */
	.parameters		= shmem_fs_parameters,   /* 仅 CONFIG_TMPFS */
	.kill_sb		= kill_litter_super,
	.fs_flags		= FS_USERNS_MOUNT | FS_ALLOW_IDMAP,
};
```

`shmem_init`（`:4622`）`register_filesystem`，再 `kern_mount` 得到内核内部用的 `shm_mnt`（`:4638`）。用户 `mount -t tmpfs` 走 [07](./07-mount-and-ns.md) 的 `do_new_mount` → `shmem_init_fs_context` → `shmem_get_tree`（`:4399`）→ `get_tree_nodev(fc, shmem_fill_super)`。`shmem_fill_super` 在 `:4291`。

超级块私有数据是 `struct shmem_sb_info`，字段表在 MM §8.4，不在这里重列。

---

## 10.2 函数表

`CONFIG_TMPFS` 下的文件操作（`mm/shmem.c:4489`）：

```c
static const struct file_operations shmem_file_operations = {
	.mmap		= shmem_mmap,                 /* :2392 */
	.open		= shmem_file_open,             /* :2411 */
	.get_unmapped_area = shmem_get_unmapped_area,
	.llseek		= shmem_file_llseek,
	.read_iter	= shmem_file_read_iter,        /* :2748 */
	.write_iter	= shmem_file_write_iter,       /* :2867 */
	.fsync		= noop_fsync,                  /* fs/libfs.c:1561，返回 0 */
	.splice_read	= shmem_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= shmem_fallocate,
};
```

没有 `read` / `write` 这两个老指针。所以 `vfs_read` 会走 `new_sync_read` → `read_iter`。

目录操作（`:4515`，同样在 `CONFIG_TMPFS` 里）：

| 指针 | 函数 | 行号 |
|---|---|---|
| `lookup` | `simple_lookup` | `fs/libfs.c:70`，装在 `:4519` |
| `create` | `shmem_create` | `mm/shmem.c:3304`，装在 `:4518` |
| `unlink` | `shmem_unlink` | `mm/shmem.c:3350`，装在 `:4521` |
| `mkdir` / `rmdir` | `shmem_mkdir` / `shmem_rmdir` | `:3292` / `:3368`，装在 `:4523` |
| `getattr` | `shmem_getattr` | 装在 `:4517` |

`simple_lookup` 只在缓存里建一个 dentry，不读盘。tmpfs 的目录项本来就只活在内存里。

地址空间操作（`:4475`）：

```c
const struct address_space_operations shmem_aops = {
	.writepage	= shmem_writepage,    /* 回收时换出，见 MM §8.4 */
	.dirty_folio	= noop_dirty_folio,
	.write_begin	= shmem_write_begin,  /* :2687，仅 CONFIG_TMPFS */
	.write_end	= shmem_write_end,    /* :2723 */
	.migrate_folio	= migrate_folio,      /* CONFIG_MIGRATION */
	.error_remove_page = shmem_error_remove_page,
};
```

没有 `read_folio`，也没有 `direct_IO`。读不走 `generic_file_read_iter`。`shmem_file_open` 却把 `FMODE_CAN_ODIRECT` 置上（`:2413`），再调 `generic_file_open`（`fs/open.c:1622`）。这个位让 `O_DIRECT` 的 open 不会在 `do_dentry_open` 里因为「没有 direct_IO」而失败。真正的读写函数不看 `IOCB_DIRECT`，仍走 folio。

建 inode 时（`shmem_get_inode` 路径，`:2504` 一带）把 `i_mapping->a_ops` 设成 `shmem_aops`，普通文件的 `i_fop` 设成 `shmem_file_operations`（`:2506`）。

---

## 10.3 open

以 `open("/tmp/foo", O_RDWR | O_CREAT, 0644)` 为例。`/tmp` 已经是 tmpfs 挂载。

```text
openat                              fs/open.c:1448
  do_sys_openat2                    :1406
    get_unused_fd_flags
    do_filp_open                    fs/namei.c:3875
      path_openat
        link_path_walk              走到 /tmp 这个挂载的目录
        open_last_lookups           最后一截 "foo"
          目录 i_op 是 shmem_dir_inode_operations
          缓存没有：simple_lookup 得到负 dentry
          O_CREAT：shmem_create     mm/shmem.c:3304
            新 inode 的 i_fop = shmem_file_operations
        do_open
          vfs_open                  fs/open.c:1062
            do_dentry_open          :873
              f_op = shmem_file_operations
              shmem_file_open       mm/shmem.c:2411
                f_mode |= FMODE_CAN_ODIRECT
                generic_file_open   文件大于 2G 且没 O_LARGEFILE 则 -EOVERFLOW
    fd_install
```

到这里进程拿到一个 fd。`struct file` 的 `f_inode->i_mapping` 指向这份 shmem inode 的 `i_data`。里面还没有 folio。

---

## 10.4 read

```text
read                                fs/read_write.c:621
  vfs_read                          :450
    没有 f_op->read，有 read_iter
    new_sync_read                   :379
      init_sync_kiocb
      iov_iter_ubuf(ITER_DEST)
      shmem_file_read_iter          mm/shmem.c:2748
```

`shmem_file_read_iter` 按页循环（`:2762`）：

```text
index = ki_pos >> PAGE_SHIFT
超过 i_size：结束
shmem_get_folio(inode, index, &folio, SGP_READ)    :2778
  shmem_get_folio 是 shmem_get_folio_gfp 的包装     :2161
  SGP_READ：不超过 i_size；洞上不分配，返回空 folio
    页缓存命中、换入、分配的细节见 MM §8.4
folio 为空：这一段是洞，往用户缓冲填 0
folio 非空：folio_unlock，再把字节拷进 iov_iter
推进 ki_pos
```

`SGP_READ` 的定义在 MM §8.4 的那张表里（`include/linux/shmem_fs.h` 的 `enum sgp_type`）。这里要记住的只有一件事：读不负责创建新页，写才创建。

拷贝发生在 folio 已经在地址空间里之后。没有 `submit_bio`。

---

## 10.5 write

```text
write                               fs/read_write.c:646
  vfs_write
    new_sync_write
      shmem_file_write_iter         mm/shmem.c:2867
        inode_lock
        generic_write_checks        偏移、RLIMIT、文件大小
        file_remove_privs
        file_update_time
        generic_perform_write       mm/filemap.c:3990
        inode_unlock
```

`generic_perform_write` 按页调用 `a_ops->write_begin`。tmpfs 的实现是 `shmem_write_begin`（`mm/shmem.c:2687`）：

```text
看 seals：F_SEAL_GROW / F_SEAL_WRITE / F_SEAL_FUTURE_WRITE 会拒绝
shmem_get_folio(inode, index, &folio, SGP_WRITE)    :2706
  SGP_WRITE 可以越过当前 i_size，新页不必先标 uptodate
把 page 交回 generic_perform_write
拷贝用户字节
shmem_write_end                                      :2723
  更新 i_size
```

`dirty_folio` 是 `noop_dirty_folio`。tmpfs 不靠页缓存的脏标记来决定「以后写回磁盘」，因为没有磁盘。内存紧的时候 `shmem_writepage` 可以把 folio 换成 swap，那是 MM §8.4 的 `writepage` 路径，不是这次 `write` 系统调用在等的事情。

`shmem_file_write_iter` 末尾没有 `generic_write_sync`。`generic_file_write_iter` 会在写成功后调用它（`mm/filemap.c:4151`）。tmpfs 这张 `write_iter` 不调用。再叠加 `fsync` 是 `noop_fsync`，`O_SYNC` 和 `fsync` 在 tmpfs 上都不等待回写。

---

## 10.6 fsync

```text
fsync                               fs/sync.c:218
  do_fsync(fd, 0)                   :206
    vfs_fsync                       :200
      vfs_fsync_range               :180
        f_op->fsync 是 noop_fsync   fs/libfs.c:1561
          return 0
```

`noop_fsync` 不看 `start` / `end` / `datasync`。没有回写等待，因为页不在块设备上。`fdatasync`（`fs/sync.c:223`）同样进 `noop_fsync`。

如果这次写的页后来被回收换出，那是 kswapd 或直接回收调用 `shmem_writepage`，和用户刚才那次 `fsync` 返回 0 不是同一次调用。

---

## 10.7 mmap 这一笔

同一份文件若 `mmap`：

```text
mmap_file → shmem_mmap            mm/shmem.c:2392
  seal_check_write
  i_nlink 还在：vma->vm_ops = shmem_vm_ops          :2405
  已经 unlink：vma->vm_ops = shmem_anon_vm_ops      :2407
```

两张表的 `fault` 都是 `shmem_fault`（`:4572` 和 `:4581`），`map_pages` 都是 `filemap_map_pages`。缺页进 `shmem_fault`（`:2180`），再 `shmem_get_folio_gfp(..., SGP_CACHE)`。这不是 [06](./06-mmap-and-file.md) 的 `filemap_fault`。`SGP_CACHE` 和换入 swap 的步骤在 MM §8.4。

`MAP_SHARED` 且没有 file 的匿名映射，会由 `shmem_zero_setup` 造一个内部 shmem 文件。那条入口在 MM §8.4，不在这次「打开 /tmp 上一个文件」的链上。

---

## 10.8 和磁盘文件差在哪

| 步骤 | tmpfs | 通用磁盘文件 |
|---|---|---|
| 查找最后一截 | `simple_lookup` + `shmem_create` | `i_op->lookup` 读目录块 |
| `read_iter` | `shmem_file_read_iter` | 常常是 `generic_file_read_iter` |
| 缓存未命中 | `shmem_get_folio` 分配内存，或换入 swap | `a_ops->read_folio` 提交 bio |
| `write_iter` | `generic_perform_write` + `shmem_write_begin` | 同样的 `generic_perform_write`，`write_begin` 是文件系统自己的 |
| `fsync` | `noop_fsync` 返回 0 | `f_op->fsync` 等待回写 |
| `mmap` 的 fault | `shmem_fault` | `filemap_fault` |

---

## 10.9 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `mm/shmem.c` | `shmem_fs_type` | 4607 | 名字 `"tmpfs"` |
| `mm/shmem.c` | `shmem_init_fs_context` | 4590 | 新挂载 API |
| `mm/shmem.c` | `shmem_get_tree` | 4399 | `get_tree_nodev` |
| `mm/shmem.c` | `shmem_fill_super` | 4291 | 填超级块 |
| `mm/shmem.c` | `shmem_file_operations` | 4489 | 读写、mmap、fsync |
| `mm/shmem.c` | `shmem_aops` | 4475 | `write_begin` / `writepage` |
| `mm/shmem.c` | `shmem_file_open` | 2411 | 置 `FMODE_CAN_ODIRECT` |
| `mm/shmem.c` | `shmem_file_read_iter` | 2748 | `SGP_READ` |
| `mm/shmem.c` | `shmem_file_write_iter` | 2867 | 再进 `generic_perform_write` |
| `mm/shmem.c` | `shmem_write_begin` | 2687 | `SGP_WRITE` |
| `mm/shmem.c` | `shmem_write_end` | 2723 | 更新 `i_size` |
| `mm/shmem.c` | `shmem_get_folio` | 2161 | 页缓存 / swap / 分配的总入口 |
| `mm/shmem.c` | `shmem_mmap` | 2392 | 装 `shmem_vm_ops` |
| `mm/shmem.c` | `shmem_fault` | 2180 | 映射缺页 |
| `mm/shmem.c` | `shmem_create` | 3304 | `O_CREAT` |
| `fs/libfs.c` | `simple_lookup` | 70 | 目录查找 |
| `fs/libfs.c` | `noop_fsync` | 1561 | `return 0` |
| `fs/open.c` | `generic_file_open` | 1622 | 大文件与 `O_LARGEFILE` |
| `mm/filemap.c` | `generic_perform_write` | 3990 | 按页 `write_begin` |

---

**导航**：[← 锁与通知](./09-locks-and-notify.md) · [目录](./README.md) · [伪文件系统 →](./11-pseudo-fs.md)
