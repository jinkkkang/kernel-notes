/*
 * 用户态教学：简易哈希桶 + hlist 风格节点（pprev）
 * 对照内核 hlist_head / hlist_node / DEFINE_HASHTABLE 直觉
 * 不是完整内核 hashtable.h；键为字符串，桶数固定 8。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct hlist_head {
	struct hlist_node *first;
};

static inline void INIT_HLIST_HEAD(struct hlist_head *h)
{
	h->first = NULL;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

static inline void hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
	n->next = NULL;
	n->pprev = NULL;
}

#define HLIST_HEAD_INIT { .first = NULL }
#define HASH_BITS 3
#define HASH_SIZE (1 << HASH_BITS)

struct item {
	char key[32];
	int value;
	struct hlist_node node;
};

static uint32_t hash_str(const char *s)
{
	uint32_t h = 5381;
	while (*s)
		h = ((h << 5) + h) + (unsigned char)(*s++);
	return h;
}

static struct hlist_head buckets[HASH_SIZE];

static void insert(const char *key, int value)
{
	struct item *it = calloc(1, sizeof(*it));
	unsigned b = hash_str(key) & (HASH_SIZE - 1);
	snprintf(it->key, sizeof(it->key), "%s", key);
	it->value = value;
	hlist_add_head(&it->node, &buckets[b]);
	printf("insert key=%s value=%d -> bucket[%u]\n", key, value, b);
}

static struct item *lookup(const char *key)
{
	unsigned b = hash_str(key) & (HASH_SIZE - 1);
	struct hlist_node *n;
	for (n = buckets[b].first; n; n = n->next) {
		struct item *it = container_of(n, struct item, node);
		if (strcmp(it->key, key) == 0)
			return it;
	}
	return NULL;
}

static void dump_all(void)
{
	unsigned i;
	for (i = 0; i < HASH_SIZE; i++) {
		struct hlist_node *n = buckets[i].first;
		if (!n)
			continue;
		printf("bucket[%u]:", i);
		for (; n; n = n->next) {
			struct item *it = container_of(n, struct item, node);
			printf(" {%s=%d}", it->key, it->value);
		}
		printf("\n");
	}
}

int main(void)
{
	unsigned i;
	struct item *found, *gone;

	for (i = 0; i < HASH_SIZE; i++)
		INIT_HLIST_HEAD(&buckets[i]);

	insert("apple", 1);
	insert("banana", 2);
	insert("cherry", 3);
	insert("date", 4);
	insert("apple2", 5); /* 可能与 apple 同桶，演示拉链 */

	dump_all();

	found = lookup("banana");
	printf("lookup banana -> %s\n",
	       found ? found->key : "(null)");
	if (found)
		printf("  value=%d\n", found->value);

	gone = lookup("cherry");
	if (gone) {
		printf("delete cherry\n");
		hlist_del(&gone->node);
		free(gone);
	}
	printf("lookup cherry after del -> %s\n",
	       lookup("cherry") ? "FOUND" : "NULL");
	dump_all();
	return 0;
}
