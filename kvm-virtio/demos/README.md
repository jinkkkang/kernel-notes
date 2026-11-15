# kvm-virtio demos

对照 v6.6.160 的 KVM 用户态接口。**demo 01 能编译。** 写作时没有跑通 `KVM_RUN`：当前用户打不开 `/dev/kvm`。状态是 **待真机验证**（成功路径）。02–05 只有计划，没有脚本。

不要在生产机上跑。脚本不提权、不 `modprobe`、不 `chmod`、不改 nested、不改启动参数。它会编译并执行 `01-minimal-kvm`，这个程序只对 `/dev/kvm` 做 `open` / `ioctl` / `mmap`，不加载模块。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `01-minimal-kvm.c` | 创建一个 VM、一个 vCPU、一页匿名客户机内存，执行 `OUT` 然后 `HLT` | 能编译。成功路径 **待真机验证** |
| `01-run.sh` | 编译，先测 SKIP 和错误路径，再跑真实设备 | 同上 |
| `Makefile` | `gcc -std=c11 -Wall -Wextra -Werror -O2` | — |

```text
./01-run.sh
```

不需要 root。缺设备、权限不够、或者没有虚拟化能力时，程序退出码是 77。真正的错误退出码是 1。成功是 0。脚本把真实设备那次的退出码原样返回。

## demo 01 每个 ioctl 做什么

程序不创建 in-kernel irqchip。下面的「内核里」都指 v6.6.160 的行为，和某一次运行的打印分开。

| 调用 | 作用 |
|---|---|
| `open` | 打开 `KN_KVM_DEVICE`，默认 `/dev/kvm` |
| `KVM_GET_API_VERSION` | 必须返回 12。参数必须是 0 |
| `KVM_CHECK_EXTENSION(KVM_CAP_USER_MEMORY)` | 返回 0 则 SKIP。这是登记内存槽的能力 |
| `KVM_CREATE_VM` | 类型 0。`ENODEV` / `ENXIO` / `EOPNOTSUPP` 算没有虚拟化能力，SKIP |
| `KVM_SET_TSS_ADDR` | 地址 `0xfffbd000`。unrestricted guest 或 AMD 的可选回调会直接返回 0；没有 unrestricted guest 的 Intel 用它铺实模式 TSS |
| `mmap` 匿名页 + `KVM_SET_USER_MEMORY_REGION` | GPA 0 的一页，HVA 是这块匿名内存 |
| `KVM_CREATE_VCPU` | id 0 |
| `KVM_GET_VCPU_MMAP_SIZE` + `mmap` | 映射 `struct kvm_run` 和后面的 PIO 页。长度用 ioctl 的返回值，不用 `sizeof` |
| `KVM_GET_SREGS` / `KVM_SET_SREGS` | CS selector 和 base 改成 0，清 CR0.PE 与 CR0.PG |
| `KVM_SET_REGS` | RIP 0，RDX = `0x3f8`，RAX = `'K'`，RFLAGS = 2 |
| `KVM_RUN` | 第一次应是 `KVM_EXIT_IO`。读 PIO 页并不退休指令 |
| 再一次 `KVM_RUN` | `complete_fast_pio_out` 跳过 `OUT`，接着执行 `HLT`。没有内核 LAPIC 时退出原因是 `KVM_EXIT_HLT` |

客户机字节是 `0xEE 0xF4`（`OUT DX, AL`；`HLT`）。端口故意不用 `0x7e`，避开 `KVM_X86_QUIRK_OUT_7E_INC_RIP` 那条在退出前就增加 RIP 的例外。

`KVM_EXIT_IO_OUT` 的数值是 1，`KVM_EXIT_IO_IN` 是 0。程序检查 `direction`、`size`（只允许 1、2、4）、`count`、以及 `data_offset + size * count` 落在映射长度里。对不上客户机程序的字段算错误，退出码 1，不打印 `result=ok`。

没有内核 irqchip 时，`lapic_in_kernel` 为假，HLT 才把 `KVM_EXIT_HLT` 交回用户态。创建了 `KVM_CREATE_IRQCHIP` 或 split irqchip 的虚拟机不会这样。本程序的 HLT 结果不能外推到普通 QEMU 虚拟机。

运行有 2 秒截止和最多 8 次 `KVM_RUN`。`SIGALRM`、`SIGINT`、`SIGTERM` 的处理函数只写一个 `sig_atomic_t`。到点仍没有看到 HLT，退出码是 1。

## SKIP 与错误

| 条件 | 退出码 | 打印 |
|---|---|---|
| 编译目标不是 `__x86_64__` | 77 | `SKIP: not x86_64` |
| `ENOENT` | 77 | `SKIP: no-device` |
| `EACCES` / `EPERM` | 77 | `SKIP: permission` |
| `ENODEV` / `ENXIO` / `EOPNOTSUPP`，或 `KVM_CAP_USER_MEMORY` 为 0 | 77 | `SKIP: no-virtualization` |
| 打开成功但 ioctl 不是 KVM（例如 `/dev/null`） | 1 | `ERROR:`，没有 `result=ok` |
| API 版本不是 12、映射失败、退出字段越界、截止时间到 | 1 | `ERROR:` |

非 x86_64 的分支在这次写作里没有执行：`gcc -m32` 缺少 `bits/wordsize.h`，没有做出 32 位二进制。

## 实际（写作时跑过）

`sampled on 6.12.113+`。`01-run.sh` 打印的 `runner_virt=kvm`。uid 1000 不在 `/dev/kvm` 的组里（设备是 `crw-rw---- root:103`）。没有 `sudo`，没有改设备权限，没有改 `kvm.enable_virt_at_load`。下面是这次 stdout/stderr 的原文。临时目录名来自 `mktemp`，不是固定路径。没有 `KVM_EXIT_IO` 或 `KVM_EXIT_HLT` 行。

```text
cc -std=c11 -Wall -Wextra -Werror -O2 -o 01-minimal-kvm 01-minimal-kvm.c
==== self-test: missing device ====
---- command: env KN_KVM_DEVICE=/tmp/tmp.FY7GdP1cbP/no-such-kvm ./01-minimal-kvm ----
exit=77
-- stderr --
SKIP: no-device /tmp/tmp.FY7GdP1cbP/no-such-kvm: No such file or directory
==== self-test: /dev/null is not KVM ====
---- command: env KN_KVM_DEVICE=/dev/null ./01-minimal-kvm ----
exit=1
-- stderr --
ERROR: /dev/null does not speak KVM ioctl: Inappropriate ioctl for device
==== real device ====
runner_uname=Linux cursor 6.12.113+ #1 SMP PREEMPT_DYNAMIC Fri Nov 13 20:22:28 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
runner_virt=kvm
---- command: ./01-minimal-kvm ----
exit=77
-- stderr --
SKIP: permission /dev/kvm: Permission denied
real_exit=77
KVM_RUN was not executed. Success path remains 待真机验证.
```

这次退出码是 77。它证明 SKIP 和「不是 KVM 的 fd」两条路径按上面的表工作，不证明客户机指令在这台机器上执行过。运行内核是 6.12，不能用来核对 v6.6.160 的行号。

## 预期（还没有这次输出）

在能打开 `/dev/kvm` 的机器上，成功时 stdout 应包含这些行，数值以那次内核实际写入的为准：

- `api_version=12`
- `cap_user_memory=` 非 0
- `vcpu_mmap_size=` 为正，且不小于后面的 `data_offset + size * count`
- `exit=KVM_EXIT_IO direction=KVM_EXIT_IO_OUT(1) port=1016 size=1 count=1 data_offset=<n> byte=0x4b`
- `exit=KVM_EXIT_HLT`
- `result=ok`

`port=1016` 是 `0x3f8`。`byte=0x4b` 是 `'K'`。v6.6.160 把 `data_offset` 设成 `KVM_PIO_PAGE_OFFSET * PAGE_SIZE`（1 页）。运行中的内核若不是这个版本，以它写进 `kvm_run` 的值为准，程序只做边界检查。不要把上面的尖括号换成编出来的数字。

若机器是嵌套虚拟化，贴输出时写明 `runner_virt`、`uname -a`，以及这是 VM 里的 KVM，不是裸机。

## demo 01 验收（给以后的 code agent）

在测试机上执行 `./01-run.sh`。不要 `sudo`，不要 `chmod /dev/kvm`，不要 `modprobe`，不要打开 nested。

1. 编译命令带 `-Wall -Wextra`。警告当失败。
2. 缺设备的自测退出 77，stderr 含 `SKIP: no-device`，stdout 没有 `result=ok`。
3. `KN_KVM_DEVICE=/dev/null` 退出码不是 0 也不是 77，stderr 含 `ERROR:`。
4. 真实 `/dev/kvm` 打不开时退出 77，正文保持 **待真机验证**，不补一句假的 IO/HLT。
5. 真实运行退出 0 时，把整段 stdout 贴进本文件，标 `sampled on <uname -r>`。必须同时有 `KVM_EXIT_IO` 和 `KVM_EXIT_HLT`，`direction` 为 OUT，`port` 为 1016，`size` 为 1，`byte` 为 `0x4b`。写明这是不是嵌套。不要把这次 `uname` 写进 v6.6.160 的行号旁。
6. 退出 0 但只有 IO 没有 HLT，或 HLT 出现在创建了 irqchip 的别的程序里：不要改本章去承诺「HLT 总会回到用户态」。

## 以后再写（只有名字）

| 编号 | 目的 | 前提 | 风险 | 状态 |
|------|------|------|------|------|
| 02 | 有内核 LAPIC 时看 HLT 不返回 `KVM_EXIT_HLT`，对照 halt poll 与 `sched_switch` 的 `S` | 测试机可打开 `/dev/kvm`。先读 [05](../05-vcpu-schedule.md)。脚本不得创建会一直睡死的 vCPU 而不设截止时间 | 打开 irqchip 之后 HLT 会把线程堵在内核里。必须有报警或超时。不要在生产机上留这个进程 | RECIPE / 待真机验证。不写脚本 |
| 03 | 对一条 split 队列和一条 packed 队列分别看 kick 是 `kvm_exit` 还是只有 eventfd。先确认后端是否真支持 packed | 测试虚拟机，客户机可协商 `VIRTIO_F_RING_PACKED`。vhost-net 的 `VHOST_FEATURES` 不含这一位，不能拿 vhost-net 当 packed 后端 | 改客户机队列特性可能让网卡或磁盘断。只用临时虚拟机 | RECIPE / 待真机验证。不写脚本 |
| 04 | 把 virtio-blk 一次读拆成客户机 `virtio_queue_rq`、kick、后端完成、`virtblk_done` 四段时间 | 测试盘，不是根文件系统。先读 [11](../11-virtio-blk.md) | 对系统盘做 trace 或故障注入会卡住启动盘。不要换电梯，不要 `blkdiscard` | RECIPE / 待真机验证。不写脚本 |
| 05 | 同一条 virtio 设备上对照 QEMU 用户态后端和内核 vhost（net 用 vhost-net，scsi 用 vhost-scsi）。不假设存在 vhost-blk | 宿主机能看见对应的 `vhost-*` 线程。先读 [12](../12-vhost.md) 和 [networking/14](../../networking/14-virtio-net-host-vhost.md) | 切换后端会重置设备。不要对管理网卡做。不要为了对照去加载树外模块 | RECIPE / 待真机验证。不写脚本 |

[14](../14-observability.md) 里的 `perf` 命令同样是 RECIPE。
