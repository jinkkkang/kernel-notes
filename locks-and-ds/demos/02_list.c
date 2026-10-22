/*
 * 用户态教学实现：最小侵入式双向链表
 * 对照内核 include/linux/list.h 的 INIT / add / del / for_each / _safe
 * 故意不用 poison，便于在用户态安全打印。
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
	entry->next = NULL;
	entry->prev = NULL;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each_entry(pos, head, member)                         \
	for (pos = list_entry((head)->next, typeof(*pos), member);     \
	     &pos->member != (head);                                   \
	     pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)                 \
	for (pos = list_entry((head)->next, typeof(*pos), member),     \
	     n = list_entry(pos->member.next, typeof(*pos), member);   \
	     &pos->member != (head);                                   \
	     pos = n, n = list_entry(n->member.next, typeof(*n), member))

struct victim {
	int age;
	char tag[8];
	struct list_head node;
};

static void print_list(const char *title, struct list_head *head)
{
	struct victim *v;
	printf("%s:", title);
	list_for_each_entry(v, head, node)
		printf(" {%s age=%d}", v->tag, v->age);
	printf("\n");
}

int main(void)
{
	struct list_head zombie_list;
	struct victim a, b, c, *pos, *tmp;

	INIT_LIST_HEAD(&zombie_list);

	a.age = 10; strcpy(a.tag, "A"); INIT_LIST_HEAD(&a.node);
	b.age = 20; strcpy(b.tag, "B"); INIT_LIST_HEAD(&b.node);
	c.age = 30; strcpy(c.tag, "C"); INIT_LIST_HEAD(&c.node);

	list_add_tail(&a.node, &zombie_list); /* FIFO: A */
	list_add_tail(&b.node, &zombie_list); /* A B */
	list_add(&c.node, &zombie_list);      /* 头插: C A B */
	print_list("after add (expect C A B)", &zombie_list);

	printf("walk with list_for_each_entry:\n");
	list_for_each_entry(pos, &zombie_list, node)
		printf("  pos=%p tag=%s age=%d\n", (void *)pos, pos->tag, pos->age);

	/* 遍历中删除 age==20 —— 必须用 _safe */
	list_for_each_entry_safe(pos, tmp, &zombie_list, node) {
		if (pos->age == 20) {
			printf("deleting %s during walk\n", pos->tag);
			list_del(&pos->node);
		}
	}
	print_list("after safe-delete B (expect C A)", &zombie_list);

	return 0;
}
