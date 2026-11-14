# 盘上文件系统与 iomap

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`）。下文路径、函数、结构体字段与行号对照该版本核对。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 本章没有本机 `filefrag` / `dmesg` / trace 数字。命令默认是 **RECIPE**。demo 01 默认 DRY-RUN，状态是 **待真机验证**。

**一句话**：盘上文件系统站在 VFS / 页缓存和块层之间，负责把文件偏移答成磁盘区间；ext4 的缓冲读写在 6.6 仍走 `buffer_head` 和自己的 `ext4_writepages`，XFS 的缓冲读写走 iomap，两边的 Direct IO、fiemap、SEEK_HOLE/SEEK_DATA 都进 `iomap_iter`。

面向 **C 语言新手**：`get_block_t` 和 `iomap_ops` 都是文件系统填好的函数指针。通用代码按名字去调，自己不认识 extent 树。`struct iomap` 是「这一段文件字节对应磁盘上哪一段」的答案，长度可以跨很多块。`struct buffer_head` 是更老的答案单位，通常按一个块来记状态。

## 阅读顺序

```text
01 总览（VFS / 页缓存 ↔ 盘上 FS ↔ 块层）
  → 02 盘上布局（ext4 与 XFS，只讲结构）
  → 03 buffer_head 老路
  → 04 iomap 核心模型
  → 05 iomap 缓冲读写
  → 06 回写组 bio
  → 07 Direct IO
  → 08 delalloc 与 unwritten
  → 09 ext4 + jbd2
  → 10 XFS 日志（CIL）
  → 11 fsync 与崩溃一致性
  → 12 观测与性能配方
```

先分清谁回答映射、谁提交 bio。映射答错，后面的 bio 扇区就是错的。日志答的是「元数据何时算提交」，和映射是两件事。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 盘上 FS 的职责；buffer_head 与 iomap 的分界 | 中 |
| [02-ondisk-layout.md](./02-ondisk-layout.md) | superblock、inode、extent / B+tree、位图与分配组 | 中 |
| [03-buffer-head.md](./03-buffer-head.md) | `fs/buffer.c`、`block_write_begin`、`get_block_t` | 中 |
| [04-iomap-model.md](./04-iomap-model.md) | `struct iomap`、`iomap_ops`、`iomap_iter` | **厚** |
| [05-buffered-io.md](./05-buffered-io.md) | `iomap_file_buffered_write`、folio 子块状态、readahead | **厚** |
| [06-writeback.md](./06-writeback.md) | `iomap_writepages`、`iomap_ioend`、组 bio | **厚** |
| [07-direct-io.md](./07-direct-io.md) | `iomap_dio_rw`、对齐、同步 / 异步完成 | **厚** |
| [08-delalloc-unwritten.md](./08-delalloc-unwritten.md) | 延迟分配、unwritten 转换、fallocate / punch hole | 中 |
| [09-ext4-jbd2.md](./09-ext4-jbd2.md) | jbd2 handle / transaction / commit / checkpoint；ext4 哪些路径用 iomap | **厚** |
| [10-xfs-log.md](./10-xfs-log.md) | CIL、log item，和 jbd2 的设计差异 | 中 |
| [11-fsync-crash.md](./11-fsync-crash.md) | `vfs_fsync_range` → `->fsync` → 日志提交与 flush/FUA；rename 边界 | **厚** |
| [12-observability.md](./12-observability.md) | tracepoint、`filefrag`、回写 / 日志 / fsync 的读法。RECIPE | **厚** |
| [demos/](./demos/) | demo 01：自建 loop 上的 ext4（可选 xfs），`filefrag -v`。**待真机验证** | — |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `read_iter` / `write_iter` 已经进到具体文件系统 | `kiocb`、`iov_iter`、`generic_file_read_iter`：[vfs/05](../vfs/05-read-write.md) |
| 页缓存 folio 已经在地址空间里，脏页已经被标脏 | 页缓存、脏页限速、flusher 何时醒来：[memory-management/08](../memory-management/08-page-cache-and-writeback.md) |
| `submit_bio` 之前，bio 的扇区和 `bi_end_io` 已经由 iomap 或 ext4 自己填好 | `bio` / request / 调度器 / flush 状态机：[block-io/](../block-io/README.md) |
| `vfs_fsync_range` 调到 `f_op->fsync` 之后，ext4 / XFS 怎么等数据和日志 | VFS 停在 `f_op->fsync`：[vfs/05](../vfs/05-read-write.md) §5.6。块设备文件的 `REQ_PREFLUSH` 见 [block-io/10](../block-io/10-writeback-and-fsync.md) |
| Direct IO 的 `iomap_dio_rw` 与 `ki_complete` | io_uring 的环、SQE/CQE：[block-io/12](../block-io/12-io-uring.md)。块设备文件自己的 `blkdev_direct_IO` 见 [block-io/11](../block-io/11-direct-io.md) |
| overlayfs 的 copy-up、FUSE 请求队列 | [containerization/](../containerization/README.md) |

btrfs、f2fs、NFS、fsck/mkfs 的工具实现不在本章。

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/buffer_head.h`、`fs/buffer.c` | `buffer_head`、`block_write_begin` |
| `include/linux/fs.h` | `get_block_t`（`:90`） |
| `include/linux/iomap.h` | `struct iomap`、`iomap_ops`、`iomap_iter`、`iomap_ioend` |
| `fs/iomap/iter.c` | `iomap_iter` |
| `fs/iomap/buffered-io.c` | 缓冲读写、readahead、`iomap_writepages` |
| `fs/iomap/direct-io.c` | `iomap_dio_rw` |
| `fs/iomap/fiemap.c`、`fs/iomap/seek.c` | FIEMAP 与 SEEK_HOLE / SEEK_DATA |
| `fs/iomap/trace.h` | `iomap_*` tracepoint |
| `fs/ext4/file.c`、`inode.c`、`extents.c`、`fsync.c`、`page-io.c` | ext4 读写、extent、fsync、unwritten 完成 |
| `fs/ext4/ext4.h`、`fs/ext4/ext4_extents.h` | 盘上 superblock / inode / group / extent |
| `fs/jbd2/`、`include/linux/jbd2.h`、`include/linux/journal-head.h` | 日志 |
| `fs/xfs/xfs_file.c`、`xfs_aops.c`、`xfs_iomap.c` | XFS 的 iomap 接入 |
| `fs/xfs/xfs_log_cil.c`、`xfs_log_priv.h`、`xfs_trans.h` | CIL 与 log item |
| `fs/xfs/libxfs/xfs_format.h` | XFS 盘上结构 |
| `include/uapi/linux/fiemap.h` | `FIEMAP_EXTENT_*` |
| `fs/sync.c` | `vfs_fsync_range` |

## 实测 vs RECIPE

- **实测**：本章没有。写作时没有建 loop、没有跑 `filefrag`、没有打开 tracepoint。
- **RECIPE / 待真机验证**：[demos/01-run.sh](./demos/01-run.sh) 默认只打印将执行的命令。`KN_DEMO_RUN=1` 且 root 才操作脚本自建的 loop 镜像。验收标准在 [demos/README.md](./demos/README.md)。backlog ID 是 `OFS-LOOP-EXTENT`。[12](./12-observability.md) 的 trace 命令同样是 RECIPE。

## 未覆盖

- btrfs / f2fs 的盘上树和日志
- NFS / 9p 等网络文件系统
- FUSE
- mkfs / fsck / e2fsck 的修复步骤（`filefrag` 的旗标字符串对照过 e2fsprogs v1.47.1 的源码，工具本身不展开）
- overlayfs
- DAX 的 `dax_iomap_rw` 只作为 ext4 / XFS 的分支点到，不读 `fs/dax.c`
- jbd2 fast commit 的日志格式。`ext4_sync_file` 会调到 `ext4_fc_commit`，函数体不展开
- XFS `xfs_rename` 的事务信用计算。只钉定义行，不逐行抄

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
