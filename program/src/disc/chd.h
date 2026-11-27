/* CHD (MAME "Compressed Hunks of Data") v5 CD images: header, track metadata, the compressed hunk
 * map and the CD codecs (cdlz = LZMA, cdzl = Deflate, cdfl = FLAC; subcode in Deflate). Read-only.
 * Every frame is 2448 bytes: a raw 2352-byte sector then 96 bytes of subcode. CD audio is stored
 * big-endian. Decoders are SBPC's own (inflate.c, lzma.c, flac.c): no zlib, LZMA SDK or libFLAC. */
#ifndef SBPC_DISC_CHD_H
#define SBPC_DISC_CHD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHD_FRAME 2448
#define CHD_MAX_TRACKS 99

enum { CHD_TRACK_MODE1_RAW, CHD_TRACK_MODE1, CHD_TRACK_MODE2_RAW, CHD_TRACK_AUDIO, CHD_TRACK_OTHER };

typedef struct {
    int type;            /* CHD_TRACK_* */
    int frames;          /* frames stored, pregap included when pregap_stored */
    int pregap;          /* pregap frames (INDEX 00 to INDEX 01) */
    bool pregap_stored;  /* PGTYPE starts with 'V': the pregap is part of the stored frames */
    uint32_t first;      /* frame index of the track's first stored frame */
} chd_track;

typedef struct chd chd;

chd *chd_open(const char *path, char *err, size_t errlen);
void chd_close(chd *c);
int chd_track_count(const chd *c);
const chd_track *chd_get_track(const chd *c, int i);
/* Reads stored frame `frame` (2448 bytes: sector + subcode). Sync and ECC of data sectors are put
 * back only when `full` is set (the user data never needs them). */
bool chd_read_frame(chd *c, uint32_t frame, uint8_t out[CHD_FRAME], bool full);
/* Decodes every hunk and checks its CRC-16 (with sync/ECC regenerated). Returns the number of bad
 * hunks, or -1 if the file can't be read. progress (may be NULL) gets 0..1. */
long chd_verify(chd *c, void (*progress)(float frac, void *user), void *user);

#endif
