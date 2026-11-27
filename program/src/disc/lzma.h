/* Raw LZMA decoder (no .lzma/.xz header): what CHD's "cdlz" codec stores, with lc=3, lp=0, pb=2 and
 * a known output size. Written for SBPC (no LZMA SDK dependency). */
#ifndef SBPC_DISC_LZMA_H
#define SBPC_DISC_LZMA_H

#include <stddef.h>
#include <stdint.h>

/* Decodes exactly dstlen bytes. Returns 0, or -1 on bad data. */
int sbpc_lzma_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen, int lc, int lp, int pb);

#endif
