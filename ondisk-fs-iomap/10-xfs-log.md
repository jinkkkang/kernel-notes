# 10. XFS 日志

**结论**：XFS 把一次修改记成挂在 `xfs_trans` 上的 `xfs_log_item`。事务提交时并不马上写盘，而是把 item 放进 CIL（Committed Item List）；CIL 积到一定程度再做成一个 checkpoint 写入日志。和 jbd2 相比，运行中的「当前事务」不是一个所有 handle 共用的 `transaction_t`，提交记录也不是每个文件系统事务各写一块。

> 本章停在对象和提交顺序。pin / unpin、AIL 推进、恢复重放的每一种 item 格式不逐个展开。`xfs_rename` 只钉到函数定义。

---

## 10.1 log item

```c
/* fs/xfs/xfs_trans.h:30 */
struct xfs_log_item {
	struct list_head		li_ail;		/* AIL pointers */
	struct list_head		li_trans;	/* transaction list */
	xfs_lsn_t			li_lsn;		/* last on-disk lsn */
	struct xlog			*li_log;
	struct xfs_ail			*li_ailp;	/* ptr to AIL */
	uint				li_type;	/* item type */
	unsigned long			li_flags;	/* misc flags */
	struct xfs_buf			*li_buf;	/* real buffer pointer */
	struct list_head		li_bio_list;
	const struct xfs_item_ops	*li_ops;	/* function list */
	/* delayed logging */
	struct list_head		li_cil;		/* CIL pointers */
	struct xfs_log_vec		*li_lv;		/* active log vector */
	struct xfs_log_vec		*li_lv_shadow;	/* standby vector */
	xfs_csn_t			li_seq;		/* CIL commit seq */
	uint32_t			li_order_id;	/* CIL commit order */
};
```

`li_ail` 是已经在磁盘日志里、还要按 LSN 写回原地的项（Active Item List）。`li_cil` 是已经从事务提交、还没写成日志 checkpoint 的项。`li_ops`（`:68`）是一张函数表：`iop_format` 把项格式化进 log vector，`iop_pin` / `iop_unpin` 在项进日志期间钉住缓冲区，`iop_committed` 在 checkpoint 提交之后返回该项要进入 AIL 的 LSN。

inode、dquot、空闲空间 intent 都是不同的 `xfs_log_item` 子类，靠 `li_ops` 区分。本章不列出每种子类。

---

## 10.2 CIL

```c
/* fs/xfs/xfs_log_priv.h:269
 * 注释 :254：跟踪已经提交、还没写入日志的 item。
 * 用来在 delayed logging 打开时避免「刷日志时堵住新事务，直到 checkpoint 的 commit LSN 出来」。
 */
struct xfs_cil {
	struct xlog		*xc_log;
	unsigned long		xc_flags;
	atomic_t		xc_iclog_hdrs;
	struct workqueue_struct	*xc_push_wq;
	struct rw_semaphore	xc_ctx_lock;
	struct xfs_cil_ctx	*xc_ctx;
	spinlock_t		xc_push_lock;
	xfs_csn_t		xc_push_seq;
	bool			xc_push_commit_stable;
	struct list_head	xc_committing;
	wait_queue_head_t	xc_commit_wait;
	wait_queue_head_t	xc_start_wait;
	xfs_csn_t		xc_current_sequence;
	wait_queue_head_t	xc_push_wait;
	void __percpu		*xc_pcp;	/* percpu CIL structures */
};
```

`xfs_cil_ctx` 的注释在 `:213`：把每个事务的细节攒起来，交给 iclog 做 checkpoint 提交后的处理；当前 ctx 提交之后，下一批事务再攒进新的 ctx。

空间怎么算，头文件 `:295` 起写得很具体：按日志空间而不是按对象个数设限；事务 ticket 里没用完的预留，在提交时转给 CIL ctx 的 commit ticket（`:319`），这样 checkpoint 自己不必再向日志要一次 grant，避免在 push 里重入分配而死锁。

```text
xfs_trans_commit
  xlog_cil_commit                     fs/xfs/xfs_log_cil.c:1556
    调用点：fs/xfs/xfs_trans.c:1004
    注释 :1542：格式化 item、需要时 pin、记账、
                把没用完的预留还掉、busy extent 交给 checkpoint ctx、解锁 item
    xlog_cil_alloc_shadow_bufs        先分配，避免和 kswapd 的 CIL push 死锁 :1571
    xlog_cil_insert_items             :1579
```

后台 push 把当前 CIL 写成一个 checkpoint：一批 item 共用一段日志和一个 commit 记录。同步操作若要等某个序列，可以在 `xc_committing` 上等到那个 checkpoint 的 commit LSN（`xfs_cil` 注释 `:260`）。

tracepoint 能对上名字的有 `xfs_log_cil_wait`（`fs/xfs/xfs_trace.h:1287`）和 whiteout 的 `xfs_cil_whiteout_*`（`:1336`）。push 本身的每一步没有在本章逐个对事件名。

---

## 10.3 和 jbd2 对照

| | jbd2（ext4） | XFS CIL |
|---|---|---|
| 调用者拿到的对象 | `handle_t`，信用是缓冲区个数 | `xfs_trans`，上面挂 `xfs_log_item` |
| 运行中的复合事务 | 一个 `j_running_transaction`，大家的 handle 都挂上去 | 每个 `xfs_trans` 自己提交进 CIL；CIL ctx 才是正在攒的 checkpoint |
| 何时写日志 | `kjournald2` 提交当前事务：数据（ordered）、元数据、commit 记录 | CIL 后台 push 把已提交的 item 写成一个 checkpoint |
| 提交记录的粒度 | 一个 `transaction_t` 一块 commit 记录 | 一个 checkpoint（很多已提交事务）一块 |
| 原地写回 | checkpoint 列表上的元数据缓冲区 | AIL 里按 LSN 排序的 item |
| 数据顺序 | `data=ordered` 在提交元数据前写数据缓冲区 | 数据 bio 是 iomap 回写；fsync 时若日志在另一设备，先刷数据设备再强制日志，见 [11](./11-fsync-crash.md) |

jbd2 的 handle 在 `jbd2_journal_stop` 之前都算「还在改这个事务」。XFS 的 `xlog_cil_commit` 返回时，事务已经离开调用者，item 在 CIL 里，日志上可能还没有这些字节。所以 XFS 的 fsync 不能只看「事务提交函数返回了」，还要 `xfs_log_force` 把 CIL 推到给定的序列。

---

## 10.4 缓冲 IO 和这条日志的关系

XFS 缓冲写的数据页不进 log item。`iomap_file_buffered_write` 只把字节放进页缓存。分配、unwritten 转换、文件大小更新才产生 `xfs_log_item`。回写完成时 `xfs_end_ioend` 在事务里做 `xfs_iomap_write_unwritten` / `xfs_setfilesize`（`xfs_aops.c:130`）。这些事务同样 `xlog_cil_commit`，然后才有 LSN。

因此一次缓冲写返回时，数据可能还在页缓存，元数据可能还在 CIL。两者都还没到 [11](./11-fsync-crash.md) 要求的那条线上。

---

## 10.5 关键符号

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/xfs/xfs_trans.h` | `struct xfs_log_item` | 30 | 日志项，含 CIL / AIL 链表 |
| `fs/xfs/xfs_trans.h` | `struct xfs_item_ops` | 68 | format / pin / committed |
| `fs/xfs/xfs_log_priv.h` | `struct xfs_cil` | 269 | 已提交未写入的 item 列表 |
| `fs/xfs/xfs_log_cil.c` | `xlog_cil_commit` | 1556 | 事务提交进 CIL |
| `fs/xfs/xfs_trans.c` | `xlog_cil_commit` 调用 | 1004 | `xfs_trans` 提交路径 |
| `fs/xfs/xfs_log.c` | `xfs_log_force` | 3183 | 把日志推到指定序列 |
| `fs/xfs/xfs_trace.h` | `xfs_log_cil_wait` | 1287 | 等 CIL 空间的 tracepoint |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← ext4 + jbd2](./09-ext4-jbd2.md) · [目录](./README.md) · [fsync →](./11-fsync-crash.md)
