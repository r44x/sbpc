#include "disc/cue.h"
#include "test.h"

int main(void)
{
    cue_sheet s;
    char err[256];
    static unsigned char buf[2352 * 300];

    /* Separate files per track. */
    CHECK(write_file("t_cue_a1.bin", buf, 2352 * 10));
    CHECK(write_file("t_cue_a2.bin", buf, 2352 * 175));
    CHECK(cue_parse("\xEF\xBB\xBF" "FILE \"t_cue_a1.bin\" BINARY\r\n"
                    "  TRACK 01 MODE1/2352\r\n"
                    "    INDEX 01 00:00:00\r\n"
                    "FILE \"t_cue_a2.bin\" BINARY\r\n"
                    "  TRACK 02 AUDIO\r\n"
                    "    INDEX 00 00:00:00\r\n"
                    "    INDEX 01 00:02:00\r\n",
                    ".", &s, err, sizeof err));
    CHECK(s.track_count == 2);
    CHECK(s.tracks[0].mode == TRACK_MODE1_2352 && s.tracks[0].sectors == 10);
    CHECK(s.tracks[1].mode == TRACK_AUDIO && s.tracks[1].index0 == 0 && s.tracks[1].index1 == 150);
    CHECK(s.tracks[1].sectors == 25);
    CHECK(track_user_data_offset(&s.tracks[0]) == 16);

    /* Single file holding several tracks. */
    CHECK(write_file("t_cue_b.bin", buf, 2352 * 300));
    CHECK(cue_parse("FILE t_cue_b.bin BINARY\n"
                    "TRACK 01 MODE2/2352\nINDEX 01 00:00:00\n"
                    "TRACK 02 AUDIO\nINDEX 00 00:01:00\nINDEX 01 00:02:00\n",
                    ".", &s, err, sizeof err));
    CHECK(s.track_count == 2);
    CHECK(s.tracks[0].sectors == 75);
    CHECK(s.tracks[1].index1 == 150 && s.tracks[1].sectors == 150);
    CHECK(track_user_data_offset(&s.tracks[0]) == 24);

    /* Errors */
    CHECK(!cue_parse("FILE \"nope.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", ".", &s, err, sizeof err));
    CHECK(strstr(err, "missing file") != NULL);
    CHECK(write_file("t_cue_c.bin", buf, 2352 * 2 + 1));
    CHECK(!cue_parse("FILE t_cue_c.bin BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", ".", &s, err, sizeof err));
    CHECK(!cue_parse("FILE t_cue_a1.bin BINARY\nTRACK 01 MODE9/1\nINDEX 01 00:00:00\n", ".", &s, err, sizeof err));
    CHECK(!cue_parse("TRACK 01 AUDIO\n", ".", &s, err, sizeof err));

    remove("t_cue_a1.bin"); remove("t_cue_a2.bin"); remove("t_cue_b.bin"); remove("t_cue_c.bin");
    TEST_DONE();
}
