# 2. 环缓冲

**结论**：v6.6.160 里 ftrace 的环对象是 `struct trace_buffer`。每个在线 CPU 一份 `ring_buffer_per_cpu`，写者在本 CPU 上 `ring_buffer_lock_reserve` 预留，填完 `ring_buffer_unlock_commit`；读者用单独的 `reader_page` 和头页交换。`RB_FL_OVERWRITE` 决定写满之后覆盖旧页还是丢掉新事件。

> 头文件 `include/linux/ring_buffer.h`。实现 `kernel/trace/ring_buffer.c`。`struct trace_buffer` 在 `:542`，`struct ring_buffer_per_cpu` 在 `:494`，`struct ring_buffer_event` 在头文件 `:15`。分配是 `__ring_buffer_alloc`（`:1821`），宏 `ring_buffer_alloc`（头文件 `:95`）给每块环单独的 lockdep key。

文件名和函数名仍带 `ring_buffer`。对外的缓冲类型名是 `trace_buffer`。

---

## 2.1 事件头

一条记录以 `ring_buffer_event` 开头。`type_len` 占 5 位，`time_delta` 占 27 位，后面是柔性数组 `array[]`。

```c
/* include/linux/ring_buffer.h:15
 * 注释写明不要直接依赖布局，用下面的访问函数。
 */
struct ring_buffer_event {
	u32		type_len:5, time_delta:27;
	u32		array[];
};
```

`type_len` 的取值在 `enum ring_buffer_type`（`:55`）：

| 值 | 含义 |
|----|------|
| `0 … RINGBUF_TYPE_DATA_TYPE_LEN_MAX`（28） | 数据。非 0 时长度是 `type_len << 2`；为 0 时 `array[0]` 存真实长度 |
| `RINGBUF_TYPE_PADDING` | 页尾填充，或被 discard 的事件。`time_delta == 0` 表示纯填充 |
| `RINGBUF_TYPE_TIME_EXTEND` | 时间增量放不进 27 位。`array[0]` 是高位，整条 8 字节 |
| `RINGBUF_TYPE_TIME_STAMP` | 绝对时间戳，布局与 TIME_EXTEND 相同 |

27 位来自 `TS_SHIFT`（`ring_buffer.c:295`）。`test_time_stamp`（`:372`）看增量是否碰到 `TS_DELTA_TEST`。放不下就先写一条 extend/stamp，再写数据。绝对时间戳的拼法在 `rb_event_time_stamp`（`:299`）：`array[0] << 27` 加上 `time_delta`。

页上的数据头是 `buffer_data_page`（`:315`）：`time_stamp` 是这一页的基准，`commit` 是已经提交的偏移，`data[]` 才是事件。`buffer_page`（`:329`）包着它，并且必须把 `list` 放在第一个成员：页按 cache line 对齐，指针低位被拿来做无锁标志。

```c
/* kernel/trace/ring_buffer.c:315 */
struct buffer_data_page {
	u64		 time_stamp;	/* 本页时间基准 */
	local_t		 commit;	/* 已提交的写偏移 */
	unsigned char	 data[] RB_ALIGN_DATA;
};

/* :329 。list 必须在最前，低位用来做 head 页标志 */
struct buffer_page {
	struct list_head list;
	local_t		 write;		/* 下一次写入的下标 */
	unsigned	 read;		/* 读者下标 */
	local_t		 entries;	/* 本页事件数 */
	unsigned long	 real_end;
	struct buffer_data_page *page;
};
```

`commit` 的最高位另有用途：`RB_MISSED_EVENTS`（`:311`，bit 31）表示有事件被覆盖，`RB_MISSED_STORED`（`:313`，bit 30）表示丢失计数存在页尾。`ring_buffer_print_page_header`（`:382`）把 `commit` 的那 1 个字节描述成用户态看到的 `overwrite` 字段。

---

## 2.2 每 CPU 的环

```c
/* kernel/trace/ring_buffer.c:494
 * 空缓冲的判断：head_page == tail_page 且 head == tail。
 */
struct ring_buffer_per_cpu {
	int				cpu;
	atomic_t			record_disabled;
	atomic_t			resize_disabled;
	struct trace_buffer		*buffer;
	raw_spinlock_t			reader_lock;	/* 串行读者 */
	arch_spinlock_t			lock;
	struct buffer_page		*head_page;	/* 从这里读 */
	struct buffer_page		*tail_page;	/* 写到这里 */
	struct buffer_page		*commit_page;	/* 已提交到的页 */
	struct buffer_page		*reader_page;	/* 读者手里的那一页，不在环链表上 */
	unsigned long			lost_events;
	local_t				entries;
	local_t				overrun;	/* overwrite 模式下盖掉的页 */
	local_t				commit_overrun;
	local_t				dropped_events;	/* 非 overwrite 时丢掉的新事件 */
	local_t				committing;
	local_t				commits;
	rb_time_t			write_stamp;
	rb_time_t			before_stamp;
	u64				read_stamp;
	/* 其余：页计数、resize 的 new_pages、irq_work */
};

/* :542 */
struct trace_buffer {
	unsigned			flags;		/* RB_FL_OVERWRITE */
	int				cpus;
	atomic_t			record_disabled;
	cpumask_var_t			cpumask;
	struct mutex			mutex;
	struct ring_buffer_per_cpu	**buffers;
	u64				(*clock)(void);
	bool				time_stamp_abs;
};
```

`__ring_buffer_alloc` 把 `clock` 设成 `trace_clock_local`（`:1841`），`flags` 原样保存。头文件 `enum ring_buffer_flags`（`ring_buffer.h:203`）目前只有 `RB_FL_OVERWRITE`。`ring_buffer_change_overwrite`（`ring_buffer.c:2378`）事后改这一位。

时间戳读取在 `rb_time_stamp`（`:1201`）。`CONFIG_RETPOLINE` 打开且 clock 仍是 `trace_clock_local` 时直接调它，避免间接调用。

写者快路径不拿 `reader_lock`。`ring_buffer_lock_reserve` 用 `preempt_disable_notrace` 把写者钉在当前 CPU，再用 `trace_recursive_lock`（`:3292`）按 NMI / IRQ / softirq / normal 的位挡住同一上下文重入。`RB_CTX_*`（`:457`）就是这些位。TRANSITION 位允许多一次「抢占计数还没更新」的误报，注释在 `:3266`。

---

## 2.3 reserve 与 commit

```text
ring_buffer_lock_reserve(buffer, length)          :3792
  preempt_disable_notrace
  buffer->record_disabled 或本 CPU record_disabled  → NULL
  length > BUF_MAX_DATA_SIZE                         → NULL
  trace_recursive_lock                               → NULL
  rb_reserve_next_event                              :3701
    __rb_reserve_next                                :3553
      页尾放不下 → rb_move_tail                       :2745
  失败则 trace_recursive_unlock + preempt_enable_notrace
```

`length` 是数据长度，不含事件头（函数注释 `:3786`）。成功时返回的指针还没提交，必须配 `ring_buffer_unlock_commit`（`:3382`），或者走 discard。返回 NULL 表示什么都没留下，调用方不要再 commit。

```text
ring_buffer_unlock_commit                         :3382
  rb_commit                                         :3181
    local_inc(entries)
    rb_end_commit                                   把 commit 推到 write
  rb_wakeups                                        有人在等就 irq_work
  trace_recursive_unlock
  preempt_enable_notrace
```

`rb_end_commit` 的注释（`:3084` 一带）说明：本 CPU 上的嵌套写者按栈结束，外层提交时可以把被打断的内层一起提交。所以 `committing` / `commits` 是一对计数，不是互斥锁。

`ring_buffer_write`（`:3939`）把 reserve、拷贝、commit 合成一次调用。

---

## 2.4 写满：overwrite 还是丢掉

`rb_move_tail`（`:2745`）在下一页就是 commit 页时认为写者绕了一整圈还没提交完，`commit_overrun` 加一然后放弃（`:2763`）。下一页是 head 页时表示缓冲满了：

```text
没有 RB_FL_OVERWRITE
  dropped_events++          :2794
  停在这里，不挪 head
有 RB_FL_OVERWRITE
  rb_handle_head_page       :2501 / :2798
  把最老的一页让出来
```

`__ring_buffer_alloc` 的注释（`:1816`）把这两条写死了：overwrite 盖旧数据，否则 tail 碰到 head 就丢新数据。tracefs 的 `per_cpu/cpuN/stats` 把这两个计数印成 `overrun` 和 `dropped events`，打印函数是 `tracing_stats_read`（`trace.c:8511`，`:8532` 与 `:8561`）。

---

## 2.5 读者页交换

读者不在写者正在写的链表上直接扫描。`rb_get_reader_page`（`:4542`）拿着 `cpu_buffer->lock`：

1. `reader_page` 上还有未读数据就直接返回。
2. 否则把这块空页插到 head 前面，`rb_head_page_replace`（`:4636`）试着把它和 head 对调。
3. 对调成功后，旧的 head 变成新的 `reader_page`，`pages_read` 加一。
4. 若这中间 `overrun` 变了，差值记进 `lost_events`（`:4658`）。

循环超过 3 次会 `RB_WARN_ON`（`:4560`）。写者换页用的 cmpxchg 是读者这边的内存屏障，注释在 `:4614`。

`ring_buffer_peek`（头文件声明 `:124`，实现 `:4973`）看下一条但不消费。`ring_buffer_consume`（`:5048`）读走一条。`trace_pipe` 走消费，`trace` 文件走迭代器 `ring_buffer_read_start`（`:5100`）。

---

## 2.6 discard

过滤掉的事件可以不提交成一条有效记录。`ring_buffer_discard_commit`（`:3893`）要求还没 commit，此时抢占仍关着（`:3910` 的 `RB_WARN_ON` 检查 `committing`）。

```text
rb_event_discard                         :3168
  把事件改成 RINGBUF_TYPE_PADDING
  array[0] 记下原来的数据长度
  time_delta 为 0 时写成 1（非 0 才表示「这是被丢掉的事件」）
rb_try_to_discard                        :3008
  若 tail 仍停在这条事件末尾
  local_try_cmpxchg 把 write 拨回去
  同时把 before_stamp 清 0，下一条用绝对时间戳
rb_end_commit                            无论是否拨回都做
```

拨回成功时，commit 追上缩短后的 write，这条 padding 不在已提交区间里。拨回失败说明后面已经有人写了，padding 留在环里，读者会跳过它。头文件 `:68` 的注释要求：调用了 discard 就不要再 `ring_buffer_unlock_commit`。

function tracer 的 `call_filter_check_discard`（`trace.c:557`）在谓词不匹配时调用 `__trace_event_discard_commit`（`trace.h:1392`），后者对普通事件调用 `ring_buffer_discard_commit`。`trace_function`（`trace.c:3072`）在过滤失败时走这条丢弃，成功才 `__buffer_unlock_commit`。

---

## 2.7 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `__ring_buffer_alloc` | `ring_buffer.c:1821` | 按每 CPU 字节数分配，`flags` 含 `RB_FL_OVERWRITE` |
| `ring_buffer_resize` | `:2162` | 调整页数。tracefs 的 `buffer_size_kb` 最终调到这里 |
| `ring_buffer_lock_reserve` | `:3792` | 预留。失败返回 NULL |
| `ring_buffer_unlock_commit` | `:3382` | 提交并唤醒等待者 |
| `ring_buffer_discard_commit` | `:3893` | 未提交事件改成 padding 或拨回写指针 |
| `rb_move_tail` | `:2745` | 换页；满了则覆盖或丢弃 |
| `rb_get_reader_page` | `:4542` | 读者用空页换出 head |
| `ring_buffer_record_off` | `:4069` | 关掉记录。`tracing_on` 的 0 走这一类开关 |
| `ring_buffer_event_data` | 头文件 `:63`，实现 `:283` | 从事件头跳到数据体 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
