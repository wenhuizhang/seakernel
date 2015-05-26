#ifndef __SEA_FS_KERFS
#define __SEA_FS_KERFS

#include <sea/fs/inode.h>

#define KERFS_TYPE_INTEGER 1

#define KERFS_PARAM 1
#define KERFS_PARAM_READONLY 2

void kerfs_init();
int kerfs_read(struct inode *node, size_t offset, size_t length, char *buffer);
int kerfs_write(struct inode *node, size_t offset, size_t length, const char *buffer);
int kerfs_register_parameter(char *path, void *param, size_t size, int flags, int type);
int kerfs_register_report(char *path, int (*fn)(size_t, size_t, char *));
int kerfs_syscall_report(size_t offset, size_t length, char *buf);
int kerfs_int_report(size_t offset, size_t length, char *buf);
int kerfs_kmalloc_report(size_t offset, size_t length, char *buf);
int kerfs_pmm_report(size_t offset, size_t length, char *buf);
int kerfs_icache_report(size_t offset, size_t length, char *buf);

#define KERFS_PRINTF(offset,length,buf,current,format...) \
	do { \
		char line[1024]; \
		int add = snprintf(line, 1024, format); \
		if(current + add > offset && current < (offset + length)) { \
			size_t linestart = current > offset ? 0 : (offset - current); \
			size_t bufstart  = current > offset ? (current - offset) : 0; \
			size_t amount = add - linestart; \
			if(amount > ((offset + length) - current)) \
				amount = (offset + length) - current; \
			memcpy(buf + bufstart, line + linestart, amount); \
			current += amount; \
		} \
	} while(0);

#endif
