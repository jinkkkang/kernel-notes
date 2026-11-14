# 6. 回写

**结论**：flusher 调用 `mapping->a_ops->writepages`。XFS 把它转给 `iomap_writepages`：按 folio 上的脏块问文件系统要映射，连续且类型相同的范围收进一个 `struct iomap_ioend`，bio 挂在 `ioend->io_bio` 上，最后 `submit_bio`。ext4 的 `writepages` 是 `ext4_writepages`，不调用 `iomap_writepages`。

> flusher 何时醒来、`struct writeback_control` 的 `sync_mode`，在 [MM §08](../memory-management/08-page-cache-and-writeback.md) 和 [block-io/10](../block-io/10-writeback-and-fsync.md) §10.1。本章从 `writepages` 的函数体写到 `submit_bio`。bio 进请求队列之后不再展开。

---

## 6.1 从 flusher 到两套 writepages

```text
wb_writeback
  mapping->a_ops->writepages(mapping, wbc)

XFS：
  xfs_vm_writepages                 fs/xfs/xfs_aops.c:473
    iomap_writepages                :480
      ops = xfs_writeback_ops

ext4：
  ext4_writepages                   fs/ext4/inode.c:2806
    ext4_do_writepages              :2565
```

`xfs_address_space_operations.writepages` 是 `xfs_vm_writepages`（`xfs_aops.c:547`）。`ext4_da_aops` / `ext4_aops` / `ext4_journalled_aops` 的 `writepages` 都是 `ext4_writepages`（`inode.c:3612`、`:3629`、`:3646`）。DAX 的表换成 `ext4_dax_writepages` / `xfs_dax_writepages`，本章不展开。

---

## 6.2 `iomap_writepage_ctx` 和 `iomap_ioend`

```c
/* include/linux/iomap.h:311 */
struct iomap_ioend {
	struct list_head	io_list;	/* next ioend in chain */
	u16			io_type;
	u16			io_flags;	/* IOMAP_F_* */
	u32			io_folios;	/* folios added to ioend */
	struct inode		*io_inode;	/* file being written to */
	size_t			io_size;	/* size of the extent */
	loff_t			io_offset;	/* offset in the file */
	sector_t		io_sector;	/* start sector of ioend */
	struct bio		*io_bio;	/* bio being built */
	struct bio		io_inline_bio;	/* MUST BE LAST! */
};

/* :347 */
struct iomap_writepage_ctx {
	struct iomap		iomap;
	struct iomap_ioend	*ioend;
	const struct iomap_writeback_ops *ops;
};
```

`io_inline_bio` 必须是最后一个成员：`iomap_alloc_ioend` 用 `container_of(bio, struct iomap_ioend, io_inline_bio)` 把 bio 和 ioend 放在同一次分配里（`buffered-io.c:1710`）。

文件系统提供的回写钩子（`iomap.h:324`）：

```c
struct iomap_writeback_ops {
	int (*map_blocks)(struct iomap_writepage_ctx *wpc, struct inode *inode,
				loff_t offset);
	int (*prepare_ioend)(struct iomap_ioend *ioend, int status);
	void (*discard_folio)(struct folio *folio, loff_t pos);
};
```

`map_blocks` 必填，把 `offset` 起的范围填进 `wpc->iomap`。`prepare_ioend` 可选，在提交前改 bio 或换 `bi_end_io`（注释举的例子是 COW 和 unwritten 转换）。`discard_folio` 在这一页没能提交任何 IO 时丢掉文件系统状态。

XFS 的表（`xfs_aops.c:466`）：`map_blocks = xfs_map_blocks`，`prepare_ioend = xfs_prepare_ioend`，`discard_folio = xfs_discard_folio`。

---

## 6.3 `iomap_writepages` 怎样组 bio

```text
iomap_writepages                    fs/iomap/buffered-io.c:2036
  write_cache_pages(..., iomap_do_writepage, wpc)
  若 wpc->ioend 还在：
    iomap_submit_ioend              :2046     提交最后一个 ioend
```

`iomap_writepage_map`（`:1815`）处理一个 folio。注释（`:1799`）说明提交策略：不把多个 ioend 串成一条链再嵌套 mempool，当前正在填的 ioend 缓存在 `wpc` 里；新块接不上去就换一个新 ioend，旧的立刻排队提交。块层的 plug 负责合并。

接得上的条件在 `iomap_can_add_to_ioend`（`:1746`）：`IOMAP_F_SHARED` 一致、`iomap.type` 一致、文件偏移连续、磁盘扇区连续。类型从 `IOMAP_MAPPED` 变成 `IOMAP_UNWRITTEN` 时必须换 ioend，因为完成时的转换只对 unwritten 那段做。

```text
iomap_alloc_ioend                   :1698
  bio_alloc_bioset(..., REQ_OP_WRITE | wbc_to_write_flags(wbc), ...)
  ioend->io_type = wpc->iomap.type
  ioend->io_bio = bio

iomap_submit_ioend                  :1673
  bi_private = ioend
  bi_end_io = iomap_writepage_end_bio
  若有 prepare_ioend：调用它，可能改掉 bi_end_io
  error：bi_status = errno_to_blk_status，bio_endio，不再交给驱动
  否则 submit_bio(ioend->io_bio)    :1693
```

bio 装不下时 `iomap_chain_bio`（`:1731`）再分配一个，`bio_chain` 后把前一个 `submit_bio`。

---

## 6.4 完成：清 writeback，必要时转换 extent

`iomap_writepage_end_bio`（`:1657`）把 `bio->bi_status` 转成 errno，调用 `iomap_finish_ioend`（`:1517`）。那里对 bio 里每个 folio 调 `iomap_finish_folio_write`：`write_bytes_pending` 减到 0 就 `folio_end_writeback`（`:1508`）。

XFS 在 `xfs_prepare_ioend`（`xfs_aops.c:397`）里，如果 ioend 是文件末尾追加、类型是 `IOMAP_UNWRITTEN`、或带 `IOMAP_F_SHARED`，把 `bi_end_io` 换成 `xfs_end_bio`（`:419`）。`xfs_end_bio`（`:179`）把 ioend 挂到 inode 上，并 `queue_work` 到 `m_unwritten_workqueue`。工作项 `xfs_end_io`（`:155`）调用 `xfs_end_ioend`（`:83`）：成功且类型是 unwritten 时 `xfs_iomap_write_unwritten`（`:131`），然后 `iomap_finish_ioends`。失败的共享 ioend 会 `xfs_reflink_cancel_cow_range` 并打掉 delalloc（`:117`）。

ext4 缓冲回写的完成在自己的 `ext4_io_end` 上。`ext4_end_io_end`（`fs/ext4/page-io.c:174`）在写回成功时调用 `ext4_convert_unwritten_io_end_vec`（`:194`）。注释（`:185`）写明写失败就不转换，避免露出未初始化数据。这条链不经过 `iomap_finish_ioend`。

---

## 6.5 ext4 的 `ext4_writepages` 停在哪

`ext4_do_writepages`（`inode.c:2565`）打 `trace_ext4_writepages`，按 `writeback_control` 挑脏页。延迟分配的范围在 `mpage_map_one_extent`（`:2173`）里调用 `ext4_map_blocks`（`:2205`，函数定义 `:511`）才分配物理块。它使用 ext4 自己的 mpage / io_end，而不是 `iomap_writepage_ctx`。

因此 [block-io/10](../block-io/10-writeback-and-fsync.md) 里「iomap 回写」那一节描述的是 `iomap_writepages` 这条通用路径（行号与本章核对一致：`iomap_writepages` 在 `buffered-io.c:2036`）。ext4 缓冲脏页走的是 `ext4_writepages`。

---

## 6.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/iomap.h` | `struct iomap_ioend` | 311 | 一段连续回写及其 bio |
| `include/linux/iomap.h` | `struct iomap_writepage_ctx` | 347 | 当前映射和当前 ioend |
| `include/linux/iomap.h` | `struct iomap_writeback_ops` | 324 | `map_blocks` / `prepare_ioend` |
| `fs/iomap/buffered-io.c` | `iomap_writepage_map` | 1815 | 一个 folio 填进 ioend |
| `fs/iomap/buffered-io.c` | `iomap_submit_ioend` | 1673 | `submit_bio` 或失败时 `bio_endio` |
| `fs/iomap/buffered-io.c` | `iomap_writepages` | 2036 | 回写入口 |
| `fs/iomap/buffered-io.c` | `iomap_finish_ioend` | 1517 | 完成时结束 folio writeback |
| `fs/xfs/xfs_aops.c` | `xfs_vm_writepages` | 473 | XFS 转到 `iomap_writepages` |
| `fs/xfs/xfs_aops.c` | `xfs_prepare_ioend` | 397 | unwritten / COW 换完成函数 |
| `fs/ext4/inode.c` | `ext4_writepages` | 2806 | ext4 自己的回写 |
| `fs/ext4/page-io.c` | `ext4_end_io_end` | 174 | ext4 回写完成后转换 unwritten |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 缓冲读写](./05-buffered-io.md) · [目录](./README.md) · [Direct IO →](./07-direct-io.md)
