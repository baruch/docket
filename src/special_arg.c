#include "special_arg.h"
#include "dev_list.h"

#include "wire_io.h"
#include "wire_log.h"

#include <string.h>
#include <errno.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

static int special_arg_net(char **items, unsigned items_size, unsigned item_size)
{
	struct ifaddrs *ifaddr;
	struct ifaddrs *ifa;
	int ret;
	int n;
	int items_len = 0;
	int item_idx;

	ret = wio_getifaddrs(&ifaddr);
	if (ret < 0) {
		wire_log(WLOG_ERR, "Failed to get interface address info: %d (%m)", errno);
		return 0;
	}

	for (ifa = ifaddr, n = 0; items_len < items_size && ifa != NULL; ifa = ifa->ifa_next, n++) {
		if (ifa->ifa_addr == NULL)
			continue;

		int family = ifa->ifa_addr->sa_family;
		if (family != AF_INET && family != AF_INET6)
			continue;

		char *colon = strchr(ifa->ifa_name, ':');
		if (colon)
			*colon = 0;

		int found = 0;
		for (item_idx = 0; item_idx < items_len; item_idx++) {
			if (strcmp(ifa->ifa_name, items[item_idx]) == 0) {
				found = 1;
				break;
			}
		}

		if (!found) {
			strncpy(items[items_len], ifa->ifa_name, item_size);
			items[items_len][item_size-1] = 0;
			items_len++;
		}
	}

	freeifaddrs(ifaddr);

	return items_len;
}

static int find_dev_path(char *dev_path, unsigned dev_path_sz, const char *dev_name)
{
	int ret;
	char dev_num[32];
	unsigned dev_major, dev_minor;

	ret = wio_read_file_content(dev_name, dev_num, sizeof(dev_num) - 1);
	if (ret <= 0) {
		wire_log(WLOG_ERR, "Failed to open file %s: %d (%m)", dev_name, errno);
		return -1;
	}

	// Null-terminate the string
	dev_num[ret] = 0;

	ret = sscanf(dev_num, "%u:%u", &dev_major, &dev_minor);
	if (ret != 2) {
		wire_log(WLOG_ERR, "Failed to parse dev_num content '%s'", dev_num);
		return -1;
	}

	dev_list_t *dev_list = dev_list_get();
	dev_list_t *n;
	ret = -1;

	for (n = dev_list; n; n = n->next) {
		if (n->major == dev_major && n->minor == dev_minor) {
			strncpy(dev_path, n->name, dev_path_sz);
			dev_path[dev_path_sz-1] = 0;
			ret = 0;
			break;
		}
	}

	dev_list_release(dev_list);
	return ret;
}

static int special_arg_block(char **items, unsigned items_size, unsigned item_size)
{
#define BASE_PATH "/sys/block"
	int num_items = 0;
	int ret;
	int i;
	glob_t globbuf;

	ret = wio_glob(BASE_PATH "/*/dev", GLOB_NOSORT, NULL, &globbuf);
	if (ret < 0) {
		wire_log(WLOG_ERR, "Failed to list files in " BASE_PATH ": %d (%m)", errno);
		return 0;
	}

	for (i = 0; i < globbuf.gl_pathc && num_items < items_size; i++) {
		if (strncmp(globbuf.gl_pathv[i], BASE_PATH "/loop", strlen(BASE_PATH "/loop")) == 0)
			continue; // Skip loopback devices

		ret = find_dev_path(items[num_items], item_size, globbuf.gl_pathv[i]);
		if (ret == 0)
			num_items++;
	}

	wio_globfree(&globbuf);
	return num_items;
}

int special_arg_match(const char *name, char **items, unsigned items_size, unsigned item_size)
{
	static const struct {
		const char *name;
		int (*func)(char **items, unsigned items_size, unsigned item_size);
	} specials[] = {
		{"%BLOCK", special_arg_block},
//		{"%SES", special_arg_ses},
//		{"%SCSI", special_arg_scsi},
		{"%NET", special_arg_net},
	};

	if (name[0] != '%')
		return 0;

	int j;
	for (j = 0; j < ARRAY_SIZE(specials); j++) {
		if (strcmp(name, specials[j].name) == 0) {
			return specials[j].func(items, items_size, item_size);
		}
	}

	return 0;
}
