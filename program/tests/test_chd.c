/* CHD support: the Deflate, LZMA and FLAC decoders against reference data (generated with Python's
 * zlib and lzma modules and hand-assembled FLAC frames, see chd_vectors.h), and CHD open errors.
 * A whole real CHD is checked with `sbpc --cli chdverify <file.chd>` (every hunk's CRC-16). */
#include "disc/chd.h"
#include "disc/disc.h"
#include "disc/flac.h"
#include "disc/inflate.h"
#include "disc/lzma.h"
#include "test.h"
#include "chd_vectors.h"

static uint8_t out[8192];

int main(void)
{
    /* Deflate: fixed, dynamic and stored blocks; truncated input fails */
    memset(out, 0xAA, sizeof out);
    CHECK(sbpc_inflate(DEFLATED, sizeof DEFLATED, out, sizeof PAYLOAD) == (long)sizeof PAYLOAD);
    CHECK(memcmp(out, PAYLOAD, sizeof PAYLOAD) == 0);
    CHECK(sbpc_inflate(STORED, sizeof STORED, out, 100) == 100 && memcmp(out, PAYLOAD, 100) == 0);
    CHECK(sbpc_inflate(DEFLATED, sizeof DEFLATED / 2, out, sizeof PAYLOAD) < 0);
    /* a dynamic-Huffman block */
    memset(out, 0xAA, sizeof out);
    CHECK(sbpc_inflate(DYNAMIC, sizeof DYNAMIC, out, sizeof PAYLOAD2) == (long)sizeof PAYLOAD2);
    CHECK(memcmp(out, PAYLOAD2, sizeof PAYLOAD2) == 0);
    /* a smaller output buffer just stops early */
    CHECK(sbpc_inflate(DEFLATED, sizeof DEFLATED, out, 50) == 50 && memcmp(out, PAYLOAD, 50) == 0);

    /* LZMA (raw, lc 3 lp 0 pb 2, as CHD's cdlz) */
    memset(out, 0xAA, sizeof out);
    CHECK(sbpc_lzma_decode(LZMA_RAW, sizeof LZMA_RAW, out, sizeof PAYLOAD, 3, 0, 2) == 0);
    CHECK(memcmp(out, PAYLOAD, sizeof PAYLOAD) == 0);
    CHECK(sbpc_lzma_decode(LZMA_RAW, sizeof LZMA_RAW / 3, out, sizeof PAYLOAD, 3, 0, 2) < 0);

    /* FLAC: three 16-sample stereo frames (independent, left/side, mid/side; constant, verbatim
     * and fixed-predictor subframes), written big-endian as CHD stores CD audio */
    {
        size_t samples = sizeof FLAC_EXPECT / sizeof FLAC_EXPECT[0] / 2;
        long used = sbpc_flac_decode(FLAC_FRAMES, sizeof FLAC_FRAMES, out, samples);
        int bad = 0;
        CHECK(used == (long)sizeof FLAC_FRAMES);
        for (size_t i = 0; i < samples * 2; i++) {
            int16_t v = (int16_t)(out[i * 2] << 8 | out[i * 2 + 1]);
            if (v != FLAC_EXPECT[i]) bad++;
        }
        CHECK(bad == 0);
        CHECK(sbpc_flac_decode(FLAC_FRAMES, 20, out, samples) < 0);
    }

    /* CHD open errors: not a CHD, and a missing file */
    {
        char err[256];
        disc d;
        CHECK(write_file("t_not.chd", "not a chd file, just text....................................................................................................", 120));
        CHECK(chd_open("t_not.chd", err, sizeof err) == NULL);
        CHECK(strstr(err, "not a CHD") != NULL);
        CHECK(!disc_open(&d, "t_not.chd", err, sizeof err));
        CHECK(chd_open("t_missing.chd", err, sizeof err) == NULL);
        CHECK(disc_is_image_name("Game (Japan).CHD") && disc_is_image_name("a.cue") && !disc_is_image_name("a.bin"));
        remove("t_not.chd");
    }
    TEST_DONE();
}
