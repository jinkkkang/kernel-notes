# 12. 观测与性能

**结论**：映射看 `filefrag -v` 打出来的 FIEMAP 旗标；iomap 的迭代看 `iomap_iter` / `iomap_dio_rw_begin`；ext4 回写和 fsync 看 `ext4_writepages` 与 `ext4_sync_file_enter`；日志提交看 `jbd2_commit_*`。下面的命令都是 **RECIPE**。本章没有采样输出，也不写延迟数字。

> tracepoint 的环、过滤和 `trace-cmd` 的用法在 [tracing/](../tracing/README.md)。这里只列 v6.6.160 里真实存在的事件名和该看的字段。`blktrace` 在 [block-io/16](../block-io/16-observability.md)。

---

## 12.1 `filefrag` 看的是 FIEMAP

内核侧：`FS_IOC_FIEMAP` 进 `ioctl_fiemap`（`fs/ioctl.c:810` → `:200`），再调 `inode->i_op->fiemap`。ext4 是 `ext4_fiemap`（`fs/ext4/file.c:959`），XFS 是 `xfs_vn_fiemap`（`fs/xfs/xfs_iops.c:1121`）。两边都进 `iomap_fiemap`（ext4：`fs/ext4/extents.c:5099`；XFS：`xfs_iops.c:1095`，xattr 时用 `xfs_xattr_iomap_ops`）。

`iomap_fiemap` 先 `fiemap_prep`（`fs/iomap/fiemap.c:76`）。`FIEMAP_FLAG_SYNC` 置位时，`fiemap_prep` 会 `filemap_write_and_wait`（`fs/ioctl.c:194`）。不带这个标志就不会为了查询去回写。

`iomap_to_fiemap`（`fiemap.c:13`）的类型对应关系：

| `iomap->type` | 加上的 `FIEMAP_EXTENT_*` |
|---|---|
| `IOMAP_HOLE` | 不产生 extent 记录 |
| `IOMAP_DELALLOC` | `DELALLOC` 和 `UNKNOWN` |
| `IOMAP_MAPPED` | 无额外类型位 |
| `IOMAP_UNWRITTEN` | `UNWRITTEN` |
| `IOMAP_INLINE` | `DATA_INLINE` |

位的定义在 `include/uapi/linux/fiemap.h:48` 起。`FIEMAP_EXTENT_DELALLOC` 的注释写明它会带上 `EXTENT_UNKNOWN`。`FIEMAP_EXTENT_UNWRITTEN` 的注释是 “Space allocated, but no data (i.e. zero).”

旗标字符串来自 **e2fsprogs v1.47.1** 的 `misc/filefrag.c`（不是内核树，也不是本机二进制）：`print_flags`（该文件 `:149`）把上述位印成 `unknown_loc`、`delalloc`、`unwritten`、`inline`、`merged`、`shared` 等。`sync_file` 默认 0（`:58`），只有 `-s` 才把 `FIEMAP_FLAG_SYNC` 放进请求（`:253`、`:651`）。`-v` 只是把详细行打出来（`:654`）。

因此 **RECIPE** 里对比 fsync 前后时不要加 `-s`：`-s` 会先把脏页写完，delalloc 可能在第一次 `filefrag` 之前就消失。即便不加 `-s`，后台回写仍可能抢先分配，所以第一次输出里没有 `delalloc` 也不表示路径错了。验收标准写在 [demos/README.md](./demos/README.md)，不在这里编一份样例输出。

`filefrag -E` 在那份 e2fsprogs 里改用 `EXT4_IOC_GET_ES_CACHE`。内核处理函数是 `ext4_ioctl_get_es_cache`（`fs/ext4/ioctl.c:1039`），数据来自 `ext4_get_es_cache` / `ext4_fill_es_cache_info`（`extents.c:2217`）。这和默认的 FIEMAP 不是同一次查询。

---

## 12.2 iomap tracepoint

定义在 `fs/iomap/trace.h`。文件头注释（`:5`）写明这些事件不是稳定 ABI。

| 事件 | 定义行 | 何时打 |
|---|---|---|
| `iomap_readpage` / `iomap_readahead` | `:50`、`:51` | 缓冲读 / 预读入口 |
| `iomap_writepage` | `:82` | 回写一个范围 |
| `iomap_iter` | `:159` | 每次 `iomap_iter()`，含 flags 和 ops 指针 |
| `iomap_iter_dstmap` / `iomap_iter_srcmap` | `:155`、`:156` | begin 之后的目的 / 源映射，type 印成 HOLE/DELALLOC/MAPPED/UNWRITTEN/INLINE（`:88`） |
| `iomap_writepage_map` | `:157` | 回写映射 |
| `iomap_dio_rw_begin` | `:192` | `__iomap_dio_rw` 入口，含 aio 与 dio_flags |
| `iomap_dio_rw_queued` | `:86` | 异步 direct 已提交、调用者要返回 |
| `iomap_dio_complete` | `:230` | 完成时的 error 和字节数 |
| `iomap_dio_invalidate_fail` | `:85` | 写前作废页缓存失败 |

**RECIPE**（不要在生产机上长时间开着全局 trace）：

```text
cd /sys/kernel/tracing
echo 1 > events/iomap/iomap_iter/enable
echo 1 > events/iomap/iomap_iter_dstmap/enable
echo 1 > events/iomap/iomap_dio_rw_begin/enable
echo 1 > events/iomap/iomap_dio_complete/enable
# 对测试文件做一次缓冲写或 O_DIRECT，然后
cat trace | head
echo 0 > events/iomap/enable
```

读的时候看 `type=` 和 `flags=`。缓冲写的 XFS 路径上，分配前可以出现 `DELALLOC`，回写之后的映射应变成 `MAPPED` 或 `UNWRITTEN`。ext4 缓冲写不进这些事件；ext4 的 DIO 会进 `iomap_dio_rw_begin`。

事件打在 `fs/iomap/iter.c:86`（`trace_iomap_iter`）和 `direct-io.c:573`（`trace_iomap_dio_rw_begin`）。

---

## 12.3 ext4 与 jbd2

事件定义在 `include/trace/events/ext4.h` 和 `include/trace/events/jbd2.h`。

| 事件 | 定义行 | 调用侧（已核对） |
|---|---|---|
| `ext4_write_begin` / `ext4_da_write_begin` | `ext4.h:378`、`:385` | 缓冲写 `write_begin` |
| `ext4_writepages` | `ext4.h:444` | `ext4_do_writepages` 入口 `inode.c:2580` |
| `ext4_writepages_result` | `ext4.h:542` | 同函数出口 `:2801` |
| `ext4_sync_file_enter` / `ext4_sync_file_exit` | `ext4.h:934`、`:961` | `ext4_sync_file` `:152`、`:191` |
| `ext4_fallocate_enter` / `ext4_punch_hole` | `ext4.h:1404`、`:1411` | fallocate / 打洞 |
| `jbd2_handle_start` | `jbd2.h:164` | handle 开始 |
| `jbd2_commit_locking` / `jbd2_commit_flushing` / `jbd2_commit_logging` | `jbd2.h:64`、`:71`、`:78` | 提交各阶段 |
| `jbd2_end_commit` | `jbd2.h:92` | 提交结束 |
| `jbd2_checkpoint` | `jbd2.h:14` | `jbd2_log_do_checkpoint` `:170` |
| `jbd2_submit_inode_data` | `jbd2.h:116` | ordered 模式提交数据 |

**RECIPE**：把 fsync 的时间拆开时，同时开 `ext4_sync_file_enter`、`ext4_sync_file_exit`、`jbd2_commit_flushing`、`jbd2_end_commit`。enter 到 exit 是这次 fsync 在 ext4 里的跨度；flushing 到 end_commit 是日志提交本身。两者对不上号时，多出来的时间在 `file_write_and_wait_range` 等脏页，或在 `blkdev_issue_flush`。不要凭空填微秒数。

```text
echo 1 > /sys/kernel/tracing/events/ext4/ext4_sync_file_enter/enable
echo 1 > /sys/kernel/tracing/events/ext4/ext4_sync_file_exit/enable
echo 1 > /sys/kernel/tracing/events/jbd2/jbd2_commit_flushing/enable
echo 1 > /sys/kernel/tracing/events/jbd2/jbd2_end_commit/enable
# 对测试文件 fsync 一次，看 trace，然后关掉
```

`ext4_da_write_begin` 出现而 `iomap_iter` 不出现，和 [09](./09-ext4-jbd2.md) 的分叉一致：这次是缓冲写。

---

## 12.4 XFS

已核对的调用点：

| 事件 | 定义 | 调用 |
|---|---|---|
| `xfs_file_buffered_write` | `fs/xfs/xfs_trace.h:1461` | `xfs_file.c:778` |
| `xfs_file_direct_write` | `xfs_trace.h:1462` | `xfs_file.c:600`，对齐的 DIO 写 |
| `xfs_file_fsync` | `xfs_trace.h:780` | `xfs_file.c:142` |
| `xfs_end_io_direct_write` | `xfs_trace.h:1552` | 调用在 `xfs_file.c:493`，转换 unwritten 之前 |
| `xfs_log_cil_wait` | `xfs_trace.h:1287` | 等 CIL 空间 |
| `xfs_rename` | 调用在 `fs/xfs/xfs_inode.c:2890` | rename |

**RECIPE**：缓冲写加 fsync 时开 `xfs_file_buffered_write` 和 `xfs_file_fsync`。fsync 里若还要等日志，应能看到 force 路径上的等待；本章没有把 `xfs_log_force` 内部的每个 trace 名对完，缺名字就写「查不到」，不要补。

---

## 12.5 延迟该看哪一段

没有实测数字。定位时按调用链分段，每一段用上面的事件或块层的 trace 卡住，而不是看一个总的 `fsync` 时间：

1. **用户字节进页缓存**：ext4 看 `ext4_da_write_begin`，XFS 看 `xfs_file_buffered_write` 和 `iomap_iter` 的 `DELALLOC`。这段不提交数据 bio。
2. **回写组 bio**：ext4 看 `ext4_writepages` → `ext4_writepages_result`。XFS 看 `iomap_writepage`。bio 提交之后的队列时间用 [block-io/16](../block-io/16-observability.md) 的 `block_rq_*`，不要在本章重写。
3. **日志**：ext4 看 `jbd2_commit_flushing` 到 `jbd2_end_commit`，ordered 模式中间还有 `jbd2_submit_inode_data`。XFS 看 fsync 是否走进 `xfs_log_force`（函数在 `xfs_log.c:3183`）以及 `xfs_log_cil_wait`。
4. **设备缓存**：ext4 在 `ext4_sync_file` 的 `blkdev_issue_flush`（`fsync.c:183`），或 commit 记录上的 `REQ_FUA`（`commit.c:154`）。XFS 在 `xfs_file_fsync` 的 `blkdev_issue_flush`（`xfs_file.c:160`、`:162`、`:185`）。flush 的块层行为在 block-io 章。

demo 02–05 只列了名字，脚本还没写，见 [demos/README.md](./demos/README.md)。

---

## 12.6 关键符号

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/ioctl.c` | `ioctl_fiemap` | 200 | `FS_IOC_FIEMAP` |
| `fs/ioctl.c` | `fiemap_prep` 里的 `FIEMAP_FLAG_SYNC` | 194 | 查询前写回 |
| `fs/iomap/fiemap.c` | `iomap_to_fiemap` | 13 | 类型到位 |
| `include/uapi/linux/fiemap.h` | `FIEMAP_EXTENT_DELALLOC` | 50 | delalloc 位 |
| `include/uapi/linux/fiemap.h` | `FIEMAP_EXTENT_UNWRITTEN` | 62 | unwritten 位 |
| `fs/iomap/trace.h` | `TRACE_EVENT(iomap_iter)` | 159 | 迭代 |
| `fs/iomap/trace.h` | `TRACE_EVENT(iomap_dio_rw_begin)` | 192 | DIO 开始 |
| `include/trace/events/ext4.h` | `ext4_sync_file_enter` | 934 | fsync 入口 |
| `include/trace/events/jbd2.h` | `jbd2_end_commit` | 92 | 提交结束 |
| `fs/xfs/xfs_trace.h` | `xfs_file_fsync` | 780 | XFS fsync |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。filefrag 的旗标拼写对照 e2fsprogs v1.47.1 `misc/filefrag.c`，本机没有跑过这份二进制。*

**导航**：[← fsync](./11-fsync-crash.md) · [目录](./README.md)
