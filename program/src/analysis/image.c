#include "analysis/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t img_phys(uint32_t addr)
{
    return addr < 0x40000000u ? addr & 0x1FFFFFFFu : addr;
}

void img_init(mem_image *im) { memset(im, 0, sizeof *im); }

void img_free(mem_image *im)
{
    for (int i = 0; i < im->count; i++) {
        free(im->regions[i].bytes);
        free(im->regions[i].kind);
    }
    img_init(im);
}

bool img_add(mem_image *im, const char *name, uint32_t base, const void *data, uint32_t size)
{
    img_region *r;
    base = img_phys(base);
    if (im->count == IMG_MAX_REGIONS || size == 0 || base + size < base)
        return false;
    for (int i = 0; i < im->count; i++) {
        const img_region *o = &im->regions[i];
        if (base < o->base + o->size && o->base < base + size)
            return false;
    }
    r = &im->regions[im->count];
    r->bytes = malloc(size);
    r->kind = calloc(size, 1);
    if (!r->bytes || !r->kind) {
        free(r->bytes);
        free(r->kind);
        return false;
    }
    memcpy(r->bytes, data, size);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->base = base;
    r->size = size;
    im->count++;
    return true;
}

img_region *img_find(mem_image *im, uint32_t addr, uint32_t len)
{
    addr = img_phys(addr);
    for (int i = 0; i < im->count; i++) {
        img_region *r = &im->regions[i];
        if (addr >= r->base && len <= r->size && addr - r->base <= r->size - len)
            return r;
    }
    return NULL;
}

bool img_read16(mem_image *im, uint32_t addr, uint16_t *out)
{
    img_region *r = img_find(im, addr, 2);
    const uint8_t *p;
    if (!r)
        return false;
    p = r->bytes + (img_phys(addr) - r->base);
    *out = (uint16_t)(p[0] << 8 | p[1]);
    return true;
}

bool img_read32(mem_image *im, uint32_t addr, uint32_t *out)
{
    img_region *r = img_find(im, addr, 4);
    const uint8_t *p;
    if (!r)
        return false;
    p = r->bytes + (img_phys(addr) - r->base);
    *out = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return true;
}

uint8_t img_kind(mem_image *im, uint32_t addr)
{
    img_region *r = img_find(im, addr, 1);
    return r ? r->kind[img_phys(addr) - r->base] : BYTE_UNKNOWN;
}

void img_mark(mem_image *im, uint32_t addr, uint32_t len, uint8_t kind)
{
    img_region *r = img_find(im, addr, len);
    if (r)
        memset(r->kind + (img_phys(addr) - r->base), kind, len);
}
