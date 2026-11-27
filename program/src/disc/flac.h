/* FLAC frame decoder for CHD's "cdfl" codec: raw FLAC frames (no stream header) of 16-bit stereo CD
 * audio. Written for SBPC (no libFLAC dependency). */
#ifndef SBPC_DISC_FLAC_H
#define SBPC_DISC_FLAC_H

#include <stddef.h>
#include <stdint.h>

/* Decodes frames from src until `samples` stereo samples are out, written as interleaved 16-bit
 * big-endian (L R L R ...) into dst. Returns the number of src bytes used, or -1 on bad data. */
long sbpc_flac_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t samples);

#endif
