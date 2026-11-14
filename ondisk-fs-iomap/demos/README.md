# ondisk-fs-iomap demos

对照 v6.6.160 的 delalloc / unwritten 和 FIEMAP。**本环境没有跑过 loop。** demo 01 脚本写完，状态是 **待真机验证** / RECIPE。02–05 只有名字。

只允许动脚本自己 `truncate` 出来的镜像和它 `losetup` 出来的那一个 `/dev/loop*`。不要把 `KN_DEMO_DEV` 指到现有盘上：脚本看到这个变量会直接退出。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `01-run.sh` | 默认 DRY-RUN。`KN_DEMO_RUN=1` 且 root 才建镜像、mkfs、挂 loop、写文件、`filefrag -v`、`fallocate` | **待真机验证** |

```text
./01-run.sh
# 测试机、非生产、root：
sudo KN_DEMO_RUN=1 ./01-run.sh
# 可选，默认 ext4：
sudo KN_DEMO_RUN=1 KN_DEMO_FS=xfs ./01-run.sh
```

需要 `truncate`、`mkfs.ext4`（xfs 时还要 `mkfs.xfs`）、`losetup`、`mount`、`filefrag`、`fallocate`、`python3`。镜像 512MiB，放在 `mktemp -d` 的目录里。退出时 trap：若挂载点仍挂着就 `umount`，仅当 `/sys/block/loopN/loop/backing_file` 等于这次的镜像才 `losetup -d`，然后删掉临时目录。

`filefrag -v` 不加 `-s`。e2fsprogs v1.47.1 的 `misc/filefrag.c` 里 `-s` 才会带 `FIEMAP_FLAG_SYNC`（该文件 `:58`、`:253`、`:651`），内核的 `fiemap_prep` 见到这个标志会先 `filemap_write_and_wait`（`fs/ioctl.c:194`）。加了 `-s`，fsync 前的那次查询就不再是「还没 fsync」。

## demo 01 验收（给以后的 code agent）

在测试机上先不带变量跑一遍，确认只有 DRY-RUN、没有 `losetup`。再 `sudo KN_DEMO_RUN=1 ./01-run.sh`。把整段 stdout 贴回时标 `sampled on <uname -r>`。下面是要核对的事实，不是一份编好的输出：

1. 脚本打印的 loop 设备匹配 `/dev/loop*`，`/sys/block/<name>/loop/backing_file` 等于这次的 `disk.img`。结束之后这个 loop 不再出现在 `losetup -j` 该镜像的结果里，临时目录被删掉。
2. **fsync 前**的 `filefrag -v`：可能含 `delalloc` 和 `unknown_loc`（内核 `IOMAP_DELALLOC` 会带上 `FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN`，`fs/iomap/fiemap.c:19`；e2fsprogs v1.47.1 把这两位印成这两个词）。后台回写可能已经分配，所以**没有这两个词不算失败**。把原文贴上即可。
3. **fsync 后**的 `filefrag -v`：同一文件应出现物理区间，旗标列里不再有 `delalloc`。物理块号以这次输出为准，不要预填。
4. `fallocate -l 4M` 之后的 `filefrag -v`：旗标列含 `unwritten`（`FIEMAP_EXTENT_UNWRITTEN`，`include/uapi/linux/fiemap.h:62`）。没有这个词就保持 RECIPE，在 backlog `OFS-LOOP-EXTENT` 里写 blocker（文件系统没做成 unwritten、`filefrag` 版本的用词不同，或命令失败）。
5. 全程没有出现脚本临时目录以外的块设备名。失败（不是 root、没有 `mkfs`、`losetup` 失败、backing file 对不上）保持 RECIPE，写明缺什么，不要编 `filefrag` 输出。

`KN_DEMO_FS=xfs` 用同一套验收。XFS 的 fiemap 是 `xfs_vn_fiemap` → `iomap_fiemap`（`fs/xfs/xfs_iops.c:1081`）。

## 以后再写（只有名字）

| 编号 | 目的 | 状态 |
|------|------|------|
| 02 | 打开 `iomap_iter` / `iomap_iter_dstmap`，跟一次缓冲写和随后的 fsync。ext4 缓冲写预期不进 `iomap_iter`，XFS 会进 | RECIPE / 待真机验证。不写脚本 |
| 03 | `O_DIRECT` 按逻辑块对齐与不对齐：对齐进 `iomap_dio_rw`，不对齐时 ext4 可能回缓冲或由 iomap 返回 `-EINVAL` | RECIPE / 待真机验证。不写脚本 |
| 04 | `jbd2_commit_flushing` 到 `jbd2_end_commit`，对照一次 ext4 fsync | RECIPE / 待真机验证。不写脚本 |
| 05 | `fallocate` 与 punch hole 前后的 `filefrag -v`，看 `unwritten` 出现和消失 | RECIPE / 待真机验证。不写脚本 |

命令骨架在 [12](../12-observability.md)，仍然是 RECIPE。
