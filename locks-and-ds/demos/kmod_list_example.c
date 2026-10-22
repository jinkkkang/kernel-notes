/*
 * 示例内核模块：演示 list_head 挂链与遍历
 * 【教学示例】勿当生产模块直接加载；需本机内核头文件才能编译。
 *
 * 对照：include/linux/list.h、locks-and-ds/02-list.md
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kernel-notes demos");
MODULE_DESCRIPTION("Teaching example: intrusive list_head walk");

struct demo_item {
	int id;
	struct list_head node;
};

static LIST_HEAD(demo_list);

static int __init demo_list_init(void)
{
	struct demo_item *it, *tmp;
	int i;

	for (i = 0; i < 3; i++) {
		it = kmalloc(sizeof(*it), GFP_KERNEL);
		if (!it)
			goto err;
		it->id = i + 1;
		list_add_tail(&it->node, &demo_list);
	}

	pr_info("kmod_list_example: walk\n");
	list_for_each_entry(it, &demo_list, node)
		pr_info("  id=%d\n", it->id);

	return 0;
err:
	list_for_each_entry_safe(it, tmp, &demo_list, node) {
		list_del(&it->node);
		kfree(it);
	}
	return -ENOMEM;
}

static void __exit demo_list_exit(void)
{
	struct demo_item *it, *tmp;

	list_for_each_entry_safe(it, tmp, &demo_list, node) {
		list_del(&it->node);
		kfree(it);
	}
	pr_info("kmod_list_example: exit\n");
}

module_init(demo_list_init);
module_exit(demo_list_exit);
