# 6. 客户机内存：接到执行和 IO

**结论**：客户机取指用的是线性地址。CR0.PG 为 0 时，这个线性地址就是 GPA。GPA 落在 memslot 里时，`__gfn_to_hva_memslot` 把它换成 QEMU 进程的 HVA。宿主机内核或 vhost 访问这个 HVA 时，走的是宿主机自己的页表，得到 HPA。客户机 CPU 在 VMX non-root 里访问同一 GPA 时，硬件走 EPT 或 NPT。PIO 退出里的数据字节不在客户机 GPA 上，它在 vCPU 映射的 `pio_data` 页。

> EPT/NPT 怎样建、`mmu_notifier` 怎样拆，本章不重写。[memory-management/11](../memory-management/11-kvm-memory.md) 从 `kvm_mmu_page_fault` 写起。这里只说明执行和端口 IO 各自用哪一个地址。

---

## 6.1 五个地址，各用一次

| 名字 | 谁产生它 | 在执行或 IO 里出现在哪 |
|---|---|---|
| GVA | 客户机的分段 + 分页 | 保护模式或长模式下，指令指针和数据访问先是 GVA。实模式 demo 没有这一步：线性地址 = CS.base + RIP |
| GPA | 客户机以为的物理地址 | VMX EPT violation 时从 `GUEST_PHYSICAL_ADDRESS` 读出（`vmx.c:5764`）。memslot 的 `guest_phys_addr` 也是 GPA |
| HVA | 宿主机用户进程的虚拟地址 | `kvm_userspace_memory_region.userspace_addr`。`__gfn_to_hva_memslot`（`include/linux/kvm_host.h:1734`）计算 `userspace_addr + (gfn - base_gfn) * PAGE_SIZE` |
| HPA | 宿主机物理页 | 宿主机页表把 HVA 翻译成它。EPT 的叶子最终也指向它。叶子格式不在本章 |
| PIO 缓冲 | 内核分配的一页 | `vcpu->arch.pio_data`。`kvm_run->io.data_offset` 指向 vCPU mmap 里的这一页（`x86.c:7962`） |

demo 把一页匿名内存登记在 GPA 0，CS base 设为 0，RIP 设为 0。实模式下取指的线性地址是 0，也就是 GPA 0。这一页的 HVA 是 `mmap` 的返回值。客户机要执行 `0xEE, 0xF4` 这两个字节，EPT（或非 EPT 时的影子页表）必须能把 GPA 0 译出来。译不出来时 `handle_ept_violation` 调用 `kvm_mmu_page_fault`，处理过程仍在同一次 `KVM_RUN` 里，用户态看不到 `KVM_EXIT_MMIO`。

GPA 没有 slot，或 slot 把这段标成 MMIO 区域时，访问才可能变成 `KVM_EXIT_MMIO`。`kvm_run->mmio` 里有 `phys_addr`、最多 8 字节的 `data`、`len` 和 `is_write`（`include/uapi/linux/kvm.h:334`，上一行是 `KVM_EXIT_MMIO` 注释）。这 8 字节在 `struct kvm_run` 里面。PIO 的数据不在这个联合体成员里，而在单独的页上。两种退出都叫「设备访问」，缓冲区位置不同。

---

## 6.2 内核怎样碰到 HVA

`gfn_to_hva`（`virt/kvm/kvm_main.c:2457`）先 `gfn_to_memslot`，再 `gfn_to_hva_many`。vCPU 热路径用 `kvm_vcpu_gfn_to_hva`（`:2463`），slot 查找会先试 `vcpu->last_used_slot`。返回值是一个 HVA；`kvm_is_error_hva` 用来识别非法值。

vhost 不调用这个函数走 KVM 的 slot 锁。它用自己的 IOTLB：`VHOST_SET_MEM_TABLE` 或 IOTLB 消息把 GPA 区间映射到用户地址，`translate_desc`（`drivers/vhost/vhost.c:2500`）在这张表里把描述符里的 GPA 收成 `struct iovec`，`iov_base` 是 `__user` 指针。那是同一段客户机内存的另一条查找路径。KVM slot 和 vhost IOTLB 必须由用户态登记成同一块 HVA，否则 vhost 读到的描述符和客户机写下的不是一页。

宿主机改这片 HVA 的页表（回收、迁移、`munmap`）时，KVM 通过 `mmu_notifier` 拆掉已经建好的第二级翻译。回调的入口在内存章 11.8。本章只记一个后果：客户机下一次访问会再 fault，`KVM_RUN` 仍可能不返回用户态。

---

## 6.3 和 IO 退出的分界

`emulator_pio_in_out`（`x86.c:7917`）对每个端口调用 `kvm_io_bus_write` 或 `kvm_io_bus_read`。总线地址是端口号，不是 GPA。内核设备（in-kernel PIC、ioeventfd）认领之后函数返回 1，指令在内核里完成。没有设备时才 `memcpy` 到 `pio_data` 并设置 `KVM_EXIT_IO`。

因此：

- 客户机 `OUT` 的那个字节，用户态在 `kvm_run` 映射的 `data_offset` 处读到。它不是客户机内存 GPA `0x3f8`。
- 客户机指令的字节在 GPA 0 的那一页里，也就是用户态当初 `mmap` 的匿名页。
- 后端稍后要读 virtqueue，读的是客户机驱动放描述符的 GPA，经 slot 或 vhost IOTLB 变成 HVA。那又是第三处内存，和 PIO 缓冲无关。

dirty log 记的是客户机对 RAM slot 的写，不是 PIO 缓冲的写。迁移边界在 [13](./13-reset-migration.md)。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← vCPU 调度](./05-vcpu-schedule.md) · [目录](./README.md) · [中断与时钟 →](./07-irq-clock.md)
