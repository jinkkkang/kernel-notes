# 8. dcache、icache 与回收

**结论**：没用完的 `dentry` 和 `inode` 挂在各自超级块的 LRU 上。内存回收调用每个超级块的 shrinker `super_cache_scan`，它先 `prune_dcache_sb` 再 `prune_icache_sb`，因为 inode 还被 dentry 牵着。负 dentry 也在同一条 LRU 上，只是 `d_inode` 为空。

> shrinker 回调在 `fs/super.c:184`。dentry LRU 在 `fs/dcache.c`，inode LRU 在 `fs/inode.c`。`shrink_slab` 何时走到这里见 [MM §10.1](../memory-management/10-01-overview.md)（`mm/vmscan.c:1040`）。

C 新手把 LRU 理解成「最近没用的放一边，紧的时候从那一边扔」。引用计数还大于 0 的对象不算没用，隔离函数看见计数就把它从 LRU 摘掉，而不是释放。

---

## 8.1 两套哈希，两条 LRU

查找用的哈希和回收用的 LRU 不是同一条链。

| 结构 | 哈希 | LRU |
|---|---|---|
| dentry | 全局 `dentry_hashtable`（`fs/dcache.c:101`），键是父指针加名字哈希 | 每超级块 `s_dentry_lru`（`super_block` 里的 `list_lru`） |
| inode | 全局 `inode_hashtable`（`fs/inode.c:59`），键是超级块加 `i_ino` | 每超级块 `s_inode_lru` |

`d_hash`（`dcache.c:103`）是 `dentry_hashtable + (hash >> d_hash_shift)`。inode 的桶下标由 `hash(sb, i_ino)` 算（`fs/inode.c:543`）。`iget_locked`（`inode.c:1337`）先在这张表里找，没有再分配并置 `I_NEW`。

dentry 的 `d_lru` 和「正在 in-lookup 时的等待队列」共用一个 union（`dcache.h:100`）。一个 dentry 不会同时在 LRU 上又在等人查找。

---

## 8.2 引用掉到 0 时 dentry 去哪

`dput`（`fs/dcache.c:901`）循环处理：可能一次丢掉的是孩子，接着还要看父目录。

```text
dput
  fast_dput 成功：引用没到 0，或者到 0 但可以留在哈希里，直接返回
  retain_dentry：留着，解开 d_lock
  否则 dentry_kill，返回下一个要 dput 的 dentry（常常是父）
```

`fast_dput` 的注释（`:804` 起）写了一种常见情况：引用到 0，dentry 仍在哈希里、没有 `d_delete`、已经在 LRU 上，就可以把引用留在 0，不必再拿 `d_lock`。

进 LRU 的函数是 `d_lru_add`（`:428`）：

```text
置 DCACHE_LRU_LIST
nr_dentry_unused 加一
若 d_is_negative：nr_dentry_negative 也加一
list_lru_add_obj(&sb->s_dentry_lru, &dentry->d_lru)
```

`nr_dentry_negative` 只统计「在超级块 LRU 上的负项」。注释（`:419`）写明：从 LRU 挪到 shrink 链表时不把这个计数减了又加，避免无谓的一减一加。`d_lru_shrink_move`（`:482`）在挪走时如果是负项就减计数。`__d_clear_type_and_inode`（`:352`）把正项变成负项时，若它正在 LRU 上（有 `DCACHE_LRU_LIST`、没有 `DCACHE_SHRINK_LIST`），就把负项计数加一。

`DCACHE_DONTCACHE`（`dcache.h:173`）的注释是最后一次 `dput` 时从内存清掉。`I_DONTCACHE` 是 inode 侧的对应位（`fs.h:2292`）。

---

## 8.3 负 dentry

负 dentry 表示「在这个父目录下，这个名字确定没有」。`d_inode` 是 `NULL`，类型位是 `DCACHE_MISS_TYPE`（值 0，`dcache.h:199`）。`d_is_negative`（`:433`）看的就是这个类型位。

它被放进同一张 `dentry_hashtable`。下次 `lookup_fast` 命中它，就不必再调用 `i_op->lookup`。路径走到一个负项并且要跨过它（例如它是中间分量），`traverse_mounts` 的快路径对负标志返回 `-ENOENT`（`fs/namei.c:1453`）。

负项也会占内存。`/proc/sys/fs/dentry-state` 的第五列是 `nr_negative`，统计的是 LRU 上的负项，不是全部负项。采样和列的含义在 [12](./12-observability.md)。

`d_invalidate` 用来把一个不再可信的缓存项从哈希摘掉。`lookup_fast` 在 `d_revalidate` 返回 0 时调用它（`namei.c:1671`）。

---

## 8.4 超级块 shrinker

`alloc_super` 注册（`fs/super.c:387`）：

```text
s->s_shrink.scan_objects = super_cache_scan;    :184
s->s_shrink.count_objects = super_cache_count;  :241
```

`super_cache_scan`（`:184`）从 `container_of(shrink, struct super_block, s_shrink)` 找回超级块。它按三类对象的数量把 `sc->nr_to_scan` 拆开（`:215`）：

```text
dentries = list_lru_shrink_count(&sb->s_dentry_lru)
inodes   = list_lru_shrink_count(&sb->s_inode_lru)
fs_objects = s_op->nr_cached_objects    文件系统自己的缓存，没有就是 0

按比例分 nr_to_scan
先 prune_dcache_sb                      :228    注释：inode 被 dentry 钉住，所以先砍 dcache
再 prune_icache_sb                      :230
还有 fs_objects：s_op->free_cached_objects
```

注释原文在 `:220`：先剪 dcache，再剪 icache，然后才是文件系统自己的缓存。并且至少扫描一个对象，方便 memcg 把缓存倒空。

谁来调用 `scan_objects`：`shrink_slab`（`mm/vmscan.c:1040`）。kswapd 和直接回收在 `shrink_node` 里按 memcg 调它。水位和 priority 的含义在 [MM §10.1](../memory-management/10-01-overview.md)。某个 shrinker 在 debugfs 里的 `count` / `scan` 文件见 [MM 观测章](../memory-management/13-observability.md) 的 shrinker debugfs 一节。

---

## 8.5 `prune_dcache_sb`

```text
prune_dcache_sb                     fs/dcache.c:1282
  list_lru_shrink_walk(s_dentry_lru, dentry_lru_isolate)
  shrink_dentry_list                  :1184
```

`dentry_lru_isolate`（`:1212`）拿不到 `d_lock` 就 `LRU_SKIP`。引用计数还在就 `d_lru_isolate` 把它从 LRU 摘掉并返回 `LRU_REMOVED`（对象还活着，只是不再躺在回收名单上）。`DCACHE_REFERENCED` 置位则清掉这一位并返回 `LRU_ROTATE`，给它再留一轮（`:1238`）。

`shrink_dcache_sb`（`:1321`）是卸载路径用的整表收缩，走 `dentry_lru_isolate_shrink`，一次最多看 1024 个（`:1327`）。它和内存压力下的 `prune_dcache_sb` 不是同一个入口。

---

## 8.6 `prune_icache_sb` 与 `iput`

```text
prune_icache_sb                     fs/inode.c:917
  list_lru_shrink_walk(s_inode_lru, inode_lru_isolate)
  dispose_list
```

`inode_lru_isolate`（`:844`）拿不到 `i_lock` 就跳过。引用计数非 0、或者 `i_state` 里除了 `I_REFERENCED` 还有别的位、或者地址空间还不能收缩，就把 inode 从 LRU 摘掉（`:863`）。只有 `I_REFERENCED` 时清掉这一位并 `LRU_ROTATE`（`:873`）。页缓存还没空时，它会在放下 LRU 锁之后 `invalidate_mapping_pages`（`:890`），把页腾掉再释放 inode 结构。kswapd 这么做会计 `KSWAPD_INODESTEAL`。

普通引用下降走 `iput`（`:1829`）→ `iput_final`（`:1777`）：

```text
s_op->drop_inode 为空则 generic_drop_inode     fs.h:2911
  真：i_nlink == 0，或已经不在 inode 哈希上
不丢、也没有 I_DONTCACHE、超级块仍 SB_ACTIVE：
  __inode_add_lru，返回
否则：
  不丢的那种先 I_WILL_FREE，write_inode_now，再清掉这个位
  置 I_FREEING，从 LRU 摘掉
  evict                               :679
    s_op->evict_inode 或 truncate_inode_pages_final 一类的清理
```

`generic_drop_inode` 的正文就是 `return !inode->i_nlink || inode_unhashed(inode);`。链接数为 0 的 inode（已经 unlink，但还有 `struct file` 打开着）要等最后的 `iput`。打开期间 `i_count` 还在，`iput_final` 不会这么早跑到。

`I_NEW` 的 inode 还在初始化，`iput_final` 开头对它 `WARN_ON`（`:1784`）。查找侧用 `iget_locked` 拿到 `I_NEW` 的 inode 后要 `unlock_new_inode`（`:1107`），别人才能在哈希里把它当成完整对象。

---

## 8.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `fs/dcache.c` | `d_lru_add` | 428 | 放进 `s_dentry_lru`，负项另计数 |
| `fs/dcache.c` | `dput` | 901 | 引用下降，可能杀掉并连带父项 |
| `fs/dcache.c` | `__d_clear_type_and_inode` | 352 | 变成负项，必要时增加负项计数 |
| `fs/dcache.c` | `dentry_lru_isolate` | 1212 | shrink 回调：忙则摘，引用过则转一圈 |
| `fs/dcache.c` | `prune_dcache_sb` | 1282 | 按 `nr_to_scan` 剪这个超级块的 dcache |
| `fs/dcache.c` | `shrink_dcache_sb` | 1321 | 卸载时把 LRU 尽量倒空 |
| `fs/inode.c` | `inode_lru_isolate` | 844 | 忙、脏、还有页缓存时的处理 |
| `fs/inode.c` | `prune_icache_sb` | 917 | 剪这个超级块的 inode LRU |
| `fs/inode.c` | `iget_locked` | 1337 | 按 `i_ino` 找或分配，新的带 `I_NEW` |
| `fs/inode.c` | `iput` | 1829 | 减 `i_count` |
| `fs/inode.c` | `iput_final` | 1777 | 决定进 LRU 还是 `evict` |
| `fs/inode.c` | `evict` | 679 | 清页并调用 `evict_inode` |
| `fs/super.c` | `super_cache_count` | 241 | shrinker 的计数 |
| `fs/super.c` | `super_cache_scan` | 184 | 先 dcache 后 icache |
| `include/linux/fs.h` | `generic_drop_inode` | 2911 | `nlink` 为 0 或已摘哈希 |
| `mm/vmscan.c` | `shrink_slab` | 1040 | 调用各 shrinker，见 MM §10.1 |

---

**导航**：[← 挂载](./07-mount-and-ns.md) · [目录](./README.md) · [锁与通知 →](./09-locks-and-notify.md)
