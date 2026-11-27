#include "disc/chd.h"
#include "disc/flac.h"
#include "disc/inflate.h"
#include "disc/lzma.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2352
#define SUBCODE 96

enum { CODEC_NONE, CODEC_CDLZ, CODEC_CDZL, CODEC_CDFL };

/* map entry types (v5) */
enum {
    COMP_TYPE0 = 0, COMP_TYPE1, COMP_TYPE2, COMP_TYPE3, COMP_NONE, COMP_SELF, COMP_PARENT,
    COMP_RLE_SMALL, COMP_RLE_LARGE, COMP_SELF_0, COMP_SELF_1, COMP_PARENT_SELF, COMP_PARENT_0, COMP_PARENT_1
};

typedef struct {
    uint8_t type;
    uint32_t length;
    uint64_t offset;
    uint16_t crc;
} map_entry;

struct chd {
    FILE *f;
    int codec[4];
    uint64_t logical;
    uint32_t hunk_bytes, unit_bytes, hunks;
    map_entry *map;
    chd_track tracks[CHD_MAX_TRACKS];
    int ntracks;
    uint8_t *hunk, *cmp, *work;    /* cached hunk, compressed input, codec scratch */
    uint32_t cached;               /* hunk number in `hunk`, or ~0 */
    bool cached_full;
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be48(const uint8_t *p) { return (uint64_t)be16(p) << 32 | be32(p + 2); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static bool read_at(chd *c, uint64_t off, void *buf, size_t n)
{
    return fs_seek(c->f, (int64_t)off) && fread(buf, 1, n, c->f) == n;
}

static bool fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return false;
}

/* ---- MSB-first bit reader and the small Huffman decoder used by the map (16 codes, 8 bits) ---- */
typedef struct {
    const uint8_t *data;
    size_t len, off;
    uint32_t buf;
    int bits;
} bitin;

static uint32_t bi_peek(bitin *b, int n)
{
    if (n == 0) return 0;
    while (b->bits < n) {
        b->buf |= (uint32_t)(b->off < b->len ? b->data[b->off] : 0) << (24 - b->bits);
        b->off++;
        b->bits += 8;
    }
    return b->buf >> (32 - n);
}

static void bi_remove(bitin *b, int n) { b->buf <<= n; b->bits -= n; }

static uint32_t bi_read(bitin *b, int n)
{
    uint32_t v = 0;
    while (n > 16) {                 /* keep each step within the 32-bit window */
        v = (v << 16) | bi_read(b, 16);
        n -= 16;
    }
    v = (v << n) | bi_peek(b, n);
    bi_remove(b, n);
    return v;
}

#define HUFF_CODES 16
#define HUFF_BITS 8

typedef struct {
    uint8_t numbits[HUFF_CODES];
    uint16_t lookup[1 << HUFF_BITS];   /* symbol << 5 | length */
} huff;

static bool huff_import_rle(huff *h, bitin *b)
{
    uint32_t histo[33] = {0}, start = 0;
    uint32_t code[HUFF_CODES];
    int cur = 0;
    while (cur < HUFF_CODES) {
        int nb = (int)bi_read(b, 4);
        if (nb != 1) { h->numbits[cur++] = (uint8_t)nb; continue; }
        nb = (int)bi_read(b, 4);
        if (nb == 1) { h->numbits[cur++] = 1; continue; }
        {
            int rep = (int)bi_read(b, 4) + 3;
            while (rep-- && cur < HUFF_CODES) h->numbits[cur++] = (uint8_t)nb;
        }
    }
    /* canonical codes, longest lengths first (as MAME's huffman_context_base does) */
    for (int i = 0; i < HUFF_CODES; i++) {
        if (h->numbits[i] > HUFF_BITS) return false;
        histo[h->numbits[i]]++;
    }
    for (int len = 32; len > 0; len--) {
        uint32_t next = (start + histo[len]) >> 1;
        if (len != 1 && next * 2 != start + histo[len]) return false;
        histo[len] = start;
        start = next;
    }
    for (int i = 0; i < HUFF_CODES; i++)
        code[i] = h->numbits[i] ? histo[h->numbits[i]]++ : 0;
    memset(h->lookup, 0, sizeof h->lookup);
    for (int i = 0; i < HUFF_CODES; i++) {
        int nb = h->numbits[i];
        if (!nb) continue;
        {
            int shift = HUFF_BITS - nb;
            uint32_t first = code[i] << shift, last = ((code[i] + 1) << shift) - 1;
            for (uint32_t k = first; k <= last && k < (1u << HUFF_BITS); k++) h->lookup[k] = (uint16_t)(i << 5 | nb);
        }
    }
    return true;
}

static int huff_decode(huff *h, bitin *b)
{
    uint16_t v = h->lookup[bi_peek(b, HUFF_BITS)];
    bi_remove(b, v & 0x1F);
    return v >> 5;
}

/* ---- map ---- */
static bool read_map(chd *c, uint64_t mapoff, char *err, size_t errlen)
{
    uint8_t hdr[16];
    uint8_t *cmp;
    uint32_t mapbytes;
    uint64_t cur;
    int lenbits, selfbits, parentbits, rep = 0;
    uint8_t lastcomp = 0;
    uint32_t last_self = 0;
    uint64_t last_parent = 0;
    bitin b;
    huff h;

    c->map = calloc(c->hunks, sizeof *c->map);
    if (!c->map) return fail(err, errlen, "out of memory");
    if (c->codec[0] == CODEC_NONE) {   /* uncompressed CHD: 4 bytes per hunk, offset in hunks */
        uint8_t e[4];
        for (uint32_t i = 0; i < c->hunks; i++) {
            if (!read_at(c, mapoff + (uint64_t)i * 4, e, 4)) return fail(err, errlen, "can't read the CHD map");
            c->map[i].type = COMP_NONE;
            c->map[i].offset = (uint64_t)be32(e) * c->hunk_bytes;
            c->map[i].length = c->hunk_bytes;
        }
        return true;
    }
    if (!read_at(c, mapoff, hdr, 16)) return fail(err, errlen, "can't read the CHD map");
    mapbytes = be32(hdr);
    cur = be48(hdr + 4);
    lenbits = hdr[12];
    selfbits = hdr[13];
    parentbits = hdr[14];
    cmp = malloc(mapbytes ? mapbytes : 1);
    if (!cmp || !read_at(c, mapoff + 16, cmp, mapbytes)) { free(cmp); return fail(err, errlen, "can't read the CHD map"); }
    memset(&b, 0, sizeof b);
    b.data = cmp;
    b.len = mapbytes;
    if (!huff_import_rle(&h, &b)) { free(cmp); return fail(err, errlen, "bad CHD map"); }
    /* compression type of every hunk, with run-length codes */
    for (uint32_t i = 0; i < c->hunks; i++) {
        if (rep > 0) { c->map[i].type = lastcomp; rep--; continue; }
        {
            int v = huff_decode(&h, &b);
            if (v == COMP_RLE_SMALL) { c->map[i].type = lastcomp; rep = 2 + huff_decode(&h, &b); }
            else if (v == COMP_RLE_LARGE) { c->map[i].type = lastcomp; rep = 2 + 16 + (huff_decode(&h, &b) << 4); rep += huff_decode(&h, &b); }
            else c->map[i].type = lastcomp = (uint8_t)v;
        }
    }
    /* then lengths, offsets and CRCs */
    for (uint32_t i = 0; i < c->hunks; i++) {
        map_entry *e = &c->map[i];
        uint64_t off = cur;
        uint32_t length = 0;
        uint16_t crc = 0;
        switch (e->type) {
        case COMP_TYPE0: case COMP_TYPE1: case COMP_TYPE2: case COMP_TYPE3:
            cur += length = bi_read(&b, lenbits);
            crc = (uint16_t)bi_read(&b, 16);
            break;
        case COMP_NONE:
            cur += length = c->hunk_bytes;
            crc = (uint16_t)bi_read(&b, 16);
            break;
        case COMP_SELF:
            last_self = (uint32_t)(off = bi_read(&b, selfbits));
            break;
        case COMP_PARENT:
            off = bi_read(&b, parentbits);
            last_parent = off;
            break;
        case COMP_SELF_1:
            last_self++;
            /* fall through */
        case COMP_SELF_0:
            e->type = COMP_SELF;
            off = last_self;
            break;
        case COMP_PARENT_SELF:
            e->type = COMP_PARENT;
            last_parent = off = (uint64_t)i * c->hunk_bytes / c->unit_bytes;
            break;
        case COMP_PARENT_1:
            last_parent += c->hunk_bytes / c->unit_bytes;
            /* fall through */
        case COMP_PARENT_0:
            e->type = COMP_PARENT;
            off = last_parent;
            break;
        default:
            free(cmp);
            return fail(err, errlen, "bad CHD map entry");
        }
        e->length = length;
        e->offset = off;
        e->crc = crc;
    }
    free(cmp);
    return true;
}

/* ---- track metadata ---- */
static int track_type(const char *s)
{
    if (strcmp(s, "MODE1_RAW") == 0) return CHD_TRACK_MODE1_RAW;
    if (strcmp(s, "MODE1") == 0) return CHD_TRACK_MODE1;
    if (strcmp(s, "MODE2_RAW") == 0) return CHD_TRACK_MODE2_RAW;
    if (strcmp(s, "AUDIO") == 0) return CHD_TRACK_AUDIO;
    return CHD_TRACK_OTHER;
}

static bool read_tracks(chd *c, uint64_t metaoff, char *err, size_t errlen)
{
    uint32_t frame = 0;
    int guard = 0;
    while (metaoff && guard++ < 1000) {
        uint8_t h[16];
        char text[256];
        uint32_t len;
        if (!read_at(c, metaoff, h, 16)) return fail(err, errlen, "can't read the CHD metadata");
        len = be32(h + 4) & 0xFFFFFF;
        if ((memcmp(h, "CHT2", 4) == 0 || memcmp(h, "CHTR", 4) == 0) && len < sizeof text && c->ntracks < CHD_MAX_TRACKS) {
            int num = 0, frames = 0, pregap = 0, postgap = 0;
            char type[32] = "", sub[32] = "", pgtype[32] = "", pgsub[32] = "";
            chd_track *t;
            if (!read_at(c, metaoff + 16, text, len)) return fail(err, errlen, "can't read the CHD metadata");
            text[len] = 0;
            if (memcmp(h, "CHT2", 4) == 0)
                sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                       &num, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
            else
                sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &num, type, sub, &frames);
            if (num != c->ntracks + 1) return fail(err, errlen, "CHD tracks out of order");
            t = &c->tracks[c->ntracks++];
            t->type = track_type(type);
            t->frames = frames;
            t->pregap = pregap;
            t->pregap_stored = pgtype[0] == 'V';
            t->first = frame;
            frame += (uint32_t)((frames + 3) & ~3);   /* every track is padded to a multiple of 4 frames */
        }
        metaoff = be64(h + 8);
    }
    if (c->ntracks == 0) return fail(err, errlen, "not a CD image (no track metadata)");
    return true;
}

/* ---- sync / ECC (only for verification: regenerating what the CD codecs strip) ---- */
static uint8_t g_ecc_f[256], g_ecc_b[256];

static void ecc_tables(void)
{
    static bool done;
    if (done) return;
    for (int i = 0; i < 256; i++) {
        int j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        g_ecc_f[i] = (uint8_t)j;
        g_ecc_b[i ^ j] = (uint8_t)i;
    }
    done = true;
}

static void ecc_block(const uint8_t *src, int major_count, int minor_count, int major_mult, int minor_inc, uint8_t *dest)
{
    int size = major_count * minor_count;
    for (int major = 0; major < major_count; major++) {
        int index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (int minor = 0; minor < minor_count; minor++) {
            uint8_t t = src[index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= t;
            b ^= t;
            a = g_ecc_f[a];
        }
        a = g_ecc_b[g_ecc_f[a] ^ b];
        dest[major] = a;
        dest[major + major_count] = a ^ b;
    }
}

static void ecc_generate(uint8_t *sector)
{
    static const uint8_t SYNC[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    ecc_tables();
    memcpy(sector, SYNC, 12);
    ecc_block(sector + 0xC, 86, 24, 2, 86, sector + 0x81C);    /* P */
    ecc_block(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);   /* Q */
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

/* ---- hunks ---- */
static bool decode_cd(chd *c, int codec, const uint8_t *src, uint32_t complen, uint8_t *dest, bool full)
{
    uint32_t frames = c->hunk_bytes / CHD_FRAME;
    uint8_t *sec = c->work, *sub = c->work + frames * SECTOR;
    if (codec == CODEC_CDFL) {
        long used = sbpc_flac_decode(src, complen, sec, frames * SECTOR / 4);
        if (used < 0) return false;
        if (full && sbpc_inflate(src + used, complen - (uint32_t)used, sub, frames * SUBCODE) < 0) return false;
        if (!full) memset(sub, 0, frames * SUBCODE);
    } else {
        uint32_t cbytes = c->hunk_bytes < 65536 ? 2 : 3, eccbytes = (frames + 7) / 8, hdr = eccbytes + cbytes;
        uint32_t base = cbytes == 2 ? be16(src + eccbytes) : (be16(src + eccbytes) << 8 | src[eccbytes + 2]);
        if (hdr + base > complen) return false;
        if (codec == CODEC_CDLZ) {
            if (sbpc_lzma_decode(src + hdr, base, sec, frames * SECTOR, 3, 0, 2) < 0) return false;
        } else if (sbpc_inflate(src + hdr, base, sec, frames * SECTOR) != (long)(frames * SECTOR)) {
            return false;
        }
        if (full && sbpc_inflate(src + hdr + base, complen - hdr - base, sub, frames * SUBCODE) < 0) return false;
        if (!full) memset(sub, 0, frames * SUBCODE);
        for (uint32_t i = 0; i < frames; i++) {
            memcpy(dest + i * CHD_FRAME, sec + i * SECTOR, SECTOR);
            memcpy(dest + i * CHD_FRAME + SECTOR, sub + i * SUBCODE, SUBCODE);
            if (full && (src[i / 8] & (1u << (i % 8))))   /* sync and ECC were stripped: put them back */
                ecc_generate(dest + i * CHD_FRAME);
        }
        return true;
    }
    for (uint32_t i = 0; i < frames; i++) {
        memcpy(dest + i * CHD_FRAME, sec + i * SECTOR, SECTOR);
        memcpy(dest + i * CHD_FRAME + SECTOR, sub + i * SUBCODE, SUBCODE);
    }
    return true;
}

static bool read_hunk(chd *c, uint32_t hunk, uint8_t *dest, bool full, int depth)
{
    map_entry *e;
    if (hunk >= c->hunks || depth > 4) return false;
    e = &c->map[hunk];
    switch (e->type) {
    case COMP_TYPE0: case COMP_TYPE1: case COMP_TYPE2: case COMP_TYPE3: {
        int codec = c->codec[e->type];
        if (e->length > c->hunk_bytes * 2 || !read_at(c, e->offset, c->cmp, e->length)) return false;
        if (codec != CODEC_CDLZ && codec != CODEC_CDZL && codec != CODEC_CDFL) return false;
        return decode_cd(c, codec, c->cmp, e->length, dest, full);
    }
    case COMP_NONE:
        if (e->offset == 0) { memset(dest, 0, c->hunk_bytes); return true; }
        return read_at(c, e->offset, dest, c->hunk_bytes);
    case COMP_SELF:
        return read_hunk(c, (uint32_t)e->offset, dest, full, depth + 1);
    default:
        return false;   /* parent CHDs (diffs) aren't supported */
    }
}

bool chd_read_frame(chd *c, uint32_t frame, uint8_t out[CHD_FRAME], bool full)
{
    uint32_t per = c->hunk_bytes / CHD_FRAME, hunk = frame / per;
    if (c->cached != hunk || (full && !c->cached_full)) {
        if (!read_hunk(c, hunk, c->hunk, full, 0)) { c->cached = ~0u; return false; }
        c->cached = hunk;
        c->cached_full = full;
    }
    memcpy(out, c->hunk + (frame % per) * CHD_FRAME, CHD_FRAME);
    return true;
}

long chd_verify(chd *c, void (*progress)(float frac, void *user), void *user)
{
    long bad = 0;
    for (uint32_t i = 0; i < c->hunks; i++) {
        map_entry *e = &c->map[i];
        uint32_t src = i;
        int guard = 0;
        while (c->map[src].type == COMP_SELF && guard++ < 4) src = (uint32_t)c->map[src].offset;
        if (!read_hunk(c, i, c->hunk, true, 0)) { bad++; continue; }
        if (c->map[src].type <= COMP_NONE && crc16(c->hunk, c->hunk_bytes) != c->map[src].crc) bad++;
        (void)e;
        if (progress && (i & 255) == 0) progress((float)i / (float)c->hunks, user);
    }
    c->cached = ~0u;
    return bad;
}

/* ---- open / close ---- */
chd *chd_open(const char *path, char *err, size_t errlen)
{
    uint8_t h[124];
    chd *c = calloc(1, sizeof *c);
    if (!c) { fail(err, errlen, "out of memory"); return NULL; }
    c->cached = ~0u;
    c->f = fs_open(path, "rb");
    if (!c->f) { fail(err, errlen, "can't open the CHD file"); free(c); return NULL; }
    if (fread(h, 1, sizeof h, c->f) != sizeof h || memcmp(h, "MComprHD", 8) != 0) {
        fail(err, errlen, "not a CHD file");
        goto bad;
    }
    if (be32(h + 12) != 5) { fail(err, errlen, "only CHD version 5 is supported (convert with chdman)"); goto bad; }
    for (int i = 0; i < 4; i++) {
        const uint8_t *t = h + 16 + i * 4;
        c->codec[i] = memcmp(t, "cdlz", 4) == 0 ? CODEC_CDLZ : memcmp(t, "cdzl", 4) == 0 ? CODEC_CDZL
                    : memcmp(t, "cdfl", 4) == 0 ? CODEC_CDFL : (t[0] | t[1] | t[2] | t[3]) == 0 ? CODEC_NONE : -1;
        if (c->codec[i] < 0) { fail(err, errlen, "this CHD uses a codec SBPC can't read (only cdlz, cdzl, cdfl)"); goto bad; }
    }
    if (memcmp(h + 104, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) != 0) {
        fail(err, errlen, "this CHD needs a parent CHD; SBPC reads standalone CHDs only");
        goto bad;
    }
    c->logical = be64(h + 32);
    c->hunk_bytes = be32(h + 56);
    c->unit_bytes = be32(h + 60);
    if (c->unit_bytes != CHD_FRAME || c->hunk_bytes == 0 || c->hunk_bytes % CHD_FRAME || c->hunk_bytes > 1024 * 1024) {
        fail(err, errlen, "not a CD CHD");
        goto bad;
    }
    c->hunks = (uint32_t)((c->logical + c->hunk_bytes - 1) / c->hunk_bytes);
    c->hunk = malloc(c->hunk_bytes);
    c->cmp = malloc((size_t)c->hunk_bytes * 2);
    c->work = malloc(c->hunk_bytes);
    if (!c->hunk || !c->cmp || !c->work) { fail(err, errlen, "out of memory"); goto bad; }
    if (!read_tracks(c, be64(h + 48), err, errlen) || !read_map(c, be64(h + 40), err, errlen)) goto bad;
    return c;
bad:
    chd_close(c);
    return NULL;
}

void chd_close(chd *c)
{
    if (!c) return;
    if (c->f) fclose(c->f);
    free(c->map);
    free(c->hunk);
    free(c->cmp);
    free(c->work);
    free(c);
}

int chd_track_count(const chd *c) { return c->ntracks; }
const chd_track *chd_get_track(const chd *c, int i) { return i >= 0 && i < c->ntracks ? &c->tracks[i] : NULL; }
