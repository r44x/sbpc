/* Input recordings: every emulated frame's pad words, so a session played in the game window can be replayed
 * headless frame for frame (`sbpc-cli --cli boot <image> <frames>` with SBPC_REPLAY=file). The machine is
 * deterministic for given inputs; its clock is the other input, so the file keeps the clock's start too.
 *
 * File: "SBPCREC1", int64 rtc_start (big-endian), then per frame: players, port layout (one byte each) and
 * SMPC_SLOTS big-endian u16 pad words as the SMPC reads them (active low). */
#ifndef SBPC_SATURN_REC_H
#define SBPC_SATURN_REC_H

#include "saturn/saturn.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint64_t frames;     /* written or read so far */
} input_rec;

typedef struct {
    uint8_t players, ports;           /* smpc_layout arguments */
    uint16_t pad[SMPC_SLOTS];
} rec_frame;

bool rec_create(input_rec *r, const char *path, int64_t rtc_start);
bool rec_write(input_rec *r, const rec_frame *fr);
bool rec_open(input_rec *r, const char *path, int64_t *rtc_start);
bool rec_read(input_rec *r, rec_frame *fr);   /* false at the end */
void rec_close(input_rec *r);

#endif
