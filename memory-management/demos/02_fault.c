/* 匿名映射后逐页写入，观察 ru_minflt 上升。
 * 对照笔记：memory-management/07-page-fault.md
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
	long pagesz = sysconf(_SC_PAGESIZE);
	const int npages = 256;
	size_t len = (size_t)pagesz * npages;
	struct rusage before, after;
	char *p;
	int i;

	if (getrusage(RUSAGE_SELF, &before) != 0) {
		perror("getrusage");
		return 1;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (i = 0; i < npages; i++)
		p[i * pagesz] = (char)i;

	if (getrusage(RUSAGE_SELF, &after) != 0) {
		perror("getrusage");
		return 1;
	}
	printf("pagesize=%ld touched=%d\n", pagesz, npages);
	printf("minflt %ld -> %ld (delta %ld)\n",
	       before.ru_minflt, after.ru_minflt,
	       after.ru_minflt - before.ru_minflt);
	printf("majflt %ld -> %ld\n", before.ru_majflt, after.ru_majflt);
	munmap(p, len);
	return 0;
}
