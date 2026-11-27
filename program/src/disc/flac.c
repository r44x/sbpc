/* FLAC frames: header, per-channel subframes (constant, verbatim, fixed and LPC prediction with
 * partitioned Rice residuals), stereo decorrelation. Checksums (CRC-8/16) are not checked here: CHD
 * checks every hunk with its own CRC-16. */
#include "disc/flac.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BLOCK 65536

typedef struct {
    const uint8_t *in;
    size_t len, pos;     /* pos in bits */
    int err;
} br;

static uint32_t rd(br *b, int n)
{
    uint32_t v = 0;
    while (n--) {
        size_t byte = b->pos >> 3;
        if (byte >= b->len) { b->err = 1; return 0; }
        v = (v << 1) | ((b->in[byte] >> (7 - (b->pos & 7))) & 1u);
        b->pos++;
    }
    return v;
}

static int32_t rd_signed(br *b, int n)
{
    uint32_t v;
    if (n <= 0 || n > 32) return 0;
    v = rd(b, n);
    if (n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n;   /* sign-extend */
    return (int32_t)v;
}

static uint32_t rd_unary(br *b)
{
    uint32_t q = 0;
    while (!b->err && rd(b, 1) == 0) q++;
    return q;
}

static void align(br *b) { b->pos = (b->pos + 7) & ~(size_t)7; }

/* UTF-8-like coded frame/sample number: just skipped */
static void skip_utf8(br *b)
{
    uint32_t first = rd(b, 8);
    int extra = 0;
    while (extra < 7 && (first & (0x80u >> extra))) extra++;
    if (extra > 1) rd(b, 8 * (extra - 1));
}

static int residual(br *b, int32_t *res, int block, int order)
{
    int method = (int)rd(b, 2), porder = (int)rd(b, 4);
    int pbits = method == 0 ? 4 : 5, escape = method == 0 ? 15 : 31, n = 0;
    if (method > 1) return -1;
    for (int p = 0; p < (1 << porder); p++) {
        int count = (block >> porder) - (p == 0 ? order : 0);
        uint32_t param = rd(b, pbits);
        if (count < 0) return -1;
        if (param == (uint32_t)escape) {
            int nbits = (int)rd(b, 5);
            for (int i = 0; i < count; i++) res[n++] = rd_signed(b, nbits);
        } else {
            for (int i = 0; i < count; i++) {
                uint32_t u = (rd_unary(b) << param) | rd(b, (int)param);
                res[n++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
        if (b->err) return -1;
    }
    return 0;
}

static int subframe(br *b, int32_t *out, int block, int bps)
{
    int type, wasted = 0, order;
    if (rd(b, 1) != 0) return -1;
    type = (int)rd(b, 6);
    if (rd(b, 1)) wasted = (int)rd_unary(b) + 1;
    bps -= wasted;
    if (type == 0) {                               /* constant */
        int32_t v = rd_signed(b, bps);
        for (int i = 0; i < block; i++) out[i] = v;
    } else if (type == 1) {                        /* verbatim */
        for (int i = 0; i < block; i++) out[i] = rd_signed(b, bps);
    } else if (type >= 8 && type <= 12) {          /* fixed predictor */
        order = type - 8;
        for (int i = 0; i < order; i++) out[i] = rd_signed(b, bps);
        if (residual(b, out + order, block, order) < 0) return -1;
        for (int i = order; i < block; i++) {
            int32_t r = out[i];
            switch (order) {
            case 1: out[i] = r + out[i - 1]; break;
            case 2: out[i] = r + 2 * out[i - 1] - out[i - 2]; break;
            case 3: out[i] = r + 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
            case 4: out[i] = r + 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
            default: break;
            }
        }
    } else if (type >= 32) {                       /* LPC */
        int32_t coef[32];
        int precision, shift;
        order = type - 31;
        for (int i = 0; i < order; i++) out[i] = rd_signed(b, bps);
        precision = (int)rd(b, 4) + 1;
        if (precision == 16) return -1;
        shift = rd_signed(b, 5);
        if (shift < 0) return -1;
        for (int i = 0; i < order; i++) coef[i] = rd_signed(b, precision);
        if (residual(b, out + order, block, order) < 0) return -1;
        for (int i = order; i < block; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)coef[j] * out[i - j - 1];
            out[i] += (int32_t)(sum >> shift);
        }
    } else {
        return -1;
    }
    if (wasted)
        for (int i = 0; i < block; i++) out[i] = (int32_t)((uint32_t)out[i] << wasted);
    return b->err ? -1 : 0;
}

static void put16be(uint8_t *p, int32_t v)
{
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    p[0] = (uint8_t)((uint16_t)v >> 8);
    p[1] = (uint8_t)v;
}

long sbpc_flac_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t samples)
{
    static const int SIZES[16] = {0, 192, 576, 1152, 2304, 4608, -1, -2, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    int32_t *ch[2];
    br b;
    size_t done = 0;
    long result;
    ch[0] = malloc(MAX_BLOCK * sizeof(int32_t));
    ch[1] = malloc(MAX_BLOCK * sizeof(int32_t));
    if (!ch[0] || !ch[1]) { free(ch[0]); free(ch[1]); return -1; }
    memset(&b, 0, sizeof b);
    b.in = src;
    b.len = srclen;

    while (done < samples) {
        int bs_code, sr_code, chan, ss_code, block, bps;
        if (rd(&b, 14) != 0x3FFE) goto bad;        /* frame sync */
        rd(&b, 2);                                 /* reserved, blocking strategy */
        bs_code = (int)rd(&b, 4);
        sr_code = (int)rd(&b, 4);
        chan = (int)rd(&b, 4);
        ss_code = (int)rd(&b, 3);
        rd(&b, 1);
        skip_utf8(&b);
        block = SIZES[bs_code];
        if (block == -1) block = (int)rd(&b, 8) + 1;
        else if (block == -2) block = (int)rd(&b, 16) + 1;
        if (sr_code == 12) rd(&b, 8);
        else if (sr_code == 13 || sr_code == 14) rd(&b, 16);
        rd(&b, 8);                                 /* CRC-8 */
        if (b.err || block <= 0 || block > MAX_BLOCK) goto bad;
        bps = ss_code == 0 || ss_code == 4 ? 16 : ss_code == 1 ? 8 : ss_code == 2 ? 12 : ss_code == 5 ? 20 : ss_code == 6 ? 24 : -1;
        if (bps < 0 || (chan != 1 && chan < 8) || chan > 10) goto bad;   /* stereo only */
        /* side channels carry one more bit */
        if (subframe(&b, ch[0], block, bps + (chan == 9 ? 1 : 0)) < 0) goto bad;
        if (subframe(&b, ch[1], block, bps + (chan == 8 || chan == 10 ? 1 : 0)) < 0) goto bad;
        align(&b);
        rd(&b, 16);                                /* CRC-16 */
        for (int i = 0; i < block && done < samples; i++, done++) {
            int32_t l = ch[0][i], r = ch[1][i];
            if (chan == 8) r = l - r;                          /* left / side */
            else if (chan == 9) l = l + r;                     /* side / right */
            else if (chan == 10) {                             /* mid / side */
                int32_t mid = (int32_t)((uint32_t)l << 1) | (r & 1), side = r;
                l = (mid + side) >> 1;
                r = (mid - side) >> 1;
            }
            if (bps != 16) { l = bps > 16 ? l >> (bps - 16) : l << (16 - bps); r = bps > 16 ? r >> (bps - 16) : r << (16 - bps); }
            put16be(dst + done * 4, l);
            put16be(dst + done * 4 + 2, r);
        }
        if (b.err) goto bad;
    }
    result = (long)((b.pos + 7) >> 3);
    free(ch[0]);
    free(ch[1]);
    return result;
bad:
    free(ch[0]);
    free(ch[1]);
    return -1;
}
