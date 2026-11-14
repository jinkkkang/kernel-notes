# 3. buffer_head 老路

**结论**：`buffer_head` 把一页里的每个块记成一条带状态位的记录。缓冲写通过 `block_write_begin` 拿到页，再对每个缺映射的块调用 `get_block_t`。ext4 的缓冲写在 6.6 仍走这条路；iomap 把「按块回调」换成「一次返回一段映射」，通用代码自己在 folio 上记子块的 uptodate / dirty。

> 头文件自己的说明在 `include/linux/buffer_head.h:50`：今天的 IO 单位是 bio，`buffer_head` 留下来做映射提取、页内状态和 `submit_bh` 的兼容包装。

---

## 3.1 `struct buffer_head`

```c
/* include/linux/buffer_head.h:59
 * 状态位在 enum bh_state_bits（:19）：Uptodate、Dirty、Mapped、Delay、Unwritten 等。
 */
struct buffer_head {
	unsigned long b_state;		/* buffer state bitmap (see above) */
	struct buffer_head *b_this_page;/* circular list of page's buffers */
	union {
		struct page *b_page;	/* the page this bh is mapped to */
		struct folio *b_folio;	/* the folio this bh is mapped to */
	};

	sector_t b_blocknr;		/* start block number */
	size_t b_size;			/* size of mapping */
	char *b_data;			/* pointer to data within the page */

	struct block_device *b_bdev;
	bh_end_io_t *b_end_io;		/* I/O completion */
	void *b_private;		/* reserved for b_end_io */
	struct list_head b_assoc_buffers; /* associated with another mapping */
	struct address_space *b_assoc_map;
	atomic_t b_count;		/* users using this buffer_head */
	spinlock_t b_uptodate_lock;	/* Used by the first bh in a page, to
					 * serialise IO completion of other
					 * buffers in the page */
};
```

一页上的 bh 用 `b_this_page` 串成环。块大小小于页时，一页有多条 bh。`BH_Delay`（`:29`）表示还没在盘上分配；`BH_Unwritten`（`:32`）表示盘上有块但数据还没写完。ext4 延迟分配的 `get_block` 会同时置 `BH_Mapped`、`BH_New`、`BH_Delay`，并且 `b_blocknr = -1`。这是 `ext4_da_get_block_prep` 上面的注释（`fs/ext4/inode.c:1830`）。

`set_buffer_dirty` 这一类函数由 `BUFFER_FNS` 宏展开（`buffer_head.h:89`）。

---

## 3.2 `block_write_begin` 的调用链

```text
ext4_buffered_write_iter          fs/ext4/file.c:285
  generic_perform_write           :306
    a_ops->write_begin
      ext4_da_write_begin         fs/ext4/inode.c:2907   默认开了 DELALLOC
        或 ext4_write_begin       :1155                  空间紧或 data=journal 等
          __block_write_begin     fs/buffer.c:2170
            get_block_t           include/linux/fs.h:90
      ext4_da_write_end / ext4_write_end
        block_write_end           fs/buffer.c:2246
```

`ext4_set_aops`（`inode.c:3668`）在 ordered / writeback 且挂载带 `DELALLOC`（`EXT4_MOUNT_DELALLOC`，`ext4.h:1225`）时把 `a_ops` 设成 `ext4_da_aops`。这张表的 `write_begin` 是 `ext4_da_write_begin`（`:3647`），`write_end` 是 `ext4_da_write_end`。没开 delalloc 时用 `ext4_aops`，`write_begin` 是 `ext4_write_begin`（`:3613`）。data=journal 用 `ext4_journalled_aops`，`write_begin` 仍是 `ext4_write_begin`，`write_end` 换成 `ext4_journalled_write_end`（`:3630`）。

`block_write_begin`（`fs/buffer.c:2223`）的注释写明两件事：做块分配，并把部分写的块先读成 uptodate。文件系统要自己处理失败时的截断。

```text
block_write_begin                 fs/buffer.c:2223
  grab_cache_page_write_begin     按页索引拿页并锁住
  __block_write_begin             :2170
    __block_write_begin_int       按块调用 get_block，必要时读旧数据
```

ext4 有自己的 `ext4_block_write_begin`（`inode.c:1052`）。`ext4_da_write_begin` 在加密等条件下走它，否则走 `__block_write_begin(&folio->page, pos, len, ext4_da_get_block_prep)`（`inode.c:2945` 附近）。`get_block` 参数就是 `ext4_da_get_block_prep`（`:1842`）。

写完后 `block_write_end`（`buffer.c:2246`）走到 `__block_commit_write`（`:2178`）：写到的整块 `set_buffer_uptodate` 并 `mark_buffer_dirty`；这一页上每个块都 uptodate 时 `folio_mark_uptodate`。

缓冲读对称：`ext4_read_folio`（`inode.c:3179`）在没有 inline data 时调用 `ext4_mpage_readpages`（`:3190`）。预读 `ext4_readahead`（`:3195`）也进同一个函数。这条路径在 `fs/ext4/inode.c` 里，调用的是 ext4 的 mpage 读，函数名是 `ext4_mpage_readpages`。通用的 `block_read_full_folio` 在 `fs/buffer.c:2369`，签名带 `get_block_t`。

---

## 3.3 为什么新代码改走 iomap

`buffer_head.h:50` 已经把 bio 写成基本 IO 单位。bh 路径还要为每个块做一次 `get_block_t`，状态位挂在 bh 上，一页一个环。大 folio 上块数变多，这条环和回调次数一起变长。

iomap 让文件系统在 `iomap_begin` 里返回一整段：文件偏移、长度、磁盘字节地址、类型。通用代码在 `struct iomap_folio_state` 的位图里记子块 uptodate / dirty（`fs/iomap/buffered-io.c:31`），不再要求每个块一条 bh。回写时按这段映射组一个 `iomap_ioend`，而不是每块问一次 `get_block`。模型在 [04](./04-iomap-model.md)，缓冲写在 [05](./05-buffered-io.md)。

6.6 的 ext4 缓冲写没有切过去。`iomap_file_buffered_write` 在已核对的 ext4 源文件里没有调用点；XFS 在 `xfs_file.c:779` 调用它。ext4 仍把 iomap 用在 DIO、fiemap、seek、bmap、swap，清单在 [09](./09-ext4-jbd2.md)。

bh 和 iomap 在一个边界上还会相遇：`iomap_write_begin` 看见 `IOMAP_F_BUFFER_HEAD` 时调用 `__block_write_begin_int`（`buffered-io.c:811`）。这是「文件系统要求这段映射仍用 bh」的标志，定义在 `include/linux/iomap.h:62`。ext4 的 `ext4_set_iomap` 摘录里没有置这个标志；ext4 缓冲写根本不进 `iomap_write_begin`。

---

## 3.4 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/fs.h` | `get_block_t` | 90 | 一块映射的回调类型 |
| `include/linux/buffer_head.h` | `struct buffer_head` | 59 | 页内一块的状态 |
| `include/linux/buffer_head.h` | `enum bh_state_bits` | 19 | `BH_Delay`、`BH_Unwritten` 等 |
| `fs/buffer.c` | `__block_write_begin` | 2170 | 按块 `get_block`，补齐部分写 |
| `fs/buffer.c` | `block_write_begin` | 2223 | 抓页再调用上面的函数 |
| `fs/buffer.c` | `block_write_end` | 2246 | 标 uptodate / dirty |
| `fs/buffer.c` | `block_read_full_folio` | 2369 | 通用的按块读 |
| `fs/ext4/inode.c` | `ext4_da_get_block_prep` | 1842 | ext4 延迟分配的 `get_block_t` |
| `fs/ext4/inode.c` | `ext4_da_write_begin` | 2907 | delalloc 缓冲写的 `write_begin` |
| `fs/ext4/inode.c` | `ext4_write_begin` | 1155 | 非 delalloc / journal 数据的 `write_begin` |
| `fs/ext4/file.c` | `ext4_buffered_write_iter` | 285 | 缓冲写入口，调 `generic_perform_write` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← 盘上布局](./02-ondisk-layout.md) · [目录](./README.md) · [iomap 模型 →](./04-iomap-model.md)
