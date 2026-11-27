#include "saturn/saturn_bup.h"

#include <string.h>

static const char SIGNATURE[] = "BackUpRam Format"; /* 16 characters, repeated over block 0 */

#define TAG_FIRST 0x80000000u
#define HDR_LEN 30 /* name 11, language 1, comment 10, date 4, size 4 */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static size_t bounded_len(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

void bup_format(uint8_t *ram)
{
    memset(ram, 0, BUP_SIZE);
    for (int i = 0; i < 4; i++)
        memcpy(ram + i * 16, SIGNATURE, 16);
}

bool bup_is_formatted(const uint8_t *ram)
{
    for (int i = 0; i < 4; i++)
        if (memcmp(ram + i * 16, SIGNATURE, 16) != 0)
            return false;
    return true;
}

int bup_blocks_for(uint32_t size) { return (int)((size + HDR_LEN + 57) / 58); }

/* ---- a file's payload stream: 60 bytes per block, through its block list ---- */
typedef struct {
    const uint8_t *ram;
    uint16_t blocks[BUP_BLOCKS];
    int nblocks;
    uint32_t pos;   /* stream offset */
} stream;

static int stream_byte_addr(const stream *st, uint32_t pos)
{
    uint32_t b = pos / 60;
    if ((int)b >= st->nblocks)
        return -1;
    return st->blocks[b] * BUP_BLOCK + 4 + (int)(pos % 60);
}

static int stream_get(stream *st)
{
    int a = stream_byte_addr(st, st->pos++);
    return a < 0 ? -1 : st->ram[a];
}

/* Opens the file whose first block is `first`, reading its block list. False if it's broken. */
static bool stream_open(stream *st, const uint8_t *ram, int first)
{
    st->ram = ram;
    st->blocks[0] = (uint16_t)first;
    st->nblocks = 1;
    st->pos = HDR_LEN;
    for (;;) {
        int hi, lo;
        uint16_t b;
        /* The list grows as it's read: each entry is in a block listed before it. */
        if (st->pos / 60 >= (uint32_t)st->nblocks)
            return false;
        hi = stream_get(st);
        lo = stream_get(st);
        if (hi < 0 || lo < 0)
            return false;
        b = (uint16_t)(hi << 8 | lo);
        if (b == 0)
            return true;
        if (b < 2 || b >= BUP_BLOCKS || st->nblocks >= BUP_BLOCKS)
            return false;
        st->blocks[st->nblocks++] = b;
    }
}

static bool is_first(const uint8_t *ram, int b) { return rd32(ram + b * BUP_BLOCK) == TAG_FIRST; }

static void read_dir(const uint8_t *ram, int first, const stream *st, bup_dir *d)
{
    const uint8_t *p = ram + first * BUP_BLOCK + 4;
    memset(d, 0, sizeof *d);
    memcpy(d->name, p, 11);
    d->language = p[11];
    memcpy(d->comment, p + 12, 10);
    d->date = rd32(p + 22);
    d->size = rd32(p + 26);
    d->blocks = (uint16_t)st->nblocks;
}

static int find_file(const uint8_t *ram, const char *name, stream *st)
{
    for (int b = 2; b < BUP_BLOCKS; b++)
        if (is_first(ram, b) && strncmp((const char *)ram + b * BUP_BLOCK + 4, name, 11) == 0 && stream_open(st, ram, b))
            return b;
    return -1;
}

static void used_map(const uint8_t *ram, bool *used)
{
    static stream st;
    memset(used, 0, BUP_BLOCKS * sizeof *used);
    used[0] = used[1] = true;
    for (int b = 2; b < BUP_BLOCKS; b++)
        if (is_first(ram, b) && stream_open(&st, ram, b))
            for (int i = 0; i < st.nblocks; i++)
                used[st.blocks[i]] = true;
}

int bup_free_blocks(const uint8_t *ram)
{
    bool used[BUP_BLOCKS];
    int n = 0;
    used_map(ram, used);
    for (int b = 0; b < BUP_BLOCKS; b++)
        n += !used[b];
    return n;
}

int bup_list(const uint8_t *ram, const char *prefix, bup_dir *out, int max)
{
    static stream st;
    size_t plen = strlen(prefix);
    int n = 0;
    if (plen > 11) plen = 11;
    for (int b = 2; b < BUP_BLOCKS; b++) {
        if (!is_first(ram, b) || strncmp((const char *)ram + b * BUP_BLOCK + 4, prefix, plen) != 0 || !stream_open(&st, ram, b))
            continue;
        if (n < max)
            read_dir(ram, b, &st, &out[n]);
        n++;
    }
    return n;
}

int bup_delete(uint8_t *ram, const char *name)
{
    static stream st;
    int b = find_file(ram, name, &st);
    if (b < 0)
        return BUP_NOT_FOUND;
    for (int i = 0; i < st.nblocks; i++)
        memset(ram + st.blocks[i] * BUP_BLOCK, 0, BUP_BLOCK);
    return BUP_OK;
}

int bup_write(uint8_t *ram, const bup_dir *d, const uint8_t *data, bool keep_existing)
{
    static stream st;
    bool used[BUP_BLOCKS];
    uint8_t hdr[HDR_LEN];
    int need = bup_blocks_for(d->size), n = 0, freeb;

    if (find_file(ram, d->name, &st) >= 0) {
        if (keep_existing)
            return BUP_FOUND;
        bup_delete(ram, d->name);
    }
    used_map(ram, used);
    freeb = 0;
    for (int b = 0; b < BUP_BLOCKS; b++) freeb += !used[b];
    if (need > freeb)
        return BUP_NOT_ENOUGH_MEMORY;
    for (int b = 2; b < BUP_BLOCKS && n < need; b++)
        if (!used[b])
            st.blocks[n++] = (uint16_t)b;
    st.nblocks = n;
    st.ram = ram;

    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, d->name, bounded_len(d->name, 11));
    hdr[11] = d->language;
    memcpy(hdr + 12, d->comment, bounded_len(d->comment, 10));
    wr32(hdr + 22, d->date);
    wr32(hdr + 26, d->size);

    for (int i = 0; i < n; i++) {
        memset(ram + st.blocks[i] * BUP_BLOCK, 0, BUP_BLOCK);
        wr32(ram + st.blocks[i] * BUP_BLOCK, i == 0 ? TAG_FIRST : 0);
    }
    {   /* payload: header, block list (all but the first, then 0), data */
        uint32_t pos = 0;
#define PUT(byte) do { ram[stream_byte_addr(&st, pos++)] = (uint8_t)(byte); } while (0)
        for (int i = 0; i < HDR_LEN; i++) PUT(hdr[i]);
        for (int i = 1; i < n; i++) { PUT(st.blocks[i] >> 8); PUT(st.blocks[i]); }
        PUT(0); PUT(0);
        for (uint32_t i = 0; i < d->size; i++) PUT(data[i]);
#undef PUT
    }
    return BUP_OK;
}

int bup_read(const uint8_t *ram, const char *name, uint8_t *data, uint32_t cap, uint32_t *size)
{
    static stream st;
    bup_dir d;
    int b = find_file(ram, name, &st);
    if (b < 0)
        return BUP_NOT_FOUND;
    read_dir(ram, b, &st, &d);
    if (size) *size = d.size;
    for (uint32_t i = 0; i < d.size; i++) {
        int c = stream_get(&st);   /* the stream is positioned just after the block list */
        if (c < 0)
            return BUP_BROKEN;
        if (i < cap) data[i] = (uint8_t)c;
    }
    return BUP_OK;
}

/* ---- dates: minutes since 1980-01-01 00:00 ---- */
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(int y, int m)
{
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : D[m - 1];
}

uint32_t bup_date_encode(int year, int month, int day, int hour, int minute)
{
    uint32_t days = 0;
    for (int y = 1980; y < year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < month; m++) days += (uint32_t)month_days(year, m);
    days += (uint32_t)(day - 1);
    return (days * 24 + (uint32_t)hour) * 60 + (uint32_t)minute;
}

void bup_date_decode(uint32_t date, int *year, int *month, int *day, int *hour, int *minute, int *weekday)
{
    uint32_t days = date / 1440, mins = date % 1440;
    int y = 1980, m = 1;
    *weekday = (int)((days + 2) % 7);   /* 1980-01-01 was a Tuesday; 0 = Sunday */
    while (days >= (uint32_t)(leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    while (days >= (uint32_t)month_days(y, m)) { days -= (uint32_t)month_days(y, m); m++; }
    *year = y; *month = m; *day = (int)days + 1;
    *hour = (int)(mins / 60); *minute = (int)(mins % 60);
}
