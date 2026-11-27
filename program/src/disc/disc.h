/* An opened disc image: the cue sheet plus sector reads. Images are cue/bin, ISO+WAV (through a
 * .cue) or CHD v5 (a .chd: the cue sheet is built from its track metadata). */
#ifndef SBPC_DISC_DISC_H
#define SBPC_DISC_DISC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "disc/cue.h"

typedef struct {
    cue_sheet cue;
    FILE *fp;          /* file of the most recently read track (cached) */
    char fp_path[SBPC_PATH_MAX];
    struct chd *chd;   /* CHD image: tracks' data_offset is their first CHD frame */
} disc;

/* path: a .cue, or a .chd. */
bool disc_open(disc *d, const char *cue_path, char *err, size_t errlen);
/* True for file names SBPC can open as a disc (.cue, .chd). */
bool disc_is_image_name(const char *name);
void disc_close(disc *d);

/* Index of the first data track, or -1. */
int disc_first_data_track(const disc *d);

/* Reads the 2048 user bytes of sector `sector` (relative to INDEX 01) of a data track. */
bool disc_read_user(disc *d, int track, int64_t sector, uint8_t out[2048]);

/* Reads the 2352 bytes of audio sector `sector` (relative to INDEX 01) of an audio track:
 * 588 stereo frames of 16-bit little-endian samples. Sectors outside the file read as silence. */
bool disc_read_audio(disc *d, int track, int64_t sector, uint8_t out[2352]);

/* SHA-1 over the user data of every sector of a data track. Same result for .bin and .iso.
 * progress (may be NULL) is called with 0..1; return false from it to cancel. */
typedef bool (*disc_progress_cb)(float frac, void *user);
bool disc_hash_track(disc *d, int track, char hex_out[41], disc_progress_cb progress, void *user);

/* Writes the whole disc into dir as ISO+WAV: "<base>.cue", "<base> (Track 01).iso" (2048-byte user
 * data) and "<base> (Track NN).wav" per audio track. Each file holds its track's pregap, then the
 * track, so the copy has the same tracks, pregaps and positions as the original and reads back the
 * same through disc_read_user / disc_read_audio. The .cue is written last: a copy without it is
 * incomplete. */
bool disc_export_isowav(disc *d, const char *dir, const char *base, disc_progress_cb progress, void *user,
                        char *err, size_t errlen);

#endif
