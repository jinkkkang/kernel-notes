# 9. kprobe 事件

**结论**：`kprobe_events` 把一行文本变成一个动态的 `trace_event_call`，标志带 `TRACE_EVENT_FL_KPROBE`。它内部调用 `register_kprobe` 或 `register_kretprobe`。参数抓取在探针命中时从寄存器、栈或内存读出来，跟在 `trace_entry` 后面写进环。perf 和 BPF 若要挂这根探针，从这里的 perf event / 事件 id 接走，程序本身不在本章。

> 解析在 `kernel/trace/trace_kprobe.c`。`__trace_kprobe_create` 的语法注释在 `:746`。文件节点在 `:1960`。抓取指令的执行在同文件的 `process_fetch_insn`（`:1318`）。通用的探针字段在 `kernel/trace/trace_probe.c`。

---

## 9.1 文本格式

`kprobe_events` 只建在顶层 tracefs（`trace_kprobe.c:1960`），权限 `TRACE_MODE_WRITE`。写一行进 `create_or_delete_trace_kprobe`（`:1000`），再 `trace_kprobe_create`（`:995`）到 `__trace_kprobe_create`。

注释里的语法（`:749`）：

```text
p[:[GRP/][EVENT]] [MOD:]KSYM[+OFFS]|KADDR [FETCHARGS]
r[MAXACTIVE][:[GRP/][EVENT]] [MOD:]KSYM[+0] [FETCHARGS]
p[:[GRP/][EVENT]] [MOD:]KSYM[+0]%return [FETCHARGS]
```

`p` 是 kprobe，`r` 是 kretprobe。组默认 `kprobes`（`KPROBE_EVENT_SYSTEM`，`:25`）。不写事件名时内核起一个。`MAXACTIVE` 传给 `kretprobe.maxactive`，上限 `KRETPROBE_MAXACTIVE_MAX` 4096（`:26`）。

抓取参数（`:757`）：

| 写法 | 取什么 |
|------|--------|
| `$retval` | 返回值。用在 kretprobe |
| `$stack` / `$stackN` | 栈地址，或第 N 个栈槽 |
| `$comm` | `current` 的 comm |
| `@ADDR` / `@SYM[+offs]` | 内核地址上的内存。`SYM` 是数据符号 |
| `%REG` | 名为 REG 的寄存器 |
| `+offs(ARG)` / `-offs(ARG)` | 先求 ARG 再解引用 |
| `NAME=FETCHARG` | 字段别名 |
| 类型前缀 | 字段类型，跟在别名或参数前 |

以 `-` 开头的行是删除。`create_or_delete_trace_kprobe`（`:1004`）看到 `raw_command[0] == '-'` 就走删除。自测里用的是 `-:testprobe`（`:2102`）。本机实测 `echo '-:myprobe' >> kprobe_events` 删掉了 `p:kprobes/myprobe`，之后文件长度为 0。

启动参数 `kprobe_event=` 进 `set_kprobe_boot_events`（`:31`），缓冲长度 `COMMAND_LINE_SIZE`。

---

## 9.2 从一行到 `register_kprobe`

`__trace_kprobe_create` 解析出符号、偏移和 fetch 参数后分配 `trace_kprobe`，设置打印格式 `traceprobe_set_print_fmt`，再 `register_trace_kprobe`（`:969`）。失败时：`-EILSEQ` 记成 `BAD_INSN_BNDRY`，`-ENOENT` 记成 `BAD_PROBE_ADDR`，其它（除了 `-ENOMEM` 和 `-EEXIST`）记成 `FAIL_REG_PROBE`。这些字符串进 tracefs 的 `error_log`。

`kprobe_register`（`:1687`）是这个动态事件的 `trace_event_class.reg`。event 的 `enable` 被写 1 时，它调用 [08](./08-kprobes-core.md) 的 `register_kprobe` 或 `register_kretprobe`。enable 为 0 时注销。所以「文件里有这一行」和「断点已经写进指令流」是两步：创建只登记事件，enable 才 arm。

事件标志包含 `TRACE_EVENT_FL_DYNAMIC` 和 `TRACE_EVENT_FL_KPROBE`（`include/linux/trace_events.h:361`）。动态事件用 `refcnt` 而不是模块引用，见 `trace_event_call` 里的联合体。

kretprobe 的 entry 回调是 `trace_kprobe_entry_handler`（`trace_kprobe.c:743` 声明）。它在进入时按 fetch 程序把参数存进 `kretprobe_instance->data`，返回时和 `$retval` 拼成一条记录。`data_size` 来自 `traceprobe_get_entry_data_size`（`:961`）。

---

## 9.3 记录长什么样

没有 fetch 参数时，记录在 `trace_entry` 之后只有探针地址。本机 6.12 上 `p:myprobe __x64_sys_getpid` 的 `format` 是：

```text
field:unsigned short common_type;       offset:0;  size:2;
field:unsigned char  common_flags;      offset:2;  size:1;
field:unsigned char  common_preempt_count; offset:3; size:1;
field:int            common_pid;        offset:4;  size:4;
field:unsigned long  __probe_ip;        offset:8;  size:8;

print fmt: "(%lx)", REC->__probe_ip
```

`common_*` 是 `struct trace_entry`（[06](./06-tracepoint-and-TRACE_EVENT.md)）。`__probe_ip` 是这次命中的地址。6.12 的文本输出把这个地址印成符号；当次两行是 `myprobe: (__do_sys_getpid+0x0/0x30)`。这是 sampled on 6.12 的 kallsyms 显示，不要拿去对 v6.6.160 的行号。命令本身写的是 `__x64_sys_getpid`。

加了 fetch 参数之后，字段接在 `__probe_ip` 后面，名字来自 `NAME=` 或参数文本。类型和偏移以当时的 `format` 文件为准。

`process_fetch_insn`（`trace_kprobe.c:1318`）执行编译好的 `fetch_insn` 序列，把值写进记录。它在探针上下文里跑，约束和 `pre_handler` 一样：不能睡眠。

---

## 9.4 交给 perf 和 BPF 的位置

event 目录里的 `id` 是 perf 打开 `PERF_TYPE_TRACEPOINT` 时用的 config。kprobe 也可以走 perf 的 kprobe PMU，由 `perf_event_open` 的 `attr` 指定符号。两种 fd 都能再 `PERF_EVENT_IOC_SET_BPF` 或 `bpf_perf_link_attach`。

本章停在：事件已经登记，`enable` 会 `register_kprobe`，记录格式在 `format` 里。BPF 程序的类型、`pt_regs` ctx、`trace_call_bpf` 的返回值，在 [ebpf/06](../ebpf/06-attach-and-link.md)。`perf` 怎样把样本放进自己的环，不在本章。

`kprobe_profile` 给出每根探针的 hits 和 miss。miss 对应 `kprobe.nmissed`（[08](./08-kprobes-core.md)）。

---

## 9.5 关键函数

| 函数 | 位置 | 作用 |
|------|------|------|
| `__trace_kprobe_create` | `trace_kprobe.c:746` | 解析一行，注册事件 |
| `trace_kprobe_create` | `:995` | 包装 `trace_probe_create` |
| `kprobe_register` | `:1687` | enable 时 `register_kprobe` |
| `process_fetch_insn` | `:1318` | 执行参数抓取 |
| `trace_create_file("kprobe_events")` | `:1960` | 顶层文件节点 |

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
