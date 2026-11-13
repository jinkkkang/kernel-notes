// SPDX-License-Identifier: GPL-2.0
/*
 * RECIPE / 待真机验证。
 * 教学模块：init 里创建一条 WQ_UNBOUND | WQ_SYSFS 的队列并 queue_work，
 * exit 里 destroy_workqueue（先 drain）。
 *
 * 对照 Linux v6.6.160：
 *   alloc_workqueue          kernel/workqueue.c:4755   EXPORT_SYMBOL_GPL
 *   destroy_workqueue        kernel/workqueue.c:4869   先 drain_workqueue
 *   INIT_WORK                include/linux/workqueue.h:302
 *   queue_work               include/linux/workqueue.h:564
 *   raw_smp_processor_id     include/linux/smp.h（SMP 时转到 asm/smp.h）
 *   module_param_string      include/linux/moduleparam.h:355
 *
 * 本仓库的写作环境不编译、不 insmod。验收见 demos/README.md。
 * 不要在生产机上加载。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

static char msg[64] = "kn-demo";
module_param_string(msg, msg, sizeof(msg), 0644);
MODULE_PARM_DESC(msg, "string printed from init, work, and exit");

static struct workqueue_struct *kn_wq;
static struct work_struct kn_work;

static void kn_demo_work(struct work_struct *work)
{
	pr_info("kn_demo_wq: work comm=%s pid=%d cpu=%d msg=%s\n",
		current->comm, current->pid, raw_smp_processor_id(), msg);
}

static int __init kn_demo_init(void)
{
	pr_info("kn_demo_wq: init comm=%s pid=%d msg=%s\n",
		current->comm, current->pid, msg);

	kn_wq = alloc_workqueue("kn-demo-wq", WQ_UNBOUND | WQ_SYSFS, 0);
	if (!kn_wq)
		return -ENOMEM;

	INIT_WORK(&kn_work, kn_demo_work);
	if (!queue_work(kn_wq, &kn_work)) {
		pr_err("kn_demo_wq: queue_work returned false\n");
		destroy_workqueue(kn_wq);
		kn_wq = NULL;
		return -EBUSY;
	}
	return 0;
}

static void __exit kn_demo_exit(void)
{
	if (kn_wq)
		destroy_workqueue(kn_wq);
	kn_wq = NULL;
	pr_info("kn_demo_wq: exit comm=%s pid=%d msg=%s\n",
		current->comm, current->pid, msg);
}

module_init(kn_demo_init);
module_exit(kn_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kernel-notes demos");
MODULE_DESCRIPTION("Teaching example: unbound workqueue (RECIPE, not loaded here)");
