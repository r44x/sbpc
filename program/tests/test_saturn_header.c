#include "disc/disc.h"
#include "disc/saturn_header.h"
#include "fake_disc.h"

int main(void)
{
    disc d;
    char err[256];
    uint8_t sec[2048];
    saturn_header h;

    CHECK(make_fake_disc("t_hdr", "TEST TITLE", "T-00001", 20, 1));
    CHECK(disc_open(&d, "t_hdr.cue", err, sizeof err));
    CHECK(d.cue.track_count == 2);
    CHECK(disc_first_data_track(&d) == 0);
    CHECK(disc_read_user(&d, 0, 0, sec));
    CHECK(saturn_header_parse(sec, &h));
    CHECK_STR(h.product, "T-00001");
    CHECK_STR(h.version, "V1.000");
    CHECK_STR(h.title, "TEST TITLE");
    CHECK_STR(h.areas, "J");
    CHECK(h.first_read_addr == 0x06010000u);

    CHECK(disc_read_user(&d, 0, 19, sec));
    CHECK(sec[5] == ((19 * 31 + 5) & 0xFF));
    CHECK(!disc_read_user(&d, 0, 20, sec)); /* past end */
    CHECK(!disc_read_user(&d, 1, 0, sec));  /* audio track */

    memset(sec, 0, sizeof sec);
    CHECK(!saturn_header_parse(sec, &h));

    disc_close(&d);
    remove("t_hdr.bin"); remove("t_hdr.cue");
    TEST_DONE();
}
