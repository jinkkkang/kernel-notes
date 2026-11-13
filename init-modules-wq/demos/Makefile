# 教学模块，对照 Linux v6.6.160 的 workqueue / module API。
# RECIPE / 待真机验证：本仓库不在这里 make，也不 insmod。
# 需要与正在运行的内核一致的头文件：
#   /lib/modules/$(uname -r)/build
# 加载必须由人在测试机上显式执行 demos/01-run.sh（KN_DEMO_LOAD=1）。
# 不要在生产机上加载。

obj-m += kn_demo_wq.o
kn_demo_wq-objs := 01-kmod-wq.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
