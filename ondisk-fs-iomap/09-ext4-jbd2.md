# 9. ext4 与 jbd2

**结论**：ext4 的元数据修改包在 jbd2 的 `handle_t` 里，多个 handle 并进一个 `transaction_t`，由 `kjournald2` 按「先数据、再元数据、再 commit 记录」提交；commit 之后旧事务还要 checkpoint 回原地，日志才能复用。6.6 的 ext4 缓冲读写走自己的 bh / `ext4_writepages`；DIO、fiemap、SEEK_HOLE/DATA、bmap、swap 激活走 iomap。

> 块层怎样拆 `REQ_PREFLUSH` / `REQ_FUA` 见 [block-io/10](../block-io/10-writeback-and-fsync.md) §10.4。本章写 jbd2 何时给 commit 记录加上这两个标志。

---

## 9.1 哪些路径用 iomap

下面都是 v6.6.160 里能对上的调用点。

**调用了 iomap：**

| 路径 | 调用 | 位置 |
|---|---|---|
| DIO 读 | `iomap_dio_rw(..., &ext4_iomap_ops, ...)` | `fs/ext4/file.c:94` |
| DIO 写 | `iomap_dio_rw`，ops 是 `ext4_iomap_ops` 或 `ext4_iomap_overwrite_ops` | `file.c:584` |
| DAX 读写 | `dax_iomap_rw(..., &ext4_iomap_ops)` | `file.c:122`、`:676` |
| DAX fault | `dax_iomap_fault(..., &ext4_iomap_ops)` | `file.c:749` |
| SEEK_HOLE / SEEK_DATA | `iomap_seek_hole` / `iomap_seek_data`，ops 是 `ext4_iomap_report_ops` | `file.c:916`、`:922` |
| FIEMAP | `inode_operations.fiemap = ext4_fiemap` → `iomap_fiemap` | `file.c:959`，`extents.c:5099` |
| `bmap` | `iomap_bmap(..., &ext4_iomap_ops)` | `inode.c:3172` |
| swap 激活 | `iomap_swapfile_activate(..., &ext4_iomap_report_ops)` | `inode.c:3605` |

`a_ops->direct_IO` 在四张表里都是 `noop_direct_IO`（例如 `inode.c:3619`）。open 靠它放行 `O_DIRECT`，字节不经过这个函数指针。

**缓冲数据路径用 bh 和 ext4 自己的回写：**

| 路径 | 函数 | 位置 |
|---|---|---|
| 缓冲写 | `ext4_buffered_write_iter` → `generic_perform_write` | `file.c:285`、`:306` |
| `write_begin` | `ext4_da_write_begin` 或 `ext4_write_begin`，内部 `__block_write_begin` | `inode.c:2907`、`:1155` |
| 缓冲读 / 预读 | `ext4_read_folio` / `ext4_readahead` → `ext4_mpage_readpages` | `inode.c:3179`、`:3195` |
| 回写 | `ext4_writepages` → `ext4_do_writepages` | `inode.c:2806`、`:2565` |
| 标脏 | `ext4_dirty_folio` → `block_dirty_folio`，要求 folio 上已有 bh | `inode.c:3595` |

`ext4_iomap_alloc` 里的 `WARN_ON(!(flags & (IOMAP_DAX | IOMAP_DIRECT)))`（`inode.c:3361`）把「带 `IOMAP_WRITE` 的 ext4 iomap」限制在 DAX 和 Direct IO。缓冲写不会带着 `IOMAP_WRITE` 进来。

`ext4_file_write_iter`（`file.c:691`）的分叉：DAX → `ext4_dax_write_iter`；`IOCB_DIRECT` → `ext4_dio_write_iter`；其余 → `ext4_buffered_write_iter`。

---

## 9.2 handle 和 transaction

`handle_t` 是一次原子更新（`include/linux/jbd2.h:75` 的 typedef 注释）。具体类型是 `struct jbd2_journal_handle`（`:477`），typedef 在 `:91`。

```c
/* include/linux/jbd2.h:477，摘信用和状态位 */
struct jbd2_journal_handle
{
	union {
		transaction_t	*h_transaction;
		journal_t	*h_journal;   /* h_reserved 时用 */
	};
	handle_t		*h_rsv_handle;
	int			h_total_credits;   /* 还能再改多少缓冲区 */
	int			h_revoke_credits;
	int			h_ref;
	int			h_err;
	unsigned int	h_sync:		1;     /* stop 时要等提交 */
	unsigned int	h_jdata:	1;     /* 强制数据进日志 */
	unsigned int	h_reserved:	1;
	unsigned int	h_aborted:	1;
	/* h_type / h_line_no 给统计 */
};
```

`transaction_t` 的 typedef 在 `include/linux/journal-head.h:17`：`typedef struct transaction_s transaction_t`。结构体在 `jbd2.h:551`。注释（`:517`）列出它要经过的阶段：接受新 handle（RUNNING）、不再接受（LOCKED）、数据还在写（FLUSH）、写 commit 记录（COMMIT）、提交完但仍要留着做 checkpoint（FINISHED）。源码里的 `t_state` 更细（`:567`）：

```c
enum {
	T_RUNNING,
	T_LOCKED,
	T_SWITCH,
	T_FLUSH,
	T_COMMIT,
	T_COMMIT_DFLUSH,
	T_COMMIT_JFLUSH,
	T_COMMIT_CALLBACK,
	T_FINISHED
} t_state;
```

事务挂着几条 `journal_head` 环：还没改的 `t_reserved_list`、元数据 `t_buffers`、可在提交后丢掉 checkpoint 的 `t_forget`、checkpoint 前仍要写回原地的 `t_checkpoint_list`（`:595`–`:614`）。`t_inode_list`（`:631`）给 ordered / journal 数据模式，提交时要特殊处理这些 inode 的数据。

`journal_t` 是 `struct journal_s`（`jbd2.h:750`）。和提交有关的三个指针：`j_running_transaction`（`:811`）、`j_committing_transaction`（`:819`）、`j_checkpoint_transactions`（`:827`）。

```text
jbd2_journal_start(journal, nblocks)     fs/jbd2/transaction.c:542
  jbd2__journal_start                      保证日志里还能放下 nblocks 个缓冲区
ext4 侧的包装：
  ext4_journal_start                       fs/ext4/ext4_jbd2.h:309 的宏

改元数据之前：
  jbd2_journal_get_write_access            声明在 jbd2.h:1538
  jbd2_journal_dirty_metadata              :1543

jbd2_journal_stop(handle)                  transaction.c:1825
  h_ref 还没到 0：只减引用
  h_sync：可能叫醒提交并等待
```

`jbd2_journal_start` 的注释（`:524`）写明：调用会堵住，直到日志能保证这 `nblocks` 个缓冲区的空间。没用完的信用在 `jbd2_journal_stop` 还回事务。

---

## 9.3 提交：kjournald2

`kjournald2`（`fs/jbd2/journal.c:160`）的注释写两项工作：按间隔 COMMIT，以及 CHECKPOINT 以便复用日志。默认最长提交间隔 `JBD2_DEFAULT_MAX_COMMIT_AGE` 是 5 秒（`jbd2.h:48`）。线程函数里调用 `jbd2_journal_commit_transaction`（`journal.c:201`）。

`jbd2_journal_commit_transaction`（`fs/jbd2/commit.c:348`）里的调试字符串把阶段标成：

```text
phase 1     commit.c:507
  清 revoke 标志，换 revoke 表
phase 2a    :540
  t_state = T_FLUSH                         :532
  journal_submit_data_buffers               :546
    注释：Data blocks go first
phase 2b    :553
  写 revoke 记录，再写元数据缓冲区
phase 3     :808
  等这些 IO
随后
  journal_finish_inode_data_buffers         :743
  日志和文件系统不在同一设备且需要数据屏障：
    blkdev_issue_flush(journal->j_fs_dev)   :785
  journal_submit_commit_record              :114
```

commit 记录在 `journal_submit_commit_record`（`:114`）。`JBD2_BARRIER` 置位且没有 async commit 特性时，写标志加上 `REQ_PREFLUSH | REQ_FUA`（`:152`）。有 async commit 时这条记录可以先提交，等 IO 的函数是 `journal_wait_on_commit_record`（`:165`）。`data=ordered` 和 `journal_async_commit` 不能一起挂载，`ext4` 在 `super.c:4982` 直接拒绝。

tracepoint 和这些阶段同名：`jbd2_commit_flushing`、`jbd2_commit_logging` 等，见 [12](./12-observability.md)。

---

## 9.4 checkpoint

提交只保证「日志里有一份描述，commit 记录已落盘」。元数据缓冲区的原地副本可能还是旧的。`t_checkpoint_list` 上的缓冲区写回原地之后，日志里对应区段才能复用。这是 `kjournald2` 注释里的第二项（`journal.c:154`）。

`jbd2_log_do_checkpoint`（`fs/jbd2/checkpoint.c:154`）在 `j_checkpoint_mutex` 下挑一个仍需 checkpoint 的事务，把缓冲区写出去。提交路径也会先调用 `__jbd2_journal_clean_checkpoint_list`（`commit.c:504`），丢掉已经写回的头。

checkpoint 完成之前，日志空间还被这个事务占着。`jbd2_journal_start` 等的就是这份空间。

---

## 9.5 data=journal / ordered / writeback

挂载参数表 `ext4_param_data`（`fs/ext4/super.c:1700`）：

| 字符串 | 标志 | 值 | 注释（`ext4.h:1201`） |
|---|---|---|---|
| `journal` | `EXT4_MOUNT_JOURNAL_DATA` | `0x00400` | Write data to journal |
| `ordered` | `EXT4_MOUNT_ORDERED_DATA` | `0x00800` | Flush data before commit |
| `writeback` | `EXT4_MOUNT_WRITEBACK_DATA` | `0x00C00` | No data ordering |

三个值都落在 `EXT4_MOUNT_DATA_FLAGS`（`0x00C00`）里。没指定时，日志支持 revoke 就默认 ordered（`super.c:4961`），否则 journal data（`:4964`）。

每个 inode 的实际模式由 `ext4_inode_journal_mode`（`fs/ext4/ext4_jbd2.c:10`）算出来，返回值是：

```c
/* fs/ext4/ext4_jbd2.h:457 */
#define EXT4_INODE_JOURNAL_DATA_MODE	0x01
#define EXT4_INODE_ORDERED_DATA_MODE	0x02
#define EXT4_INODE_WRITEBACK_DATA_MODE	0x04
```

没有日志的文件系统直接当 writeback（`ext4_jbd2.c:13`）。`data=journal`、或 inode 带 journal-data 标志且没开 delalloc 时走 journal data；加密的普通文件在这条上改回 ordered（`:21`）。注释（`:14`）写明延迟分配和数据进日志不能一起用。然后才看挂载是 ordered 还是 writeback（`:25`）。

三种模式对一次缓冲写的差别：

- **journal**：数据缓冲区也 `jbd2_journal_dirty_metadata` 进事务。`a_ops` 是 `ext4_journalled_aops`，`write_end` 是 `ext4_journalled_write_end`。提交时数据在日志里，commit 记录之后才算可见。
- **ordered**：数据不进日志，但提交要先把这些数据缓冲区写出去。`journal_submit_data_buffers`（`commit.c:546`）在元数据之前。ext4 把回调安在 `j_submit_inode_data_buffers` / `j_finish_inode_data_buffers`（`super.c:4991`）。`struct jbd2_inode` 的注释（`jbd2.h:394`）说明它就是用来在 ordered 模式的提交里同步 inode 数据的。`JI_WRITE_DATA` / `JI_WAIT_DATA`（`:389`、`:391`）区分「提交前写出脏块」和「等已经发出的数据写」。
- **writeback**：元数据进日志，数据由普通回写带走，顺序不保证。崩溃后可能看到新的 inode 大小配上旧数据。

ordered 是默认。它保证的是「commit 记录可见时，这个事务关联的数据写已经提交过」，不是「每次 `write(2)` 返回时数据已落盘」。落盘要等 [11](./11-fsync-crash.md) 的 fsync。

---

## 9.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/jbd2.h` | `handle_t` | 91 | 一次原子更新 |
| `include/linux/jbd2.h` | `struct jbd2_journal_handle` | 477 | handle 的字段 |
| `include/linux/journal-head.h` | `transaction_t` | 17 | 事务 typedef |
| `include/linux/jbd2.h` | `struct transaction_s` | 551 | 事务本体 |
| `include/linux/jbd2.h` | `struct journal_s` | 750 | 日志 |
| `fs/jbd2/transaction.c` | `jbd2_journal_start` | 542 | 拿到 handle |
| `fs/jbd2/transaction.c` | `jbd2_journal_stop` | 1825 | 还信用，必要时等提交 |
| `fs/jbd2/journal.c` | `kjournald2` | 160 | 提交与 checkpoint 线程 |
| `fs/jbd2/commit.c` | `jbd2_journal_commit_transaction` | 348 | 一次提交 |
| `fs/jbd2/commit.c` | `journal_submit_commit_record` | 114 | commit 记录，可带 FUA |
| `fs/jbd2/checkpoint.c` | `jbd2_log_do_checkpoint` | 154 | 把旧事务写回原地 |
| `fs/ext4/ext4_jbd2.c` | `ext4_inode_journal_mode` | 10 | 这个 inode 用哪种 data 模式 |
| `fs/ext4/super.c` | `ext4_param_data` | 1700 | `data=` 参数 |
| `fs/ext4/file.c` | `ext4_file_write_iter` | 691 | 缓冲 / DIO / DAX 分叉 |
| `fs/ext4/extents.c` | `ext4_fiemap` | 5072 | FIEMAP → `iomap_fiemap` |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← delalloc](./08-delalloc-unwritten.md) · [目录](./README.md) · [XFS 日志 →](./10-xfs-log.md)
