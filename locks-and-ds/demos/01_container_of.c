/*
 * 用户态教学实现：offsetof + container_of
 * 对照内核 include/linux/container_of.h、include/linux/stddef.h
 * 非内核源码拷贝；类型检查简化，仅演示指针算术。
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* 与内核同思路：成员相对结构体开头的字节偏移 */

/* 教学版 container_of：已知成员指针 → 外层对象指针 */
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct my_obj {
	int id;
	char name[16];
	struct {
		struct list_node *next;
		struct list_node *prev;
	} link; /* 模拟嵌在对象里的 list_head */
};

struct list_node {
	struct list_node *next, *prev;
};

int main(void)
{
	struct my_obj obj = {
		.id = 42,
		.name = "demo",
	};

	/* 假设我们只拿到了 &obj.link（像 list 遍历时那样） */
	void *member_ptr = &obj.link;
	struct my_obj *recovered = container_of(member_ptr, struct my_obj, link);

	printf("sizeof(struct my_obj)     = %zu\n", sizeof(struct my_obj));
	printf("offsetof(my_obj, id)      = %zu\n", offsetof(struct my_obj, id));
	printf("offsetof(my_obj, name)    = %zu\n", offsetof(struct my_obj, name));
	printf("offsetof(my_obj, link)    = %zu\n", offsetof(struct my_obj, link));
	printf("&obj                      = %p\n", (void *)&obj);
	printf("&obj.link                 = %p\n", member_ptr);
	printf("container_of(...)         = %p\n", (void *)recovered);
	printf("recovered->id             = %d\n", recovered->id);
	printf("recovered->name           = %s\n", recovered->name);
	printf("same object?              = %s\n",
	       recovered == &obj ? "YES" : "NO");

	return recovered == &obj ? 0 : 1;
}
