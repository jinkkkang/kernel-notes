// SPDX-License-Identifier: GPL-2.0
/*
 * RECIPE：自定义 kmem_cache 示例。本仓库不装载模块。
 * 对照 v7.3：kmem_cache_create() include/linux/slab.h
 *            kmem_cache_alloc_noprof() mm/slub.c:5001
 *
 * 模块参数和 insmod 步骤见 memory-management/04-slab.md。
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

struct demo_obj {
	unsigned long seq;
	char tag[24];
};

static struct kmem_cache *demo_cache;

static int __init demo_init(void)
{
	struct demo_obj *obj;

	demo_cache = kmem_cache_create("demo_obj", sizeof(struct demo_obj),
				       0, SLAB_HWCACHE_ALIGN, NULL);
	if (!demo_cache)
		return -ENOMEM;

	obj = kmem_cache_alloc(demo_cache, GFP_KERNEL);
	if (!obj) {
		kmem_cache_destroy(demo_cache);
		return -ENOMEM;
	}
	obj->seq = 1;
	strscpy(obj->tag, "slab-demo", sizeof(obj->tag));
	pr_info("demo_obj %p seq=%lu tag=%s\n", obj, obj->seq, obj->tag);
	kmem_cache_free(demo_cache, obj);
	return 0;
}

static void __exit demo_exit(void)
{
	kmem_cache_destroy(demo_cache);
}

module_init(demo_init);
module_exit(demo_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kernel-notes SLUB cache example");
