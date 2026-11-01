# 8. 多队列与亲和

**结论**：每颗 CPU 有一条软件队列 `blk_mq_ctx`，设备有若干条硬件队列 `blk_mq_hw_ctx`；`mq_map[cpu]` 决定这颗 CPU 的请求进哪条硬件队列，PCI 和 virtio 用中断亲和掩码来填这张表，轮询队列没有 IRQ，改由 `blk_mq_map_queues` 平分 CPU。

> 映射在 `block/blk-mq-cpumap.c`、`block/blk-mq-pci.c`、`block/blk-mq-virtio.c`。软件队列挂上硬件队列在 `blk_mq_map_swqueue`（`block/blk-mq.c:4021`）。类型枚举在 `include/linux/blk-mq.h:457`。

C 新手把两层队列分开记。软件队列解决「很多 CPU 同时提交时不要抢同一把锁」：每 CPU 一条链表。硬件队列解决「设备能同时消化几条命令流」：NVMe 的一个 I/O Submission Queue，或 virtio 的一个 virtqueue。提交时先落到本 CPU 的软件队列，再按映射送到某一条硬件队列。完成中断最好打在会处理这条硬件队列的 CPU 上，否则请求在 CPU A 提交、中断在 CPU B 完成，缓存和调度都不划算。中断怎么送到某颗 CPU 的通用机制在 [interrupts/06](../interrupts/06-request-and-flow.md)。

---

## 8.1 三种硬件队列

`enum hctx_type`（`include/linux/blk-mq.h:457`）：

| 值 | 名字 | 谁走进来 |
|---|---|---|
| 0 | `HCTX_TYPE_DEFAULT` | 写、丢弃，以及没有单独读队列时的读 |
| 1 | `HCTX_TYPE_READ` | `REQ_OP_READ` |
| 2 | `HCTX_TYPE_POLL` | `REQ_POLLED` |

`blk_mq_get_hctx_type`（`block/blk-mq.h:85`）先看 `REQ_POLLED`，再看是不是读。驱动可以只提供一种：`tag_set->nr_maps` 最小是 1，最大是 `HCTX_MAX_TYPES`（3）。某一种 `map[j].nr_queues == 0` 时，`blk_mq_map_swqueue` 把这种类型的 ctx 指回 `HCTX_TYPE_DEFAULT`（`blk-mq.c:4044`）。

virtio-blk 的 `init_vq` 把读队列数设成 0（`drivers/block/virtio_blk.c:1047`），只分 default 和 poll。NVMe 可以有独立的读队列，个数在 `nvme_setup_irqs` 路径里算，见 [13](./13-nvme-walkthrough.md)。

---

## 8.2 CPU 到硬件队列的表

```c
/* include/linux/blk-mq.h:444 */
struct blk_mq_queue_map {
	unsigned int *mq_map;     /* 下标是 CPU 号，值是硬件队列下标 */
	unsigned int nr_queues;
	unsigned int queue_offset; /* 这一类队列在 tag set 里的起始编号 */
};
```

`tag_set->map[HCTX_MAX_TYPES]` 每种类型一张表。查表：

```c
/* block/blk-mq.h:78 */
static inline struct blk_mq_hw_ctx *blk_mq_map_queue_type(struct request_queue *q,
							  enum hctx_type type,
							  unsigned int cpu)
{
	return xa_load(&q->hctx_table, q->tag_set->map[type].mq_map[cpu]);
}
```

热路径不每次查这张表。`blk_mq_map_swqueue`（`blk-mq.c:4021`）在初始化或 CPU 拓扑变化时，把结果写进 `ctx->hctxs[type]`。之后 `blk_mq_map_queue`（`block/blk-mq.h:105`）只读这个指针。

```text
for_each_possible_cpu(i)
  ctx = per_cpu_ptr(q->queue_ctx, i)
  对每种 map：
    这种队列数为 0：ctx->hctxs[j] 改指 DEFAULT
    否则 hctx_idx = map[j].mq_map[i]
    这条 hctx 还没有 tag：尝试分配，失败则把 CPU 改映射到 0 号队列
    ctx->hctxs[j] = 对应 hctx
    把 cpu i 放进 hctx->cpumask
    ctx->index_hw[j] = 它在 hctx->ctxs[] 里的下标
```

`hctx->cpumask` 是「哪些 CPU 的软件队列挂在我这」。`hctx->nr_ctx` 和 `hctx->ctxs[]` 是同一件事的数组形式。`none` 忙路径的轮转就走 `ctxs[]`，见 [04](./04-sched-none.md)。

CPU 热拔时，`blk_mq_hctx_notify_offline`（`blk-mq.c:3575`）和 `blk_mq_hctx_notify_dead`（`:3651`）把这条硬件队列上属于死 CPU 的请求挪走。全部 CPU 都下线后 `BLK_MQ_S_INACTIVE` 置位。

---

## 8.3 两张默认映射

没有 IRQ 亲和可用时，`blk_mq_map_queues`（`block/blk-mq-cpumap.c:18`）调用 `group_cpus_evenly(nr_queues)`，把可能的 CPU 分成 `nr_queues` 组，组号加上 `queue_offset` 写进 `mq_map[cpu]`。分配失败则所有 CPU 都指向 `queue_offset`（`:25`）。

PCI 设备用中断亲和，不自己平分：

```c
/* block/blk-mq-pci.c:25
 * 假定 pdev 的向量数不少于队列数。
 * 队列 queue 的 CPU 集合 = pci_irq_get_affinity(pdev, queue + offset) 的掩码。
 */
void blk_mq_pci_map_queues(struct blk_mq_queue_map *qmap, struct pci_dev *pdev,
			   int offset)
{
	/* 对每个 queue：mask = pci_irq_get_affinity(...)
	 * for_each_cpu(cpu, mask)
	 *     mq_map[cpu] = queue_offset + queue
	 * 拿不到 mask：WARN（多队列时），再 blk_mq_clear_mq_map
	 */
}
```

`offset` 用来跳过管理队列。NVMe 管理队列占向量 0，I/O 队列从 `queue_irq_offset`（通常是 1，`drivers/nvme/host/pci.c:431`）开始。所以 I/O 队列 0 的亲和掩码来自 IRQ 向量 1，不是向量 0。

virtio 的对应函数是 `blk_mq_virtio_map_queues`（`block/blk-mq-virtio.c:23`）。它问 `vdev->config->get_vq_affinity(vdev, first_vec + queue)`。传输层没有这个回调，或某一条返回空，就整张表退回 `blk_mq_map_queues`（`:44`）。

中断亲和掩码本身由 `pci_alloc_irq_vectors_affinity` 和内核的 IRQ 分配器生成。向量耗尽、有效亲和和用户写的 `smp_affinity` 不是一回事，见 [interrupts/06](../interrupts/06-request-and-flow.md) 和 [interrupts/12](../interrupts/12-observability.md)。块层只消费已经算好的 cpumask。

---

## 8.4 NVMe 怎么填

`nvme_setup_irqs`（`drivers/nvme/host/pci.c:2296`）：

```text
irq_affinity.pre_vectors = 1          管理队列单独一条向量
calc_sets = nvme_calc_irq_sets        把 I/O 向量分成 default 和 read 两组
poll 队列不申请中断
  poll_queues = min(nr_poll_queues, nr_io_queues - 1)
  至少留一条非轮询 I/O 队列
irq_queues = 1 + (nr_io_queues - poll_queues)
  NVME_QUIRK_SINGLE_VECTOR 时不加后面这项
pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues,
    PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY, &affd)
```

`nvme_pci_map_queues`（`:440`）对每种 `hctx_type`：

```text
map->nr_queues = dev->io_queues[i]
map->queue_offset = 已经分配过的队列数
不是 POLL 且 offset 非 0：
  blk_mq_pci_map_queues(map, pdev, offset)
否则：
  blk_mq_map_queues(map)          轮询队列，或只有一条向量时
```

注释（`:455`）写明轮询队列没有 IRQ，也就没有 IRQ 亲和，所以用普通的 CPU 平分。管理队列不在这张 I/O map 里，它用 `nvme_mq_admin_ops`（`:1715`），那张表没有 `map_queues`。

I/O 队列的中断在 `queue_request_irq`（调用点 `:1697`）。非轮询队列注册 `nvme_irq`，线程化时前面还有 `nvme_irq_check`（`:1618`）。轮询队列不注册。完成路径见 [13](./13-nvme-walkthrough.md)。

---

## 8.5 virtio-blk 怎么填

`init_vq`（`virtio_blk.c:1017`）：

```text
读 VIRTIO_BLK_F_MQ 的 num_queues，没有特性则 num_vqs = 1
再和模块参数 num_request_queues、nr_cpu_ids 取最小
num_poll_vqs = min(poll_queues, num_vqs - 1)
io_queues[DEFAULT] = num_vqs - num_poll_vqs
io_queues[READ] = 0
io_queues[POLL] = num_poll_vqs
前一段 virtqueue 的回调是 virtblk_done
后一段 poll 的 callbacks[i] = NULL，名字 req_poll.%u
virtio_find_vqs(..., &desc)          desc 是 irq_affinity
```

模块参数在文件顶部：`num_request_queues`（`:34`）、`poll_queues`（`:41`）。`poll_queues` 的说明是「专用的轮询 virtqueue 条数」。

`virtblk_map_queues`（`:1221`）挂在 `virtio_mq_ops.map_queues`（`:1293`）：

```text
POLL：blk_mq_map_queues
其它：blk_mq_virtio_map_queues(&set->map[i], vblk->vdev, 0)
```

注释（`:1236`）和 NVMe 同一句话：普通队列的 CPU 集合由 virtio 核心按中断亲和给出，轮询队列没有中断，交给块层平分。`first_vec` 传 0，因为 virtio-blk 没有一条单独的管理队列占掉向量 0。

`virtblk_poll`（`:1259`）在轮询硬件队列上 `virtqueue_get_buf`，不经过 `virtblk_done`。有中断的队列仍由 `virtblk_done`（`:351`）在 virtqueue 回调里收完成。两条完成路径在 [14](./14-virtio-blk-walkthrough.md)。

---

## 8.6 提交时实际走哪条

```text
blk_mq_get_ctx(q)                 当前 CPU 的软件队列
type = blk_mq_get_hctx_type(opf)  POLL > READ > DEFAULT
hctx = ctx->hctxs[type]
请求的 mq_ctx、mq_hctx 在 blk_mq_rq_ctx_init 里写下
```

同一块盘上，CPU 2 的写和 CPU 2 的读可能进不同的硬件队列（NVMe 开了读队列时）。CPU 2 和 CPU 3 如果落在同一条 IRQ 亲和掩码里，会共享一条硬件队列，两条软件队列的 `hctxs[type]` 指向同一个 `hctx`。`hctx->ctx_map` 记录其中哪些软件队列有请求。

`blk_mq_unique_tag`（`block/blk-mq-tag.c:679`）把硬件队列号放在高 16 位、tag 放在低 16 位。完成路径用它在共享 tag 时找回 request。位宽是 `BLK_MQ_UNIQUE_TAG_BITS`（16，`include/linux/blk-mq.h:766`）。

硬件队列的 `cpumask` 还决定 `run_work` 跑在哪。`blk_mq_hctx_next_cpu`（`blk-mq.c:2212`）在 `hctx->cpumask` 里轮转，一批 `BLK_MQ_CPU_WORK_BATCH`（8）次换下一颗。异步 `blk_mq_run_hw_queue` 把工作排到这颗 CPU，避免所有队列的派发都挤在提交者那一颗上。

---

## 8.7 关键函数表

| 文件 | 符号 | 行号 | 作用 |
|---|---|---|---|
| `include/linux/blk-mq.h` | `enum hctx_type` | 457 | default / read / poll |
| `include/linux/blk-mq.h` | `struct blk_mq_queue_map` | 444 | `mq_map[cpu]` |
| `block/blk-mq.h` | `blk_mq_map_queue_type` | 78 | CPU 和类型 → hctx |
| `block/blk-mq.h` | `blk_mq_get_hctx_type` | 85 | 操作码 → 类型 |
| `block/blk-mq.c` | `blk_mq_map_swqueue` | 4021 | 把映射写进每个 ctx |
| `block/blk-mq.c` | `blk_mq_hctx_next_cpu` | 2212 | `run_work` 在 cpumask 里轮转 |
| `block/blk-mq-cpumap.c` | `blk_mq_map_queues` | 18 | 没有 IRQ 亲和时平分 CPU |
| `block/blk-mq-pci.c` | `blk_mq_pci_map_queues` | 25 | 按 `pci_irq_get_affinity` 填表 |
| `block/blk-mq-virtio.c` | `blk_mq_virtio_map_queues` | 23 | 按 virtqueue 亲和填表 |
| `block/blk-mq-tag.c` | `blk_mq_unique_tag` | 679 | 硬件队列号和 tag 拼成一个 u32 |
| `drivers/nvme/host/pci.c` | `nvme_setup_irqs` | 2296 | 管理队列 + I/O 队列的 MSI-X |
| `drivers/nvme/host/pci.c` | `nvme_pci_map_queues` | 440 | 轮询队列走平分，其余走 PCI 亲和 |
| `drivers/block/virtio_blk.c` | `init_vq` | 1017 | default 与 poll virtqueue |
| `drivers/block/virtio_blk.c` | `virtblk_map_queues` | 1221 | poll 平分，其余问 virtio |

---

**导航**：[← bfq](./07-sched-bfq.md) · [目录](./README.md) · [ioprio →](./09-ioprio-and-cgroup-io.md)
