# 10. `bpf(2)` 与 bpffs

**结论**：所有对象操作都进 `SYSCALL_DEFINE3(bpf)`。`union bpf_attr` 比内核认识的更大时，多出来的字节必须是 0。权限按命令和程序类型拆开：`CAP_BPF` 管装载，`CAP_NET_ADMIN` 管网络类型，`CAP_PERFMON` 管跟踪类型。bpffs 是一个 `simple_fill_super` 的伪文件系统，用来把 map、程序和 link 的引用 pin 住，使它们不跟着 fd 关闭而消失。

> 系统调用在 `kernel/bpf/syscall.c:5599`，分发在 `__sys_bpf`（`:5458`）。属性尾部检查是 `bpf_check_uarg_tail_zero`（`:85`）。bpffs 的 `bpf_init` 在 `kernel/bpf/inode.c:812`，魔数 `BPF_FS_MAGIC` 是 `0xcafe4a11`（`include/uapi/linux/magic.h:95`）。

---

## 10.1 命令

`__sys_bpf` 先 `bpf_check_uarg_tail_zero`：用户态传入的 `size` 大于内核这版 `union bpf_attr` 时，多出来的区间里不能有非 0 字节，否则 `-E2BIG`。然后把属性拷进内核栈上的 `union bpf_attr`（不足的部分保持 0），`security_bpf` 之后按 `cmd` 分发（`:5477`）：

| `cmd` | 处理函数 | 作用 |
|-------|----------|------|
| `BPF_MAP_CREATE` | `map_create` | 新 map fd |
| `BPF_MAP_LOOKUP_ELEM` / `UPDATE` / `DELETE` / `GET_NEXT_KEY` | `map_*_elem` | 按 key 访问 |
| `BPF_MAP_LOOKUP_BATCH` 等四个 batch | 批量 | |
| `BPF_MAP_FREEZE` | `map_freeze` | 冻结写 |
| `BPF_PROG_LOAD` | `bpf_prog_load` | 见 [02](./02-prog-and-insn.md) |
| `BPF_OBJ_PIN` / `BPF_OBJ_GET` | `bpf_obj_pin` / `bpf_obj_get` | bpffs 路径 |
| `BPF_PROG_ATTACH` / `DETACH` / `QUERY` | `bpf_prog_attach` 等 | 无 link fd 的挂钩 |
| `BPF_PROG_TEST_RUN` | `bpf_prog_test_run` | 也叫 `BPF_PROG_RUN` |
| `BPF_PROG_GET_NEXT_ID` / `BPF_MAP_GET_NEXT_ID` / `BPF_LINK_GET_NEXT_ID` | 枚举 idr | |
| `BPF_*_GET_FD_BY_ID` | 按 id 再拿一个 fd | |
| `BPF_OBJ_GET_INFO_BY_FD` | 填 `bpf_prog_info` / `bpf_map_info` | |
| `BPF_RAW_TRACEPOINT_OPEN` | `bpf_raw_tracepoint_open` | 见 [06](./06-attach-and-link.md) |
| `BPF_BTF_LOAD` / `BPF_BTF_GET_FD_BY_ID` | `bpf_btf_load`（`:4836`） | |
| `BPF_LINK_CREATE` / `UPDATE` / `DETACH` | `link_create` 等 | |
| `BPF_ENABLE_STATS` | 打开 `bpf_stats_enabled_key` | |
| `BPF_ITER_CREATE` | iterator fd | |
| `BPF_PROG_BIND_MAP` | 把 map 算进程序的 used_maps | |

枚举顺序以 `enum bpf_cmd`（`include/uapi/linux/bpf.h:883`）为准。新命令加在末尾，旧内核看到不认识的 `cmd` 会在 switch 的 default 里失败；更大的 `bpf_attr` 靠尾部清零保持兼容。

`BPF_PROG_TEST_RUN` 的属性在匿名结构 `test` 里（`bpf.h:1506`）：`prog_fd`、`data_in` / `data_size_in`、`data_out`、`repeat`、`retval`、`duration`。XDP 和部分类型实现了 `bpf_prog_ops.test_run`，用来在没有设备的情况下跑一遍程序。返回的 `retval` 是程序的 R0。

---

## 10.2 权限

`bpf_capable()`（`include/linux/capability.h:200`）为真当且仅当有 `CAP_BPF` 或 `CAP_SYS_ADMIN`。

`sysctl_unprivileged_bpf_disabled`（`syscall.c:62`）在 `CONFIG_BPF_UNPRIV_DEFAULT_OFF` 编进内核时初值是 2，否则是 0。非 0 时，没有 `bpf_capable()` 的 `BPF_MAP_CREATE` 和 `BPF_PROG_LOAD` 返回 `-EPERM`（`:1197`、`:2628`）。

内核文档 `Documentation/admin-guide/sysctl/kernel.rst` 对三个值的定义是：

| 值 | 含义 |
|----|------|
| 0 | 允许无特权调用 `bpf()` |
| 1 | 禁止无特权调用，并且不能再改回去 |
| 2 | 禁止无特权调用；具备权限的管理员仍可写成 0 或 1 |

`bpf_unpriv_handler`（`syscall.c:5808`）在当前值已经是 1 时拒绝把它改成别的数。sysctl 节点挂在 `kernel` 表下（`bpf_syscall_sysctl_init`，`:5851`），所以路径是 `/proc/sys/kernel/unprivileged_bpf_disabled`。

即便这个 sysctl 是 0，具体类型仍有第二道门槛（`bpf_prog_load`，`:2634`）：

- 除了 `SOCKET_FILTER` 和 `CGROUP_SKB`，都要 `bpf_capable()`。
- `is_net_admin_prog_type` 为真的类型还要 `CAP_NET_ADMIN` 或 `CAP_SYS_ADMIN`。XDP、tc、sockops、netfilter 在这张表里（`:2544`）。
- `is_perfmon_prog_type` 为真的类型还要 `perfmon_capable()`。kprobe、tracepoint、raw tracepoint、`TRACING`、`LSM` 在这张表里（`:2575`）。`perfmon_capable()`（`include/linux/capability.h:195`）接受 `CAP_PERFMON` 或 `CAP_SYS_ADMIN`。

无特权能装的程序还有指令条数上限 `BPF_MAXINSNS`（4096），并且 verifier 走加锁的那条更严的路径，见 [03](./03-verifier.md)。

`BPF_OBJ_GET` 打开 bpffs 上的文件时，走的是文件系统权限。目录模式在挂载选项里，和 `CAP_BPF` 是两回事。

---

## 10.3 bpffs 与 pin

```c
/* kernel/bpf/inode.c:812 */
static int __init bpf_init(void)
{
	sysfs_create_mount_point(fs_kobj, "bpf");   /* /sys/fs/bpf 这个挂载点 */
	register_filesystem(&bpf_fs_type);
}
```

`bpf_fill_super`（`:749`）调用 `simple_fill_super(sb, BPF_FS_MAGIC, ...)`，根 inode 的 `i_op` 换成 `bpf_dir_iops`，模式加上 `S_ISVTX` 和挂载选项里的 `mode`。文件类型是 map、程序或 link：inode 的私有数据握着对应对象的引用。

`BPF_OBJ_PIN`（`bpf_obj_pin`，`syscall.c:2807`）把 `bpf_fd` 指着的对象 pin 到 `pathname`。`BPF_F_PATH_FD` 置位时路径相对于 `path_fd`，否则相对于当前目录（`AT_FDCWD`）。`BPF_OBJ_GET` 按路径再拿一个 fd，`file_flags` 里的只读只写要和对象允许的访问一致。

pin 住的对象在最后一个 fd 关闭后仍然活着，因为 bpffs 的 inode 持有引用。删除这个目录项才会 `put`。升级程序时常见的做法是 pin 到一个固定路径，新进程 `BPF_OBJ_GET`，而不是把 fd 从父进程继承下来。

`BPF_OBJ_GET_INFO_BY_FD` 填充的结构随 fd 种类变化：程序是 `bpf_prog_info`（类型、`id`、`jited_prog_len`、map id 列表、名字），map 是 `bpf_map_info`。这些结构和 fdinfo 的字段对应，见 [12](./12-observability.md)。

id 存在三张 idr 里：`prog_idr`、`map_idr`、`link_idr`（`syscall.c:55`）。`BPF_PROG_GET_FD_BY_ID` 会再拿一次引用，所以枚举本身不会让对象提前释放。没有 `CAP_SYS_ADMIN` 时，按 id 打开别人的程序会在 `security_bpf` 或查找函数里失败；本章不展开 LSM hook 的名字。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
