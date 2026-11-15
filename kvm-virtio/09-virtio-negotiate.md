# 9. virtio 模型与协商

**结论**：客户机里 virtio 是一条总线。传输层（PCI modern、PCI legacy、MMIO）探测到设备后 `register_virtio_device`，总线按 `virtio_device_id` 匹配驱动。驱动在 `virtio_dev_probe` 里把设备 feature 和自己的 feature 表求交，写回设备，等设备接受 `FEATURES_OK`，再 `probe`，最后置 `DRIVER_OK`。在 `DRIVER_OK` 之前，行为良好的设备不向队列发通知。

> 主线是 virtio-pci modern：`virtio_pci_probe` 先 `virtio_pci_modern_probe`，失败且错误是 `-ENODEV` 才试 legacy。MMIO 只点 `virtio_mmio.c` 的队列建立函数。

---

## 9.1 设备和驱动

```c
/* include/linux/virtio.h:122，内核对象，不是用户态 ABI */
struct virtio_device {
	int index;
	bool failed;
	bool config_enabled;
	bool config_change_pending;
	spinlock_t config_lock;
	spinlock_t vqs_list_lock;
	struct device dev;                         /* 挂在 virtio 总线上 */
	struct virtio_device_id id;                /* device / vendor，用来匹配 */
	const struct virtio_config_ops *config;    /* 传输层填的 get/set_status、get_features */
	const struct vringh_config_ops *vringh_config;
	struct list_head vqs;
	u64 features;                              /* 双方都同意的 feature */
	void *priv;                                /* 驱动的私有数据，virtio-blk 放 struct virtio_blk */
};

/* :182，驱动注册时填的表 */
struct virtio_driver {
	struct device_driver driver;
	const struct virtio_device_id *id_table;
	const unsigned int *feature_table;
	unsigned int feature_table_size;
	const unsigned int *feature_table_legacy;
	unsigned int feature_table_size_legacy;
	int (*validate)(struct virtio_device *dev);
	int (*probe)(struct virtio_device *dev);
	void (*scan)(struct virtio_device *dev);
	void (*remove)(struct virtio_device *dev);
	void (*config_changed)(struct virtio_device *dev);
	int (*freeze)(struct virtio_device *dev);
	int (*restore)(struct virtio_device *dev);
};
```

`virtio_device` 嵌着 `struct device`，`dev_to_virtio` 用 `container_of` 取回来。匹配发生在 virtio 总线的 `match` 上，比较 `id_table` 里的设备号。virtio-blk 的设备号在 `include/uapi/linux/virtio_ids.h`。传输层不认识 blk 请求的布局；它只提供配置空间、队列发现和通知寄存器。

状态字节在 `include/uapi/linux/virtio_config.h:36`：

| 宏 | 值 | 谁置位 |
|---|---|---|
| `VIRTIO_CONFIG_S_ACKNOWLEDGE` | 1 | 驱动发现设备 |
| `VIRTIO_CONFIG_S_DRIVER` | 2 | 找到了驱动，`virtio_dev_probe` 一进来就加（`virtio.c:247`） |
| `VIRTIO_CONFIG_S_FEATURES_OK` | 8 | 驱动写完 feature，设备必须回读仍然带着这一位 |
| `VIRTIO_CONFIG_S_DRIVER_OK` | 4 | 驱动准备好使用队列 |
| `VIRTIO_CONFIG_S_NEEDS_RESET` | `0x40` | 设备进入了驱动必须重置的状态 |
| `VIRTIO_CONFIG_S_FAILED` | `0x80` | 协商或 probe 失败（`virtio.c:320`） |

---

## 9.2 `virtio_dev_probe` 的顺序

`virtio_dev_probe`（`drivers/virtio/virtio.c:237`）：

1. `virtio_add_status(DRIVER)`。
2. `config->get_features` 读设备提供的 64 位 feature。
3. 把 `feature_table` 里的位或进 `driver_features`。设备没有 `VIRTIO_F_VERSION_1`（bit 32，`virtio_config.h:67`）时，改用 `feature_table_legacy`。
4. `dev->features` 是两边的交集。传输层保留位 `VIRTIO_TRANSPORT_F_START`（28）到 `VIRTIO_TRANSPORT_F_END`（41）若设备提供了，额外设上（`:278`）。`VIRTIO_F_RING_PACKED` 是 34（`:83`），落在这个范围里，所以它是传输 feature，不是 virtio-blk 私有的位。
5. `config->finalize_features` 把交集写回设备。
6. 可选的 `drv->validate`。若它改了 `dev->features`，再 `finalize_features` 一次。
7. `virtio_features_ok`（`:171`）。没有 `VIRTIO_F_VERSION_1` 时直接返回 0，不写 `FEATURES_OK`（legacy）。有 v1 时 `virtio_add_status(FEATURES_OK)`，再 `get_status`。设备若清掉了这一位，返回 `-ENODEV`（`:196`）。需要平台访问检查的设备还要求同时有 `VIRTIO_F_ACCESS_PLATFORM`（bit 33）。
8. `drv->probe`。virtio-blk 在这里建队列和 `gendisk`。
9. 若 probe 没有自己置 `DRIVER_OK`，调用 `virtio_device_ready`（`include/linux/virtio_config.h:270`）。它在 `CONFIG_VIRTIO_HARDEN_NOTIFICATION` 下先同步回调并 `__virtio_unbreak_device`，然后 `set_status(status | DRIVER_OK)`（`:297`）。注释写明：行为良好的设备只在看到 `DRIVER_OK` 之后通知队列。

失败路径统一 `virtio_add_status(FAILED)`。此时队列还不该被后端消费。reset 把状态清掉并拆队列，见 [13](./13-reset-migration.md)。

---

## 9.3 modern PCI，legacy 与 MMIO

`virtio_pci_probe`（`drivers/virtio/virtio_pci_common.c:524`）在 `force_legacy` 为假时先 `virtio_pci_modern_probe`（`:555`）。返回 `-ENODEV` 才 `virtio_pci_legacy_probe`。`force_legacy` 为真时顺序反过来，并且 legacy 映射 BAR0 失败（`-ENODEV` 或 `-ENOMEM`）仍会再试 modern。成功之后 `pci_set_master`，`register_virtio_device`。

`virtio_pci_modern_probe`（`drivers/virtio/virtio_pci_modern.c:535`）调用 `vp_modern_probe`（`virtio_pci_modern_dev.c:215`）映射 capability 指向的 common config、notify、ISR 和 device config，然后把 `setup_vq` 设成 modern 的 `setup_vq`（`virtio_pci_modern.c:553`）。建队列时（`:300`）若协商了 `VIRTIO_F_NOTIFICATION_DATA` 就用 `vp_notify_with_data`，否则用 `vp_notify`。`vring_create_virtqueue` 按有没有 `VIRTIO_F_RING_PACKED` 选择 packed 或 split（`virtio_ring.c:2693`）。队列内存和通知寄存器都准备好之后，才回到上一节的 `DRIVER_OK`。

legacy 的 `setup_vq` 在 `drivers/virtio/virtio_pci_legacy.c:110`。它使用 IO 端口上的队列选择器和 PFN，没有 common config capability。功能上仍能 `virtqueue_add_*`，但 feature 协商停在没有 `FEATURES_OK` 的那条分支。

MMIO 传输的队列函数是 `vm_setup_vq`（`drivers/virtio/virtio_mmio.c:355`）。寄存器在内存映射的一页里，不经过 PCI capability。本章的 blk 和 vhost 例子都按 PCI modern 讲通知寄存器；MMIO 的 kick 同样是一次客户机存储，ioeventfd 登记的是那个 MMIO 地址而不是 PIO 端口。

*笔记生成对照源码：Linux v6.6.160。内核树不放进本仓库。*

**导航**：[← PIO / MMIO](./08-pio-mmio-notify.md) · [目录](./README.md) · [virtqueue →](./10-virtqueue.md)
