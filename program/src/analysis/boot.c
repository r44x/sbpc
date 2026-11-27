#include "analysis/boot.h"
#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

bool boot_load(iso_fs *fs, mem_image *im, boot_info *bi, char *err, size_t errlen)
{
    uint8_t sec[2048];
    uint8_t *buf;
    const iso_file *first;
    uint32_t ip_size, areas = 0;

    memset(bi, 0, sizeof *bi);
    if (!disc_read_user(fs->d, fs->track, 0, sec) || !saturn_header_parse(sec, &bi->header))
        return fail(err, errlen, "no Saturn header in sector 0");

    /* IP: header + security code (0x100-0xDFF) + one 0x20-byte area block per area code + AIP. */
    ip_size = bi->header.ip_size;
    /* at least the header, the security code and one area block: a single-area disc can be as
     * small as 0xF00 (Europe: area E only) */
    if (ip_size < 0xE20 || ip_size > 0x8000)
        return fail(err, errlen, "unexpected IP size 0x%X", (unsigned)ip_size);
    buf = malloc(ip_size);
    if (!buf)
        return fail(err, errlen, "out of memory");
    for (uint32_t off = 0; off < ip_size; off += 2048) {
        uint32_t take = ip_size - off < 2048 ? ip_size - off : 2048;
        if (!disc_read_user(fs->d, fs->track, off / 2048, sec)) {
            free(buf);
            return fail(err, errlen, "cannot read IP sector %u", (unsigned)(off / 2048));
        }
        memcpy(buf + off, sec, take);
    }
    if (!img_add(im, "IP", SATURN_IP_ADDR, buf, ip_size)) {
        free(buf);
        return fail(err, errlen, "cannot map IP");
    }
    free(buf);
    for (const char *p = bi->header.areas; *p; p++)
        areas += *p != ' ';
    bi->ip_size = ip_size;
    bi->aip_addr = SATURN_IP_ADDR + 0xE00 + 0x20 * areas;

    /* 1st read file */
    first = iso_first_root_file(fs);
    if (!first)
        return fail(err, errlen, "no files in the root directory");
    bi->first_addr = img_phys(bi->header.first_read_addr);
    bi->first_size = bi->header.first_read_size ? bi->header.first_read_size : first->size;
    if (bi->first_size > first->size)
        bi->first_size = first->size;
    snprintf(bi->first_file, sizeof bi->first_file, "%s", first->path);
    if (bi->first_size == 0 || bi->first_size > 0x100000)
        return fail(err, errlen, "1st read file %s has an unexpected size (%u)", first->path, (unsigned)bi->first_size);
    buf = malloc(bi->first_size);
    if (!buf)
        return fail(err, errlen, "out of memory");
    if (!iso_read(fs, first, 0, buf, bi->first_size)) {
        free(buf);
        return fail(err, errlen, "cannot read %s", first->path);
    }
    if (!img_add(im, fs_basename(first->path), bi->first_addr, buf, bi->first_size)) {
        free(buf);
        return fail(err, errlen, "cannot map %s at %08X", first->path, (unsigned)bi->first_addr);
    }
    free(buf);
    LOGI(SUB_DISC, "boot: IP %u bytes, AIP at %08X, %s (%u bytes) at %08X", (unsigned)ip_size,
         (unsigned)bi->aip_addr, first->path, (unsigned)bi->first_size, (unsigned)bi->first_addr);
    return true;
}
