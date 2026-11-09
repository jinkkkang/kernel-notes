# 10. uprobe

**结论**：uprobe 打在一个 inode 的文件偏移上，不是内核符号。`uprobe_events` 的一行文本登记动态事件，enable 之后 `uprobe_register` 按 inode 和 offset 找到映射了这块文件的 VMA，在对应的用户地址放断点。返回探针是 uretprobe。和 kprobe 共用 fetch 参数的写法，断点落在用户态指令流里。

> 事件解析在 `kernel/trace/trace_uprobe.c`，语法注释在 `:530`。挂点是 `uprobe_register`（`kernel/events/uprobes.c:1205`）。

---

## 10.1 和 kprobe 对照

| | kprobe | uprobe |
|---|---|---|
| 位置 | 内核地址或 `KSYM+offset` | `PATH:OFFSET`，路径对应一个 inode |
| 事件系统名 | `kprobes` | `uprobes`（`UPROBE_EVENT_SYSTEM`，`trace_uprobe.c:25`） |
| 注册 | `register_kprobe` | `uprobe_register(inode, offset, consumer)` |
| 返回探针 | `r` 或 `%return` | 同一行里的 `r` 或 `%return` |
| 记录头 | `trace_entry` + `__probe_ip` | `uprobe_trace_entry_head` 里带 `vaddr[]` |
| 谁执行断点 | 内核态 int3 / 优化跳转 | 命中该 VMA 的用户态线程 |

`uprobe_trace_entry_head`（`trace_uprobe.c:27`）把 `trace_entry` 放在前面，后面是柔性数组 `vaddr[]`。返回探针多一个槽：`SIZEOF_TRACE_ENTRY`（`:32`）在 return 时按两个 `unsigned long` 计算，否则一个。

---

## 10.2 文本格式

注释（`:530`）：

```text
p|r[:[GRP/][EVENT]] PATH:OFFSET[%return][(REF)] [FETCHARGS]
```

`PATH` 是文件路径，`OFFSET` 是文件内偏移，不是进程虚拟地址。进程把这段文件映射到哪段 VMA，由 `register_for_each_vma` 去对。`%return` 表示返回探针。`(REF)` 是引用计数偏移，对应 `uprobe_register_refctr`（`uprobes.c:1212`）的 `ref_ctr_offset`，用来支持 SDT 一类的引用计数。

`FETCHARGS` 和 kprobe 事件同一套：寄存器、栈、内存解引用、别名。执行函数也叫 `process_fetch_insn`，定义在 `trace_uprobe.c:214`，和 kprobe 那份不是同一个函数。用户态地址的读取要能处理缺页，这条路径的细节不在本章。

`trace_uprobe_create`（`:721`）解析文本。同名事件已存在且类型不同（一个是入口、一个是返回）时错误号 `DIFF_PROBE_TYPE`（`:502`）。类型相同则 `append_trace_uprobe`，把新的消费者挂到已有事件上。

---

## 10.3 `uprobe_register`

```text
uprobe_register(inode, offset, uc)          uprobes.c:1205
  __uprobe_register(inode, offset, 0, uc)
    按 inode+offset 找到或创建 uprobe
    consumer_add
    register_for_each_vma
```

`struct uprobe_consumer *uc` 是回调。trace 事件的 consumer 在命中时走和 kprobe 事件相同的「reserve、填 fetch、commit」。`uprobe_register` 与 `delete` 可能并发，`__uprobe_register` 在 `uprobe_is_active` 为假时返回 `-EAGAIN` 并重试（`:1184`）。

断点写在映射该 inode 的用户页上。哪个进程、哪段 VMA 包含这个 offset，由 VMA 的文件映射决定。本章不写 ptrace 单步、也不写用户态调试器如何把 offset 算出来。

enable 与创建仍是两步：`uprobe_events` 里有这一行只表示事件存在；`events/uprobes/<name>/enable` 才调用 `uprobe_register`。文件节点和 `kprobe_events` 一样在顶层 tracefs。instance 可以单独 enable。

---

## 10.4 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `trace_uprobe_create` | `trace_uprobe.c:721` | 解析一行 |
| `process_fetch_insn` | `:214` | uprobe 的参数抓取 |
| `uprobe_register` | `uprobes.c:1205` | inode + offset 挂消费者 |
| `uprobe_register_refctr` | `:1212` | 带引用计数偏移 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
