/* SCSP: the sound chip's registers, timers, interrupts and DMA, the 68000 that runs the sound driver
 * out of sound RAM, the 32 sample-playback slots with their envelope generators, and the effects DSP.
 * Not yet: LFO and FM modulation (the USA driver uses neither).
 *
 * Interrupt sources (SCIEB/SCIPD/SCIRE for the 68000, MCIEB/MCIPD/MCIRE for the SH-2), by bit:
 *   0-2 external, 3 MIDI in, 4 DMA end, 5 CPU (set by writing SCIPD/MCIPD bit 5), 6 timer A,
 *   7 timer B, 8 timer C, 9 MIDI out, 10 one sample.
 * The 68000 level of a source is SCILV2:SCILV1:SCILV0 at the source's bit (bits 7 and above share
 * bit 7). The 68000 takes them as autovectored interrupts. */
#include "core/log.h"
#include "saturn/saturn.h"

#include <math.h>
#include <string.h>

#define SAMPLE_RATE 44100.0
#define M68K_HZ 11289600.0

enum {
    R_COMMON = 0x400 / 2,
    R_MIDI_IN = 0x404 / 2,
    R_DMEA_LO = 0x412 / 2, R_DMEA_HI = 0x414 / 2, R_DMA_CTL = 0x416 / 2,
    R_TIMA = 0x418 / 2, R_TIMB = 0x41A / 2, R_TIMC = 0x41C / 2,
    R_SCIEB = 0x41E / 2, R_SCIPD = 0x420 / 2, R_SCIRE = 0x422 / 2,
    R_SCILV0 = 0x424 / 2, R_SCILV1 = 0x426 / 2, R_SCILV2 = 0x428 / 2,
    R_MCIEB = 0x42A / 2, R_MCIPD = 0x42C / 2, R_MCIRE = 0x42E / 2,
};

/* ---- interrupts ---------------------------------------------------------------------------------------- */

static void update_irqs(saturn *m)
{
    scsp_state *p = &m->scsp;
    uint32_t pend = p->scipd & p->r[R_SCIEB] & 0x7FF;
    int level = 0;
    for (int b = 0; b < 11; b++) {
        if (!(pend & (1u << b))) continue;
        {
            int lb = b > 7 ? 7 : b;
            int l = ((p->r[R_SCILV0] >> lb) & 1) | ((p->r[R_SCILV1] >> lb) & 1) << 1 | ((p->r[R_SCILV2] >> lb) & 1) << 2;
            if (l > level) level = l;
        }
    }
    m68k_set_irq(&p->cpu, level);
    {   /* SH-2 side: SCU "sound request" (bit 6), raised when a request appears */
        bool line = (p->mcipd & p->r[R_MCIEB] & 0x7FF) != 0;
        if (line && !p->mc_line)
            scu_raise(m, 6);
        p->mc_line = line;
    }
}

static void raise(saturn *m, uint32_t bits)
{
    m->scsp.scipd |= bits;
    m->scsp.mcipd |= bits;
    update_irqs(m);
}


/* ---- slots (sample playback) ----------------------------------------------------------------------------
 * Per slot (16 words): 0 KYONEX(12) KYONB(11) SBCTL SSCTL LPCTL(6-5) PCM8B(4) SA[19:16]; 1 SA[15:0];
 * 2 LSA; 3 LEA; 4 D2R D1R EGHOLD AR; 5 LPSLNK KRS DL RR; 6 STWINH SDIR TL(7-0); 7 modulation;
 * 8 OCT(14-11) FNS(9-0); 9 LFO; 10 ISEL/IMXL; 11 DISDL(15-13) DIPAN(12-8) EFSDL EFPAN.
 * LFO, modulation and the DSP are not modelled yet. */
static uint16_t sr_(const scsp_state *p, int s, int w) { return p->r[s * 16 + w]; }

/* ---- envelope generator ---------------------------------------------------------------------------------
 * Four states on a 10-bit level (1023 = full): attack rises, decay 1 falls to the decay level DL, decay 2
 * keeps falling, release falls to silence. A state's speed is rate = base + 2 * R (0-63), where base
 * comes from key rate scaling: OCT (signed) + 2 * KRS + FNS bit 9, or 0 when KRS = 15. R = 0 holds.
 * Times for a full sweep, in ms, per rate (the usual SCSP tables). The level maps to -96..0 dB at
 * 3/32 dB per step. The USA driver uses attack, both decays and KRS on every note (see "SCSP use" in
 * the boot report). */
static const double AR_MS[64] = {
    100000, 100000, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860,
    760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 62, 55,
    48, 38, 31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4,
    3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0, 0};
static const double DR_MS[64] = {
    100000, 100000, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700,
    14800, 12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100,
    920, 790, 690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49, 43, 34,
    28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};

#define EG_SHIFT 16
#define EG_MAX (1023 << EG_SHIFT)
static int32_t g_ar_step[64], g_dr_step[64];   /* level change per sample, 10.16 */
static int32_t g_eg_amp[1024];                 /* level -> amplitude, Q16 */
static int32_t g_tl_amp[256];                  /* TL: 0.375 dB per step, Q16 */
static int32_t g_pan_amp[32];                  /* DIPAN/EFPAN low 4 bits: 3 dB per step, 15 = off, Q16 */
static int32_t g_sdl_amp[8];                   /* DISDL/EFSDL: 0 = off, else 6 dB per step below 7, Q16 */

static void scsp_tables(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < 64; i++) {
        g_ar_step[i] = AR_MS[i] <= 0 ? EG_MAX : (int32_t)(1023.0 * 1000.0 / (SAMPLE_RATE * AR_MS[i]) * 65536.0);
        g_dr_step[i] = DR_MS[i] <= 0 ? EG_MAX : (int32_t)(1023.0 * 1000.0 / (SAMPLE_RATE * DR_MS[i]) * 65536.0);
    }
    for (int i = 0; i < 1024; i++)
        g_eg_amp[i] = (int32_t)(pow(10.0, (3.0 * (i - 1023) / 32.0) / 20.0) * 65536.0);
    for (int i = 0; i < 256; i++)
        g_tl_amp[i] = (int32_t)(pow(10.0, -0.375 * i / 20.0) * 65536.0);
    for (int i = 0; i < 32; i++)
        g_pan_amp[i] = (i & 15) == 15 ? 0 : (int32_t)(pow(10.0, -3.0 * (i & 15) / 20.0) * 65536.0);
    for (int i = 0; i < 8; i++)
        g_sdl_amp[i] = i == 0 ? 0 : (int32_t)(pow(10.0, -6.0 * (7 - i) / 20.0) * 65536.0);
}

static int eg_rate(const scsp_state *p, int s, int r)
{
    int krs = (sr_(p, s, 5) >> 10) & 0xF, oct = (sr_(p, s, 8) >> 11) & 0xF, fns = sr_(p, s, 8) & 0x3FF, rate;
    int base = krs == 0xF ? 0 : ((oct ^ 8) - 8) + 2 * krs + ((fns >> 9) & 1);
    if (r == 0) return 0;                     /* hold */
    rate = base + 2 * r;
    return rate < 0 ? 0 : rate > 63 ? 63 : rate;
}

/* ---- effects DSP ---------------------------------------------------------------------------------------
 * Runs once per output sample over microcode steps 0..dsp_steps-1 (MPRO, 0x800-0xBFF, 4 words a step).
 * Inputs: MIXS[16] (slot sends: ISEL/IMXL, 20-bit), EXTS[2] (CD audio), MEMS[32] (ring-buffer reads),
 * TEMP[128] (a ring indexed by the step counter DEC). Coefficients COEF (0x700, 13-bit in bits 15-3)
 * and ring-buffer addresses MADRS (0x780). The ring buffer is RBL (8K << n words) at RBP * 4K words in
 * sound RAM, holding 16-bit words or a 4-bit-exponent float (PACK/UNPACK). Outputs EFREG[16] are mixed
 * with EFSDL/EFPAN of slots 0-15; EXTS with those of slots 16-17. The USA driver loads a 279-word
 * program, sends every effect slot to MIXS 0 at -30/-24 dB and returns EFREG 0-3 (reverb). */
#define R_MPRO (0x800 / 2)
#define R_COEF (0x700 / 2)
#define R_MADRS (0x780 / 2)

static int32_t sext24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }

static uint16_t dsp_pack(int32_t val)
{
    int sign = (val >> 23) & 1, exponent = 0;
    uint32_t temp = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent++;
    }
    if (exponent < 12) val = (int32_t)(((uint32_t)val << exponent) & 0x3FFFFF);
    else val = (int32_t)((uint32_t)val << 11);
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return (uint16_t)val;
}

static int32_t dsp_unpack(uint16_t val)
{
    int sign = (val >> 15) & 1, exponent = (val >> 11) & 0xF;
    int32_t uval = (val & 0x7FF) << 11;
    if (exponent > 11) { exponent = 11; uval |= sign << 22; }
    else uval |= (sign ^ 1) << 22;
    uval |= sign << 23;
    uval = sext24(uval);
    return uval >> exponent;
}

static void dsp_find_end(scsp_state *p)
{
    int i;
    for (i = 127; i >= 0; i--) {
        const uint16_t *ip = &p->r[R_MPRO + i * 4];
        if (ip[0] | ip[1] | ip[2] | ip[3]) break;
    }
    p->dsp_steps = i + 1;
}

static uint16_t ram16(const saturn *m, uint32_t word) { word = (word * 2) & 0x7FFFE; return (uint16_t)(m->sound_ram[word] << 8 | m->sound_ram[word + 1]); }
static void ram16w(saturn *m, uint32_t word, uint16_t v) { word = (word * 2) & 0x7FFFE; m->sound_ram[word] = (uint8_t)(v >> 8); m->sound_ram[word + 1] = (uint8_t)v; }

static void dsp_run(saturn *m)
{
    scsp_state *p = &m->scsp;
    int32_t acc = 0, shifted = 0, x, y, b, inputs, memval = 0, frc_reg = 0, y_reg = 0;
    uint32_t adrs_reg = 0, rbp = (uint32_t)(p->r[0x402 / 2] & 0x7F) << 12;
    uint32_t rbl = (8u * 1024u) << ((p->r[0x402 / 2] >> 7) & 3);
    memset(p->dsp_efreg, 0, sizeof p->dsp_efreg);
    for (int step = 0; step < p->dsp_steps; step++) {
        const uint16_t *ip = &p->r[R_MPRO + step * 4];
        uint32_t tra = (ip[0] >> 8) & 0x7F, twt = (ip[0] >> 7) & 1, twa = ip[0] & 0x7F;
        uint32_t xsel = (ip[1] >> 15) & 1, ysel = (ip[1] >> 13) & 3, ira = (ip[1] >> 6) & 0x3F, iwt = (ip[1] >> 5) & 1, iwa = ip[1] & 0x1F;
        uint32_t table = (ip[2] >> 15) & 1, mwt = (ip[2] >> 14) & 1, mrd = (ip[2] >> 13) & 1, ewt = (ip[2] >> 12) & 1;
        uint32_t ewa = (ip[2] >> 8) & 0xF, adrl = (ip[2] >> 7) & 1, frcl = (ip[2] >> 6) & 1, shift = (ip[2] >> 4) & 3;
        uint32_t yrl = (ip[2] >> 3) & 1, negb = (ip[2] >> 2) & 1, zero = (ip[2] >> 1) & 1, bsel = ip[2] & 1;
        uint32_t nofl = (ip[3] >> 15) & 1, coef = (ip[3] >> 9) & 0x3F, masa = (ip[3] >> 2) & 0x1F, adreb = (ip[3] >> 1) & 1, nxadr = ip[3] & 1;
        int32_t temp_rd = sext24(p->dsp_temp[(tra + p->dsp_dec) & 0x7F]);

        /* input read (and the MEMS write of the value read from memory earlier) */
        if (ira <= 0x1F) inputs = p->dsp_mems[ira];
        else if (ira <= 0x2F) inputs = p->dsp_mixs[ira - 0x20] << 4;      /* MIXS: 20 bits */
        else if (ira <= 0x31) inputs = p->dsp_exts[ira - 0x30] << 8;      /* EXTS: 16 bits */
        else inputs = 0;
        inputs = sext24(inputs);
        if (iwt) {
            p->dsp_mems[iwa] = memval;
            if (ira == iwa) inputs = memval;
        }

        /* operands */
        b = zero ? 0 : bsel ? acc : temp_rd;
        if (!zero && negb) b = -b;
        x = xsel ? inputs : temp_rd;
        if (ysel == 0) y = frc_reg;
        else if (ysel == 1) y = (int16_t)p->r[R_COEF + coef] >> 3;
        else if (ysel == 2) y = (y_reg >> 11) & 0x1FFF;
        else y = (y_reg >> 4) & 0x0FFF;
        if (yrl) y_reg = inputs;

        /* shifter (from last step's accumulator) */
        if (shift == 0 || shift == 1) {
            shifted = shift ? acc * 2 : acc;
            if (shifted > 0x7FFFFF) shifted = 0x7FFFFF;
            if (shifted < -0x800000) shifted = -0x800000;
        } else {
            shifted = sext24(shift == 2 ? acc * 2 : acc);
        }

        /* multiply-accumulate: 24-bit x 13-bit >> 12 */
        y = (int32_t)((uint32_t)y << 19) >> 19;
        acc = (int32_t)(((int64_t)x * (int64_t)y) >> 12) + b;

        if (twt) p->dsp_temp[(twa + p->dsp_dec) & 0x7F] = shifted;
        if (frcl) frc_reg = shift == 3 ? (shifted & 0x0FFF) : ((shifted >> 11) & 0x1FFF);

        if (mrd || mwt) {
            uint32_t addr = p->r[R_MADRS + masa];
            if (!table) addr += p->dsp_dec;
            if (adreb) addr += adrs_reg & 0x0FFF;
            if (nxadr) addr++;
            addr = table ? (addr & 0xFFFF) : (addr & (rbl - 1));
            addr += rbp;
            if (mrd) memval = nofl ? (int32_t)(int16_t)ram16(m, addr) << 8 : dsp_unpack(ram16(m, addr));
            if (mwt) ram16w(m, addr, nofl ? (uint16_t)(shifted >> 8) : dsp_pack(shifted));
        }
        if (adrl) adrs_reg = shift == 3 ? (uint32_t)(shifted >> 12) & 0xFFF : (uint32_t)(inputs >> 16);
        if (ewt) p->dsp_efreg[ewa] = (int16_t)(p->dsp_efreg[ewa] + (shifted >> 8));
    }
    p->dsp_dec--;
    memset(p->dsp_mixs, 0, sizeof p->dsp_mixs);
}

/* One sample of envelope for slot s; returns false when the slot falls silent in release. */
static bool eg_step(scsp_state *p, int s, uint32_t idx)
{
    scsp_slot *v = &p->slot[s];
    uint16_t w4 = sr_(p, s, 4), w5 = sr_(p, s, 5);
    int rate;
    switch (v->eg_state) {
    case SCSP_EG_ATTACK:
        rate = eg_rate(p, s, w4 & 0x1F);
        if (rate) v->eg_level += g_ar_step[rate];
        if (v->eg_level >= EG_MAX || ((w5 & 0x4000) && idx >= sr_(p, s, 2))) {   /* LPSLNK: at the loop start */
            v->eg_level = EG_MAX;
            v->eg_state = SCSP_EG_DECAY1;
        }
        break;
    case SCSP_EG_DECAY1:
        rate = eg_rate(p, s, (w4 >> 6) & 0x1F);
        if (rate) v->eg_level -= g_dr_step[rate];
        if (v->eg_level < 0) v->eg_level = 0;
        if ((v->eg_level >> (EG_SHIFT + 5)) <= 0x1F - ((w5 >> 5) & 0x1F))      /* reached the decay level */
            v->eg_state = SCSP_EG_DECAY2;
        break;
    case SCSP_EG_DECAY2:
        rate = eg_rate(p, s, (w4 >> 11) & 0x1F);
        if (rate) v->eg_level -= g_dr_step[rate];
        if (v->eg_level < 0) v->eg_level = 0;
        break;
    default:
        rate = eg_rate(p, s, w5 & 0x1F);
        v->eg_level -= rate ? g_dr_step[rate] : 0;
        if (v->eg_level <= 0) { v->eg_level = 0; v->env = 0; return false; }
        break;
    }
    v->env = v->eg_level >> EG_SHIFT;
    return true;
}

static void key_on_off(saturn *m)
{
    scsp_state *p = &m->scsp;
    for (int s = 0; s < 32; s++) {
        bool kyonb = (sr_(p, s, 0) & 0x0800) != 0;
        scsp_slot *v = &p->slot[s];
        if (kyonb && (!v->active || v->released)) {
            uint16_t w0 = sr_(p, s, 0), w4 = sr_(p, s, 4), w5 = sr_(p, s, 5), w7 = sr_(p, s, 7), w9 = sr_(p, s, 9);
            p->keyons++;
            p->use_eg[0] += (w4 & 0x1F) != 0x1F;
            p->use_eg[1] += ((w4 >> 6) & 0x1F) != 0;
            p->use_eg[2] += ((w4 >> 11) & 0x1F) != 0;
            p->use_eg[3] += ((w5 >> 10) & 0xF) != 0xF;
            p->use_eg[4] += (w4 & 0x20) != 0;
            p->use_eg[5] += (w5 & 0x4000) != 0;
            p->use_lfo_p += ((w9 >> 5) & 7) != 0;
            p->use_lfo_a += (w9 & 7) != 0;
            p->use_fm += ((w7 >> 12) & 0xF) != 0;
            p->use_efsdl += ((sr_(p, s, 11) >> 5) & 7) != 0;
            p->use_pcm8 += (w0 & 0x10) != 0;
            p->use_loop[(w0 >> 5) & 3]++;
            v->active = true;
            v->released = false;
            v->pos = 0;
            v->dir = 1;
            v->eg_state = SCSP_EG_ATTACK;
            v->eg_level = 0x17F << EG_SHIFT;   /* attacks start around -60 dB, not from silence */
            v->env = 0x17F;
        } else if (!kyonb && v->active && !v->released) {
            v->released = true;
            v->eg_state = SCSP_EG_RELEASE;
        }
    }
}

static int32_t slot_sample(saturn *m, int s)
{
    scsp_state *p = &m->scsp;
    scsp_slot *v = &p->slot[s];
    uint16_t w0 = sr_(p, s, 0);
    uint32_t sa = ((uint32_t)(w0 & 0xF) << 16 | sr_(p, s, 1)) & 0x7FFFF;
    uint32_t lsa = sr_(p, s, 2), lea = sr_(p, s, 3);
    int lpctl = (w0 >> 5) & 3;
    bool pcm8 = (w0 & 0x10) != 0;
    uint32_t idx = (uint32_t)(v->pos >> 32);
    int32_t smp;
    int oct = (sr_(p, s, 8) >> 11) & 0xF;
    uint32_t fns = sr_(p, s, 8) & 0x3FF;
    uint64_t step;

    if (pcm8) smp = (int32_t)(int8_t)m->sound_ram[(sa + idx) & 0x7FFFF] << 8;
    else { uint32_t a = (sa + idx * 2) & 0x7FFFE; smp = (int16_t)(m->sound_ram[a] << 8 | m->sound_ram[a + 1]); }

    if (oct & 8) oct -= 16;
    step = ((uint64_t)(1024 + fns) << 22);                 /* (1024+FNS)/1024 in 32.32 */
    step = oct >= 0 ? step << oct : step >> -oct;
    if (v->dir > 0) v->pos += step; else v->pos -= step;
    idx = (uint32_t)(v->pos >> 32);
    if (v->dir > 0 && idx >= lea) {
        switch (lpctl) {
        case 0: v->active = false; break;
        case 1: v->pos -= (uint64_t)(lea - lsa) << 32; break;
        case 2: v->pos = (uint64_t)lea << 32; v->dir = -1; break;       /* reverse: play LEA -> LSA */
        default: v->pos = (uint64_t)lea << 32; v->dir = -1; break;      /* alternating */
        }
        if (lea <= lsa) v->pos = (uint64_t)lsa << 32;
    } else if (v->dir < 0 && (idx < lsa || (v->pos >> 63))) {
        if (lpctl == 3) { v->pos = (uint64_t)lsa << 32; v->dir = 1; }
        else v->pos = (uint64_t)lea << 32;
    }
    if (!eg_step(p, s, idx))
        v->active = false;
    return smp;
}

static void mix_sample(saturn *m)
{
    scsp_state *p = &m->scsp;
    int32_t l = 0, r = 0;
    uint32_t keyed = 0;
    for (int s = 0; s < 32; s++) {
        int64_t smp;
        int disdl, dipan, tl;
        if (!p->slot[s].active) continue;
        keyed++;
        smp = slot_sample(m, s);
        disdl = (sr_(p, s, 11) >> 13) & 7;
        dipan = (sr_(p, s, 11) >> 8) & 0x1F;
        tl = sr_(p, s, 6) & 0xFF;
        /* envelope and total level, in dB; the overlay's sound-effect volume */
        smp = (smp * g_eg_amp[p->slot[s].env & 1023]) >> 16;
        smp = (smp * g_tl_amp[tl]) >> 16;
        smp = (smp * p->gain_sfx) >> 8;
        {   /* send to the DSP: ISEL picks the MIXS input, IMXL the level (6 dB steps, 0 = off) */
            int imxl = sr_(p, s, 10) & 7;
            if (imxl && p->dsp_steps)
                p->dsp_mixs[(sr_(p, s, 10) >> 3) & 15] += (int32_t)(((smp * g_sdl_amp[imxl]) >> 16) << 4);
        }
        if (!disdl) continue;
        /* direct send level, then pan attenuates one side (DIPAN bit 4 set: the right) */
        smp = (smp * g_sdl_amp[disdl]) >> 16;
        {
            int32_t a = (int32_t)((smp * g_pan_amp[dipan]) >> 16);
            if (dipan & 0x10) { l += (int32_t)smp; r += a; } else { l += a; r += (int32_t)smp; }
        }
    }
    p->keyed = keyed;
    {   /* CD audio comes in through the SCSP's external input (EXTS) */
        cdb_state *cd = &m->cdb;
        p->dsp_exts[0] = p->dsp_exts[1] = 0;
        if (cd->cdda_r != cd->cdda_w) {
            uint32_t at = cd->cdda_r % CDB_CDDA_FRAMES;
            p->dsp_exts[0] = (int16_t)((cd->cdda[at * 2] * p->gain_music) >> 8);
            p->dsp_exts[1] = (int16_t)((cd->cdda[at * 2 + 1] * p->gain_music) >> 8);
            cd->cdda_r++;
        }
    }
    if (p->dsp_steps)
        dsp_run(m);
    {   /* effect returns: EFREG 0-15 with EFSDL/EFPAN of slots 0-15 (DSP off: nothing), EXTS L/R with
         * those of slots 16-17 (the USA driver: full level, hard left / hard right) */
        int32_t el = 0, er = 0;
        for (int i = 0; i < 18; i++) {
            int efsdl = (sr_(p, i, 11) >> 5) & 7, efpan = sr_(p, i, 11) & 0x1F;
            int64_t v;
            int32_t a;
            if (!efsdl) continue;
            if (i < 16) { if (!p->dsp_steps) continue; v = p->dsp_efreg[i]; }
            else v = p->dsp_exts[i - 16];
            v = (v * g_sdl_amp[efsdl]) >> 16;
            a = (int32_t)((v * g_pan_amp[efpan]) >> 16);
            if (i < 16) { if (efpan & 0x10) { el += (int32_t)v; er += a; } else { el += a; er += (int32_t)v; } }
            else if (efpan & 0x10) { l += (int32_t)(v * 2); r += a * 2; }   /* x2: undo the headroom halving below */
            else { l += a * 2; r += (int32_t)(v * 2); }
        }
        l += el;
        r += er;
    }
    l >>= 1;                                               /* headroom: many full-level slots clip */
    r >>= 1;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    p->out[(p->out_w % SCSP_OUT_FRAMES) * 2] = (int16_t)l;
    p->out[(p->out_w % SCSP_OUT_FRAMES) * 2 + 1] = (int16_t)r;
    p->out_w++;
    if (p->out_w - p->out_r > SCSP_OUT_FRAMES) p->out_r = p->out_w - SCSP_OUT_FRAMES;
}

/* Monitor register 0x408: MSLC selects a slot; CA (bits 10-7) is bits 15-12 of its sample index,
 * SGC (6-5) its envelope state, bits 4-0 its envelope level (0 = loudest). */
static uint16_t monitor(const scsp_state *p)
{
    int s = (p->r[0x408 / 2] >> 11) & 0x1F;
    const scsp_slot *v = &p->slot[s];
    uint32_t ca = (uint32_t)(v->pos >> 44) & 0xF;
    uint16_t sgc = v->active ? (v->released ? 3 : 1) : 3;
    uint16_t lvl = v->active ? (uint16_t)(31 - (v->env >> 5)) : 0x1F;
    return (uint16_t)((p->r[0x408 / 2] & 0xF800) | ca << 7 | sgc << 5 | lvl);
}

/* ---- DMA (sound RAM <-> registers) --------------------------------------------------------------------- */

static void dma(saturn *m)
{
    scsp_state *p = &m->scsp;
    uint32_t mem = ((uint32_t)(p->r[R_DMEA_HI] & 0xF000) << 4) | (p->r[R_DMEA_LO] & 0xFFFE);
    uint32_t reg = p->r[R_DMEA_HI] & 0x0FFE;
    uint32_t len = p->r[R_DMA_CTL] & 0x0FFE;
    bool to_mem = (p->r[R_DMA_CTL] & 0x2000) != 0, gate = (p->r[R_DMA_CTL] & 0x4000) != 0;
    for (uint32_t i = 0; i < len; i += 2) {
        uint32_t a = (mem + i) & 0x7FFFE;
        if (to_mem) {
            uint16_t v = gate ? 0 : scsp_read16(m, reg + i);
            m->sound_ram[a] = (uint8_t)(v >> 8);
            m->sound_ram[a + 1] = (uint8_t)v;
        } else {
            uint16_t v = gate ? 0 : (uint16_t)(m->sound_ram[a] << 8 | m->sound_ram[a + 1]);
            scsp_write16(m, reg + i, v, 0xFFFF);
        }
    }
    p->r[R_DMA_CTL] &= (uint16_t)~0x1000;
    p->dma_count++;
    raise(m, 1u << 4);
}

/* ---- registers ----------------------------------------------------------------------------------------- */

uint16_t scsp_read16(saturn *m, uint32_t off)
{
    scsp_state *p = &m->scsp;
    uint32_t r = (off & 0xFFE) / 2;
    switch (r) {
    case R_MIDI_IN: return 0x0100;                         /* MIDI input FIFO empty */
    case 0x408 / 2: return monitor(p);
    case R_TIMA: case R_TIMB: case R_TIMC:
        return (uint16_t)((p->r[r] & 0x0700) | p->timer[r - R_TIMA]);
    case R_SCIPD: return (uint16_t)p->scipd;
    case R_MCIPD: return (uint16_t)p->mcipd;
    case R_SCIRE: case R_MCIRE: return 0;
    default: return r < 0x800 ? p->r[r] : 0;
    }
}

void scsp_write16(saturn *m, uint32_t off, uint16_t v, uint16_t mask)
{
    scsp_state *p = &m->scsp;
    uint32_t r = (off & 0xFFE) / 2;
    uint16_t nv;
    if (r >= 0x800) return;
    nv = (uint16_t)((p->r[r] & ~mask) | (v & mask));
    switch (r) {
    case R_SCIPD:                                          /* only bit 5 (CPU interrupt) can be set */
        if (v & mask & 0x20) { p->scipd |= 0x20; update_irqs(m); }
        return;
    case R_MCIPD:
        if (v & mask & 0x20) { p->mcipd |= 0x20; update_irqs(m); }
        return;
    case R_SCIRE: p->scipd &= ~(uint32_t)(v & mask); update_irqs(m); return;
    case R_MCIRE: p->mcipd &= ~(uint32_t)(v & mask); update_irqs(m); return;
    case R_TIMA: case R_TIMB: case R_TIMC:
        p->r[r] = nv;
        if (mask & 0x00FF) p->timer[r - R_TIMA] = (uint8_t)nv;
        return;
    case R_SCIEB: case R_SCILV0: case R_SCILV1: case R_SCILV2: case R_MCIEB:
        p->r[r] = nv;
        update_irqs(m);
        return;
    case R_DMA_CTL:
        p->r[r] = nv;
        if (nv & 0x1000) dma(m);
        return;
    default:
        p->r[r] = nv;
        if (r >= R_MPRO && r < R_MPRO + 128 * 4)
            dsp_find_end(p);
        if (r < 0x200 && (r & 15) == 0 && (nv & 0x1000)) {   /* KYONEX: apply KYONB of every slot */
            p->r[r] = (uint16_t)(nv & ~0x1000);
            key_on_off(m);
        }
        return;
    }
}

/* ---- the 68000's bus ------------------------------------------------------------------------------------ */

static uint8_t snd_rd8(void *u, uint32_t a)
{
    saturn *m = u;
    if (a < 0x100000) return m->sound_ram[a & 0x7FFFF];
    if (a < 0x101000) { uint16_t v = scsp_read16(m, a & 0xFFF); return (a & 1) ? (uint8_t)v : (uint8_t)(v >> 8); }
    return 0;
}

static uint16_t snd_rd16(void *u, uint32_t a)
{
    saturn *m = u;
    if (a < 0x100000) { a &= 0x7FFFE; return (uint16_t)(m->sound_ram[a] << 8 | m->sound_ram[a + 1]); }
    if (a < 0x101000) return scsp_read16(m, a & 0xFFF);
    return 0;
}

static void snd_wr8(void *u, uint32_t a, uint8_t v)
{
    saturn *m = u;
    if (a < 0x100000) { m->sound_ram[a & 0x7FFFF] = v; return; }
    if (a < 0x101000) scsp_write16(m, a & 0xFFF, (a & 1) ? v : (uint16_t)(v << 8), (a & 1) ? 0x00FF : 0xFF00);
}

static void snd_wr16(void *u, uint32_t a, uint16_t v)
{
    saturn *m = u;
    if (a < 0x100000) { a &= 0x7FFFE; m->sound_ram[a] = (uint8_t)(v >> 8); m->sound_ram[a + 1] = (uint8_t)v; return; }
    if (a < 0x101000) scsp_write16(m, a & 0xFFF, v, 0xFFFF);
}

/* ---- control and timing ---------------------------------------------------------------------------------- */

void scsp_init(saturn *m)
{
    scsp_state *p = &m->scsp;
    scsp_tables();
    memset(p, 0, sizeof *p);
    p->cpu.bus = (m68k_bus){m, snd_rd8, snd_rd16, snd_wr8, snd_wr16};
    for (int i = 0; i < 3; i++) p->timer_div[i] = 1;
    p->gain_sfx = p->gain_music = 256;
}

void scsp_sound_cpu(saturn *m, bool on)
{
    scsp_state *p = &m->scsp;
    if (on && !p->cpu_on) {
        m68k_reset(&p->cpu);
        LOGI(SUB_IO, "sound CPU on: SSP %06X PC %06X", (unsigned)p->cpu.a[7], (unsigned)p->cpu.pc);
    } else if (!on && p->cpu_on) {
        LOGI(SUB_IO, "sound CPU off");
    }
    p->cpu_on = on;
}

static void tick_sample(saturn *m)
{
    scsp_state *p = &m->scsp;
    uint32_t bits = 1u << 10;                              /* one-sample interrupt */
    for (int t = 0; t < 3; t++) {
        if (--p->timer_div[t] == 0) {
            p->timer_div[t] = 1u << ((p->r[R_TIMA + t] >> 8) & 7);
            if (++p->timer[t] == 0)
                bits |= 1u << (6 + t);
        }
    }
    p->samples++;
    mix_sample(m);
    raise(m, bits);
}

void scsp_run_line(saturn *m, double lines_per_sec)
{
    scsp_state *p = &m->scsp;
    int n;
    p->smp_acc += SAMPLE_RATE / lines_per_sec;
    n = (int)p->smp_acc;
    p->smp_acc -= n;
    p->cyc_acc += M68K_HZ / lines_per_sec;
    {
        int cyc = (int)p->cyc_acc;
        p->cyc_acc -= cyc;
        /* interleave the samples with the CPU so timer interrupts land inside the line */
        for (int i = 0; i < n; i++) {
            tick_sample(m);
            if (p->cpu_on)
                m68k_run(&p->cpu, cyc / n + (i == n - 1 ? cyc % n : 0));
        }
        if (n == 0 && p->cpu_on)
            m68k_run(&p->cpu, cyc);
    }
}
