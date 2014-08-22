#include "tar.h"

#include <memory.h>
#include <assert.h>
#include <stdio.h>

#define DEFAULT_MODE "0000666"
#define DEFAULT_UID  "0000000"
#define DEFAULT_USTAR "ustar"

void tar_set_header(struct tar *hdr, const char *prefix, const char *dir, const char *filename, unsigned filesize, unsigned timestamp)
{
	assert(sizeof(*hdr) == 512);
	memset(hdr, 0, sizeof(*hdr));

	snprintf(hdr->filename, sizeof(hdr->filename), "./%s/%s/%s", prefix, dir, filename);
	memcpy(hdr->mode, DEFAULT_MODE, sizeof(DEFAULT_MODE)); assert(sizeof(DEFAULT_MODE) == sizeof(hdr->mode));
	memcpy(hdr->uid, DEFAULT_UID, sizeof(DEFAULT_UID)); assert(sizeof(DEFAULT_UID) == sizeof(hdr->uid));
	memcpy(hdr->gid, DEFAULT_UID, sizeof(DEFAULT_UID)); assert(sizeof(DEFAULT_UID) == sizeof(hdr->gid));
    snprintf(hdr->filesize, sizeof(hdr->filesize), "%011o", filesize);
	snprintf(hdr->timestamp, sizeof(hdr->timestamp), "%011o", timestamp);
	hdr->filetype = TAR_NORMAL;
	memcpy(hdr->ustar, DEFAULT_USTAR, sizeof(DEFAULT_USTAR)); assert(sizeof(DEFAULT_USTAR) == sizeof(hdr->ustar));
	hdr->ver[0] = '0';
	hdr->ver[1] = '0';

	// Calculate and set the checksum
	memset(hdr->checksum, ' ', sizeof(hdr->checksum));
	int i;
	unsigned checksum = 0;
	for (i = 0; i < sizeof(hdr->pad); i++)
		checksum += hdr->pad[i];
	snprintf(hdr->checksum, sizeof(hdr->checksum), "%07o", checksum);
}
