#include "disc/iso9660.h"
#include "core/log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (err && errlen) {
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool add_file(iso_fs *fs, const iso_file *f)
{
    if (fs->count == ISO_MAX_FILES)
        return false;
    fs->files[fs->count++] = *f;
    return true;
}

/* Reads one directory extent and appends its entries; recurses into subdirectories. */
static bool read_dir(iso_fs *fs, const char *prefix, uint32_t lba, uint32_t size, int depth,
                     char *err, size_t errlen)
{
    uint8_t sec[2048];
    uint32_t sectors = (size + 2047) / 2048;
    int order = 0;
    int first_child = fs->count;

    if (depth > 8)
        return fail(err, errlen, "directory nesting too deep");
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t pos = 0;
        if (!disc_read_user(fs->d, fs->track, lba + s, sec))
            return fail(err, errlen, "cannot read directory sector %u", (unsigned)(lba + s));
        while (pos < 2048) {
            uint8_t len = sec[pos];
            iso_file f;
            uint8_t name_len;
            char name[ISO_NAME_MAX];
            if (len == 0)
                break; /* rest of this sector is padding */
            if (len < 34 || pos + len > 2048)
                return fail(err, errlen, "bad directory record at sector %u", (unsigned)(lba + s));
            name_len = sec[pos + 32];
            if (33u + name_len > len)
                return fail(err, errlen, "bad name length at sector %u", (unsigned)(lba + s));
            /* skip "." (0x00) and ".." (0x01) */
            if (!(name_len == 1 && sec[pos + 33] <= 1)) {
                size_t n = name_len < ISO_NAME_MAX - 1 ? name_len : ISO_NAME_MAX - 1;
                memcpy(name, sec + pos + 33, n);
                name[n] = 0;
                if (strchr(name, ';'))
                    *strchr(name, ';') = 0;
                if (n && name[strlen(name) - 1] == '.') /* "FILE." -> "FILE" */
                    name[strlen(name) - 1] = 0;
                memset(&f, 0, sizeof f);
                if ((size_t)snprintf(f.path, sizeof f.path, "%s/%s", prefix, name) >= sizeof f.path)
                    return fail(err, errlen, "path too long: %s/%s", prefix, name);
                f.lba = le32(sec + pos + 2);
                f.size = le32(sec + pos + 10);
                f.is_dir = (sec[pos + 25] & 2) != 0;
                f.order = order++;
                if (!add_file(fs, &f))
                    return fail(err, errlen, "too many files");
            }
            pos += len;
        }
    }
    /* Recurse after listing this directory so siblings stay together. */
    for (int i = first_child, end = fs->count; i < end; i++) {
        if (fs->files[i].is_dir) {
            iso_file dir = fs->files[i];
            if (!read_dir(fs, dir.path, dir.lba, dir.size, depth + 1, err, errlen))
                return false;
        }
    }
    return true;
}

bool iso_open(iso_fs *fs, disc *d, int track, char *err, size_t errlen)
{
    uint8_t pvd[2048];
    const uint8_t *root;
    memset(fs, 0, sizeof *fs);
    fs->d = d;
    fs->track = track;
    if (!disc_read_user(d, track, 16, pvd))
        return fail(err, errlen, "cannot read the volume descriptor (sector 16)");
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0)
        return fail(err, errlen, "no ISO 9660 primary volume descriptor");
    memcpy(fs->volume_id, pvd + 40, 32);
    for (int i = 31; i >= 0 && fs->volume_id[i] == ' '; i--)
        fs->volume_id[i] = 0;
    fs->files = calloc(ISO_MAX_FILES, sizeof *fs->files);
    if (!fs->files)
        return fail(err, errlen, "out of memory");
    root = pvd + 156;
    if (!read_dir(fs, "", le32(root + 2), le32(root + 10), 0, err, errlen)) {
        iso_close(fs);
        return false;
    }
    LOGD(SUB_DISC, "iso9660: volume '%s', %d entries", fs->volume_id, fs->count);
    return true;
}

void iso_close(iso_fs *fs)
{
    free(fs->files);
    fs->files = NULL;
    fs->count = 0;
}

static bool path_eq(const char *a, const char *b)
{
    if (*a == '/') a++;
    if (*b == '/') b++;
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
    return *a == *b;
}

const iso_file *iso_find(const iso_fs *fs, const char *path)
{
    for (int i = 0; i < fs->count; i++)
        if (path_eq(fs->files[i].path, path))
            return &fs->files[i];
    return NULL;
}

const iso_file *iso_first_root_file(const iso_fs *fs)
{
    for (int i = 0; i < fs->count; i++) {
        const iso_file *f = &fs->files[i];
        if (!f->is_dir && strchr(f->path + 1, '/') == NULL)
            return f;
    }
    return NULL;
}

bool iso_read(iso_fs *fs, const iso_file *f, uint32_t offset, void *buf, uint32_t len)
{
    uint8_t sec[2048];
    uint8_t *out = buf;
    if (f->is_dir || offset > f->size || len > f->size - offset)
        return false;
    while (len) {
        uint32_t s = offset / 2048, in = offset % 2048;
        uint32_t take = 2048 - in < len ? 2048 - in : len;
        if (!disc_read_user(fs->d, fs->track, f->lba + s, sec))
            return false;
        memcpy(out, sec + in, take);
        out += take;
        offset += take;
        len -= take;
    }
    return true;
}
