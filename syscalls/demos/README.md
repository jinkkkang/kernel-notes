# syscalls demos

用户态小程序，普通用户即可跑。对照的内核结构是 v6.6.160；本机计时打在你跑它的那颗内核上，程序自己打印 `uname.release`。

```bash
make
./01-vdso-vs-syscall
./02-bad-pointer-efault
./03-restart-eintr
```

| 程序 | 看什么 |
|------|--------|
| `01-vdso-vs-syscall` | libc `clock_gettime`（x86-64 上走 vDSO）和 `syscall(__NR_clock_gettime)` / `syscall(__NR_getpid)` 的 rdtsc 周期 |
| `02-bad-pointer-efault` | 坏用户指针进 `read`/`write`，期望 `-EFAULT`；栈上的一字节作为对照 |
| `03-restart-eintr` | 空管道上的 `read` 被 `SIGALRM` 打断。没有 `SA_RESTART` 时应为 `EINTR`；有 `SA_RESTART` 时读应在约 200 ms 后拿到写出的那一字节 |

`strace`、`perf trace`、`bpftrace` 和 PTI 开关对比不在这里跑，配方写在 [10](../10-observability.md) 和 [11](../11-perf-analysis.md)，标成 RECIPE。
