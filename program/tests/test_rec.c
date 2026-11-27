/* Input recordings: what is written comes back frame for frame, with the clock start. */
#include "saturn/saturn_rec.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *path = "test_rec.rec";
    input_rec r;
    rec_frame a, b;
    int64_t t0 = 0;

    CHECK(rec_create(&r, path, 1700000000LL));
    for (int f = 0; f < 300; f++) {
        memset(&a, 0, sizeof a);
        a.players = (uint8_t)(f < 100 ? 1 : 3);
        a.ports = (uint8_t)(f < 200 ? PORTS_AUTO : PORTS_TAP1);
        for (int i = 0; i < SMPC_SLOTS; i++)
            a.pad[i] = (uint16_t)(0xFFFF ^ (f * 7 + i));
        CHECK(rec_write(&r, &a));
    }
    rec_close(&r);

    CHECK(rec_open(&r, path, &t0));
    CHECK(t0 == 1700000000LL);
    for (int f = 0; f < 300; f++) {
        CHECK(rec_read(&r, &b));
        CHECK(b.players == (f < 100 ? 1 : 3));
        CHECK(b.ports == (f < 200 ? PORTS_AUTO : PORTS_TAP1));
        CHECK(b.pad[0] == (uint16_t)(0xFFFF ^ (f * 7)) && b.pad[11] == (uint16_t)(0xFFFF ^ (f * 7 + 11)));
    }
    CHECK(!rec_read(&r, &b));   /* the end */
    CHECK(r.frames == 300);
    rec_close(&r);

    {   /* not a recording */
        FILE *f = fopen(path, "wb");
        if (f) { fputs("hello", f); fclose(f); }
        CHECK(!rec_open(&r, path, &t0));
    }
    remove(path);
    TEST_DONE();
}
