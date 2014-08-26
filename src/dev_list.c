#include "dev_list.h"

#include "wire_lock.h"
#include "wire_io.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

static dev_list_t *last_dev_list;
static time_t next_update;
static int count;
static wire_lock_t lock;

void dev_list_init(void)
{
	wire_lock_init(&lock);
}

static void dev_list_free(dev_list_t *l)
{
	dev_list_t *next;

	while (l) {
		next = l->next;
		free(l);
		l = next;
	}
}

static int dev_list_gt(dev_list_t *a, dev_list_t *b)
{
	if (a->major > b->major)
		return 1;
	if (a->major < b->major)
		return 0;

	if (a->minor > b->minor)
		return 1;
	if (a->minor < b->minor)
		return 0;

	if (a->mtime > b->mtime)
		return 1;
	return 0;
}

static dev_list_t *dev_list_add(dev_list_t *head, dev_list_t *n)
{
	dev_list_t *l = head;
	dev_list_t *prev = NULL;

	while (l && dev_list_gt(l, n)) {
		prev = l;
		l = prev->next;
	}

	if (!prev) {
		// Add as head
		n->next = l;
		return n;
	} else {
		n->next = prev->next;
		prev->next = n;
		return head;
	}
}

static int dev_list_func(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	if (typeflag == FTW_NS) {
		return 0;
	}

	if (S_ISBLK(sb->st_mode) || S_ISCHR(sb->st_mode)) {
		dev_list_t *n = malloc(sizeof(dev_list_t));
		if (n == NULL)
			return -1;

		n->major = major(sb->st_rdev);
		n->minor = minor(sb->st_rdev);
		n->is_blk = S_ISBLK(sb->st_mode);
		n->mtime = sb->st_mtime;
		strncpy(n->name, fpath, sizeof(n->name));
		n->name[sizeof(n->name)-1] = 0;
		last_dev_list = dev_list_add(last_dev_list, n);
	}

	return 0;
}

/* This assumes the data structure is locked and unchanged. */
static void dev_list_collect(void)
{
	assert(last_dev_list == NULL);
	wio_nftw("/dev", dev_list_func, 32, FTW_MOUNT|FTW_PHYS);
}

dev_list_t *dev_list_get(void)
{
	time_t now;

	// Lock to avoid generation by multiple wires, generation will yield
	wire_lock_take(&lock);

	now = time(NULL);
	if (last_dev_list && now < next_update) {
		count++;
	} else {
		dev_list_free(last_dev_list);
		last_dev_list = NULL;
		dev_list_collect();
		next_update = now + 10;
	}

	if (last_dev_list)
		count++;

	wire_lock_release(&lock);
	return last_dev_list;
}

void dev_list_release(dev_list_t *l)
{
	// No locking needed, this happens without a yield inside
	assert(count > 0);
	assert(l == last_dev_list);

	count--;

	if (count == 0) {
		time_t now = time(NULL);
		if (now >= next_update) {
			last_dev_list = NULL;
			dev_list_free(l);
		}
	}
}
