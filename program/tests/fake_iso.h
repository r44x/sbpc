/* Builds a tiny ISO 9660 data track (MODE1/2048 + cue) for tests. Contents are made up. */
#ifndef SBPC_FAKE_ISO_H
#define SBPC_FAKE_ISO_H

#include "test.h"

#define FISO_SECTORS 32

static void fiso_le32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void fiso_be32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

/* Appends a directory record; returns its length. */
static int fiso_rec(unsigned char *p, const char *name, int name_len, unsigned lba, unsigned size, int dir)
{
    int len = 33 + name_len + ((33 + name_len) & 1);
    memset(p, 0, (size_t)len);
    p[0] = (unsigned char)len;
    fiso_le32(p + 2, lba); fiso_be32(p + 6, lba);
    fiso_le32(p + 10, size); fiso_be32(p + 14, size);
    p[25] = dir ? 2 : 0;
    p[32] = (unsigned char)name_len;
    memcpy(p + 33, name, (size_t)name_len);
    return len;
}

static void fiso_put(unsigned char *p, const char *s, size_t w)
{
    size_t n = strlen(s);
    memset(p, ' ', w);
    memcpy(p, s, n < w ? n : w);
}

/* Layout: IP header in sector 0, PVD 16, root dir 18, SUB dir 19,
 * 0FIRST.BIN (program `prog`, `prog_len` bytes) at 20, SUB/DATA.BIN at 24. */
static int make_fake_iso(const char *base, const unsigned char *prog, unsigned prog_len, unsigned load_addr)
{
    static unsigned char img[FISO_SECTORS * 2048];
    unsigned char *s;
    char path[256], cue[512];
    int o;
    memset(img, 0, sizeof img);

    s = img;
    fiso_put(s, "SEGA SEGASATURN ", 16);
    fiso_put(s + 0x10, "SEGA TP TEST", 16);
    fiso_put(s + 0x20, "T-00003", 10);
    fiso_put(s + 0x2A, "V1.000", 6);
    fiso_put(s + 0x30, "20260101", 8);
    fiso_put(s + 0x38, "CD-1/1", 8);
    fiso_put(s + 0x40, "JU", 10);
    fiso_put(s + 0x50, "J", 16);
    fiso_put(s + 0x60, "FAKE TEST DISC", 112);
    fiso_be32(s + 0xE0, 0x1000);
    fiso_be32(s + 0xF0, load_addr);

    s = img + 16 * 2048; /* PVD */
    s[0] = 1; memcpy(s + 1, "CD001", 5); s[6] = 1;
    fiso_put((unsigned char *)s + 40, "FAKEDISC", 32);
    fiso_rec(s + 156, "\0", 1, 18, 2048, 1);

    s = img + 18 * 2048; /* root */
    o = fiso_rec(s, "\0", 1, 18, 2048, 1);
    o += fiso_rec(s + o, "\1", 1, 18, 2048, 1);
    o += fiso_rec(s + o, "0FIRST.BIN;1", 12, 20, prog_len, 0);
    o += fiso_rec(s + o, "SUB", 3, 19, 2048, 1);

    s = img + 19 * 2048; /* SUB */
    o = fiso_rec(s, "\0", 1, 19, 2048, 1);
    o += fiso_rec(s + o, "\1", 1, 18, 2048, 1);
    fiso_rec(s + o, "DATA.BIN;1", 10, 24, 3000, 0);

    if (prog_len > 4 * 2048) return 0;
    memcpy(img + 20 * 2048, prog, prog_len);
    for (int i = 0; i < 3000; i++) img[24 * 2048 + i] = (unsigned char)(i * 7);

    snprintf(path, sizeof path, "%s.iso", base);
    if (!write_file(path, img, sizeof img)) return 0;
    snprintf(cue, sizeof cue, "FILE \"%s.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n", base);
    snprintf(path, sizeof path, "%s.cue", base);
    return write_file(path, cue, strlen(cue));
}

#endif
