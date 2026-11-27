/* CUE sheet parsing: cue/bin (MODE1/2352, MODE2/2352, AUDIO) and ISO+WAV (MODE1/2048, WAVE).
 * Tracks may each have their own file or share one file. */
#ifndef SBPC_DISC_CUE_H
#define SBPC_DISC_CUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/fs.h"

#define CUE_MAX_TRACKS 99

typedef enum { TRACK_MODE1_2048, TRACK_MODE1_2352, TRACK_MODE2_2352, TRACK_AUDIO } track_mode;

typedef struct {
    int number;
    track_mode mode;
    int sector_size;          /* bytes per sector in the file */
    char path[SBPC_PATH_MAX]; /* resolved file path */
    bool is_wave;             /* file is a RIFF WAVE; data_offset skips its header */
    int64_t data_offset;      /* byte offset of sector 0 within the file */
    int64_t index0;           /* file sector of INDEX 00 (pregap), or -1 if none */
    int64_t index1;           /* file sector of INDEX 01 (track start) */
    int64_t sectors;          /* sectors from INDEX 01 to end of track */
} cue_track;

typedef struct {
    int track_count;
    cue_track tracks[CUE_MAX_TRACKS];
} cue_sheet;

/* Parse a .cue file from disk. File paths are resolved relative to the cue's folder and
 * every referenced file must exist. err receives a human-readable reason on failure. */
bool cue_load(const char *cue_path, cue_sheet *out, char *err, size_t errlen);

/* Same, from text already in memory (used by tests). */
bool cue_parse(const char *text, const char *base_dir, cue_sheet *out, char *err, size_t errlen);

const char *track_mode_name(track_mode m);
bool track_is_data(const cue_track *t);
int track_user_data_offset(const cue_track *t); /* offset of the 2048 user bytes in a sector */

#endif
