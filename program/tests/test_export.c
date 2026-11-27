/* The game's own copy of a disc (disc_export_isowav, used by Build & Play): exporting a cue/bin disc as
 * ISO+WAV must give the same tracks, pregaps, data, audio and data hash when read back. No game data:
 * the disc is synthetic. */
#include "disc/disc.h"
#include "core/fs.h"
#include "test.h"

#define DATA_SECTORS 20
#define AUDIO_SECTORS 12
#define PREGAP 3

static unsigned char bin[(DATA_SECTORS > AUDIO_SECTORS + PREGAP ? DATA_SECTORS : AUDIO_SECTORS + PREGAP) * 2352];

int main(void)
{
    char err[256];
    disc a, b;
    uint8_t x[2352], y[2352];
    char ha[41], hb[41];
    const char *cue =
        "FILE \"t_exp_data.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
        "FILE \"t_exp_audio.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:03\n";

    /* data track: raw MODE1 sectors with a pattern in the user data */
    for (int s = 0; s < DATA_SECTORS; s++) {
        unsigned char *sec = bin + (size_t)s * 2352;
        memset(sec, 0, 2352);
        memset(sec + 1, 0xFF, 10);
        sec[15] = 1;
        for (int i = 0; i < 2048; i++) sec[16 + i] = (unsigned char)(s * 7 + i * 3);
    }
    CHECK(write_file("t_exp_data.bin", bin, (size_t)DATA_SECTORS * 2352));
    /* audio track: a 3-sector pregap, then sectors with a sample pattern */
    for (int i = 0; i < (AUDIO_SECTORS + PREGAP) * 2352; i++) bin[i] = (unsigned char)(i < PREGAP * 2352 ? 0 : i * 13 + 1);
    CHECK(write_file("t_exp_audio.bin", bin, (size_t)(AUDIO_SECTORS + PREGAP) * 2352));
    CHECK(write_file("t_exp.cue", cue, strlen(cue)));

    CHECK(disc_open(&a, "t_exp.cue", err, sizeof err));
    CHECK(disc_export_isowav(&a, "t_exp_copy", "Test Disc", NULL, NULL, err, sizeof err));
    CHECK(fs_is_file("t_exp_copy/Test Disc.cue"));
    CHECK(fs_is_file("t_exp_copy/Test Disc (Track 01).iso"));
    CHECK(fs_is_file("t_exp_copy/Test Disc (Track 02).wav"));
    CHECK(disc_open(&b, "t_exp_copy/Test Disc.cue", err, sizeof err));

    /* same layout */
    CHECK(b.cue.track_count == 2);
    for (int t = 0; t < 2; t++) {
        CHECK(b.cue.tracks[t].sectors == a.cue.tracks[t].sectors);
        CHECK(b.cue.tracks[t].index1 - (b.cue.tracks[t].index0 >= 0 ? b.cue.tracks[t].index0 : b.cue.tracks[t].index1) ==
              a.cue.tracks[t].index1 - (a.cue.tracks[t].index0 >= 0 ? a.cue.tracks[t].index0 : a.cue.tracks[t].index1));
    }
    CHECK(b.cue.tracks[0].mode == TRACK_MODE1_2048 && b.cue.tracks[1].mode == TRACK_AUDIO && b.cue.tracks[1].is_wave);
    /* same data, sector by sector, and the same data-track hash (what identifies a game) */
    for (int s = 0; s < DATA_SECTORS; s++) {
        CHECK(disc_read_user(&a, 0, s, x) && disc_read_user(&b, 0, s, y));
        CHECK(memcmp(x, y, 2048) == 0);
    }
    CHECK(disc_hash_track(&a, 0, ha, NULL, NULL) && disc_hash_track(&b, 0, hb, NULL, NULL));
    CHECK_STR(ha, hb);
    /* same audio, including the pregap (silence) and past the end */
    for (int s = -PREGAP; s <= AUDIO_SECTORS; s++) {
        CHECK(disc_read_audio(&a, 1, s, x) && disc_read_audio(&b, 1, s, y));
        CHECK(memcmp(x, y, 2352) == 0);
    }
    CHECK(disc_read_audio(&b, 1, 0, y) && y[0] == ((PREGAP * 2352 * 13 + 1) & 0xFF));

    disc_close(&a);
    disc_close(&b);
    remove("t_exp_data.bin"); remove("t_exp_audio.bin"); remove("t_exp.cue");
    remove("t_exp_copy/Test Disc.cue"); remove("t_exp_copy/Test Disc (Track 01).iso"); remove("t_exp_copy/Test Disc (Track 02).wav");
    TEST_DONE();
}
