/*
 * 示例内核模块：IDR 分配整数 ID → 指针
 * 【教学示例】示意 idr_alloc / idr_find / idr_remove；勿直接生产加载。
 *
 * 对照：include/linux/idr.h、locks-and-ds/03-trees-maps.md
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/idr.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kernel-notes demos");
MODULE_DESCRIPTION("Teaching example: idr alloc/find/remove");

struct demo_obj {
	const char *name;
};

static DEFINE_IDR(demo_idr);

static int __init demo_idr_init(void)
{
	struct demo_obj *a, *b, *found;
	int id_a, id_b;

	a = kmalloc(sizeof(*a), GFP_KERNEL);
	b = kmalloc(sizeof(*b), GFP_KERNEL);
	if (!a || !b)
		goto err;
	a->name = "alpha";
	b->name = "beta";

	id_a = idr_alloc(&demo_idr, a, 1, 0, GFP_KERNEL);
	id_b = idr_alloc(&demo_idr, b, 1, 0, GFP_KERNEL);
	if (id_a < 0 || id_b < 0)
		goto err;

	pr_info("kmod_idr_example: allocated id_a=%d id_b=%d\n", id_a, id_b);

	found = idr_find(&demo_idr, id_a);
	pr_info("kmod_idr_example: find(%d) -> %s\n",
		id_a, found ? found->name : "(null)");

	return 0;
err:
	kfree(a);
	kfree(b);
	idr_destroy(&demo_idr);
	return -ENOMEM;
}

static void __exit demo_idr_exit(void)
{
	struct demo_obj *obj;
	int id;

	idr_for_each_entry(&demo_idr, obj, id) {
		idr_remove(&demo_idr, id);
		kfree(obj);
	}
	idr_destroy(&demo_idr);
	pr_info("kmod_idr_example: exit\n");
}

module_init(demo_idr_init);
module_exit(demo_idr_exit);
