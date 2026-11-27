/* ISO 9660 filesystem on a disc's data track (the Saturn uses plain ISO 9660 level 1). */
#ifndef SBPC_DISC_ISO9660_H
#define SBPC_DISC_ISO9660_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "disc/disc.h"

#define ISO_MAX_FILES 4096
#define ISO_NAME_MAX 128

typedef struct {
    char path[ISO_NAME_MAX]; /* "/DIR/FILE.BIN" with the ";1" version removed */
    uint32_t lba;            /* first sector, relative to the start of the data track */
    uint32_t size;           /* bytes */
    bool is_dir;
    int order;               /* position in its directory (0 = first entry after . and ..) */
} iso_file;

typedef struct {
    disc *d;
    int track;
    char volume_id[33];
    int count;
    iso_file *files;         /* in directory order, depth first */
} iso_fs;

bool iso_open(iso_fs *fs, disc *d, int track, char *err, size_t errlen);
void iso_close(iso_fs *fs);

/* Case-insensitive path lookup; a leading '/' is optional. */
const iso_file *iso_find(const iso_fs *fs, const char *path);

/* First regular file in the root directory: what the Saturn BIOS loads as the "1st read file". */
const iso_file *iso_first_root_file(const iso_fs *fs);

/* Reads `len` bytes from `offset` within a file. */
bool iso_read(iso_fs *fs, const iso_file *f, uint32_t offset, void *buf, uint32_t len);

#endif
