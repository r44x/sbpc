/* Saturn internal backup RAM (32 KB) in its real on-device format, and the backup library
 * (BUP_*) the game calls through the table BUP_Init builds. Pure functions over the 32 KB
 * image, so they can be tested without a machine.
 *
 * Format: 512 blocks of 64 bytes. Block 0 holds "BackUpRam Format" four times; block 1 is
 * reserved. Every data block starts with a 4-byte tag (0x80000000 = first block of a file,
 * 0 = continuation) followed by 60 payload bytes. A file's payload stream, which spans its
 * blocks skipping the tags, is: name[11], language, comment[10], date (BE32, minutes since
 * 1980), size (BE32), the BE16 numbers of its other blocks ending with 0, then the data.
 * So a file of n bytes takes ceil((n + 30) / 58) blocks. */
#ifndef SBPC_SATURN_BUP_H
#define SBPC_SATURN_BUP_H

#include <stdbool.h>
#include <stdint.h>

#define BUP_SIZE   0x8000
#define BUP_BLOCK  64
#define BUP_BLOCKS (BUP_SIZE / BUP_BLOCK)

/* Library return codes */
enum {
    BUP_OK = 0,
    BUP_NON = 1,              /* no such device */
    BUP_UNFORMAT = 2,
    BUP_WRITE_PROTECT = 3,
    BUP_NOT_ENOUGH_MEMORY = 4,
    BUP_NOT_FOUND = 5,
    BUP_FOUND = 6,            /* write with overwrite off: the file exists */
    BUP_NO_MATCH = 7,         /* verify: data differs */
    BUP_BROKEN = 8
};

typedef struct {
    char name[12];            /* 11 characters + NUL */
    char comment[11];         /* 10 characters + NUL */
    uint8_t language;
    uint32_t date;            /* minutes since 1980-01-01 00:00 */
    uint32_t size;            /* data bytes */
    uint16_t blocks;
} bup_dir;

void bup_format(uint8_t *ram);
bool bup_is_formatted(const uint8_t *ram);
int bup_blocks_for(uint32_t size);
int bup_free_blocks(const uint8_t *ram);
/* Files whose name starts with prefix ("" = all). Fills up to max entries; returns the total. */
int bup_list(const uint8_t *ram, const char *prefix, bup_dir *out, int max);
int bup_write(uint8_t *ram, const bup_dir *d, const uint8_t *data, bool keep_existing);
/* Reads up to cap bytes into data; *size gets the file's size. */
int bup_read(const uint8_t *ram, const char *name, uint8_t *data, uint32_t cap, uint32_t *size);
int bup_delete(uint8_t *ram, const char *name);

uint32_t bup_date_encode(int year, int month, int day, int hour, int minute);
void bup_date_decode(uint32_t date, int *year, int *month, int *day, int *hour, int *minute, int *weekday);

#endif
