# 3. 层次与控制接口

**结论**：v2 树上，父节点用 `cgroup.subtree_control` **授权**子树能启用哪些控制器；子节点再在自己的 `subtree_control` 里向下传。另两条硬规矩：**top-down**（孩子不能启用父未授权的控制器）、**no internal process**（非 root 上若启用了向下的控制器，本节点一般不能再挂进程）。

> 实现：`kernel/cgroup/cgroup.c`  
> 用户文档：`Documentation/admin-guide/cgroup-v2.rst` §2.4

---

## 3.1 你天天碰的文件

| 文件 | 读写 | 含义 |
|---|---|---|
| `cgroup.controllers` | 只读 | 本节点**可用**的控制器列表 |
| `cgroup.subtree_control` | 读写 | 写 `+cpu +memory` / `-io`：允许（或收回）**子 cgroup** 启用这些控制器 |
| `cgroup.procs` | 读写 | 本节点线程组 leader 的 PID 列表；**写入 PID** = 把该进程（线程组）迁入 |
| `cgroup.threads` | 读写 | 线程粒度（threaded 模式） |
| `cgroup.events` | 只读 | 如 `populated`、`frozen` |
| `cgroup.type` | 读写 | `domain` / `threaded` 等（非 root） |
| `cgroup.controllers` 在子节点 | 只读 | 通常 ⊆ 父的 `subtree_control` 生效结果 |

本机 root 曾见：`cgroup.controllers` = `cpuset cpu io memory hugetlb pids`（见 [09](./09-observability.md)）。

---

## 3.2 Top-down 约束

父 `subtree_control` 没 `+memory`，孩子就**不能**自己变出 memory 控制器接口。  
生效掩码计算：`cgroup_calc_subtree_ss_mask()`（`cgroup.c:1623`）；写入路径：`cgroup_subtree_control_write`（`:3614`），展示：`cgroup_subtree_control_show`（`:3232`）。

---

## 3.3 No internal process 约束

大意：在默认层次上，若某非 root cgroup 的 `subtree_control != 0`（要给孩子开控制器），则**本节点不应再容纳进程**——进程应落在「叶子」或专门的叶子域。迁移前校验见 `cgroup_migrate_vet_dst`（`:2870`）附近注释与逻辑（`:2887` 一带显式提到 no-internal-process）。

**为什么**：避免「父节点既跑业务又给孩子限流」时，资源归属和委派模型变糊。systemd 常见布局：`system.slice` / `user.slice` / 具体 `*.service` 叶子。

**例外直觉**：root 特殊；threaded 子树有另一套粒度规则（`cgroup.type`）。

---

## 3.4 创建 / 迁移 / 委托

### 创建

```text
mkdir /sys/fs/cgroup/myjob
  → kernfs → cgroup_mkdir()          # cgroup.c:6116
       → 分配 struct cgroup、挂 kernfs、按父掩码准备 css
```

### 迁移（写 procs）

```text
echo <pid> > .../cgroup.procs
  → cgroup_procs_write (:5526)
  → __cgroup_procs_write (:5481)
       → cgroup_kn_lock_live
       → cgroup_procs_write_start (:3120)   # 解析 pid、拿 task
       → 权限：cgroup_attach_permissions / cgroup_procs_write_permission (:5429)
       → cgroup_attach_task (:3090)
            → cgroup_migrate_prepare_dst
            → cgroup_migrate (:3060)
            → 各 ss->can_attach / attach
```

`cgroup_attach_task` 核心骨架（`:3090`）：

```c
int cgroup_attach_task(struct cgroup *dst_cgrp, struct task_struct *leader,
		       bool threadgroup)
{
	/* 收集源 css_set → 准备目标 css_set → cgroup_migrate → 收尾 */
	...
	ret = cgroup_migrate_prepare_dst(&mgctx);
	if (!ret)
		ret = cgroup_migrate(leader, threadgroup, &mgctx);
	cgroup_migrate_finish(&mgctx);
	...
}
```

### 委托（delegation）

把某子树目录的权限交给非特权用户：核心文件带 `CFTYPE_NS_DELEGATABLE`（如 `cgroup.procs`）。容器常用 **cgroup namespace** + 委托目录，使容器内只能在自己的子树内搬家。细节以 `cgroup-v2.rst` §2.5 为准；权限检查走 `cgroup_attach_permissions`。

---

## 3.5 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `cgroup_mkdir` | `cgroup.c:6116` | 创建子 cgroup |
| `cgroup_subtree_control_write` | `:3614` | 写 `subtree_control` |
| `cgroup_calc_subtree_ss_mask` | `:1623` | 算有效子系统掩码 |
| `cgroup_procs_write` / `__cgroup_procs_write` | `:5526` / `:5481` | 迁进程入口 |
| `cgroup_procs_write_start` / `_finish` | `:3120` / `:3197` | 解析 PID、锁模式 |
| `cgroup_attach_task` | `:3090` | 执行迁移 |
| `cgroup_migrate` | `:3060` | 迁移引擎 |
| `cgroup_migrate_vet_dst` | `:2870` | 目标合法性（含 no-internal-process） |
| `cgroup_kn_lock_live` | 内部头文件声明 / `cgroup.c` | 对 kernfs 节点取活 cgroup |

---

## 3.6 操作口诀

1. 先在父上 `echo +cpu +memory > cgroup.subtree_control`  
2. 再 `mkdir` 孩子——孩子才会出现对应 `cpu.*` / `memory.*`  
3. 把进程写进**叶子**的 `cgroup.procs`  
4. 改限额写控制器文件（`memory.max`、`cpu.max` …），不是写 `subtree_control`

可选 shell 示意见 [demos/](./demos/)。

---

*对照：`kernel/cgroup/cgroup.c`。*
