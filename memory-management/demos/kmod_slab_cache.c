// SPDX-License-Identifier: GPL-2.0
/*
 * RECIPE：自定义 kmem_cache 示例。本仓库不编译、不装载。
 * 对照 v6.6.160 的声明（include/linux/slab.h）：
 *   kmem_cache_create()   五参数，定义在 mm/slab_common.c
 *   kmem_cache_alloc()    定义在 mm/slub.c
 *   kmem_cache_free()     定义在 mm/slub.c
 *   kmem_cache_destroy()  定义在 mm/slab_common.c
 *
 * 模块步骤见 memory-management/04-slab.md，标成 RECIPE。
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

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
