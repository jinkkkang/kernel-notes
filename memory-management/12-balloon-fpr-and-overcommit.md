# 12. balloon、free page reporting 与超卖

**结论**：超卖是宿主把同一堆物理页分给多个 guest 的「承诺」。balloon 是 guest 主动交回页；free page reporting（FPR）是 guest 把 buddy 里已经空闲的高阶页报告给宿主，宿主可以暂时拿去用，guest 再碰时再还；free page hinting 是另一条 virtqueue，在关机或空闲时提示整段空闲。三条路不要写成同一个驱动回调。

> FPR 核心：`mm/page_reporting.c`，挂钩在 `__free_one_page`。
> 驱动：`drivers/virtio/virtio_balloon.c`。热插拔：`drivers/virtio/virtio_mem.c`。
> 导航：[上一节 KVM](./11-kvm-memory.md) · [目录](./README.md) · [下一节 观测](./13-observability.md)

本机没有 virtio-balloon 设备可操作，下面的操作步骤全部是 **RECIPE**。计数是 6.12.94+ 的 `/proc/vmstat` 实测，说明这台机器的内核曾经统计过 balloon 活动（见 [13](./13-observability.md)），不代表当前 shell 能 inflate。

---

## 12.1 三条通道

| 机制 | guest 侧做什么 | 宿主拿到什么 | 页还在 guest buddy 里吗 |
|---|---|---|---|
| inflate | 分配页，从 buddy 拿出来，交给 inflate virtqueue | 这些页的 HPA，可以从 qemu 的 RSS 里拿掉 | 不在。guest 把它们当「气球占着」 |
| deflate | 从气球还回 buddy | 宿主必须把页还回来 | 回到空闲链表 |
| FPR（`VIRTIO_BALLOON_F_REPORTING`） | **不分配**。从空闲链表上隔离已经空闲的页，报告后放回空闲链表，并打上 reported 标记 | 一段「guest 现在不用」的提示。guest 再分配到它时要先收回 | 报告期间短暂隔离，之后仍算 guest 的空闲页 |
| free page hinting（`VIRTIO_BALLOON_F_FREE_PAGE_HINT`） | `free_page_vq`，按 hint cmd id 上报 | 宿主在迁移或压缩时跳过这些页 | 语义以 virtio 规格为准，和 FPR 不是同一个 vq |
| virtio-mem | 热插拔内存块，走 `add_memory` / `offline` | 真正改变 guest 的物理内存大小 | 拔掉的块不是空闲页，是地址空间没了 |

`virtio_balloon` 结构里这几条队列是分开的（`virtio_balloon.c:58` 一带）：`inflate_vq`、`deflate_vq`、`stats_vq`、`free_page_vq`，另外 `reporting_vq` 在 `:121`。

本机 `balloon_inflate` 4718592、`balloon_deflate` 2097152、`balloon_migrate` 45672（单位是页，vmstat 事件）。净 inflate 约 (4718592−2097152)×4 KiB ≈ 10 GiB 的历史累计，不是当前气球大小。当前大小要看驱动的 config 或 sysfs，vmstat 只累加。

---

## 12.2 inflate / deflate

通用气球页在 `include/linux/balloon.h`。驱动分配一页，`balloon_page_enqueue` 挂到设备链表，通过 inflate 队列把 PFN 告诉宿主。宿主把对应 HVA 从 qemu 进程里 `madvise(MADV_DONTNEED)` 或释放，物理页才能进宿主 buddy。

`struct balloon_dev_info`（`balloon.h:53`）全部成员：

| 成员 | 中文 |
|---|---|
| `unsigned long isolated_pages` | 正被迁移隔离的气球页数 |
| `struct list_head pages` | 已经交给宿主的页 |
| `int (*migratepage)(...)` | 压缩要搬家时的回调。气球页是 `PageOffline` + `PageMovableOps`，可以迁移，但迁移要先 inflate 新页、再 deflate 旧页（`virtio_balloon.c:847` 一带的注释） |
| `bool adjust_managed_page_count` | 为真时，enqueue/dequeue 会改 zone 的 `managed_pages`。guest 的 `MemTotal` 会跟着掉或涨 |

文件头注释（`balloon.h:9`）规定了竞态：

1. inflate/deflate 改 `page->private` 必须拿 `balloon_pages_lock`。
2. 隔离或 dequeue 必须在同一把锁下把页从设备链表摘掉。
3. `page->private` 非空表示页在气球链表上或正在迁移。为空表示还在 inflate 途中，或已经 deflate、即将还给 buddy，这两种都不能隔离。

OOM：`VIRTIO_BALLOON_F_DEFLATE_ON_OOM` 时驱动注册 OOM notifier。`VIRTIO_BALLOON_OOM_NR_PAGES` 是 256（`virtio_balloon.c:31`），一次通知最多 deflate 这么多 4K 页。这是 guest 自救，宿主不一定立刻有页可还。

stats 队列上报 guest 的空闲、缓存、可用。宿主调度器用它决定再 inflate 多少。stats 是提示，不是账本。

---

## 12.3 free page reporting

### 数据结构

`struct page_reporting_dev_info`（`include/linux/page_reporting.h:11`）全部成员：

| 成员 | 中文 |
|---|---|
| `int (*report)(...)` | 把 sg 里的页「处理」掉。virtio-balloon 的实现是 `virtballoon_free_page_report`（`virtio_balloon.c:202`），把 PFN 推进 `reporting_vq` |
| `struct delayed_work work` | 后台处理 |
| `atomic_t state` | 空闲 / 已请求 / 正在跑。用来合并多次通知 |
| `unsigned int order` | 最小报告阶。设备可以指定；未指定时用 `pageblock_order` |
| `unsigned int capacity` | 一批最多多少页。0 表示用 `PAGE_REPORTING_CAPACITY`（32，`:8`） |

全局 `page_reporting_order`（`mm/page_reporting.c:16`）是模块参数（`:37`，`0644`）。`page_reporting_register`（`:360`）在设备没指定 order 时把它设成 `prdev->order` 或 `pageblock_order`（`:380`）。

virtio-balloon 的特例（`:1021`）：

- 有 `VIRTIO_BALLOON_F_REPORTING` 才注册。
- order 默认 `PAGE_REPORTING_ORDER_UNSPECIFIED`（-1）。
- `capacity` 用 reporting virtqueue 的 vring 大小。
- **ARM64 且 64K 页**时把 order 硬编码成 5（2MB）。注释说默认 `pageblock_order` 在 64K 页上是 512MB，空闲块根本凑不出来，FPR 永远不触发；order 5 也避免宿主 4K 页时把 THP 拆碎。x86 4K 页不走这个 `#if`。

### 和 buddy 的挂钩

```text
__free_one_page                         # page_alloc.c:940
  合并 buddy，order 升高
  page_reporting_notify_free(order)     # :1025
    # 实现在 mm/page_reporting.h:33
    静态键 page_reporting_enabled 没开就返回
    order < page_reporting_order 就返回
    __page_reporting_notify()           # page_reporting.c:98
      调度 page_reporting_dev_info.work

page_reporting_process                  # page_reporting.c:315
  state = ACTIVE
  每个 zone：page_reporting_process_zone
    拿 zone->lock
    从 free_area[order] 的空闲链表上找还没 PageReported 的页
    __isolate_free_page                 # 短暂离开 buddy
    放锁
    prdev->report()                     # virtio：告诉宿主
    page_reporting_drain                # 标 PageReported，放回空闲链表
```

隔离只发生在报告的那一小段。报告完页回到 guest 的空闲链表。宿主若把对应物理页挪作他用，guest 下次从 buddy 分到这一页并真正访问时，宿主要能把内容（全 0 或未定义，规格要求报告页内容可丢）重新准备好。所以 FPR 不是 balloon：guest 的 `MemFree` 不会因为报告成功而下降。

`PageReported` 避免同一空闲页被反复报告。页一旦被分配再释放，标记清掉，可以再报。

`page_reporting_order` 设得太高：空闲块凑不出这个阶，FPR 安静地什么都不做。设得太低：order 0 的抖动会把工作队列打满。默认跟 pageblock 走，就是在这两者之间。

---

## 12.4 free page hinting

和 FPR 并列，不是替代。`virtio_balloon.c` 里 `report_free_page_func`（`:794`）处理 hint 工作项，`virtio_balloon_report_free_page`（`:774`）走 `free_page_vq`。

hint 用命令 id（`free_page_hint_cmd_id`，`:656` 一带）。宿主发出一个 id，guest 把当前空闲页报上去，宿主在迁移时可以不传这些页。下一轮 id 变了，旧报告作废。

FPR 是持续的、和 buddy 合并挂钩的；hint 是一轮一轮的。两个特性位可以同时存在。看 `virtio_balloon` 的 feature 而不是看「有没有气球」。

---

## 12.5 KSM

KSM（`mm/ksm.c`）把内容相同的匿名页合成一页，写时再拆开（COW）。线程是 `ksm_scan_thread`（`:2836`），每轮 `ksm_do_scan`（`:2815`）。

`ksm_scan_thread` 把自己标成 freezable，nice 5。循环里拿 `ksm_thread_mutex`，先 `wait_while_offlining`，再看 `ksmd_should_run`（`:2831`）：`ksm_run & KSM_RUN_MERGE` 且 `ksm_mm_head` 非空。该跑就 `ksm_do_scan(ksm_thread_pages_to_scan)`。跑完按 `ksm_thread_sleep_millisecs` 睡；不该跑就睡在 `ksm_thread_wait` 上，等 run 位或 kthread stop。

```text
ksm_do_scan(scan_npages)                  # ksm.c:2815
  while 还没扫够，且当前线程没在 freezing：
    cond_resched
    rmap_item = scan_get_next_rmap_item(&page)   # 下一个候选匿名页
    没有了：return                          # 这一轮 mm 链表走完，seqnr++ 在 scan_get_next_rmap_item 末尾
    cmp_and_merge_page(page, rmap_item)   # 和稳定树里的页比，相同就合并
    put_page(page)
    ksm_pages_scanned++
```

进程要 `madvise(MADV_MERGEABLE)` 或全局 `merge_across_nodes`。`ksm_madvise` 在 `:3010`。

和超卖的关系：宿主开 KSM，扫的是 qemu 进程的匿名页。两个 guest 里相同的零页或相同的代码页可以合成一页。写的时候拆开，延迟尖刺和内存突然涨是成对出现的。

`mm_struct` 上的计数：`ksm_merging_pages`、`ksm_rmap_items`、`ksm_zero_pages`（见 [06](./06-mm-and-vma.md)）。本机 vmstat `cow_ksm` 0、`ksm_swpin_copy` 0。

KSM 不合并 hugetlb，也几乎不合并已经 pin 住的页。DPDK 的大页不要指望 KSM 来超卖。

---

## 12.6 宿主 THP 和 guest THP

| 层 | 影响 |
|---|---|
| guest THP | 只影响 guest 自己的页表和 guest buddy 的高阶块。EPT 不自动变成 2MB |
| 宿主 THP | qemu 匿名内存被折叠成 2MB。EPT 才有机会建大页。`khugepaged` 在宿主上跑，会给 vcpu 线程带来停顿 |
| 宿主 hugetlb 后端 | qemu `-mem-path` / memfd hugetlb。EPT 大页稳定，但 balloon 不能随便拆走大页里的 4KB |
| FPR order 5（ARM64 64K 那个特例） | 注释明确说是为了少拆宿主 THP |

balloon inflate 从 guest buddy 拿页。guest 若开着 THP，inflate 会把高阶块拆碎，`thp_split_*` 上升。反过来，宿主把 guest 的内存后端放在 hugetlb 上时，inflate 交回去的 4KB 并不能让宿主把整个 2MB 大页还给 buddy，除非整页都空了。超卖率和大页是冲突的。

---

## 12.7 virtio-mem 与热插拔

`struct virtio_mem` 从 `drivers/virtio/virtio_mem.c:102` 开始，后面还有 SBM（sub-block）和 BBM（big block）两套状态机，成员很多。本节只列地址和大小这一层，状态机计数器不逐个编，需要时从 `:102` 往下读。

已核对的成员：

| 成员 | 中文 |
|---|---|
| `struct virtio_device *vdev` | virtio 设备 |
| `bool unplug_all_required` | 启动时可能得先把内存全部拔掉 |
| `struct work_struct wq` | 处理插拔请求 |
| `atomic_t wq_active` / `config_changed` | 工作队列是否在跑、配置是否变了 |
| `struct virtqueue *vq` | guest 向宿主发请求的队列 |
| `wait_queue_head_t host_resp` | 等宿主回应 |
| `struct virtio_mem_req req` / `resp` | 一对请求和响应的缓冲 |
| `uint64_t plugged_size` | 当前已经插上的大小 |
| `uint64_t requested_size` | 宿主要求的大小 |
| `uint64_t device_block_size` | 和设备通信的块大小 |
| `int nid` | 这些内存所属 NUMA 节点 |
| `uint64_t addr` | 区域物理起始 |
| `uint64_t region_size` / `usable_region_size` | 最大 / 当前可用 |
| `struct resource *parent_resource` | 资源树里的父节点 |
| `const char *resource_name` | `add_memory_driver_managed()` 用的名字 |
| `int mgid` | memory group |
| `atomic64_t offline_size` / `uint64_t offline_threshold` | 还没 online 的大小。默认阈值 1GB（`:154`），避免插得太快把系统插到 OOM |
| `bool in_sbm` | 真表示走 sub-block 模式，否则 big-block |

插上的内存要经过内存热插拔：`add_memory` → online → 页进 buddy。拔的时候要 offline，不可移动页会让 offline 失败。`ZONE_MOVABLE` 把可拔的范围限制成可移动分配（zone 本身见 [02](./02-physical-layout.md)，online/offline 的函数在 12.9）。

virtio-mem 改变的是 guest 的 `MemTotal`。balloon 不改变 memmap 的范围，只是把页从可用里扣掉（若 `adjust_managed_page_count`）。FPR 两者都不改。

---

## 12.8 操作步骤（RECIPE）

下面假设宿主能跑 qemu，guest 里有 virtio-balloon。本 box 没有这套设备，**不要在这台机器上执行**。

```text
# guest 里看特性。没有对应文件就是内核不支持或设备没暴露。
ls /sys/devices/virtual/misc/vballoon/ 2>/dev/null || ls /sys/bus/virtio/drivers/virtio_balloon/

# 宿主 qemu 监视器（不是 guest shell）：
# (qemu) info balloon
# (qemu) balloon 2048          # 把 guest 逻辑内存压到 2048 MB。单位以该 qemu 版本文档为准

# guest 里看结果：
grep -E 'MemTotal|MemFree|MemAvailable' /proc/meminfo
grep balloon /proc/vmstat

# FPR：guest 内核要 CONFIG_PAGE_REPORTING，设备要有 REPORTING 特性。
# 当前阶：
cat /sys/module/page_reporting/parameters/page_reporting_order
# 改阶（RECIPE，会改变哪些空闲块被报告）：
# echo 9 > /sys/module/page_reporting/parameters/page_reporting_order

# KSM，宿主上，扫 qemu 的匿名页：
# echo 1 > /sys/kernel/mm/ksm/run
# cat /sys/kernel/mm/ksm/pages_sharing

# virtio-mem 的请求大小在设备 config 里，由宿主管理软件写。
# guest 侧看块是否 online：
grep . /sys/devices/system/memory/memory*/state
```

失败时先分清是哪条通道：`MemTotal` 变了是 virtio-mem 或 `adjust_managed_page_count` 的 balloon；`MemTotal` 没变但宿主 RSS 掉了，是 inflate 或 FPR 让宿主把页还进了自己的 buddy。

---

## 12.9 内存热插拔：online / offline 与 `ZONE_MOVABLE`

**结论**：热插拔以 memory block 为单位，块里是整数个 section（`memmap_on_memory` 例外，它先留一段 pageblock 对齐的 vmemmap）。`online_pages` 把 PFN 段挂进某个 zone 的 buddy；`offline_pages` 先隔离，再把还能动的页迁走，迁不走就失败。virtio-mem 走的就是这两条，气球不走。

选 zone 的函数是 `zone_for_pfn_range`（`mm/memory_hotplug.c:1045`）：

| `online_type` | 落到哪 |
|---|---|
| `MMOP_ONLINE_KERNEL` | `default_kernel_zone_for_pfn` |
| `MMOP_ONLINE_MOVABLE` | 该 node 的 `ZONE_MOVABLE`（`:1053`） |
| `ONLINE_POLICY_AUTO_MOVABLE` | `auto_movable_zone_for_pfn`。注释（`:164`）写：配置允许时 online 到 `ZONE_MOVABLE` |
| 其余 | `default_zone_for_pfn`（`:1021`）。区间只和一个 zone 相交就继承那个 zone；都不相交或两个重叠时，只有 `movable_node` 打开才用 `ZONE_MOVABLE`，否则用 kernel zone |

`movable_node` 是 early_param（`:1941`，`cmdline_parse_movable_node`）。默认 online 类型在 `mhp_default_online_type`（`:225`），`mhp_get_default_online_type`（`:226`）在还没人设置时按编译选项填 `MMOP_OFFLINE` / `MMOP_ONLINE` / `MMOP_ONLINE_KERNEL` / `MMOP_ONLINE_MOVABLE`。

```text
online_pages(pfn, nr_pages, zone, group)  # memory_hotplug.c:1140
  要求 pageblock 对齐，且结束于 section 边界，否则 -EINVAL
  move_pfn_range_to_zone(..., MIGRATE_MOVABLE)
  节点第一次有内存：node_notify(NODE_ADDING_FIRST_MEMORY)
  memory_notify(MEM_GOING_ONLINE)
  zone 还没 populated：记一笔，后面 build_all_zonelists
  online_pages_range                      # 页进入 buddy
  adjust_present_page_count
  该 node 置 N_MEMORY；zone 不超过 ZONE_NORMAL 时再置 N_NORMAL_MEMORY
  undo_isolate_page_range                 # 允许分配

offline_pages(start_pfn, nr_pages, zone, group)  # :1955，要拿着 mem_hotplug_lock 写锁
  同样的对齐检查
  walk_system_ram_range 对不上 nr_pages：有洞，-EINVAL
  起止页不在同一个 zone：-EINVAL
  zone_pcp_disable + lru_cache_disable    # 隔离和 PCP 释放不能赛跑
  start_isolate_page_range(..., PB_ISOLATE_MODE_MEM_OFFLINE)
  memory_notify(MEM_GOING_OFFLINE)
  内层循环：
    scan_movable_pages                    # :1799
      找到 LRU 页或带 movable_ops 的页：返回 0
      PageOffline 且还有引用、又没有 movable_ops：-EBUSY（:1819）
      扫完没有可迁页：-ENOENT（:1840）
    返回 0：do_migrate_range              # :1846，void
      migrate_pages(..., MIGRATE_SYNC, MR_MEMORY_HOTPLUG)  # :1920
      失败只 putback。调用点 :2073 不看返回值
      :2070 的 TODO 写着致命失败本应退出，当前没有
    返回值既不是 0 也不是 -ENOENT：当不可移动页，offline 失败
  -ENOENT 之后：dissolve_free_hugetlb_folios，再 test_pages_isolated
    隔离检查失败就再来一轮
  __offline_isolated_pages                # :2099，从 buddy 摘下空闲页
  adjust_present_page_count(..., -nr_pages)  # :2116
  zone 不再 populated：zone_pcp_reset，build_all_zonelists（:2138）
```

`ZONE_MOVABLE` 上的页必须能迁。`scan_movable_pages` 把「`PageOffline`、没有 movable_ops、引用还在」的页当成 `-EBUSY`（`:1819`），offline 就此失败。要能拔的内存，online 时就得进 `ZONE_MOVABLE`，并且上面不要放不可移动的分配。`do_migrate_range` 优先把页迁到别的 node（`:1917` 清掉本 node）；没有别的在线 node 时才允许迁回本 node。

userspace 看块状态：`/sys/devices/system/memory/memory*/state`。字符串表在 `drivers/base/memory.c:36`，`MMOP_ONLINE_MOVABLE` 对应 `"online_movable"`。

---

## 12.10 关键函数表

| 符号 | 位置 | 作用 |
|---|---|---|
| `balloon_page_enqueue` / `balloon_page_dequeue` | `include/linux/balloon.h:62` | 气球页进出设备链表 |
| `virtballoon_free_page_report` | `drivers/virtio/virtio_balloon.c:202` | FPR 的 report 回调 |
| `page_reporting_register` | `mm/page_reporting.c:360` | 注册 FPR 设备 |
| `__page_reporting_notify` | `:98` | 合并通知，调度 work |
| `page_reporting_process` | `:315` | 扫 zone，隔离，报告，放回 |
| `page_reporting_notify_free` | `mm/page_reporting.h:33` | `__free_one_page` 里的筛子 |
| `report_free_page_func` | `virtio_balloon.c:794` | free page hint 的工作函数 |
| `virtio_balloon_report_free_page` | `:774` | 走 hint virtqueue |
| `ksm_scan_thread` | `mm/ksm.c:2836` | KSM 线程 |
| `ksm_do_scan` | `:2815` | 一轮扫描 |
| `ksm_madvise` | `:3010` | `MADV_MERGEABLE` |
| `ksmd_should_run` | `:2831` | `KSM_RUN_MERGE` 且 mm 链表非空 |
| `scan_get_next_rmap_item` | `mm/ksm.c:2610` | 取下一个候选页。`ksm_do_scan` 在 `:2822` 调用 |
| `cmp_and_merge_page` | `:2276` | 和稳定树比较并合并。调用点 `:2825` |
| `online_pages` | `mm/memory_hotplug.c:1140` | 一段 PFN online 进 zone |
| `offline_pages` | `:1955` | 隔离、迁移、摘下 |
| `zone_for_pfn_range` | `:1045` | 按 online 类型选 zone |
| `do_migrate_range` | `:1846` | offline 时迁走可移动页 |
| `scan_movable_pages` | `:1799` | 找下一段可迁页 |

---

[上一节 KVM](./11-kvm-memory.md) · [目录](./README.md) · [下一节 观测](./13-observability.md)
