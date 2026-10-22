/* 打印本进程 maps 中 demo 自己 mmap 的那一行，并读 smaps_rollup。
 * 对照笔记：memory-management/06-mm-and-vma.md
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAP_BYTES (2 * 1024 * 1024)

static void dump_file(const char *path, const char *needle)
{
	char line[512];
	FILE *fp = fopen(path, "r");

	if (!fp) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return;
	}
	printf("--- %s ---\n", path);
	while (fgets(line, sizeof(line), fp)) {
		if (!needle || strstr(line, needle))
			fputs(line, stdout);
	}
	fclose(fp);
}

int main(void)
{
	char needle[64];
	void *p = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	/* 碰第一页，让 VMA 上出现匿名 RSS。 */
	((volatile char *)p)[0] = 1;
	snprintf(needle, sizeof(needle), "%lx", (unsigned long)p);
	printf("mmap [%p, +%d)\n", p, MAP_BYTES);
	dump_file("/proc/self/maps", needle);
	dump_file("/proc/self/smaps_rollup", NULL);
	munmap(p, MAP_BYTES);
	return 0;
}
