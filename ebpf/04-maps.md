# 4. map

**结论**：map 是 `struct bpf_map` 加一张 `bpf_map_ops`。`BPF_MAP_CREATE` 用类型下标索引 `bpf_map_types[]`，调用 `map_alloc`。程序里的 lookup 返回指向 value 的指针；hash 的更新拿桶上的 `raw_spinlock`，array 用下标直接算地址。per-cpu 类型让每个 CPU 写自己的槽，避免这条锁。

> `bpf_map` 在 `include/linux/bpf.h:275`，`bpf_map_ops` 在 `:75`。类型枚举在 `include/uapi/linux/bpf.h:923`。分发表在 `kernel/bpf/syscall.c:65`，由 `include/linux/bpf_types.h` 填充。创建入口 `map_create` 在 `syscall.c:1138`。

---

## 4.1 `bpf_map`

```c
/* include/linux/bpf.h:275
 * 头两个 cacheline 是读多的热字段。refcnt 单独对齐，避免和 lookup 抢同一行。
 */
struct bpf_map {
	const struct bpf_map_ops *ops ____cacheline_aligned;
	struct bpf_map *inner_map_meta;   /* map-in-map 的内层模板 */
	enum bpf_map_type map_type;
	u32 key_size;
	u32 value_size;
	u32 max_entries;
	u64 map_extra;          /* 个别类型的附加参数，bloom filter 用 */
	u32 map_flags;          /* BPF_F_NO_PREALLOC 等 */
	u32 id;
	struct btf_record *record; /* value 里的 spin_lock / timer / kptr */
	int numa_node;
	u32 btf_key_type_id;
	u32 btf_value_type_id;
	struct btf *btf;
	char name[BPF_OBJ_NAME_LEN];
	atomic64_t refcnt ____cacheline_aligned;
	atomic64_t usercnt;    /* 用户态 fd 持有的引用 */
	union {
		struct work_struct work;
		struct rcu_head rcu;
	};
	struct mutex freeze_mutex;
	atomic64_t writecnt;   /* 进行中的用户态更新 */
	bool frozen;           /* BPF_MAP_FREEZE 之后程序不能再写 */
	s64 __percpu *elem_count;
	u64 cookie;            /* 只写一次 */
};
```

`bpf_map_ops`（`:75`）分成三截。只给系统调用用的：`map_alloc`、`map_free`、`map_get_next_key`、批量 lookup。系统和程序都能用的：`map_lookup_elem`、`map_update_elem`、`map_delete_elem`，以及 queue/stack 的 push/pop/peek。给 verifier 内联的是 `map_gen_lookup`：它往缓冲里填一组等价指令，`do_misc_fixups` 用这组指令替换 helper 调用。

`map_meta_equal` 是 map-in-map 的运行时检查。外层在验证时已经按内层的 key/value 大小做过证明，插入时必须确认新内层的元数据一致（`:148` 的注释）。

类型到 ops 的表：

```c
/* kernel/bpf/syscall.c:65 ，宏来自 include/linux/bpf_types.h */
static const struct bpf_map_ops * const bpf_map_types[] = {
#define BPF_MAP_TYPE(_id, _ops) [_id] = &_ops,
#include <linux/bpf_types.h>
};
```

`bpf_types.h:87` 起登记 array、percpu array、prog array、hash、lru、lpm、devmap、cpumap、xskmap、sockmap、ringbuf、task storage 等。某一类在当前 `.config` 里没编进去时，对应槽是空的，`map_create` 取到 `ops == NULL` 就返回错误。

`map_create`（`:1138`）先 `CHECK_ATTR`，再按 `map_flags` 算出 fd 的读写模式（`bpf_get_file_flag`）。`sysctl_unprivileged_bpf_disabled` 非 0 且调用者没有 `CAP_BPF` 时直接 `-EPERM`（`:1197`）。接着 `ops->map_alloc_check`（如果有）和 `ops->map_alloc`，分配 id，`bpf_map_new_fd`。

用户态更新统一进 `bpf_map_update_value`（`:155`）。per-cpu hash/array、fd 数组、queue/stack、bloom filter 各走自己的函数；其余类型在 `rcu_read_lock` 下调用 `ops->map_update_elem`。flags 的三个基本值在 `bpf.h:1281`：`BPF_ANY = 0`、`BPF_NOEXIST = 1`、`BPF_EXIST = 2`。

引用归零后的释放走 RCU 或 workqueue（`bpf_map_put`，`:752`）。`free_after_rcu_gp` / `free_after_mult_rcu_gp` 两个位决定等几轮 grace period。程序还在跑 lookup 时，value 指针不能在 grace period 结束前被回收。

---

## 4.2 array

`struct bpf_array`（`bpf.h:1782`）把 `bpf_map` 嵌在开头，value 是柔性数组，按 8 字节对齐：

```c
struct bpf_array {
	struct bpf_map map;
	u32 elem_size;
	u32 index_mask;          /* max_entries 为 2 的幂时用来掩下标 */
	struct bpf_array_aux *aux;
	union {
		DECLARE_FLEX_ARRAY(char, value) __aligned(8);
		DECLARE_FLEX_ARRAY(void *, ptrs) __aligned(8);           /* prog array 等 */
		DECLARE_FLEX_ARRAY(void __percpu *, pptrs) __aligned(8); /* per-cpu */
	};
};
```

`array_map_lookup_elem`（`kernel/bpf/arraymap.c:166`）：

```c
index = *(u32 *)key;
if (index >= array->map.max_entries)
	return NULL;
return array->value + (u64)array->elem_size * (index & array->index_mask);
```

没有桶锁。下标越界返回 NULL，verifier 把它看成或空指针。`elem_size` 是 value 向上取整后的步长，所以 value 里如果嵌了 `bpf_spin_lock`，对齐由 BTF 记录保证，查找本身只做乘法。

`max_entries == 1` 时 `array_map_direct_value_addr`（`:177`）可以把 value 的内核地址编进指令，程序连 lookup 调用都省掉。ops 表在 `:750`。

`BPF_MAP_TYPE_PROG_ARRAY` 的 `ptrs[]` 里是 `bpf_prog *`。tail call 用的就是这张表（解释器在 `core.c:2046`）。`BPF_MAP_TYPE_PERF_EVENT_ARRAY` 同样是 fd 数组，元素是 perf event，给 `bpf_perf_event_output` 用。

`BPF_MAP_TYPE_PERCPU_ARRAY` 的 lookup 返回当前 CPU 的槽（`percpu_array_map_lookup_elem`，`:241`）。用户态 `bpf_map_update_value` 对这种类型调用 `bpf_percpu_array_update`，value 缓冲的长度是 `round_up(value_size, 8) * num_possible_cpus()`（`syscall.c:132`）。

---

## 4.3 hash

```c
/* kernel/bpf/hashtab.c:86 */
struct bpf_htab {
	struct bpf_map map;
	struct bucket *buckets;
	void *elems;                 /* 预分配时的元素池 */
	union {
		struct pcpu_freelist freelist;
		struct bpf_lru lru;
	};
	u32 n_buckets;
	u32 elem_size;               /* htab_elem + key + value */
	u32 hashrnd;
};

/* :111 ，key[] 8 字节对齐，value 紧跟在 round_up(key_size, 8) 之后 */
struct htab_elem {
	union {
		struct hlist_nulls_node hash_node;
		/* 空闲时走 freelist */
	};
	union {
		void *ptr_to_pptr;           /* per-cpu value */
		struct bpf_lru_node lru_node;
	};
	u32 hash;
	char key[] __aligned(8);
};
```

`__htab_map_lookup_elem`（`:677`）要求 `bpf_rcu_lock_held()`，用 `htab_map_hash` 选桶，再 `lookup_nulls_elem_raw`。`htab_map_lookup_elem`（`:697`）返回 `l->key + round_up(key_size, 8)`，这就是 value 的地址。查找不拿桶锁，靠 RCU 保证 `hlist_nulls` 节点还在。

更新在 `htab_map_update_elem`（`:1103`）。普通路径调用 `htab_lock_bucket`（`:149`）：

```text
preempt_disable
local_irq_save
每 CPU 的 map_locked[hash] 计数，已经非 0 则 -EBUSY
    （避免同一 CPU 上 BPF 程序递归进同一张表时死锁）
raw_spin_lock(&bucket->raw_lock)
```

`BPF_F_LOCK` 是另一条路（`:1128`）：value 里必须有 BTF 标记的 `bpf_spin_lock`，先在无桶锁下找到元素，再用元素自己的锁原地拷贝。找不到才退回桶锁。锁的 verifier 规则在 [09](./09-helpers-and-sync.md)。

默认预分配。`BPF_F_NO_PREALLOC`（`bpf.h:1289`）关掉它，`htab_is_prealloc`（`hashtab.c:131`）就是看这个位。预分配从 `pcpu_freelist` 拿 `htab_elem`，满了返回 `-E2BIG`，更新路径上不再 `kmalloc`。LRU 类型（`htab_lru_map_ops`，`:2318`）在元素用尽时按 LRU 节点淘汰，而不是直接失败。`BPF_MAP_TYPE_PERCPU_HASH` 与 `LRU_PERCPU_HASH` 的 value 在每 CPU 上各有一份，ops 在 `:2472`。

桶锁是 `raw_spinlock`，查找路径又要求 RCU。所以 hash map 的 BPF 侧 lookup/update 不能出现在会睡眠的程序段里。`BPF_F_SLEEPABLE` 的程序要写这种 map，走的是系统调用侧或 verifier 允许的睡眠安全 ops，不能假设和原子程序同一套桶锁。

---

## 4.4 LPM、queue、ringbuf

`BPF_MAP_TYPE_LPM_TRIE` 的 key 是 `bpf_lpm_trie_key_u8`（`bpf.h:95`）：`prefixlen` 加上字节串。IPv4 的前缀长度到 32，IPv6 到 128。`trie_lookup_elem`（`kernel/bpf/lpm_trie.c:228`）从根沿匹配的前缀往下走，在 RCU 读锁下进行，返回最长匹配节点的 value。ops 在 `:750`。这张表适合「按前缀选下一跳」这种只读多、写少的策略；它没有 hash 那种 per-cpu 变体。

`BPF_MAP_TYPE_QUEUE` 和 `BPF_MAP_TYPE_STACK` 没有 key。系统调用侧的 update 被 `bpf_map_update_value` 转成 `map_push_elem`（`syscall.c:197`）。ops 在 `kernel/bpf/queue_stack_maps.c:273` 和 `:289`。

`BPF_MAP_TYPE_RINGBUF` 的 ops 在 `kernel/bpf/ringbuf.c:353`。程序用 `bpf_ringbuf_reserve` / `submit` / `discard`，这些 proto 挂在 `bpf_base_func_proto`（`helpers.c:1856`）。数据区可以 `mmap` 给用户态，消费端不必每条记录都 `bpf(BPF_MAP_LOOKUP_ELEM)`。`BPF_MAP_TYPE_USER_RINGBUF` 方向相反，用户态生产，程序用 `bpf_user_ringbuf_drain` 拉。

`BPF_MAP_TYPE_BLOOM_FILTER` 只有 push 和 peek，`map_extra` 在创建时被允许非 0（`map_create` 里 `:1159` 对其他类型要求 `map_extra == 0`）。

---

## 4.5 map-in-map 与本地存储

`BPF_MAP_TYPE_ARRAY_OF_MAPS` 和 `HASH_OF_MAPS` 的 value 是内层 map 的 fd。创建时用 `inner_map_meta` 记住内层的 key/value 大小。插入走 `bpf_fd_array_map_update_elem` 或 `bpf_fd_htab_map_update_elem`，并调用内层的 `map_meta_equal`。

用户态更新这两类之后，`maybe_wait_bpf_programs`（`syscall.c:144`）做一次 `synchronize_rcu()`。返回到用户态时，已经在跑的程序要么还拿着旧内层，要么已经看见新内层，不会看见释放到一半的 map。

本地存储不按 key 哈希，而按持有者：

| 类型 | ops 登记 | 持有者 |
|------|----------|--------|
| `BPF_MAP_TYPE_TASK_STORAGE` | `bpf_types.h:112` | `task_struct` |
| `BPF_MAP_TYPE_SK_STORAGE` | `:116`，`CONFIG_NET` | `sock` |
| `BPF_MAP_TYPE_INODE_STORAGE` | `:110`，`CONFIG_BPF_LSM` | inode |
| `BPF_MAP_TYPE_CGRP_STORAGE` | `:93` | cgroup |

`bpf_map_ops.map_owner_storage_ptr` 返回持有者里的 `bpf_local_storage *`。程序用 `bpf_*_storage_get` 按当前 task 或 skb 上的 sock 取元素。`BPF_MAP_TYPE_CGROUP_STORAGE` 在枚举里标了 `DEPRECATED`（`bpf.h:943`），新代码用 `CGRP_STORAGE`：后者不要求程序一定挂在 cgroup 上。

`BPF_MAP_TYPE_DEVMAP`、`DEVMAP_HASH`、`CPUMAP`、`XSKMAP` 的 value 布局是 UAPI 的一部分（`bpf_devmap_val`、`bpf_cpumap_val` 在 `bpf.h:6337` 和 `:6350`）。它们的 redirect 行为放在 [07](./07-xdp.md)，这里只承认它们是 map。

---

## 4.6 对齐和冻结

hash 的 value 起始地址是 `key + round_up(key_size, 8)`。array 的 `elem_size` 同样把 value 垫到对齐边界。用户态按 `value_size` 拷贝，不要按 C 结构的 `sizeof` 去猜内核步长：结构末尾的 padding 和 key 的对齐不是同一件事。

`BPF_MAP_FREEZE` 进 `map_freeze`（`syscall.c:2005`）。冻结后程序侧的更新被拒绝，lookup 仍可用。适合「装载时填好、运行期只读」的配置表。`writecnt` 非 0 时冻结会等待，避免和进行中的用户态更新交叉。

`BPF_F_RDONLY` / `BPF_F_WRONLY` 限制 fd。`BPF_F_RDONLY_PROG` / `BPF_F_WRONLY_PROG` 限制程序。`bpf_map_flags_to_cap`（`bpf.h:1817`）把这两位换成 verifier 用的读写能力。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*
