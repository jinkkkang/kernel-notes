# vfs demos

对照内核 **v6.6.160** 的 splice / sendfile / `copy_file_range` 路径。用户态示例只用 libc。

| 文件 | 做什么 |
|---|---|
| [01_splice_sendfile_copy.c](./01_splice_sendfile_copy.c) | 在 tmpfs 上把 256KiB 文件分别用 `read`+`write`、`splice`、`sendfile`、`copy_file_range` 拷一份，并读 `/proc/self/io` |
| [splice_kprobe.bt](./splice_kprobe.bt) | RECIPE。`kprobe:do_splice` 和三个系统调用跟踪点。本环境没有跑 |

用户态程序在写笔记时用本机 gcc 编过并跑过。主机内核是 6.12.94+，**sampled on 6.12**。默认目录 `/dev/shm`（tmpfs）。`SPLICE_DEMO_DIR` 可以改到别的可写目录；那个目录若不是 tmpfs，`read_bytes` 可能不再是 0。

```bash
gcc -Wall -Wextra -O2 -o /tmp/splice_demo 01_splice_sendfile_copy.c && /tmp/splice_demo
```

```bash
# RECIPE：本环境没有 bpftrace，/sys/kernel/tracing 是 Permission denied。
bpftrace splice_kprobe.bt
```

两个普通文件直接 `splice` 会失败，程序把它打印成 `EINVAL`。文件到文件用 `sendfile` 或 `copy_file_range`。
