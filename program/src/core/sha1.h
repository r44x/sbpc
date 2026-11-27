/* SHA-1 (FIPS 180-4). Used only to identify discs, not for security. */
#ifndef SBPC_CORE_SHA1_H
#define SBPC_CORE_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t n);
void sha1_final(sha1_ctx *c, uint8_t out[20]);
void sha1_hex(const uint8_t digest[20], char out[41]);

#endif
