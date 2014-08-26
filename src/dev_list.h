#ifndef DOCKET_DEV_LIST_H
#define DOCKET_DEV_LIST_H

#include <time.h>

typedef struct dev_list {
	struct dev_list *next;
	unsigned int major;
	unsigned int minor;
	int is_blk;
	time_t mtime;
	char name[128];
} dev_list_t;

void dev_list_init(void);
dev_list_t *dev_list_get(void);
void dev_list_release(dev_list_t *l);

#endif
