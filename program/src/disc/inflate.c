/* Raw Deflate decoder in the style of Mark Adler's "puff": canonical Huffman codes decoded bit by bit.
 * Simple rather than fast; CHD hunks are small (about 19 KB) and decoded once each. */
#include "disc/inflate.h"

#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define FIXLCODES 288   /* the fixed code has 288 literal/length codes (286 and 287 unused but present) */
#define MAXDCODES 30

typedef struct {
    const uint8_t *in;
    size_t inlen, incnt;
    uint8_t *out;
    size_t outlen, outcnt;
    uint32_t bitbuf;
    int bitcnt;
    int err;
} state;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} huffman;

static int bits(state *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int decode(state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int count;
        code |= bits(s, 1);
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

/* Builds the decoding tables from code lengths. Returns 0 for a complete code, >0 incomplete, <0 bad. */
static int construct(huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    int left = 1;
    memset(h->count, 0, sizeof h->count);
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static void put(state *s, uint8_t b)
{
    if (s->outcnt < s->outlen) s->out[s->outcnt] = b;
    s->outcnt++;
}

static int codes(state *s, const huffman *lencode, const huffman *distcode)
{
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(s, lencode);
        if (s->err) return -1;
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dist;
            sym -= 257;
            if (sym >= 29) return -1;
            len = lens[sym] + bits(s, lext[sym]);
            sym = decode(s, distcode);
            if (s->err || sym < 0 || sym >= 30) return -1;
            dist = dists[sym] + bits(s, dext[sym]);
            if ((size_t)dist > s->outcnt) return -1;
            while (len--) {
                uint8_t b = s->outcnt - (size_t)dist < s->outlen ? s->out[s->outcnt - (size_t)dist] : 0;
                put(s, b);
            }
        }
        if (s->outcnt >= s->outlen) return 0;   /* the caller only wants this much */
    }
}

static int stored(state *s)
{
    unsigned len;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    if ((s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8) != (~len & 0xFFFF)) return -1;
    s->incnt += 4;
    if (s->incnt + len > s->inlen) return -1;
    while (len--) put(s, s->in[s->incnt++]);
    return 0;
}

static int fixed(state *s)
{
    static int built;
    static huffman lencode, distcode;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(state *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    huffman lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4, index;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return -1;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode), len = 0, rep;
        if (s->err || sym < 0) return -1;
        if (sym < 16) { lengths[index++] = (short)sym; continue; }
        if (sym == 16) {
            if (index == 0) return -1;
            len = lengths[index - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + bits(s, 3);
        } else {
            rep = 11 + bits(s, 7);
        }
        if (index + rep > nlen + ndist) return -1;
        while (rep--) lengths[index++] = (short)len;
    }
    if (lengths[256] == 0) return -1;
    if (construct(&lencode, lengths, nlen) < 0 || (construct(&lencode, lengths, nlen) > 0 && nlen - lencode.count[0] != 1))
        return -1;
    if (construct(&distcode, lengths + nlen, ndist) < 0 ||
        (construct(&distcode, lengths + nlen, ndist) > 0 && ndist - distcode.count[0] != 1))
        return -1;
    return codes(s, &lencode, &distcode);
}

long sbpc_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    state s;
    int last;
    memset(&s, 0, sizeof s);
    s.in = src;
    s.inlen = srclen;
    s.out = dst;
    s.outlen = dstlen;
    do {
        int type, r;
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.err) return -1;
        r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r != 0 || s.err) return -1;
    } while (!last && s.outcnt < s.outlen);
    return (long)(s.outcnt < s.outlen ? s.outcnt : s.outlen);
}
