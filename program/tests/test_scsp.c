/* SCSP slot playback: envelope generator (attack, decay, release, key rate scaling), levels and pan.
 * A constant 16-bit sample is looped in slot 0 and the mixed output is checked. No game data. */
#include "saturn/saturn.h"
#include "test.h"

#define SLOT0 0x000u
#define LINES 263

static void wr(saturn *m, uint32_t word, uint16_t v) { scsp_write16(m, SLOT0 + word * 2, v, 0xFFFF); }

static void run_lines(saturn *m, int n)
{
    for (int i = 0; i < n; i++) scsp_run_line(m, LINES * 60.0);
}

static int last_l(saturn *m) { return m->scsp.out[((m->scsp.out_w - 1) % SCSP_OUT_FRAMES) * 2]; }
static int last_r(saturn *m) { return m->scsp.out[((m->scsp.out_w - 1) % SCSP_OUT_FRAMES) * 2 + 1]; }

/* Slot 0: forward loop over 64 samples of 0x4000 at 0x1000, pitch 1:1, full level, centre. */
static void setup(saturn *m, uint16_t w4, uint16_t w5, uint16_t oct_fns)
{
    for (int i = 0; i < 64; i++) { m->sound_ram[0x1000 + i * 2] = 0x40; m->sound_ram[0x1001 + i * 2] = 0x00; }
    wr(m, 1, 0x1000);              /* SA */
    wr(m, 2, 0);                   /* LSA */
    wr(m, 3, 64);                  /* LEA */
    wr(m, 4, w4);                  /* D2R D1R EGHOLD AR */
    wr(m, 5, w5);                  /* LPSLNK KRS DL RR */
    wr(m, 6, 0);                   /* TL 0 */
    wr(m, 8, oct_fns);
    wr(m, 11, 0xE000);             /* DISDL 7, DIPAN 0 */
}

static void key(saturn *m, bool on) { wr(m, 0, (uint16_t)(0x1000 | (on ? 0x0800 : 0) | 0x0020)); }

int main(void)
{
    saturn *m = saturn_create();
    CHECK(m != NULL);

    /* instant attack (AR 31), no decay (D1R 0), no key rate scaling (KRS 15): full level at once.
     * 0x4000 at 0 dB, halved for mixing headroom = 8192 on both sides */
    setup(m, 0x001F, 0x3C1F, 0);
    key(m, true);
    run_lines(m, 10);
    /* DL 0: the decay-1 target is full level, so it goes straight on to decay 2 (D2R 0 holds) */
    CHECK(m->scsp.slot[0].active && m->scsp.slot[0].eg_state == SCSP_EG_DECAY2);
    CHECK(last_l(m) >= 8180 && last_l(m) <= 8192 && last_r(m) == last_l(m));

    /* TL 0x10 = -6 dB: about half */
    wr(m, 6, 0x10);
    run_lines(m, 2);
    CHECK(last_l(m) >= 4050 && last_l(m) <= 4150);
    wr(m, 6, 0);

    /* pan: DIPAN 0x1F silences the right side, 0x0F the left */
    wr(m, 11, 0xFF00);
    run_lines(m, 2);
    CHECK(last_l(m) >= 8180 && last_r(m) == 0);
    wr(m, 11, 0xEF00);
    run_lines(m, 2);
    CHECK(last_l(m) == 0 && last_r(m) >= 8180);
    wr(m, 11, 0xE000);

    /* release (RR 31, ~3.6 ms): still sounding just after key off, silent and free after ~5 ms */
    key(m, false);
    run_lines(m, 4);
    CHECK(m->scsp.slot[0].active && m->scsp.slot[0].eg_state == SCSP_EG_RELEASE && last_l(m) < 8180);
    run_lines(m, 80);
    CHECK(!m->scsp.slot[0].active && last_l(m) == 0);

    /* slow attack: AR 10 (rate 20, ~380 ms): quiet after 1 ms, full after half a second */
    setup(m, 0x000A, 0x3C1F, 0);
    key(m, true);
    run_lines(m, 16);
    CHECK(m->scsp.slot[0].eg_state == SCSP_EG_ATTACK && last_l(m) < 2000);
    {
        int early = last_l(m);
        run_lines(m, 32 * LINES);
        CHECK(last_l(m) > early && last_l(m) >= 8100);
    }
    key(m, false);
    run_lines(m, 80);

    /* key rate scaling: KRS 0 at OCT +4 adds 4 to the rate, so the same AR rises faster */
    setup(m, 0x000A, 0x001F, 0x2000);         /* KRS 0, OCT 4 */
    key(m, true);
    run_lines(m, 2 * LINES);
    {
        int scaled = m->scsp.slot[0].env;
        key(m, false);
        run_lines(m, 80);
        setup(m, 0x000A, 0x3C1F, 0x2000);     /* KRS 15: no scaling */
        key(m, true);
        run_lines(m, 2 * LINES);
        CHECK(scaled > m->scsp.slot[0].env);
        key(m, false);
        run_lines(m, 80);
    }

    /* decay: D1R 20 down to DL 16 (level 0x1F - 16 = 15 << 5), then D2R 0 holds there */
    setup(m, (uint16_t)(20 << 6 | 0x1F), (uint16_t)(0x3C00 | 16 << 5 | 0x1F), 0);
    key(m, true);
    run_lines(m, 10);
    {
        int full = last_l(m);
        run_lines(m, 60 * LINES);
        CHECK(m->scsp.slot[0].eg_state == SCSP_EG_DECAY2);
        CHECK(last_l(m) < full / 2 && last_l(m) > 0);
        CHECK((m->scsp.slot[0].env >> 5) <= 15);
    }
    key(m, false);
    run_lines(m, 80);

    /* DSP: step 0 reads MIXS 0 (X), times COEF 0 = 0x7FF8 (~1.0, Y), ACC = X * Y >> 12; step 1 writes the
     * shifted ACC to EFREG 0. Slot 0 sends at IMXL 7 (0 dB) with no direct output; EFREG 0 comes back at
     * EFSDL 7, centred. 0x4000 -> MIXS 0x40000 (20 bits) -> 0x400000 (24 bits) -> EFREG ~16380 -> ~8190. */
    scsp_write16(m, 0x700, 0x7FF8, 0xFFFF);                                   /* COEF 0 */
    scsp_write16(m, 0x800 + 2, (uint16_t)(1u << 15 | 1u << 13 | 0x20u << 6), 0xFFFF);   /* XSEL, YSEL=COEF, IRA=MIXS0 */
    scsp_write16(m, 0x800 + 4, 1u << 1, 0xFFFF);                              /* ZERO */
    scsp_write16(m, 0x808 + 4, (uint16_t)(1u << 12 | 1u << 1), 0xFFFF);       /* EWT EFREG 0, ZERO */
    CHECK(m->scsp.dsp_steps == 2);
    setup(m, 0x001F, 0x3C1F, 0);
    wr(m, 10, 0x0007);                                                        /* ISEL 0, IMXL 7 */
    wr(m, 11, 0x00E0);                                                        /* DISDL 0; EFSDL 7, EFPAN 0 */
    key(m, true);
    run_lines(m, 4);
    CHECK(last_l(m) >= 8180 && last_l(m) <= 8192 && last_r(m) == last_l(m));
    wr(m, 10, 0x0001);                                                        /* IMXL 1 = -36 dB */
    run_lines(m, 2);
    CHECK(last_l(m) > 100 && last_l(m) < 200);
    key(m, false);
    run_lines(m, 80);
    for (int i = 0; i < 8; i++) scsp_write16(m, 0x800 + i * 2, 0, 0xFFFF);  /* stop the DSP */
    CHECK(m->scsp.dsp_steps == 0);
    wr(m, 10, 0);

    /* the overlay's sound-effect volume scales the slots */
    setup(m, 0x001F, 0x3C1F, 0);
    m->scsp.gain_sfx = 128;
    key(m, true);
    run_lines(m, 4);
    CHECK(last_l(m) >= 4090 && last_l(m) <= 4096);

    saturn_destroy(m);
    TEST_DONE();
}
