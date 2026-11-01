# 2. bio 与 bvec

**结论**：`struct bio` 是块层的传输单位：`bi_opf` 放操作码和标志，`bi_io_vec` 指向一组 `bio_vec`，`bi_iter` 记住还剩多少扇区和读到第几段；`submit_bio` 记账并设 ioprio 后进入 `submit_bio_noacct`。

> 结构在 `include/linux/blk_types.h:265` 和 `include/linux/bvec.h:31`。分配与完成在 `block/bio.c`。拆分在 `block/blk-merge.c`。

C 新手把 `bio_vec` 想成「一页里的一段」。`bv_page` 是页指针，不是页号。`bv_offset` 是这一页里的字节偏移，`bv_len` 是字节长度。一个 bio 可以挂很多段，段数上限是 `BIO_MAX_VECS`（256，`include/linux/bio.h:13`）。段少的时候，内核把 `bio_vec` 数组直接嵌在 bio 末尾的 `bi_inline_vecs[]` 里，避免再分配一次。

---

## 2.1 `struct bio_vec` 与迭代器

```c
/* include/linux/bvec.h:31
 * 注释要求：若 n * PAGE_SIZE < bv_offset + bv_len，
 * 则 nth_page(bv_page, n) == bv_page + n。
 * page_is_mergeable() 会检查这个性质。一段可以跨同一 folio 里相邻的页。
 */
struct bio_vec {
	struct page	*bv_page;    /* 这段的第一页 */
	unsigned int	bv_len;      /* 字节数 */
	unsigned int	bv_offset;   /* 相对 bv_page 起点的字节偏移 */
};
```

填一段用 `bvec_set_page`（`:44`）、`bvec_set_folio`（`:59`）或 `bvec_set_virt`（`:74`）。folio 版会先算落在第几页，再把偏移收成页内偏移。

```c
/* include/linux/bvec.h:80。bio 里嵌着它，名字是 bi_iter */
struct bvec_iter {
	sector_t		bi_sector;     /* 设备上的当前位置，单位 512 字节扇区 */
	unsigned int		bi_size;       /* 还剩多少字节 */
	unsigned int		bi_idx;        /* 当前 bi_io_vec 下标 */
	unsigned int		bi_bvec_done;  /* 当前这一段里已经完成的字节 */
} __packed __aligned(4);
```

`mp_bvec_iter_len`（`bvec.h:107`）取 `bi_size` 和「这一段还剩的字节」里较小的那个。所以一段可以只完成一半，迭代器不会把整段丢掉。

---

## 2.2 `struct bio`

字段顺序按 `include/linux/blk_types.h:265`。带 `CONFIG_*` 的成员只有编进内核时才在。`bio_reset()` 保留从 `bi_max_vecs` 起的尾巴（`:308` 的注释，宏 `BIO_RESET_BYTES` 在 `:327`）。

```c
struct bio {
	struct bio		*bi_next;   /* 链到下一个 bio，或 request 里的链接 */
	struct block_device	*bi_bdev;   /* 目标块设备，含分区 */
	blk_opf_t		bi_opf;     /* 低 8 位是 REQ_OP_*，高位是 REQ_* 标志 */
	unsigned short		bi_flags;   /* BIO_* 位，见下面的 enum */
	unsigned short		bi_ioprio;  /* 这次 IO 的优先级，submit_bio 里可能补上 */
	blk_status_t		bi_status;  /* 完成状态，0 是 BLK_STS_OK */
	atomic_t		__bi_remaining; /* BIO_CHAIN 时还剩几段没完成 */

	struct bvec_iter	bi_iter;    /* 扇区、剩余字节、当前段 */

	blk_qc_t		bi_cookie;  /* 轮询完成时用的队列 cookie，BLK_QC_T_NONE 表示没有 */
	bio_end_io_t		*bi_end_io; /* 完成回调。typedef 在 blk_types.h:20 */
	void			*bi_private;/* 回调的私有指针，文件系统自己解释 */

	/* --- CONFIG_BLK_CGROUP --- */
	struct blkcg_gq		*bi_blkg;   /* 这个 bio 属于哪个 cgroup×队列 */
	struct bio_issue	bi_issue;   /* 提交时刻，限流用 */
	/* CONFIG_BLK_CGROUP_IOCOST 时还有 u64 bi_iocost_cost */

	/* --- CONFIG_BLK_INLINE_ENCRYPTION --- */
	struct bio_crypt_ctx	*bi_crypt_context;

	/* --- CONFIG_BLK_DEV_INTEGRITY 时 union 里有 bi_integrity --- */

	unsigned short		bi_vcnt;    /* 已经放进 bi_io_vec 的段数 */

	/* bio_reset() 从这里开始保留 */
	unsigned short		bi_max_vecs;/* bi_io_vec 最多能放几段 */
	atomic_t		__bi_cnt;   /* 引用计数，bio_put 减到 0 才释放 */
	struct bio_vec		*bi_io_vec; /* 段数组。可能指向下面的内嵌数组 */
	struct bio_set		*bi_pool;   /* 从哪个 bioset 分配的 */

	/* 必须放在结构体最后。段少时不再单独 kmalloc bio_vec */
	struct bio_vec		bi_inline_vecs[];
};
```

`bi_opf` 的类型是 `blk_opf_t`（`blk_types.h:256`），底层是 `__u32`。低 8 位用 `REQ_OP_MASK` 取出操作码（`:355`）。`bio_op()`（`:475`）就是 `bio->bi_opf & REQ_OP_MASK`。

6.6 里和读写有关的操作码（`enum req_op`，`:371`）。最低位为 1 表示数据往设备走（写）：

| 枚举 | 值 | 含义 |
|---|---|---|
| `REQ_OP_READ` | 0 | 从设备读 |
| `REQ_OP_WRITE` | 1 | 往设备写 |
| `REQ_OP_FLUSH` | 2 | 刷掉易失写缓存。注释写明不能当 bio 提交，只由 flush 状态机合成 request（`submit_bio_noacct` `:782`） |
| `REQ_OP_DISCARD` | 3 | 丢弃扇区 |
| `REQ_OP_SECURE_ERASE` | 5 | 安全擦除 |
| `REQ_OP_ZONE_APPEND` | 7 | 写到当前 zone 写指针 |
| `REQ_OP_WRITE_ZEROES` | 9 | 把一段写成零 |
| `REQ_OP_DRV_IN` / `REQ_OP_DRV_OUT` | 34 / 35 | 驱动私有，给直通命令 |

常用标志从 `enum req_flag_bits`（`:404`）起，位号接在 8 位操作码后面：

| 宏 | 含义 |
|---|---|
| `REQ_FAILFAST_DEV` / `_TRANSPORT` / `_DRIVER` | 设备、传输、驱动错误不再由驱动重试 |
| `REQ_SYNC` | 同步读写。读本身就算同步，见 `op_is_sync`（`:499`） |
| `REQ_FUA` | 写完必须落稳 |
| `REQ_PREFLUSH` | 写数据之前先刷缓存 |
| `REQ_RAHEAD` | 预读，可以失败 |
| `REQ_NOWAIT` | 不能为这次 IO 睡觉 |
| `REQ_POLLED` | 调用者用 `bio_poll` 轮询完成 |
| `REQ_NOMERGE` | 不许合并 |
| `REQ_BACKGROUND` | 后台 IO |
| `REQ_SWAP` | swap IO |

`op_is_flush`（`:489`）为真的条件是带了 `REQ_FUA` 或 `REQ_PREFLUSH`。`REQ_NOMERGE_FLAGS` 是 `REQ_NOMERGE | REQ_PREFLUSH | REQ_FUA`（`:463`）。

`bi_flags` 用的是另一套位，`enum` 从 `:333` 起，按声明顺序从 0 编号：

| 位名 | 含义 |
|---|---|
| `BIO_PAGE_PINNED` | `bio_release_pages` 时要 unpin |
| `BIO_CLONED` | 不拥有数据，克隆来的 |
| `BIO_BOUNCED` | bounce bio |
| `BIO_QUIET` | 出错时少打印 |
| `BIO_CHAIN` | 用 `__bi_remaining` 等子 bio |
| `BIO_REFFED` | `__bi_cnt` 被抬高过 |
| `BIO_BPS_THROTTLED` | 已经按字节限流过，不要再限 |
| `BIO_TRACE_COMPLETION` | `bio_endio` 要打完成 trace |
| `BIO_CGROUP_ACCT` | 已经记到 cgroup |
| `BIO_QOS_THROTTLED` / `BIO_QOS_MERGED` | 走过 rq_qos 的限流或合并 |
| `BIO_REMAPPED` | 分区起始扇区已经加进 `bi_sector` |
| `BIO_ZONE_WRITE_LOCKED` | 持有 zone 写锁 |

---

## 2.3 分配、加页、拆分

```text
bio_alloc                         include/linux/bio.h:433
  bio_alloc_bioset                block/bio.c:492     默认池是 fs_bio_set
    段数少：用 bio 末尾的 bi_inline_vecs
    否则从 biovec 池另拿数组

bio_add_page                      bio.c:1091
  能并进上一段（同一页且偏移相接）就加长 bv_len
  否则 __bio_add_page             :1069     bi_vcnt++，bi_iter.bi_size += len

bio_add_folio                     :1141     转成 bio_add_page(&folio->page, ...)

bio_iov_iter_get_pages            :1331     从 iov_iter 钉页并填段。Direct IO 会用
```

设备有自己的上限：`queue_limits`（`include/linux/blkdev.h:300`）里的 `max_sectors`、`max_segments`、`logical_block_size`。bio 超过这些上限时要拆。

```text
bio_split_to_limits               blk-merge.c:409
  __bio_split_to_limits           :362
    REQ_OP_DISCARD：bio_split_discard
    REQ_OP_WRITE_ZEROES：bio_split_write_zeroes
    读写：bio_split_rw            :287
      按 max_sectors 和段数切出前半
      bio_split                   bio.c:1628   克隆出一个较短的 bio，推进原 bio 的迭代器
      bio_chain                   bio.c:338    子 bio 完成才算父 bio 完成
      submit_bio                  先把前半提交
      返回后半，调用者继续
```

`bio_split`（`:1628`）不复制页的内容，两半指向同一批 `bio_vec` 的不同区间。前半的 `bi_end_io` 由 `bio_chain` 接到父 bio 上：父的 `__bi_remaining` 减到 1 才真正调用上层回调（`bio_endio` `:1578` 里看 `bio_remaining_done`，`:1546`）。

---

## 2.4 合并

合并发生在 bio 还没变成新 request 的时候，目的是少一次 tag、少一次 `queue_rq`。

```text
blk_mq_attempt_bio_merge          blk-mq.c:2928
  队列没有 QUEUE_FLAG_NOMERGES，且 bio 允许合并：
    blk_attempt_plug_merge        blk-merge.c:1113
      看 current->plug 里已有的 request，能否接到头或尾
    否则 blk_mq_sched_bio_merge   blk-mq-sched.c:339
      有调度器就问 e->ops.bio_merge
      否则在这个 CPU 的软件队列里找
```

接到已有 request 的尾部是 `bio_attempt_back_merge`（`blk-merge.c:987`），接到头部是 `bio_attempt_front_merge`（`:1013`）。`ll_back_merge_fn`（`:644`）检查扇区是否相接、段数会不会超 `max_segments`。对不上就返回，调用者去分配新 request。

两个相邻 request 合成一个是调度器插入时的事（`blk_mq_sched_try_insert_merge`，`blk-mq-sched.c:375`）。`none` 没有这张排序树，主要靠 plug 里的 bio 合并。

---

## 2.5 `submit_bio`

```text
submit_bio                        blk-core.c:860
  REQ_OP_READ：
    task_io_account_read(bi_iter.bi_size)
    count_vm_events(PGPGIN, bio_sectors)
  REQ_OP_WRITE：
    count_vm_events(PGPGOUT, bio_sectors)
  bio_set_ioprio                  :839
    bi_ioprio 的类还是 IOPRIO_CLASS_NONE：改成 get_current_ioprio()
    blkcg_set_ioprio(bio)         见 09
  submit_bio_noacct               :732
```

`submit_bio_noacct` 可以睡眠（开头 `might_sleep()`，`:738`）。它拒绝不支持的操作，而不是把非法 bio 送给驱动：

```text
REQ_NOWAIT 但设备不支持 nowait：BLK_STS_NOTSUPP
should_fail_bio：失败注入
bio_check_ro：写只读设备
还没 BIO_REMAPPED：
  bio_check_eod                     越过设备末尾
  分区：blk_partition_remap         加上 bd_start_sect，置 BIO_REMAPPED
带 PREFLUSH/FUA：
  必须是 WRITE 或 ZONE_APPEND
  队列没有 QUEUE_FLAG_WC：清掉这两个标志
    清完后没有数据扇区：直接 BLK_STS_OK 结束（空 flush 被吃掉）
队列没有 QUEUE_FLAG_POLL：清掉 REQ_POLLED
按 bio_op 检查 discard / write_zeroes / zone 是否支持
blk_throtl_bio 返回真：bio 被限流挂起，函数返回，完成以后再提交
否则 submit_bio_noacct_nocheck    :695
```

`submit_bio_noacct_nocheck` 先 `blk_cgroup_bio_start` 和 `blkcg_bio_issue_init`，再打 `trace_block_bio_queue`，置 `BIO_TRACE_COMPLETION`。若当前任务已经在一次 `submit_bio` 里（`current->bio_list` 非空），新 bio 先入链表，避免叠层设备递归把栈撑爆。注释在 `:619` 和 `:709`。

普通 mq 盘的 `bd_has_submit_bio` 为假，走 `__submit_bio_noacct_mq` → `__submit_bio` → `blk_mq_submit_bio`。叠层驱动自己实现 `gendisk->fops->submit_bio`，块层在 `bio_queue_enter` 成功后调用它（`:611`）。

完成：

```text
bio_endio                         bio.c:1578
  bio_remaining_done 为假：链式 bio 还没齐，返回
  BIO_TRACE_COMPLETION：trace_block_bio_complete
  blk_throtl_bio_endio
  若 bio_chain：先结束子链，再对父 bio 递归 bio_endio
  调用 bi_end_io(bio)
```

上层在 `bi_end_io` 里把 `blk_status_t` 转成 errno（`blk_status_to_errno`，`blk-core.c:194`），并 `folio_end_writeback` 或叫醒 Direct IO。状态码表在 [15](./15-errors-timeout-retry.md)。

轮询完成不走这条提交函数。`bio_poll`（`blk-core.c:886`）读 `bi_cookie`，队列要有 `QUEUE_FLAG_POLL`，然后进入 `blk_mq_poll`。`blk_mq_start_request` 在请求带 `REQ_POLLED` 时把 `bi_cookie` 写成硬件队列号（`blk-mq.c:1274`）。

---

## 2.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/bvec.h` | `struct bio_vec` | 31 | 一段页内字节 |
| `include/linux/bvec.h` | `struct bvec_iter` | 80 | bio 上的游标 |
| `include/linux/blk_types.h` | `struct bio` | 265 | 传输描述 |
| `include/linux/blk_types.h` | `enum req_op` | 371 | 操作码 |
| `include/linux/blk_types.h` | `bio_op` | 475 | 取出低 8 位 |
| `include/linux/bio.h` | `BIO_MAX_VECS` | 13 | 一个 bio 最多 256 段 |
| `include/linux/bio.h` | `bio_alloc` | 433 | 从 `fs_bio_set` 分配 |
| `block/bio.c` | `bio_alloc_bioset` | 492 | 分配实现 |
| `block/bio.c` | `bio_add_page` | 1091 | 追加一段，能并则并 |
| `block/bio.c` | `bio_add_folio` | 1141 | folio 包装 |
| `block/bio.c` | `bio_iov_iter_get_pages` | 1331 | 从迭代器钉页 |
| `block/bio.c` | `bio_split` | 1628 | 按扇区切成两半 |
| `block/bio.c` | `bio_chain` | 338 | 子 bio 完成才结束父 bio |
| `block/bio.c` | `bio_endio` | 1578 | 完成回调 |
| `block/blk-merge.c` | `bio_split_rw` | 287 | 按队列上限拆读写 |
| `block/blk-merge.c` | `bio_split_to_limits` | 409 | 提交前的拆分入口 |
| `block/blk-merge.c` | `blk_attempt_plug_merge` | 1113 | 并进当前任务的 plug |
| `block/blk-core.c` | `submit_bio` | 860 | 上层入口 |
| `block/blk-core.c` | `submit_bio_noacct` | 732 | 检查后下发或结束 |
| `block/blk-core.c` | `bio_poll` | 886 | 轮询一条 bio 的完成 |

---

**导航**：[← 总览](./01-overview.md) · [目录](./README.md) · [request →](./03-request-and-blk-mq.md)
