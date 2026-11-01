# 9. ioprio 与 cgroup io

**结论**：`submit_bio` 在类还是 `IOPRIO_CLASS_NONE` 时用任务的 ioprio 填 `bio->bi_ioprio`，再让 cgroup 的 `blkcg_set_ioprio` 可能把它抬高或压低；同一时刻 `bi_blkg` 把这个 bio 记到「cgroup × 请求队列」，限流、权重和延迟策略都挂在这份 `blkcg_gq` 上。

> 系统调用在 `block/ioprio.c`。cgroup 改写优先级在 `block/blk-ioprio.c`。记账在 `block/blk-cgroup.c` 的 `blk_cgroup_bio_start`（`:2160`）。v2 文件由谁登记已经写在 [cgroup/07](../cgroup/07-io-pids-misc.md) §7.1，这里只接块层怎么用。

C 新手把 ioprio 想成一个 16 位整数：高 3 位是类，低 13 位是类里的数据。类决定「能不能插队」，数据在 RT 和 BE 里是 0 到 7 的等级，数字小更优先。cgroup 不替换这个整数的存储位置，它改的是 bio 上已经填好的那一份。

---

## 9.1 类和等级

`include/uapi/linux/ioprio.h`。`IOPRIO_CLASS_SHIFT` 是 13（`:11`）。

```c
#define IOPRIO_PRIO_CLASS(ioprio) \
	(((ioprio) >> IOPRIO_CLASS_SHIFT) & IOPRIO_CLASS_MASK)
#define IOPRIO_PRIO_LEVEL(ioprio) ((ioprio) & IOPRIO_LEVEL_MASK)
```

`IOPRIO_LEVEL_NR_BITS` 是 3，所以等级只有低 3 位，`IOPRIO_NR_LEVELS` 是 8（`:42`）。

| 类 | 值 | 头文件注释（`:20`） |
|---|---|---|
| `IOPRIO_CLASS_NONE` | 0 | 还没指定。块层会换成任务的默认类 |
| `IOPRIO_CLASS_RT` | 1 | 实时。BFQ 和 mq-deadline 都认。注释还提到支持 NCQ 优先级的 ATA 盘会用高优先级命令 |
| `IOPRIO_CLASS_BE` | 2 | 尽力而为，进程的默认类 |
| `IOPRIO_CLASS_IDLE` | 3 | 别人不用盘时才服务 |
| `IOPRIO_CLASS_INVALID` | 7 | 非法值 |

`ioprio_set` / `ioprio_get` 的 `which`（`:54`）：`IOPRIO_WHO_PROCESS = 1`，然后是 `IOPRIO_WHO_PGRP`、`IOPRIO_WHO_USER`。

谁都没设时，内核从调度策略和 nice 推一个默认值（`include/linux/ioprio.h:29`）：

```text
task_nice_ioclass
  SCHED_IDLE        → IOPRIO_CLASS_IDLE
  实时任务          → IOPRIO_CLASS_RT
  其它              → IOPRIO_CLASS_BE
task_nice_ioprio    (task_nice(task) + 20) / 5     nice 0 → 4
```

`__get_task_ioprio`（`block/ioprio.c:149`）在任务没有显式 ioprio 时走上面两步。`get_current_ioprio`（`include/linux/ioprio.h:58`）就是对 `current` 调用它。`IOPRIO_DEFAULT` 是 `IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0)`（`:14`）。

系统调用：

| 符号 | 文件:行 | 作用 |
|---|---|---|
| `ioprio_set` | `block/ioprio.c:69` | `SYSCALL_DEFINE3`，改进程、进程组或用户的 ioprio |
| `ioprio_get` | `block/ioprio.c:210` | 读回来 |
| `ioprio_check_cap` | 声明在 `include/linux/ioprio.h:66` | 设 RT 类需要能力检查 |

mq-deadline 把类映射成 `DD_RT_PRIO` / `DD_BE_PRIO` / `DD_IDLE_PRIO`（`mq-deadline.c:109`），`NONE` 和 `BE` 同一档。BFQ 把 `ioprio` 和 `ioprio_class` 放进 `bfq_queue`（`bfq-iosched.h:255`），类之间严格优先，类内部再按 weight。kyber 和 `none` 不读这个字段来排序。所以「设了 ioprio 却看不到差别」时，先看 `/sys/block/<disk>/queue/scheduler` 当前是不是 `none` 或 `kyber`。

---

## 9.2 bio 上的那一次填写

```text
submit_bio                         blk-core.c:860
  bio_set_ioprio                   :839
    IOPRIO_PRIO_CLASS(bio->bi_ioprio) == IOPRIO_CLASS_NONE
      bio->bi_ioprio = get_current_ioprio()
    blkcg_set_ioprio(bio)          blk-ioprio.c:187
```

文件系统可以在调用 `submit_bio` 之前自己写 `bi_ioprio`（io_uring 的 SQE 带 `ioprio` 字段，见 [12](./12-io-uring.md)）。类已经不是 `NONE` 时，`bio_set_ioprio` 不会用任务的 nice 覆盖它，但 `blkcg_set_ioprio` 仍可能再改。

request 分配时把 `bio->bi_ioprio` 抄进 `rq->ioprio`。读它用 `req_get_ioprio`（`include/linux/blk-mq.h:203`）。

---

## 9.3 cgroup 的 ioprio 策略

`block/blk-ioprio.c` 是一个 blkcg policy，不是又一个调度器。v2 和 v1 都登记属性 `prio.class`（`:167`、`:172`）。策略枚举（`:34`）：

| `enum prio_policy` | 文件里的字符串 | 行为（`blkcg_set_ioprio` `:187`） |
|---|---|---|
| `POLICY_NO_CHANGE` (0) | `no-change` | 直接返回，不改 bio |
| `POLICY_PROMOTE_TO_RT` (1) | `promote-to-rt` | 类还不是 RT 就写成 `IOPRIO_CLASS_RT` 等级 4 |
| `POLICY_RESTRICT_TO_BE` (2) | `restrict-to-be` | 和下面的 max 逻辑一起，把优先级压到不超过 BE |
| `POLICY_ALL_TO_IDLE` (3) | `idle` | 压到 IDLE |
| `POLICY_NONE_TO_RT` (4) | `none-to-rt` | 注释说是 `PROMOTE_TO_RT` 的别名，走同一个分支 |

RT 分支的注释（`:197`）解释等级为什么是 4：RT 线程的默认 nice 是 0，`task_nice_ioprio` 算出来就是 4。已经是 RT 的 bio 保持原等级，这样进程仍可以用 `ioprio_set` 要一个比 4 更高的 RT 等级。

其余策略（`:209` 的注释）：除了 `IOPRIO_CLASS_NONE`，数字越大优先级越低，所以 `max_t` 选出 bio 和 cgroup 策略里更低的那个。bio 还是 `NONE` 时，直接采用 cgroup 的类。

```c
/* block/blk-ioprio.c:56。每个 blkg 一份，目前只有嵌入的 pd */
struct ioprio_blkg {
	struct blkg_policy_data pd;
};

/* :65。每个 blkcg 一份 */
struct ioprio_blkcg {
	struct blkcg_policy_data cpd;
	enum prio_policy prio_policy;
};
```

`blk_ioprio_init`（`:227`）在磁盘上 `blkcg_activate_policy`。没有这个 policy 时 `blkcg_set_ioprio` 的调用仍然在 `bio_set_ioprio` 里；policy 没激活则 `ioprio_blkcg_from_bio`（`:87`）拿不到 blkcg，函数开头就返回。

---

## 9.4 blkg：cgroup 和队列的交点

[cgroup/07](../cgroup/07-io-pids-misc.md) 已经写了 `io_cgrp_subsys`、`struct blkcg`、`struct blkcg_gq`（blkg），以及这些文件：

| 文件 | 登记处 | 块层里谁消费 |
|---|---|---|
| `io.stat` | `blk-cgroup.c` | `blk_cgroup_bio_start` 把字节和 io 次数加进 blkg 的 per-cpu `iostat` |
| `io.max` | `blk-throttle.c:1700` | `submit_bio_noacct` 里的 `blk_throtl_bio` |
| `io.weight`、`io.cost.qos`、`io.cost.model` | `blk-iocost.c` | rq_qos，在 `rq_qos_throttle`（分配 request 前） |
| `io.latency` | `blk-iolatency.c` | 同样是 rq_qos |
| `io.bfq.weight` 等 | `bfq-cgroup.c` | 只在调度器是 bfq 时影响 `bfq_entity.weight` |

`bio->bi_blkg` 的注释在 `struct bio`（`blk_types.h:281`）：它表示这个 bio 的 css 和 request_queue 的关联。直达设备、没有 request_queue 的 bio 可以没有 blkg。引用在 bio 释放时放下。

关联发生在 bio 进入队列之前。`bio_associate_blkg`（`blk-cgroup.c:2122`）按当前任务的 blkcg css 找到或创建这个队列上的 blkg，再写进 `bi_blkg`。

`submit_bio_noacct_nocheck`（`blk-core.c:695`）紧接着：

```text
blk_cgroup_bio_start(bio)          blk-cgroup.c:2160
  只在 cgroup v2 默认层次上记账
  root cgroup 直接返回，根的统计用整盘的 diskstats
  非 BIO_CGROUP_ACCT：把 bi_iter.bi_size 加进 bytes[读或写]
  ios[rwd]++
  置 BIO_CGROUP_ACCT，拆分出来的后半 bio 不会把字节再加一遍
blkcg_bio_issue_init(bio)          block/blk-cgroup.h:381
  bio_issue_init(&bio->bi_issue, bio_sectors(bio))
  在 64 位里记下时刻和大小，限流算延迟时用
```

读 `io.stat` 时的 rstat 刷回在 cgroup 章 §7.1，不在这里重复。

---

## 9.5 限流插在提交的哪一步

字节和 IOPS 上限在 `blk_throtl_bio`（`block/blk-throttle.h:207`，需要 `CONFIG_BLK_DEV_THROTTLING`）。没编这个选项时同一名字的 inline 直接返回 false（`:174`）。

```text
blk_should_throtl
  v1：补记 blkg 的 stat_bytes / stat_ios
  这一方向有 iops 规则，或有 bps 规则且还没 BIO_BPS_THROTTLED：返回真
为真则 __blk_throtl_bio             blk-throttle.c:2185
  超限：bio 挂在 throtl 的服务树上，submit_bio_noacct 返回
  稍后定时器再 submit_bio_noacct_nocheck
```

`BIO_BPS_THROTTLED`（`blk_types.h` 的 bio 标志）避免拆分或重提交时按字节再限一次。

权重和延迟目标不在 `submit_bio_noacct` 的这个开关里。它们实现成 `struct rq_qos`，挂在 `q->rq_qos`（`request_queue` 的字段，`blkdev.h:398`）。`blk_mq_get_new_requests` 在真正拿 tag 之前调用 `rq_qos_throttle`（`blk-mq.c:2955`）。iolatency 发现这个 cgroup 的延迟超过目标，会在这里让进程睡一会儿再分配 request。iocost 用 `bio->bi_iocost_cost`（`CONFIG_BLK_CGROUP_IOCOST` 时嵌在 bio 里，`blk_types.h:291`）做成本记账。具体的线性模型和 QoS 百分位在 `block/blk-iocost.c`，本章停在「throttle 钩子位于分配 request 之前」。

wbt（写回限流）也是一条 rq_qos，和 cgroup 无关。BFQ 初始化时 `wbt_disable_default`（`bfq-iosched.c:7405`），避免两套深度限制叠在一起。kyber 自己用域令牌，不靠 wbt 的那一层。

---

## 9.6 一条请求上能同时看见的标记

```text
任务 ioprio 或 SQE 里的 ioprio
  → bio->bi_ioprio
  → blkcg_set_ioprio 可能改类
  → rq->ioprio
  → mq-deadline 的 per_prio，或 bfq_queue 的 ioprio_class / weight

current 的 cgroup
  → bio->bi_blkg
  → blk_cgroup_bio_start 增加 io.stat
  → blk_throtl_bio 可能推迟 submit
  → rq_qos_throttle 可能推迟分配 request
```

调度器是 `none` 时，`rq->ioprio` 仍然有值，只是派发循环不读它。cgroup 的 `io.max` 和 `io.latency` 不依赖调度器名字，它们在 elevator 之前就作用了。

---

## 9.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/uapi/linux/ioprio.h` | `IOPRIO_CLASS_*` | 29 | NONE / RT / BE / IDLE |
| `include/linux/ioprio.h` | `task_nice_ioclass` | 39 | 没设过 ioprio 时按调度类给一个类 |
| `include/linux/ioprio.h` | `get_current_ioprio` | 58 | 当前任务的 ioprio |
| `block/ioprio.c` | `ioprio_set` | 69 | 系统调用 |
| `block/ioprio.c` | `__get_task_ioprio` | 149 | 显式值或 nice 换算 |
| `block/blk-core.c` | `bio_set_ioprio` | 839 | 填 `bi_ioprio` 并调用 cgroup 策略 |
| `block/blk-ioprio.c` | `blkcg_set_ioprio` | 187 | `prio.class` 抬高或压低 |
| `block/blk-ioprio.c` | `ioprio_files` | 167 | v2 的 `prio.class` |
| `block/blk-cgroup.c` | `bio_associate_blkg` | 2122 | bio 绑上 blkg |
| `block/blk-cgroup.c` | `blk_cgroup_bio_start` | 2160 | v2 `io.stat` 的 per-cpu 增量 |
| `block/blk-cgroup.h` | `blkcg_bio_issue_init` | 381 | 记下 issue 时间和扇区数 |
| `block/blk-throttle.h` | `blk_throtl_bio` | 207 | `io.max` 的入口 |
| `block/blk-throttle.c` | `__blk_throtl_bio` | 2185 | 超限则挂起 bio |
| `block/blk-mq.c` | `rq_qos_throttle` 调用点 | 2955 | iolatency / iocost / wbt |

---

**导航**：[← 多队列](./08-multiqueue-affinity.md) · [目录](./README.md) · [回写 →](./10-writeback-and-fsync.md)
