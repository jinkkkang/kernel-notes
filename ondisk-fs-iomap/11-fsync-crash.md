# 11. fsync 与崩溃一致性

**结论**：`fsync` 先在 VFS 里调到 `f_op->fsync`。ext4 等到范围内的脏页写完，再等这个 inode 的日志事务（`ext4_fc_commit`），需要时再 `blkdev_issue_flush`。XFS 同样先等脏页，然后若 inode 仍被日志钉住就 `xfs_log_force_inode`，单设备且日志强制没有刷缓存时再补一次数据设备 flush。`rename` 的原子性是目录项在一个日志事务里提交；它不代替文件数据的 fsync。

> VFS 停在 `vfs_fsync_range` → `f_op->fsync`，见 [vfs/05](../vfs/05-read-write.md) §5.6。设备写缓存怎样拆 flush 见 [block-io/10](../block-io/10-writeback-and-fsync.md)。

---

## 11.1 从系统调用到文件系统

```text
fsync                               fs/sync.c:218
  do_fsync(fd, 0)                   :220     函数本体在 :206
    vfs_fsync                       :212     函数本体在 :200
      vfs_fsync_range               :180
        f_op->fsync(file, start, end, datasync)
fdatasync                           :223    do_fsync(fd, 1) 在 :225
```

`vfs_fsync_range` 的注释（`:169`）：把 `start..end` 的数据和这个文件的元数据写盘；`datasync` 非 0 时只写访问已改数据所需要的元数据。`vfs_fsync` 把范围设成 `0` 到 `LLONG_MAX`（`:202`）。

带 `O_SYNC` / `O_DSYNC` 的写在返回前走 `generic_write_sync`（`include/linux/fs.h:2675`）：`iocb_is_dsync` 时调用 `vfs_fsync_range`，范围是刚写过的字节。`IOCB_SYNC` 时 `datasync` 参数是 0，否则是 1。ext4 缓冲写在 `ext4_buffered_write_iter` 末尾调用它（`file.c:312`）。XFS 缓冲写在 `xfs_file_buffered_write` 末尾调用它（`xfs_file.c:816`）。

Direct IO 的 `O_DSYNC` 不一定再走这次 `generic_write_sync`：`iomap_dio_rw` 的注释（`direct-io.c:535`）说 O_[D]SYNC 写在 dio 完成路径里做完同步。ext4 / XFS 的 `write_iter` 是否在 dio 返回后再包一层，以各自函数末尾为准；dio 内部已经置了 `IOMAP_DIO_NEED_SYNC`（`direct-io.c:633`）。

---

## 11.2 ext4：`ext4_sync_file`

```text
ext4_sync_file                      fs/ext4/fsync.c:141
  没有 journal：
    ext4_fsync_nojournal            :82
      generic_buffers_fsync_noflush
      ext4_write_inode              把 inode 表缓冲区写出去
      EXT4_MOUNT_BARRIER 时记 needs_barrier
  有 journal：
    file_write_and_wait_range       :170     范围内脏页写完并等待
    ext4_fsync_journal              :109
      非普通文件：ext4_force_commit :121
      否则 ext4_fc_commit(journal, commit_tid)  :127
        commit_tid = datasync ? i_datasync_tid : i_sync_tid
  needs_barrier：
    blkdev_issue_flush(s_bdev)      :183
  file_check_and_advance_wb_err
```

`ext4_fsync_journal` 用 inode 上记下的事务号，而不是「把整个日志提交一遍」。`datasync` 用 `i_datasync_tid`，否则用 `i_sync_tid`（`:114`）。注释（`fsync.c:131`）说：调用者的写回把数据推下去，元数据在日志里，这里等该提交的那个事务。

`JBD2_BARRIER` 且 `jbd2_trans_will_send_data_barrier` 认为这个事务自己不会发数据屏障时，置 `needs_barrier`（`:123`），函数末尾再 `blkdev_issue_flush`。commit 记录本身在 `journal_submit_commit_record` 里，若 `JBD2_BARRIER` 且没有 async commit，已经带 `REQ_PREFLUSH | REQ_FUA`（`fs/jbd2/commit.c:152`）。两条路合在一起，目标是：事务可见时，它依赖的数据和 commit 记录都不只停在设备的易失缓存里。

没有日志的 ext4（`s_journal` 为空，`:162`）走 `ext4_fsync_nojournal`：等缓冲区、写 inode、按 `BARRIER` 决定要不要 flush。这时没有 commit 记录可等。

`ext4_fc_commit` 是 fast commit 的入口。本章确认调用点在 `fsync.c:127`，不读 fast commit 的日志格式。非普通文件（目录等）不走 fast commit，直接 `ext4_force_commit`（`:121`）。

`data=` 模式影响的是「等的那个事务里有没有数据、数据是否必须先于元数据」。ordered 下，事务提交前 `journal_submit_data_buffers` 已经把关联数据写出去（[09](./09-ext4-jbd2.md)）。fsync 的 `file_write_and_wait_range` 再覆盖「还没挂进任何事务的脏页」。writeback 模式没有这层顺序，fsync 仍等待调用时刻已经脏的页，但崩溃窗口里别的写可以让元数据先于数据可见。journal 模式的数据在日志里，等事务就包含那些数据块。

---

## 11.3 XFS：`xfs_file_fsync`

```text
xfs_file_fsync                      fs/xfs/xfs_file.c:131
  trace_xfs_file_fsync              :142
  file_write_and_wait_range         :144
  实时 inode：blkdev_issue_flush(rtdev)           :160
  否则日志设备和数据设备不同：
    blkdev_issue_flush(ddev)        :162
      注释 :153：先让新数据落盘，再记录变大的 inode
  inode 仍 pin（xfs_ipincount）：
    xfs_fsync_flush_log             :171
      里面 xfs_log_force_inode      :79
  单设备、且上面的 log force 没有刷过缓存：
    blkdev_issue_flush(ddev)        :185
      注释 :176：fdatasync 覆盖写、没有元数据要提交时会走到这里
```

`xfs_log_force` 的定义在 `fs/xfs/xfs_log.c:3183`。它把 CIL 里还没写成 checkpoint 的项推到 inode 需要的序列。只 `xfs_trans_commit` 而不 force，项可能还在 `struct xfs_cil` 里，见 [10](./10-xfs-log.md)。

数据设备和日志设备是同一块盘时，前半段不单独刷数据设备。若随后的 log force 发出的 checkpoint 自带了缓存刷出（`log_flushed`），函数就不再补 flush。纯覆盖写的 fdatasync 没有新的 inode 项，pin 计数可能是 0，于是走到最后那次 `blkdev_issue_flush`，把数据写缓存推下去。

---

## 11.4 崩溃时哪条线算数

把一次缓冲写和一次 fsync 拆开看：

```text
write 返回
  数据在页缓存，已标脏
  ext4：可能只有 BH_Delay，盘上还没有块
  XFS：可能是 IOMAP_DELALLOC，item 也许还在 CIL
fsync 返回
  file_write_and_wait_range 已等待这些脏页的 writeback
  ext4：i_sync_tid / i_datasync_tid 对应的事务已提交
        ordered：该事务的数据缓冲区在 commit 记录之前提交
        需要时设备 flush 已发出
  XFS：需要的日志序列已 force，数据设备按上面的条件 flush
```

fsync 返回之后再掉电，调用时刻之前完成的写应对这次 fsync 可见。fsync 返回之前掉电，页缓存、CIL、还没写 commit 记录的 jbd2 事务都可以消失。本章不给「多少毫秒」这种数字。

`O_SYNC` 写把 fsync 包进同一次系统调用，边界相同，只是不等用户再调一次 `fsync`。

---

## 11.5 rename 的原子性停在目录项

ext4 的目录 `rename` 是 `ext4_rename2` → `ext4_rename`（`fs/ext4/namei.c:4258`，定义 `:3838`）。函数里用 `ext4_journal_start(old.dir, EXT4_HT_DIR, credits)` 开 handle（`:3922` 和 `:4150`）。目录项的增删改在这个 handle 上记入日志，`jbd2_journal_stop` 之后它们属于同一个事务。提交要么看见新名字，要么仍看见旧名字，不会看见两个半成品目录项——前提是这次 handle 覆盖了这些目录块。credits 的计算公式本章没有逐行核对。

这个事务不包含文件的数据页。典型顺序是：

```text
写文件 A.tmp
fsync(A.tmp)          数据 + 文件元数据
rename(A.tmp, A)      目录项事务
fsync(目录)           让目录事务提交；ext4 对非普通文件走 ext4_force_commit
```

少了 `fsync(A.tmp)`，rename 的目录事务可以先提交，崩溃后名字 `A` 指向一个还没有数据的 inode。少了目录的 fsync，数据已经在盘上，名字替换仍可能只在内存日志里。

XFS 的 `xfs_rename` 定义在 `fs/xfs/xfs_inode.c:2868`，函数里 `trace_xfs_rename`（`:2890`）。它走一笔 `xfs_trans`，提交进 CIL。和 ext4 一样，目录项的原子性和文件数据的 fsync 是两笔账。事务里具体记了哪些 item，本章没有逐项列出。

白出（whiteout）和 `RENAME_EXCHANGE` 会让 handle 多改几个 inode。它们仍是目录事务里的元数据，不把文件数据写进日志。

---

## 11.6 关键函数

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/sync.c` | `vfs_fsync_range` | 180 | 调 `f_op->fsync` |
| `fs/sync.c` | `do_fsync` | 206 | `fsync`（`:220`）/ `fdatasync`（`:225`）的公共路径 |
| `fs/sync.c` | `fsync` / `fdatasync` | 218 / 223 | 系统调用 |
| `include/linux/fs.h` | `generic_write_sync` | 2675 | `O_SYNC` 写末尾的 fsync |
| `fs/ext4/fsync.c` | `ext4_sync_file` | 141 | ext4 的 `->fsync` |
| `fs/ext4/fsync.c` | `ext4_fsync_journal` | 109 | 等 `i_sync_tid` / `i_datasync_tid` |
| `fs/ext4/fsync.c` | `blkdev_issue_flush` 调用 | 183 | 数据设备屏障 |
| `fs/jbd2/commit.c` | commit 记录加 `REQ_PREFLUSH \| REQ_FUA` | 152 | `JBD2_BARRIER` 且非 async commit |
| `fs/xfs/xfs_file.c` | `xfs_file_fsync` | 131 | XFS 的 `->fsync` |
| `fs/xfs/xfs_log.c` | `xfs_log_force` | 3183 | 把 CIL 推到给定序列 |
| `fs/ext4/namei.c` | `ext4_rename` | 3838 | 目录项事务 |
| `fs/xfs/xfs_inode.c` | `xfs_rename` | 2868 | XFS rename |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← XFS 日志](./10-xfs-log.md) · [目录](./README.md) · [观测 →](./12-observability.md)
