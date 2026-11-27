#include "saturn/saturn_rec.h"
#include "core/fs.h"

#include <string.h>

static const char MAGIC[8] = {'S', 'B', 'P', 'C', 'R', 'E', 'C', '1'};
#define FRAME_BYTES (2 + 2 * SMPC_SLOTS)

bool rec_create(input_rec *r, const char *path, int64_t rtc_start)
{
    uint8_t h[16];
    memset(r, 0, sizeof *r);
    r->f = fs_open(path, "wb");
    if (!r->f)
        return false;
    memcpy(h, MAGIC, 8);
    for (int i = 0; i < 8; i++)
        h[8 + i] = (uint8_t)((uint64_t)rtc_start >> (56 - 8 * i));
    if (fwrite(h, 1, sizeof h, r->f) != sizeof h) {
        rec_close(r);
        return false;
    }
    return true;
}

bool rec_write(input_rec *r, const rec_frame *fr)
{
    uint8_t b[FRAME_BYTES];
    if (!r->f)
        return false;
    b[0] = fr->players;
    b[1] = fr->ports;
    for (int i = 0; i < SMPC_SLOTS; i++) {
        b[2 + 2 * i] = (uint8_t)(fr->pad[i] >> 8);
        b[3 + 2 * i] = (uint8_t)fr->pad[i];
    }
    if (fwrite(b, 1, sizeof b, r->f) != sizeof b)
        return false;
    r->frames++;
    if ((r->frames & 255) == 0)
        fflush(r->f);   /* a crash or a forced quit still leaves a usable file */
    return true;
}

bool rec_open(input_rec *r, const char *path, int64_t *rtc_start)
{
    uint8_t h[16];
    uint64_t t = 0;
    memset(r, 0, sizeof *r);
    r->f = fs_open(path, "rb");
    if (!r->f)
        return false;
    if (fread(h, 1, sizeof h, r->f) != sizeof h || memcmp(h, MAGIC, 8) != 0) {
        rec_close(r);
        return false;
    }
    for (int i = 0; i < 8; i++)
        t = t << 8 | h[8 + i];
    *rtc_start = (int64_t)t;
    return true;
}

bool rec_read(input_rec *r, rec_frame *fr)
{
    uint8_t b[FRAME_BYTES];
    if (!r->f || fread(b, 1, sizeof b, r->f) != sizeof b)
        return false;
    fr->players = b[0];
    fr->ports = b[1];
    for (int i = 0; i < SMPC_SLOTS; i++)
        fr->pad[i] = (uint16_t)(b[2 + 2 * i] << 8 | b[3 + 2 * i]);
    r->frames++;
    return true;
}

void rec_close(input_rec *r)
{
    if (r->f)
        fclose(r->f);
    r->f = NULL;
}
