/* Guest memory image used by static analysis: loaded bytes plus a per-byte classification.
 * Lives only in memory / the user's local data folder; never in the repository. */
#ifndef SBPC_ANALYSIS_IMAGE_H
#define SBPC_ANALYSIS_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#define IMG_MAX_REGIONS 8

enum { BYTE_UNKNOWN = 0, BYTE_CODE = 1, BYTE_CODE_CONT = 2, BYTE_DATA = 3 };

typedef struct {
    char name[32];
    uint32_t base, size;
    uint8_t *bytes;
    uint8_t *kind;
} img_region;

typedef struct {
    int count;
    img_region regions[IMG_MAX_REGIONS];
} mem_image;

/* Physical address: strips the cache-through mirror (0x2xxxxxxx -> 0x0xxxxxxx). */
uint32_t img_phys(uint32_t addr);

void img_init(mem_image *im);
void img_free(mem_image *im);
/* Copies `size` bytes in at `base` (taken through img_phys). Regions must not overlap. */
bool img_add(mem_image *im, const char *name, uint32_t base, const void *data, uint32_t size);

img_region *img_find(mem_image *im, uint32_t addr, uint32_t len);
bool img_read16(mem_image *im, uint32_t addr, uint16_t *out);
bool img_read32(mem_image *im, uint32_t addr, uint32_t *out);
uint8_t img_kind(mem_image *im, uint32_t addr);       /* BYTE_UNKNOWN if outside */
void img_mark(mem_image *im, uint32_t addr, uint32_t len, uint8_t kind);

#endif
