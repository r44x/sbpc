/* Raw Deflate (RFC 1951) decoder, no zlib/gzip wrapper: what CHD's "cdzl" codec and its subcode
 * streams use. Written for SBPC (no zlib dependency). */
#ifndef SBPC_DISC_INFLATE_H
#define SBPC_DISC_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Decodes src into dst (at most dstlen bytes). Returns the number of bytes written, or -1 on bad data.
 * Stops early once dst is full. */
long sbpc_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

#endif
