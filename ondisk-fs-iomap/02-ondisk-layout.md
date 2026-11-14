# 2. 盘上布局

**结论**：ext4 把磁盘切成块组，每组有块位图、inode 位图和 inode 表；文件的块映射在 inode 的 `i_block[15]` 里，打开 extents 特性时这里是一棵 extent 树。XFS 把磁盘切成分配组，空闲空间和 inode 位置记在组内的 B+tree 里，文件的区间记在 inode 的 extent 数组或 bmap B+tree 里。两边都是「超级块描述整盘，组描述一块地盘，inode 描述一个文件」。

> 只讲结构。分配器怎样挑块、日志怎样改这些结构，分别在 [08](./08-delalloc-unwritten.md)、[09](./09-ext4-jbd2.md)、[10](./10-xfs-log.md)。

---

## 2.1 ext4：超级块和块组

盘上超级块是 `struct ext4_super_block`（`fs/ext4/ext4.h:1300`）。开头这几项就够定位块组：

```c
/* fs/ext4/ext4.h:1300，摘到能算出块组。后面还有特性位、日志 inode 等 */
struct ext4_super_block {
	__le32	s_inodes_count;		/* Inodes count */
	__le32	s_blocks_count_lo;	/* Blocks count */
	__le32	s_r_blocks_count_lo;	/* Reserved blocks count */
	__le32	s_free_blocks_count_lo;	/* Free blocks count */
	__le32	s_free_inodes_count;	/* Free inodes count */
	__le32	s_first_data_block;	/* First Data Block */
	__le32	s_log_block_size;	/* Block size */
	__le32	s_log_cluster_size;	/* Allocation cluster size */
	__le32	s_blocks_per_group;	/* # Blocks per group */
	__le32	s_clusters_per_group;	/* # Clusters per group */
	__le32	s_inodes_per_group;	/* # Inodes per group */
	/* ... s_magic 在 :1316；日志文件的 inode 号 s_journal_inum 在 :1360 */
};
```

块大小是 `EXT4_MIN_BLOCK_SIZE << s_log_block_size`（`ext4.h:339` 的 `EXT4_BLOCK_SIZE`）。组的个数由块数和 `s_blocks_per_group` 推出来。每一组有一条 `struct ext4_group_desc`（`ext4.h:391`）：

```c
/* fs/ext4/ext4.h:391，64bit 描述符后半是高位，这里保留源码注释 */
struct ext4_group_desc
{
	__le32	bg_block_bitmap_lo;	/* Blocks bitmap block */
	__le32	bg_inode_bitmap_lo;	/* Inodes bitmap block */
	__le32	bg_inode_table_lo;	/* Inodes table block */
	__le16	bg_free_blocks_count_lo;/* Free blocks count */
	__le16	bg_free_inodes_count_lo;/* Free inodes count */
	__le16	bg_used_dirs_count_lo;	/* Directories count */
	__le16	bg_flags;		/* EXT4_BG_flags (INODE_UNINIT, etc) */
	/* ... 高位与校验和到 bg_reserved，:415 */
};
```

块位图的一位对应组里的一个块，inode 位图的一位对应 inode 表里的一个 inode。空闲计数在描述符里，分配时要改位图也要改计数。多组描述符可以叠成 flex group（`struct flex_groups`，`ext4.h:429`），那是分配时的汇总，不改变「位图 + inode 表」这一组的布局。

---

## 2.2 ext4：inode 与 extent 树

盘上 inode 是 `struct ext4_inode`（`ext4.h:767`）。`i_block[EXT4_N_BLOCKS]` 占 60 字节。`EXT4_NDIR_BLOCKS` 是 12，后面是一级、二级、三级间接（`ext4.h:461`–`:465`），所以 `EXT4_N_BLOCKS` 是 15。

inode 标志 `EXT4_INODE_EXTENTS` 的值是 19（`ext4.h:591`）。这个特性对应 `EXT4_FEATURE_INCOMPAT_EXTENTS`（`0x0040`，`ext4.h:2047`）。标志置上时，这 60 字节的前 12 字节是 `ext4_extent_header`，剩下是 `ext4_extent` 数组。`fs/ext4/ext4_extents.h:40` 的注释写了这件事。

```c
/* fs/ext4/ext4_extents.h:63 叶子 */
struct ext4_extent {
	__le32	ee_block;	/* first logical block extent covers */
	__le16	ee_len;		/* number of blocks covered by extent */
	__le16	ee_start_hi;	/* high 16 bits of physical block */
	__le32	ee_start_lo;	/* low 32 bits of physical block */
};

/* :74 索引节点，指向下一层 */
struct ext4_extent_idx {
	__le32	ei_block;	/* index covers logical blocks from 'block' */
	__le32	ei_leaf_lo;
	__le16	ei_leaf_hi;
	__u16	ei_unused;
};

/* :85 每层都有 */
struct ext4_extent_header {
	__le16	eh_magic;	/* probably will support different formats */
	__le16	eh_entries;	/* number of valid entries */
	__le16	eh_max;		/* capacity of store in entries */
	__le16	eh_depth;	/* has tree real underlying blocks? */
	__le32	eh_generation;	/* generation of the tree */
};
```

魔数 `EXT4_EXT_MAGIC` 是 `0xf30a`（`:93`）。`eh_depth == 0` 时下一层就是叶子 extent；非 0 时是索引。树深上限 `EXT4_MAX_EXTENT_DEPTH` 是 5（`:94`）。

`ee_len` 的最高位表示 unwritten。`ext4_extents.h:140` 起的注释：已初始化 extent 最长 `EXT_INIT_MAX_LEN`（`1 << 15`），unwritten 最长 `EXT_UNWRITTEN_MAX_LEN`（那个值减 1）。`ee_len == 0x8000` 被当成已初始化的特例，避免出现长度为 0 的 unwritten。转换时机在 [08](./08-delalloc-unwritten.md)。

没有 extents 标志的 inode 仍走间接块。`ext4_set_iomap` 见到已映射且没有 extents 标志时给 iomap 加上 `IOMAP_F_MERGED`（`fs/ext4/inode.c:3302`）。FIEMAP 会把这个标志变成 `FIEMAP_EXTENT_MERGED`（`fs/iomap/fiemap.c:37`）。

---

## 2.3 XFS：超级块和分配组

盘上超级块是 `struct xfs_dsb`（`fs/xfs/libxfs/xfs_format.h:187`），魔数 `XFS_SB_MAGIC` 写在字段注释里。和布局直接相关的是：

```c
/* fs/xfs/libxfs/xfs_format.h:187，摘字段。内存里的 xfs_sb 在 :96，字节序不同 */
struct xfs_dsb {
	__be32		sb_magicnum;	/* magic number == XFS_SB_MAGIC */
	__be32		sb_blocksize;	/* logical block size, bytes */
	__be64		sb_dblocks;	/* number of data blocks */
	__be64		sb_rblocks;	/* number of realtime blocks */
	/* sb_uuid、内部日志起点 sb_logstart、根 inode sb_rootino ... */
	__be32		sb_agblocks;	/* size of an allocation group */
	__be32		sb_agcount;	/* number of allocation groups */
	__be32		sb_logblocks;	/* number of log blocks */
	/* ... */
};
```

分配组（allocation group）是一段连续的 `sb_agblocks` 个块。`xfs_format.h:465` 的注释：组头分成三块，放在该组超级块副本之后的连续 512 字节缓冲里。

- `xfs_agf`（`:491` 起的 typedef）魔数 `XFS_AGF_MAGIC`（`'XAGF'`，`:470`）。`agf_roots[]` / `agf_levels[]` 指向空闲空间 B+tree。注释（`:480`）写 btree 0 是按块号（bno），1 是按空闲长度（cnt），2 是 rmap（可选）。
- `xfs_agi`（`:587`）魔数 `XFS_AGI_MAGIC`（`'XAGI'`），记 inode 的分配 B+tree。
- AGFL 是空闲列表，魔数 `XFS_AGFL_MAGIC`（`'XAFL'`，`:472`）。

空闲空间因此是树，不是 ext4 那种「一组一个位图块」的单层位图。位图仍然存在于 btree 叶子附近的实现里；本章停在「AGF 指向 bno/cnt 两棵树」。

---

## 2.4 XFS：dinode 与 bmap 记录

盘上 inode 是 `struct xfs_dinode`（`xfs_format.h:792`），魔数 `XFS_DINODE_MAGIC` 是 `0x494e`（`'IN'`，`:791`）。`di_format`（`:796`）决定数据 fork 的形状。枚举在 `:891`：

```c
enum xfs_dinode_fmt {
	XFS_DINODE_FMT_DEV,		/* xfs_dev_t */
	XFS_DINODE_FMT_LOCAL,		/* bulk data */
	XFS_DINODE_FMT_EXTENTS,		/* struct xfs_bmbt_rec */
	XFS_DINODE_FMT_BTREE,		/* struct xfs_bmdr_block */
	XFS_DINODE_FMT_UUID		/* added long ago, but never used */
};
```

extent 放得进 inode 时用 `XFS_DINODE_FMT_EXTENTS`。放不下就变成 B+tree，根在 inode 里，格式是 `XFS_DINODE_FMT_BTREE`。叶子记录是两个大端 64 位：

```c
/* fs/xfs/libxfs/xfs_format.h:1676
 * 文件偏移字段宽 54 bit，见 :1671 的注释。
 */
typedef struct xfs_bmbt_rec {
	__be64			l0, l1;
} xfs_bmbt_rec_t;
```

延迟分配在内存里用特殊的 startblock（`HOLESTARTBLOCK` / `DELAYSTARTBLOCK`），写到盘上之前不是一条普通的已分配记录。`xfs_bmbt_to_iomap`（`fs/xfs/xfs_iomap.c:91`）把 `DELAYSTARTBLOCK` 或 null startblock 译成 `IOMAP_DELALLOC`（`:108`），把 `XFS_EXT_UNWRITTEN` 译成 `IOMAP_UNWRITTEN`（`:117`）。

实时卷（`sb_rblocks`）和内部 / 外部日志（`sb_logstart`、`sb_logblocks`）只在 fsync 要决定刷哪块设备时用到，见 [11](./11-fsync-crash.md)。

---

## 2.5 对照

| 问题 | ext4 | XFS |
|---|---|---|
| 整盘头 | `ext4_super_block` | `xfs_dsb` |
| 空间划分 | 块组，描述符指向两张位图和 inode 表 | 分配组，AGF/AGI 指向 B+tree |
| 文件的块映射 | `i_block[]`：间接块，或 extent 树 | dinode fork：extent 数组或 bmap B+tree |
| 未写入的预留 | extent 的 `ee_len` 最高位 | bmap 记录的 unwritten 状态 |
| 延迟分配记在哪 | 内存里的 extent status 与 `BH_Delay`；盘上还没有物理块 | 内存里的 delalloc startblock；盘上还没有普通分配记录 |

---

## 2.6 关键符号

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/ext4/ext4.h` | `struct ext4_super_block` | 1300 | 盘上超级块 |
| `fs/ext4/ext4.h` | `struct ext4_group_desc` | 391 | 块组：位图与 inode 表 |
| `fs/ext4/ext4.h` | `struct ext4_inode` | 767 | 盘上 inode，含 `i_block[]` |
| `fs/ext4/ext4.h` | `EXT4_NDIR_BLOCKS` | 461 | 间接映射的 12 个直接块 |
| `fs/ext4/ext4_extents.h` | `struct ext4_extent` | 63 | 叶子 extent |
| `fs/ext4/ext4_extents.h` | `EXT_INIT_MAX_LEN` | 156 | 已初始化 extent 的长度上限 |
| `fs/xfs/libxfs/xfs_format.h` | `struct xfs_dsb` | 187 | 盘上超级块 |
| `fs/xfs/libxfs/xfs_format.h` | `xfs_agf` | 491 | 分配组空闲空间头 |
| `fs/xfs/libxfs/xfs_format.h` | `struct xfs_dinode` | 792 | 盘上 inode |
| `fs/xfs/libxfs/xfs_format.h` | `xfs_bmbt_rec_t` | 1676 | bmap 叶子记录 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [buffer_head →](./03-buffer-head.md)
