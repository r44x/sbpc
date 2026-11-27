/* Minimal PNG writer (uncompressed deflate), no dependencies. Pixels are 0xAARRGGBB. */
#ifndef SBPC_PNG_H
#define SBPC_PNG_H

#include <stdbool.h>
#include <stdint.h>

bool png_write(const char *path, const uint32_t *argb, int w, int h);

#endif
