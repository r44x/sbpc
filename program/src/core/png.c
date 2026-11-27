#include "core/png.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_upd(uint32_t crc, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 255] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8], tail[4];
    uint32_t crc;
    put32(hdr, len);
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
    crc = crc_upd(0xFFFFFFFFu, hdr + 4, 4);
    crc = crc_upd(crc, data, len) ^ 0xFFFFFFFFu;
    put32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

bool png_write(const char *path, const uint32_t *argb, int w, int h)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    size_t rowbytes = (size_t)w * 3 + 1, raw = rowbytes * (size_t)h;
    size_t nblocks = (raw + 65534) / 65535, zlen = 2 + raw + nblocks * 5 + 4;
    uint8_t *img = malloc(raw), *z = malloc(zlen);
    FILE *f;
    bool ok = false;
    if (!img || !z) goto done;
    for (int y = 0; y < h; y++) {
        uint8_t *r = img + (size_t)y * rowbytes;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            *r++ = (uint8_t)(c >> 16); *r++ = (uint8_t)(c >> 8); *r++ = (uint8_t)c;
        }
    }
    {
        size_t zp = 0, pos = 0;
        uint32_t a = 1, b = 0;
        z[zp++] = 0x78; z[zp++] = 0x01;
        while (pos < raw) {
            size_t n = raw - pos > 65535 ? 65535 : raw - pos;
            z[zp++] = (pos + n == raw) ? 1 : 0;
            z[zp++] = (uint8_t)n; z[zp++] = (uint8_t)(n >> 8);
            z[zp++] = (uint8_t)~n; z[zp++] = (uint8_t)(~n >> 8);
            for (size_t i = 0; i < n; i++) { z[zp++] = img[pos + i]; a = (a + img[pos + i]) % 65521u; b = (b + a) % 65521u; }
            pos += n;
        }
        put32(z + zp, (b << 16) | a); zp += 4;
        crc_init();
        f = fs_open(path, "wb");
        if (!f) goto done;
        {
            uint8_t ihdr[13];
            put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
            ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
            ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (uint32_t)zp) &&
                 chunk(f, "IEND", NULL, 0);
        }
        fclose(f);
    }
done:
    free(img); free(z);
    return ok;
}
