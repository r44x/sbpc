/* Builds a small synthetic Saturn-style disc for tests. The header is made up. */
#ifndef SBPC_FAKE_DISC_H
#define SBPC_FAKE_DISC_H

#include "test.h"

static void put(unsigned char *p, const char *s, size_t width)
{
    size_t n = strlen(s);
    memset(p, ' ', width);
    memcpy(p, s, n < width ? n : width);
}

/* Writes <base>.bin (MODE1/2352, `sectors` sectors) and <base>.cue with `audio` extra audio
 * tracks in the same file. */
static int make_fake_disc(const char *base, const char *title, const char *product, int sectors, int audio)
{
    char path[256], cue[4096];
    size_t total = (size_t)(sectors + audio * 10) * 2352;
    unsigned char *bin = calloc(1, total), *h;
    int ok, n;
    if (!bin) return 0;
    for (int s = 0; s < sectors; s++) {
        unsigned char *sec = bin + (size_t)s * 2352;
        memset(sec + 1, 0xFF, 10);
        sec[15] = 1;
        for (int i = 0; i < 2048; i++) sec[16 + i] = (unsigned char)(s * 31 + i);
    }
    h = bin + 16;
    put(h, "SEGA SEGASATURN ", 16);
    put(h + 0x10, "SEGA TP TEST", 16);
    put(h + 0x20, product, 10);
    put(h + 0x2A, "V1.000", 6);
    put(h + 0x30, "20260101", 8);
    put(h + 0x38, "CD-1/1", 8);
    put(h + 0x40, "J", 10);
    put(h + 0x50, "J", 16);
    put(h + 0x60, title, 112);
    h[0xF0] = 0x06; h[0xF1] = 0x01; h[0xF2] = 0; h[0xF3] = 0;

    snprintf(path, sizeof path, "%s.bin", base);
    ok = write_file(path, bin, total);
    free(bin);
    n = snprintf(cue, sizeof cue, "FILE \"%s.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", base);
    for (int a = 0; a < audio; a++) {
        int sec = sectors + a * 10;
        n += snprintf(cue + n, sizeof cue - (size_t)n, "  TRACK %02d AUDIO\n    INDEX 01 %02d:%02d:%02d\n",
                      a + 2, sec / 75 / 60, sec / 75 % 60, sec % 75);
    }
    snprintf(path, sizeof path, "%s.cue", base);
    return ok && write_file(path, cue, strlen(cue));
}

#endif
