#ifndef DOCKET_TAR_H
#define DOCKET_TAR_H

#pragma pack(1)
struct tar {
	union {
		unsigned char pad[512];
		struct {
			char filename[100];
			char mode[8];
			char uid[8];
			char gid[8];
			char filesize[12];
			char timestamp[12];
			char checksum[8];
			char filetype;
			char linked_file[100];
			char ustar[6];
			char ver[2];
			char user[32];
			char group[32];
			char devmajor[8];
			char devminor[8];
			char filename_prefix[155];
		};
	};
};
#pragma pack()

enum tar_file_type {
	TAR_NORMAL = '0',
	TAR_HARDLINK = '1',
	TAR_SYMLINK = '2',
	TAR_CHAR_SPECIAL = '3',
	TAR_BLOCK_SPECIAL = '4',
	TAR_DIRECTORY = '5',
	TAR_FIFO = '6',
	TAR_CONTIG_FILE = '7',
};

void tar_set_header(struct tar *hdr, const char *prefix, const char *dir, const char *filename, unsigned filesize, unsigned timestamp);

#endif
