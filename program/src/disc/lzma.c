/* LZMA decoding, following the structure of the reference decoder in the LZMA specification
 * (LzmaSpec): a binary range decoder with adaptive probabilities, literal / match / rep states.
 * The whole output is the dictionary, since CHD hunks are decoded in one piece. */
#include "disc/lzma.h"

#include <stdlib.h>
#include <string.h>

#define PROB_INIT 1024
#define NUM_STATES 12
#define POS_BITS_MAX 4
#define LEN_TO_POS_STATES 4
#define ALIGN_BITS 4
#define END_POS_MODEL_INDEX 14
#define FULL_DISTANCES 128
#define MATCH_MIN_LEN 2

typedef uint16_t prob;

typedef struct {
    const uint8_t *in;
    size_t inlen, pos;
    uint32_t range, code;
    int err;
} rc;

static uint8_t rc_byte(rc *r)
{
    if (r->pos >= r->inlen) { r->err = 1; return 0; }
    return r->in[r->pos++];
}

static void rc_norm(rc *r)
{
    if (r->range < (1u << 24)) {
        r->range <<= 8;
        r->code = (r->code << 8) | rc_byte(r);
    }
}

static int rc_bit(rc *r, prob *p)
{
    uint32_t bound = (r->range >> 11) * *p;
    int bit;
    if (r->code < bound) {
        *p = (prob)(*p + ((2048 - *p) >> 5));
        r->range = bound;
        bit = 0;
    } else {
        *p = (prob)(*p - (*p >> 5));
        r->code -= bound;
        r->range -= bound;
        bit = 1;
    }
    rc_norm(r);
    return bit;
}

static uint32_t rc_direct(rc *r, int n)
{
    uint32_t res = 0;
    while (n--) {
        uint32_t t;
        r->range >>= 1;
        r->code -= r->range;
        t = 0u - (r->code >> 31);
        r->code += r->range & t;
        res = (res << 1) + (t + 1);
        rc_norm(r);
    }
    return res;
}

static uint32_t tree(rc *r, prob *probs, int nbits)
{
    uint32_t m = 1;
    for (int i = 0; i < nbits; i++) m = (m << 1) + (uint32_t)rc_bit(r, &probs[m]);
    return m - (1u << nbits);
}

static uint32_t tree_rev(rc *r, prob *probs, int nbits)
{
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < nbits; i++) {
        uint32_t bit = (uint32_t)rc_bit(r, &probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

typedef struct {
    prob choice, choice2;
    prob low[1 << POS_BITS_MAX][1 << 3];
    prob mid[1 << POS_BITS_MAX][1 << 3];
    prob high[1 << 8];
} len_dec;

static void len_init(len_dec *l)
{
    l->choice = l->choice2 = PROB_INIT;
    for (size_t i = 0; i < sizeof l->low / sizeof(prob); i++) (&l->low[0][0])[i] = PROB_INIT;
    for (size_t i = 0; i < sizeof l->mid / sizeof(prob); i++) (&l->mid[0][0])[i] = PROB_INIT;
    for (size_t i = 0; i < sizeof l->high / sizeof(prob); i++) l->high[i] = PROB_INIT;
}

static uint32_t len_decode(rc *r, len_dec *l, uint32_t pos_state)
{
    if (!rc_bit(r, &l->choice)) return tree(r, l->low[pos_state], 3);
    if (!rc_bit(r, &l->choice2)) return 8 + tree(r, l->mid[pos_state], 3);
    return 16 + tree(r, l->high, 8);
}

typedef struct {
    prob is_match[NUM_STATES << POS_BITS_MAX], is_rep[NUM_STATES], is_rep_g0[NUM_STATES];
    prob is_rep_g1[NUM_STATES], is_rep_g2[NUM_STATES], is_rep0_long[NUM_STATES << POS_BITS_MAX];
    prob pos_slot[LEN_TO_POS_STATES][1 << 6];
    prob pos_dec[1 + FULL_DISTANCES - END_POS_MODEL_INDEX];
    prob align[1 << ALIGN_BITS];
    len_dec len, rep_len;
} model;

static void probs_init(prob *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = PROB_INIT; }

int sbpc_lzma_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen, int lc, int lp, int pb)
{
    model *md;
    prob *lit;
    rc r;
    size_t out = 0, nlit = (size_t)0x300 << (lc + lp);
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned state = 0;
    int result = 0;

    if (lc > 8 || lp > 4 || pb > POS_BITS_MAX) return -1;
    md = malloc(sizeof *md);
    lit = malloc(nlit * sizeof *lit);
    if (!md || !lit) { free(md); free(lit); return -1; }
    probs_init((prob *)md, offsetof(model, len) / sizeof(prob));
    len_init(&md->len);
    len_init(&md->rep_len);
    probs_init(lit, nlit);

    memset(&r, 0, sizeof r);
    r.in = src;
    r.inlen = srclen;
    r.range = 0xFFFFFFFFu;
    if (rc_byte(&r) != 0) result = -1;
    for (int i = 0; i < 4; i++) r.code = (r.code << 8) | rc_byte(&r);

    while (result == 0 && out < dstlen && !r.err) {
        uint32_t pos_state = (uint32_t)out & ((1u << pb) - 1), len;
        if (!rc_bit(&r, &md->is_match[(state << POS_BITS_MAX) + pos_state])) {
            /* literal */
            uint32_t prev = out ? dst[out - 1] : 0;
            uint32_t lit_state = (((uint32_t)out & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc));
            prob *probs = &lit[0x300 * lit_state];
            uint32_t sym = 1;
            if (state >= 7) {   /* after a match: the byte at rep0 steers the first bits */
                uint32_t match_byte = out > rep0 ? dst[out - rep0 - 1] : 0;
                do {
                    uint32_t match_bit = (match_byte >> 7) & 1, bit;
                    match_byte <<= 1;
                    bit = (uint32_t)rc_bit(&r, &probs[((1 + match_bit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (match_bit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | (uint32_t)rc_bit(&r, &probs[sym]);
            dst[out++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (rc_bit(&r, &md->is_rep[state])) {
            if (out == 0) { result = -1; break; }
            if (!rc_bit(&r, &md->is_rep_g0[state])) {
                if (!rc_bit(&r, &md->is_rep0_long[(state << POS_BITS_MAX) + pos_state])) {
                    /* short rep: one byte from rep0 */
                    state = state < 7 ? 9 : 11;
                    dst[out] = dst[out - rep0 - 1];
                    out++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(&r, &md->is_rep_g1[state])) {
                    dist = rep1;
                } else {
                    if (!rc_bit(&r, &md->is_rep_g2[state])) dist = rep2;
                    else { dist = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = len_decode(&r, &md->rep_len, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            uint32_t pos_slot, len_state;
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = len_decode(&r, &md->len, pos_state);
            state = state < 7 ? 7 : 10;
            len_state = len < LEN_TO_POS_STATES - 1 ? len : LEN_TO_POS_STATES - 1;
            pos_slot = tree(&r, md->pos_slot[len_state], 6);
            if (pos_slot < 4) {
                rep0 = pos_slot;
            } else {
                int direct = (int)(pos_slot >> 1) - 1;
                uint32_t dist = (2 | (pos_slot & 1)) << direct;
                if (pos_slot < END_POS_MODEL_INDEX) {
                    dist += tree_rev(&r, md->pos_dec + dist - pos_slot, direct);
                } else {
                    dist += rc_direct(&r, direct - ALIGN_BITS) << ALIGN_BITS;
                    dist += tree_rev(&r, md->align, ALIGN_BITS);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) break;   /* end marker */
        }
        len += MATCH_MIN_LEN;
        if (rep0 >= out) { result = -1; break; }
        while (len-- && out < dstlen) {
            dst[out] = dst[out - rep0 - 1];
            out++;
        }
    }
    free(lit);
    free(md);
    if (r.err && out < dstlen) result = -1;
    return result == 0 && out == dstlen ? 0 : -1;
}
