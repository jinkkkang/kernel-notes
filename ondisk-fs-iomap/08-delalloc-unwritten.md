# 8. delalloc 与 unwritten

**结论**：延迟分配让缓冲写先在页缓存里标脏，盘上先不占物理块；回写时才分配。unwritten extent 是盘上已经留出的块，读到的必须是零，直到 IO 完成把它们标成已写。`fallocate` 造的是 unwritten；`FALLOC_FL_PUNCH_HOLE` 把一段变成洞。

> 映射类型 `IOMAP_DELALLOC` / `IOMAP_UNWRITTEN` 的定义在 [04](./04-iomap-model.md)。回写和 DIO 的完成函数在 [06](./06-writeback.md)、[07](./07-direct-io.md)。

---

## 8.1 延迟分配

缓冲写如果马上分配，崩溃后文件里会留下没写完的块，而且短命文件会白占一次分配。delalloc 把分配推迟到回写。

ext4 挂载选项 `DELALLOC` 是 `EXT4_MOUNT_DELALLOC`（`fs/ext4/ext4.h:1225`）。`ext4_set_aops` 在 ordered / writeback 模式且这个选项打开时选用 `ext4_da_aops`（`inode.c:3682`）。

```text
ext4_da_write_begin               fs/ext4/inode.c:2907
  空间紧或 verity：改走 ext4_write_begin     :2921
  否则 __block_write_begin(..., ext4_da_get_block_prep)
ext4_da_get_block_prep            :1842
  ext4_da_map_blocks              :1729
  延迟的 bh：BH_Mapped | BH_New | BH_Delay，b_blocknr = -1
    注释在 :1835
回写：
  ext4_writepages → ext4_do_writepages     inode.c:2565
    mpage_map_one_extent          :2173
      ext4_map_blocks             :2205    这时才分配物理块
                                          函数定义在 :511
```

FIEMAP 在块还没分配时要能看见这段。`ext4_iomap_begin_report`（`:3510`）先 `ext4_map_blocks`；返回 0 再问 extent status 是不是 delayed（`ext4_iomap_is_delalloc`，`:3487`）。是，且 iomap 仍是洞，就把类型改成 `IOMAP_DELALLOC`（`:3561`）。`iomap_to_fiemap`（`fs/iomap/fiemap.c:19`）给这种类型加上 `FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN`。

XFS 在缓冲写的 `iomap_begin` 里就可以返回 delalloc。`xfs_bmbt_to_iomap`（`fs/xfs/xfs_iomap.c:91`）见到 `DELAYSTARTBLOCK` 或 null startblock：`addr = IOMAP_NULL_ADDR`，`type = IOMAP_DELALLOC`（`:108`）。回写的 `xfs_map_blocks` 里会 `xfs_bmapi_convert_delalloc`（调用点 `xfs_aops.c:362`）把这段换成真实块。没提交成功的 folio 由 `xfs_discard_folio`（`:439`）调用 `xfs_bmap_punch_delalloc_range` 打掉，注释说明否则干净页盖住的 delalloc 会变成陈旧映射。

短写退回预留是 `xfs_buffered_write_iomap_end` → `iomap_file_buffered_write_punch_delalloc`（`xfs_iomap.c:1214`）。

---

## 8.2 unwritten

预分配要在盘上占块，又不能让读看到旧内容。ext4 用 extent `ee_len` 的最高位表示 unwritten，长度上限见 [02](./02-ondisk-layout.md)。XFS 用 bmap 记录的 `br_state == XFS_EXT_UNWRITTEN`，译成 `IOMAP_UNWRITTEN`（`xfs_iomap.c:117`）。

转换发生在数据 IO 成功之后，失败就保持 unwritten，读路径继续当零。

| 路径 | 转换函数 | 位置 |
|---|---|---|
| ext4 缓冲回写 | `ext4_convert_unwritten_io_end_vec` | `fs/ext4/page-io.c:194`，由 `ext4_end_io_end` 调用 |
| ext4 DIO 写 | `ext4_convert_unwritten_extents` | `fs/ext4/file.c:388`，`ext4_dio_write_end_io` 里、`IOMAP_DIO_UNWRITTEN` 时 |
| XFS 缓冲回写 | `xfs_iomap_write_unwritten` | `fs/xfs/xfs_aops.c:131`，`xfs_end_ioend` |
| XFS DIO 写 | `xfs_iomap_write_unwritten` | `fs/xfs/xfs_file.c:529`，`xfs_dio_write_end_io` |

`ext4_convert_unwritten_extents` 的定义在 `fs/ext4/extents.c:4916`。更底层的 `ext4_convert_unwritten_extents_endio` 在 `:3787`。`ext4_end_io_end` 的注释（`page-io.c:185`）：写回失败就不转换，否则会露出未初始化数据。

DIO 侧，`iomap_dio_bio_iter` 看见 `IOMAP_UNWRITTEN` 就给这次 dio 置 `IOMAP_DIO_UNWRITTEN`（`direct-io.c:306`），完成时原样传给 `end_io`。

ext4 直接写分配时，`ext4_iomap_alloc`（`inode.c:3331`）对文件范围内的 extent 文件使用 `EXT4_GET_BLOCKS_IO_CREATE_EXT`（`:3373`），超出 `i_size` 的用 `EXT4_GET_BLOCKS_CREATE`（`:3371`）。注释（`:3365`）说明用 `i_size` 而不是 `i_disksize`：delalloc 回写可能在 DIO 进行中把 `i_disksize` 推到 `i_size`，暴露出还没写的块。

---

## 8.3 fallocate 与 punch hole

ext4 的 `f_op->fallocate` 是 `ext4_fallocate`（`fs/ext4/extents.c:4808`，登记在 `file.c:950`）。接受的模式位（`:4829`）：

```text
FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE
FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE | FALLOC_FL_INSERT_RANGE
```

其它位返回 `-EOPNOTSUPP`。加密 inode 再拒绝 collapse 和 insert（`:4824`）。

```text
FALLOC_FL_PUNCH_HOLE → ext4_punch_hole          extents.c:4841
                                              实现在 inode.c:4024
FALLOC_FL_COLLAPSE_RANGE / INSERT_RANGE / ZERO_RANGE
  各自的分支在 :4845、:4850、:4855
其余：预分配，留下 unwritten extent
```

`ext4_punch_hole`（`inode.c:4024`）打 `trace_ext4_punch_hole`（`:4036`），然后在 journal handle 里把范围内的映射拿掉。部分块要先读出来、把洞的边缘写成零，再打洞；整块直接变成洞。函数后半的每个分支这里不逐行抄。

XFS 的 `xfs_file_fallocate`（`xfs_file.c:939`）接受的位在 `XFS_FALLOC_FL_SUPPORTED`（`:933`），比 ext4 多 `FALLOC_FL_UNSHARE_RANGE`。打洞调用 `xfs_free_file_space`（`xfs_file.c:997` 附近，函数定义在 `fs/xfs/xfs_bmap_util.c:987`）。预分配走出 unwritten extent，FIEMAP 类型仍是 `IOMAP_UNWRITTEN`。

demo 01 用 `fallocate -l` 看 `filefrag` 的 `unwritten` 旗标，步骤在 [demos/README.md](./demos/README.md)。没有在本环境跑过。

---

## 8.4 读到这两种范围时

`IOMAP_HOLE` 和 `IOMAP_DELALLOC` 没有磁盘地址（`addr` 是 `IOMAP_NULL_ADDR`）。缓冲读把对应的 folio 范围当零，不提交读 bio。`IOMAP_UNWRITTEN` 同样对读返回零，即使 `addr` 指向已分配的块——那些块的旧内容不能当文件数据。

FIEMAP 把洞跳过（`iomap_to_fiemap` 对 `IOMAP_HOLE` 返回 0，`fiemap.c:16`），把 unwritten 标上 `FIEMAP_EXTENT_UNWRITTEN`（`:24`）。所以 `filefrag` 看得到预分配，看不到 iomap 这条路径上报的洞。

---

## 8.5 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/ext4/inode.c` | `ext4_da_write_begin` | 2907 | 缓冲写走延迟分配 |
| `fs/ext4/inode.c` | `ext4_da_get_block_prep` | 1842 | 置 `BH_Delay` |
| `fs/ext4/inode.c` | `ext4_map_blocks` | 511 | 查找或分配 |
| `fs/ext4/inode.c` | `ext4_iomap_begin_report` | 3510 | FIEMAP 时补上 delalloc |
| `fs/ext4/inode.c` | `ext4_punch_hole` | 4024 | 打洞 |
| `fs/ext4/extents.c` | `ext4_fallocate` | 4808 | fallocate 分发 |
| `fs/ext4/extents.c` | `ext4_convert_unwritten_extents` | 4916 | unwritten → 已写 |
| `fs/xfs/xfs_iomap.c` | `xfs_bmbt_to_iomap` | 91 | delalloc / unwritten 译成类型 |
| `fs/xfs/xfs_aops.c` | `xfs_discard_folio` | 439 | 回写失败打掉 delalloc |
| `fs/xfs/xfs_file.c` | `xfs_file_fallocate` | 939 | XFS 的 fallocate |
| `fs/iomap/fiemap.c` | `iomap_to_fiemap` | 13 | 类型译成 `FIEMAP_EXTENT_*` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← Direct IO](./07-direct-io.md) · [目录](./README.md) · [ext4 + jbd2 →](./09-ext4-jbd2.md)
