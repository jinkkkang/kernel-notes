# 容器化大章

> 内核版本：**v6.6.160**（`Makefile` 为 `VERSION=6`、`PATCHLEVEL=6`、`SUBLEVEL=160`，tag 提交 `0aebf2870d53`）。下文路径、函数、结构体字段与行号对照该版本。
> 在线对照：[elixir.bootlin.com — linux/v6.6.160](https://elixir.bootlin.com/linux/v6.6.160/source)
> 用户态创建路径对照 **runc v1.5.2**（[opencontainers/runc](https://github.com/opencontainers/runc) 提交 `29dd3dc2`，tag `v1.5.2`）。只串 runc。
> 本机观测快照内核是 **6.12**（见 [11](./11-observability.md)）。数字标 sampled on 6.12；结构体与调用链以 v6.6.160 为准。

**一句话**：容器是一次 `clone`/`unshare` 拆开的命名空间，加上一棵换过根的挂载树、一个挂上去的 cgroup，以及可选的 overlay/FUSE 根和收紧后的 seccomp、能力、LSM。内核没有 `struct container`。

## 阅读顺序

```text
01 总览与章边界
  → 02 八种命名空间、nsproxy、unshare/setns
  → 03 mount ns 与 pivot_root
  → 04 pid / user / uts / ipc / time（user 映射写厚）
  → 05 net ns（veth/bridge/CNI 只点到）
  → 06 容器里的 cgroup（链回 cgroup/）
  → 07 overlayfs → 08 FUSE
  → 09 runc 创建路径
  → 10 seccomp / capabilities / LSM（薄）
  → 11 观测 → 12 放大与坑
```

先分清三件事：命名空间决定「看见哪一套对象」，cgroup 决定「记在哪棵资源树上」，根文件系统决定「`/` 背后是哪一层存储」。三者在 `task_struct` 上是不同指针。

## 子章节

| 文件 | 内容 | 厚度 |
|------|------|------|
| [01-overview.md](./01-overview.md) | 进程 → namespace → cgroup → 根文件系统 → 网络，以及和已有章的边界 | 中 |
| [02-namespaces.md](./02-namespaces.md) | `nsproxy`、八个 `CLONE_NEW*`、`unshare`/`setns`、`/proc/<pid>/ns/` | **厚** |
| [03-mount-ns-pivot-root.md](./03-mount-ns-pivot-root.md) | `copy_mnt_ns`、传播、`pivot_root` | **厚** |
| [04-pid-user-uts-ipc-time-ns.md](./04-pid-user-uts-ipc-time-ns.md) | pid/uts/ipc/time 的进入规则；user ns 的 uid/gid 映射 | **厚** |
| [05-net-ns.md](./05-net-ns.md) | `struct net`、设备进 ns；veth/bridge/CNI 只点到 | 中 |
| [06-cgroup-in-containers.md](./06-cgroup-in-containers.md) | 创建时挂组、`CLONE_INTO_CGROUP`、委托；链回 cgroup 章 | 中 |
| [07-overlayfs.md](./07-overlayfs.md) | lower/upper/work/merged、copy-up、whiteout、opaque | **厚** |
| [08-fuse.md](./08-fuse.md) | `fuse_req`、请求队列、用户态守护进程、镜像与卷 | **厚** |
| [09-runc-create-path.md](./09-runc-create-path.md) | runc v1.5.2：nsexec → 挂 ns → pivot_root → cgroup → exec | **厚** |
| [10-seccomp-caps-lsm.md](./10-seccomp-caps-lsm.md) | seccomp、能力、LSM 在容器边界上的落点 | 中 |
| [11-observability.md](./11-observability.md) | `lsns`、`/proc/<pid>/cgroup`、nsfs、overlay 模块参数、fusectl | **厚** |
| [12-perf-and-pitfalls.md](./12-perf-and-pitfalls.md) | copy-up 放大、user ns 映射、cgroup v1/v2 混用、FUSE 延迟 | 中 |

## 边界

| 本章写到哪 | 留给别章 |
|---|---|
| `nsproxy` 怎么拆、`setns`/`unshare` 的安装顺序 | `task_struct` 字段与 `copy_process` 总序：[process-management/02](../process-management/02-task-struct.md)、[04](../process-management/04-fork-clone.md) |
| pid ns 在容器里何时生效、号怎么分层 | `struct pid`、`alloc_pid`、pidfd：[process-management/03](../process-management/03-pid-namespaces.md) |
| 容器创建时把任务挂进 cgroup、委托边界 | cgroup 对象、控制器、`cgroup.subtree_control`：[cgroup/](../cgroup/README.md) |
| `pivot_root` 与 mount ns 复制时的传播 | `struct mount`、shared/slave/private/unbindable、新挂载 API：[vfs/07](../vfs/07-mount-and-ns.md) |
| overlay 的 copy-up、whiteout；FUSE 的请求队列 | VFS 对象与路径查找：[vfs/](../vfs/README.md)。页缓存见 [memory-management/08](../memory-management/08-page-cache-and-writeback.md) |
| 网卡、路由、netfilter 状态跟着 `struct net` 走 | veth 转发、bridge、CNI 在内核里留下的设备：[networking/11](../networking/11-netns-veth-bridge.md)。本章只点到入口 |
| 容器常用的 seccomp 过滤器、能力集、LSM 标签写入 | 完整 ptrace、审计、各 LSM 策略语言。进程章也把完整 seccomp 留在外面 |

## 源码地图

| 路径 | 角色 |
|------|------|
| `include/linux/nsproxy.h`、`kernel/nsproxy.c` | `nsproxy`、`copy_namespaces`、`setns` |
| `include/linux/ns_common.h`、`fs/nsfs.c`、`fs/proc/namespaces.c` | 公共 ns 头、nsfs、`/proc/<pid>/ns` |
| `fs/namespace.c` | `copy_mnt_ns`、`pivot_root` |
| `kernel/user_namespace.c`、`include/linux/user_namespace.h` | user ns 与 uid/gid 映射 |
| `kernel/pid_namespace.c`、`kernel/utsname.c`、`ipc/namespace.c`、`kernel/time/namespace.c` | pid/uts/ipc/time |
| `include/net/net_namespace.h`、`net/core/net_namespace.c`、`net/core/dev.c` | `struct net`、设备换 ns |
| `drivers/net/veth.c` | veth 对端。转发细节不在本章展开 |
| `kernel/cgroup/cgroup.c`、`kernel/cgroup/namespace.c` | `CLONE_INTO_CGROUP`、cgroup ns |
| `fs/overlayfs/` | overlay |
| `fs/fuse/fuse_i.h`、`fs/fuse/dev.c`、`fs/fuse/control.c` | FUSE 队列与 fusectl |
| `kernel/seccomp.c`、`security/commoncap.c` | seccomp、`cap_capable` |
| runc `libcontainer/nsenter/nsexec.c`、`libcontainer/rootfs_linux.go`、`libcontainer/standard_init_linux.go` | 用户态创建路径（v1.5.2） |

## 实测 vs RECIPE

- **实测**（sampled on 6.12）：[11](./11-observability.md) 里的 `lsns`、`/proc/self/ns`、`uid_map`、一次 `unshare --user --map-root-user`、cgroup2 挂载选项、`/sys/module/overlay/parameters`、fusectl。
- **RECIPE**：[12](./12-perf-and-pitfalls.md) 的 perf/ftrace，以及需要 `CAP_SYS_ADMIN` 才能做的 `pivot_root`、移网卡、建 veth。本环境没有这些能力，步骤只作配方。

## 未覆盖

- veth 的对端收发、bridge 转发数据库、CNI 插件协议。见 [networking/11](../networking/11-netns-veth-bridge.md)
- runc 以外的运行时
- ext4/xfs 盘上格式；overlay 的 upper 最终写到哪种文件系统
- 完整 LSM 策略、seccomp 通知监听进程的业务协议
- checkpoint/restore 的全套 `set_tid`

*笔记生成对照源码：Linux v6.6.160。内核树与 runc 树都不放进本仓库。*
