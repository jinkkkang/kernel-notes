# cgroup demos（用户态）

> 这些是 **shell 示意**，帮助把 [03](../03-hierarchy-control.md) 的文件语义跑通。  
> **不要**在共享生产机器上对不属于你的树 `echo` 限额。  
> 本 box 上 `/sys/fs/cgroup` 多为委托/只读策略；脚本默认 **DRY-RUN**（只打印将要执行的命令）。

## 文件

| 文件 | 说明 |
|---|---|
| `01_cg_smoke.sh` | 打印当前 cgroup、controllers；可选在可写目录建子组并把自己写进 `cgroup.procs` |

## 用法

```bash
# 只看、不改
./01_cg_smoke.sh

# 明确要求尝试写入（仍会先检查目录可写）
CG_DEMO_ROOT=/sys/fs/cgroup/agent CG_DEMO_WRITE=1 ./01_cg_smoke.sh
```

内核模块 demo **不在本章**（cgroup 核心是 sysfs/kernfs 状态机；控制器逻辑在 mm/sched/block）。
