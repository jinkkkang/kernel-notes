/*
 * 示例内核模块：completion 的计数与 wait_for_completion_timeout
 * 【教学示例】勿当生产模块直接加载；需本机内核头文件才能编译。
 *
 * 对照：include/linux/completion.h、kernel/sched/completion.c、
 *       locks-and-ds/08-wait-queue-and-completion.md
 *
 * 笔记钉 Linux v6.6.160 的符号。本文件没有在该版本上 make / insmod。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kernel-notes demos");
MODULE_DESCRIPTION("Teaching example: wait_for_completion_timeout");

static struct completion demo_comp;

static int demo_completer(void *unused)
{
	msleep(50);
	complete(&demo_comp);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static int __init demo_init(void)
{
	struct task_struct *task;
	unsigned long left;

	init_completion(&demo_comp);
	task = kthread_run(demo_completer, NULL, "kmod_compl");
	if (IS_ERR(task))
		return PTR_ERR(task);

	/*
	 * complete 若先发生，done 记着这一次，这里不睡也返回正数。
	 * 返回 0 只表示超时。完成时至少是 1，或剩余 jiffies。
	 */
	left = wait_for_completion_timeout(&demo_comp, msecs_to_jiffies(200));
	pr_info("kmod_completion_timeout: after complete, left=%lu\n", left);

	kthread_stop(task);

	reinit_completion(&demo_comp);
	left = wait_for_completion_timeout(&demo_comp, msecs_to_jiffies(20));
	pr_info("kmod_completion_timeout: no complete, left=%lu\n", left);
	return 0;
}

static void __exit demo_exit(void)
{
	pr_info("kmod_completion_timeout: exit\n");
}

module_init(demo_init);
module_exit(demo_exit);
