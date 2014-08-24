#include "special_arg.h"

#include "wire_io.h"
#include "wire_log.h"

#include <string.h>
#include <errno.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

static int special_arg_net(char items[32][32], unsigned items_size)
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

	for (ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
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
			strncpy(&items[items_len][0], ifa->ifa_name, 31);
			items[items_len][31] = 0;
			items_len++;
		}
	}

	freeifaddrs(ifaddr);

	return 3;
}

int special_arg_match(const char *name, char items[32][32], unsigned items_size)
{
	static const struct {
		const char *name;
		int (*func)(char items[32][32], unsigned items_size);
	} specials[] = {
//		{"%BLOCK", special_arg_block},
//		{"%SES", special_arg_ses},
//		{"%SCSI", special_arg_scsi},
		{"%NET", special_arg_net},
	};

	if (name[0] != '%')
		return 0;

	int j;
	for (j = 0; j < ARRAY_SIZE(specials); j++) {
		if (strcmp(name, specials[j].name) == 0) {
			return specials[j].func(items, items_size);
		}
	}

	return 0;
}
