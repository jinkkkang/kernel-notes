# init-modules-wq demos

对照 v6.6.160 的 `finit_module` 路径和 cmwq。**本环境没有 `insmod`。** demo 01 源码写完，状态是 **待真机验证** / RECIPE。

需要 `CAP_SYS_MODULE`，以及 `/lib/modules/$(uname -r)/build` 与正在运行的内核一致。不要在生产机上加载。

| 文件 | 做什么 | 状态 |
|------|--------|------|
| `01-kmod-wq.c` | 模块 `kn_demo_wq`：`module_param_string(msg)`，`alloc_workqueue("kn-demo-wq", WQ_UNBOUND \| WQ_SYSFS, 0)`，`INIT_WORK` + `queue_work`，exit 里 `destroy_workqueue` | **待真机验证**。源码在，没有加载 |
| `01-run.sh` | 默认 DRY-RUN。`KN_DEMO_LOAD=1` 才 `make` / `insmod` / 检查 / `rmmod` | **待真机验证** |
| `Makefile` | out-of-tree，`KDIR=/lib/modules/$(uname -r)/build`，产物 `kn_demo_wq.ko` | 未在本环境执行 |

```text
./01-run.sh
# 测试机、非生产：
sudo KN_DEMO_LOAD=1 ./01-run.sh
```

`msg` 默认是脚本的 token `kn-demo-<pid>`，也可以 `KN_DEMO_MSG=hello`（不要含空格）。三条 `pr_info` 都带这个字符串，用来从 dmesg 里把一次运行抠出来。

## demo 01 验收（给以后的 code agent）

加载成功、`rmmod` 之后，把整段 stdout 贴回本章时标 `sampled on <uname -r>`。下面是要出现的事实，不是一份编好的输出：

1. `/proc/modules` 有一行以 `kn_demo_wq ` 开头，状态字段是 `Live`（`kernel/module/procfs.c` 的 `m_show`）。`rmmod` 之后这一行消失。
2. `/sys/module/kn_demo_wq/` 存在，`parameters/msg` 的内容是这次的 token。`rmmod` 之后目录消失。
3. `/sys/bus/workqueue/devices/kn-demo-wq/` 存在（`workqueue_sysfs_register` 的注释）。`per_cpu` 为 `0`（队列带了 `WQ_UNBOUND`）。`max_active` 读到的是 `saved_max_active`；创建时传了 0，源码会换成 `WQ_DFL_ACTIVE`（256），以文件内容为准。`rmmod` 之后目录消失。
4. `/sys/devices/virtual/workqueue/kn-demo-wq/`：**在则记下来，不在不算失败。** 脚本只报告。
5. dmesg 里有三条含本次 token 的行，前缀分别是 `kn_demo_wq: init`、`kn_demo_wq: work`、`kn_demo_wq: exit`。init 的 `comm`/`pid` 是加载者；work 的 `comm`/`pid` 是执行它的 worker，`cpu` 是 `raw_smp_processor_id()`。不要预填这三行的数字。
6. 失败（没有头文件、没有 `CAP_SYS_MODULE`、`insmod` 返回非 0）就保持 RECIPE，在 backlog `IMW-KMOD-WQ` 里写 blocker，不要编 dmesg。

## 以后再写（只有名字）

| 编号 | 内容 |
|------|------|
| 02 | 只读启动日志 / cmdline；可选 `initcall_debug`（可能要重启） |
| 03 | 两个模块，用导出符号做依赖 |
| 04 | `delayed_work` 的 cancel 与 flush |
| 05 | `max_active` 积压和观测 |
