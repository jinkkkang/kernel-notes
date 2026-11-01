# 4. 调度器 none

**结论**：6.6 里没有名叫 `mq-noop` 的调度器。`none` 表示 `request_queue->elevator` 是 `NULL`：请求留在每 CPU 的软件队列里，硬件队列忙时轮流出队，闲时一次把有请求的软件队列倒空再交给驱动。

> 名字出现在 `block/elevator.c`。派发在 `block/blk-mq-sched.c` 的 `__blk_mq_sched_dispatch_requests`（`:270`）。没有单独的 `block/noop-iosched.c`。

C 新手可以把 `none` 理解成「不按扇区重排、不按权重挑人」。它仍然有队列：软件队列按提交 CPU 分开，硬件队列按设备通道分开。它不做的是把请求从这些链表里抽出来再插进红黑树。

---

## 4.1 名字从哪来

`elv_iosched_show`（`block/elevator.c:764`）拼 sysfs 字符串：

```text
队列不支持调度器（不是 mq，或带 BLK_MQ_F_NO_SCHED）：
  只打印 "none\n"                         :771
q->elevator 为空：
  当前项写成 "[none] "                    :774
否则：
  先写 "none "，再把正在用的调度器名字用方括号括上  :776
```

写 `"none"` 进 `scheduler` 文件时，`elevator_change`（`:718`）用 `strncmp(elevator_name, "none", 4)`（`:727`）。已经有 elevator 就调用 `elevator_disable`（`:697`）：退出调度器、`q->elevator = NULL`、清 `QUEUE_FLAG_SQ_SCHED`、把 `nr_requests` 设回 `tag_set->queue_depth`，trace 消息是 `elv switch: none`（`:709`）。

默认值在 `elevator_get_default`（`:570`），注释在 `:567`：

```text
tag set 带 BLK_MQ_F_NO_SCHED_BY_DEFAULT：返回 NULL（none）
nr_hw_queues != 1 且不是共享 tag：返回 NULL（none）
否则 elevator_find_get(q, "mq-deadline")
找不到或 blk_mq_init_sched 失败：留下 none，printk 里写 falling back to "none"
```

所以多队列 NVMe、virtio-blk 注册出来时，常见的当前项就是 `[none]`。单队列且编了 `CONFIG_MQ_IOSCHED_DEADLINE`（`block/Kconfig.iosched:3`，`default y`）时，默认会挂上 `mq-deadline`。这是注册时的选择，之后仍可以写 sysfs 改掉。

`elv_support_iosched`（`:558`）为假的队列连切换都不做：`elv_iosched_store`（`:748`）直接返回 `count`，等于写了也没改。

---

## 4.2 请求停在哪

没有 elevator 时，`blk_mq_submit_bio`（`blk-mq.c:3014`）的后半段：

```text
能直接下发（没有 RQF_USE_SCHED，且硬件队列不忙，或这次是同步且有多条硬件队列）：
  blk_mq_try_issue_directly       :2705
    当前上下文里 queue_rq
否则：
  blk_mq_insert_request           :2567
    插进 rq->mq_ctx->rq_lists[hctx->type]
    blk_mq_hctx_mark_pending      blk-mq.c:72    在 hctx->ctx_map 里把这个软件队列的位置 1
  blk_mq_run_hw_queue(hctx, true) 异步再跑
```

`RQF_USE_SCHED` 在分配 request 时、发现 `q->elevator` 非空才会置上。`none` 的请求不带这个标志，所以同步 IO 在硬件队列空闲时经常不进软件队列，直接 `queue_rq`。plug 仍然有效：一批 bio 先挂在 `current->plug->mq_list`，unplug 时再 `blk_mq_flush_plug_list`（`blk-mq.c:2845`）。

合并只靠两处：`blk_attempt_plug_merge`（plug 里相邻的 request）和 `blk_mq_sched_bio_merge` 在没有 `ops.bio_merge` 时扫描本 CPU 软件队列（`blk-mq-sched.c:339`）。没有按扇区的红黑树，所以隔了一个不相关请求之后再来的相邻 bio，不一定还能并上。

---

## 4.3 派发

`blk_mq_sched_dispatch_requests`（`blk-mq-sched.c:319`）在硬件队列没停、队列没 quiesce 时调用 `__blk_mq_sched_dispatch_requests`（`:270`）。

```text
hctx->dispatch 上还有上次没送走的请求：
  先把它们交给 blk_mq_dispatch_rq_list
  送不完就标记重启，本次返回
  送完了也继续下面的步骤（need_dispatch = true）

q->elevator 非空：
  走调度器，本章不进入

need_dispatch 为真（刚才送过 dispatch 链表，或 hctx->dispatch_busy）：
  blk_mq_do_dispatch_ctx          :215
否则：
  blk_mq_flush_busy_ctxs          blk-mq.c:1719
  blk_mq_dispatch_rq_list         一次把倒出来的链表送走
```

忙和闲是两条策略，注释写在 `:311`：「queue is busy 时从软件队列一个一个出队」。

**闲**：`blk_mq_flush_busy_ctxs` 扫 `hctx->ctx_map` 里置位的软件队列，把 `rq_lists[hctx->type]` 上的请求全部搬到一张临时链表，再一次性 `blk_mq_dispatch_rq_list`。适合设备跟得上、希望少拿几次锁的情况。

**忙**：`blk_mq_do_dispatch_ctx`（`:215`）：

```text
从 hctx->dispatch_from 记住的那个软件队列开始
ctx_map 没有置位：停
hctx->dispatch 又非空：返回 -EAGAIN，让 run_work 再跑，避免饿死 flush
blk_mq_dequeue_from_ctx         blk-mq.c 里从 ctx 起找下一个有请求的软件队列
一次只放一个 request 进 rq_list
blk_mq_dispatch_rq_list(..., 1) 送一个
成功则 ctx = blk_mq_next_ctx    :196    下标加一，到头回到 0
把新的起点写回 hctx->dispatch_from
```

`blk_mq_next_ctx` 用的是 `ctx->index_hw[hctx->type]`，在 `hctx->ctxs[]` 里转一圈。这就是 `none` 的公平：忙的时候每个提交 CPU 轮流送一个，不让一个 CPU 把硬件队列占满。它不看扇区是否连续，也不看 ioprio。

`dispatch_from` 的注释在 `struct blk_mq_hw_ctx`（`include/linux/blk-mq.h:351`）：「没有选择调度器时，用来决定从哪个软件队列接着取」。

外层如果连续两次得到 `-EAGAIN`，`blk_mq_sched_dispatch_requests`（`:333`）会 `blk_mq_run_hw_queue(hctx, true)` 再排队一次，避免 flush 请求卡在 `hctx->dispatch` 里。

---

## 4.4 和三个调度器比，none 缺什么

| 行为 | none | 有 elevator 时 |
|---|---|---|
| `q->elevator` | `NULL` | 指向 `struct elevator_queue`（`block/elevator.h:114`） |
| 插入 | 软件队列链表 | `ops.insert_requests` |
| 取出 | `blk_mq_dequeue_from_ctx` 或整表倒出 | `ops.dispatch_request` |
| 按扇区排序 | 没有 | deadline、bfq 有红黑树 |
| 按优先级 | 不看 `rq->ioprio` | deadline 分 RT/BE/IDLE；bfq 按权重 |
| 限制在途深度 | 只有设备 tag 数 | kyber 还有每域令牌；deadline 限制异步深度 |
| `QUEUE_FLAG_SQ_SCHED` | `elevator_disable` 会清掉 | deadline 和 bfq 会置上；kyber 会清掉 |

`struct elevator_queue`（`elevator.h:114`）只有 `type`、`elevator_data`、kobject、sysfs 锁、flags 和一张合并哈希。`none` 连这个对象都没有，所以也没有 `queue/iosched/*` 那些调度器自己的 sysfs 文件。

直接下发仍然可能失败。`queue_rq` 返回 `BLK_STS_RESOURCE` 或 `BLK_STS_DEV_RESOURCE` 时，请求进 `hctx->dispatch`，下次派发优先送它（`__blk_mq_sched_dispatch_requests` 开头，`:279`）。这条「没送走的放队头」对四个名字都一样，不是 `none` 独有。

---

## 4.5 什么时候会离开 none

```text
echo mq-deadline > /sys/block/<disk>/queue/scheduler
  elv_iosched_store                 elevator.c:748
    elevator_change                 :718
      elevator_find_get            按名字找已注册的 elevator_type
      找不到：request_module("%s-iosched", name)   :738
      elevator_switch               :660
        冻队列、quiesce
        旧 elevator 退出
        blk_mq_init_sched           blk-mq-sched.c:443
          调 ops.init_sched
          给每条 hctx 分配 sched_tags（如果调度器需要）
        elv_register_queue
```

`blk_mq_init_sched` 失败时队列留在 none，警告在 `elevator_switch` `:690`。模块名是 `mq-deadline-iosched`（`mq-deadline.c:1286`）、kyber 没有 `MODULE_ALIAS` 的 iosched 后缀（它的 `elevator_name` 就是 `kyber`，`request_module("kyber-iosched")` 能否找到取决于模块别名；源码里 kyber 只有 `elv_register`，没有 `MODULE_ALIAS("kyber-iosched")`）。bfq 有 `MODULE_ALIAS("bfq-iosched")`（`bfq-iosched.c:7671`）。写 sysfs 时内核用 `"%s-iosched"` 去 `request_module`。deadline 的别名对得上。kyber 若编成模块且没有这条别名，自动加载可能失败，需要模块已经在内存里或名字本身能被找到。本章不猜发行版的 modules.alias。能确定的是：已经注册的类型靠 `elevator_find_get` 按 `elevator_name` 匹配（`elevator.c:736`）。

切回：

```text
echo none > /sys/block/<disk>/queue/scheduler
  elevator_disable                :697
```

切换会冻队列。正在飞的请求用旧的完成路径收尾，新的请求按新的 `q->elevator` 插入。

---

## 4.6 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `block/elevator.c` | `elv_support_iosched` | 558 | `BLK_MQ_F_NO_SCHED` 时根本没有调度器 |
| `block/elevator.c` | `elevator_get_default` | 570 | 单队列默认找 `mq-deadline`，否则 none |
| `block/elevator.c` | `elevator_init_mq` | 612 | 注册磁盘时调用上面的选择 |
| `block/elevator.c` | `elevator_disable` | 697 | 切到 none，`q->elevator = NULL` |
| `block/elevator.c` | `elevator_change` | 718 | 认出字符串 `"none"` |
| `block/elevator.c` | `elv_iosched_store` | 748 | sysfs 写 `scheduler` |
| `block/elevator.c` | `elv_iosched_show` | 764 | 方括号标出当前名字 |
| `block/blk-sysfs.c` | `elv_iosched` 属性 | 496 | 文件名 `"scheduler"` |
| `block/blk-mq-sched.c` | `__blk_mq_sched_dispatch_requests` | 270 | elevator 为空时走软件队列 |
| `block/blk-mq-sched.c` | `blk_mq_do_dispatch_ctx` | 215 | 忙：每个软件队列轮流一个 |
| `block/blk-mq-sched.c` | `blk_mq_next_ctx` | 196 | 软件队列下标加一 |
| `block/blk-mq.c` | `blk_mq_flush_busy_ctxs` | 1719 | 闲：把置位的软件队列倒空 |
| `block/blk-mq.c` | `blk_mq_try_issue_directly` | 2705 | 不进调度器、也不进软件队列 |
| `block/blk-mq.c` | `blk_mq_insert_request` | 2567 | 插入 `ctx->rq_lists` |
| `include/linux/blk-mq.h` | `BLK_MQ_F_NO_SCHED` | 661 | 禁止配置调度器 |
| `include/linux/blk-mq.h` | `BLK_MQ_F_NO_SCHED_BY_DEFAULT` | 666 | 注释写明这时选 `none` 而不是 `mq-deadline` |

---

**导航**：[← request](./03-request-and-blk-mq.md) · [目录](./README.md) · [mq-deadline →](./05-sched-mq-deadline.md)
