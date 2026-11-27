/* SCU: interrupt controller, DMA (levels 0-2, direct and indirect), timers. The DSP is a stub. */
#include "core/log.h"
#include "saturn/saturn.h"
#include "sh2/sh2_interp.h"

/* bit -> vector and SH-2 interrupt level */
static const struct { uint8_t vector, level; const char *name; } SRC[16] = {
    {0x40, 0xF, "VBlank-IN"},  {0x41, 0xE, "VBlank-OUT"}, {0x42, 0xD, "HBlank-IN"},
    {0x43, 0xC, "Timer 0"},    {0x44, 0xB, "Timer 1"},    {0x45, 0xA, "DSP end"},
    {0x46, 0x9, "Sound request"}, {0x47, 0x8, "SMPC"},    {0x48, 0x8, "Pad"},
    {0x49, 0x6, "DMA level 2 end"}, {0x4A, 0x6, "DMA level 1 end"}, {0x4B, 0x5, "DMA level 0 end"},
    {0x4C, 0x3, "DMA illegal"}, {0x4D, 0x2, "Sprite draw end"}, {0, 0, NULL}, {0, 0, NULL},
};

void scu_raise(saturn *m, int bit)
{
    m->scu.ist |= 1u << bit;
    m->scu.pending |= 1u << bit;
}

/* Gives the master SH-2 the highest-priority unmasked pending interrupt it will accept. */
void scu_deliver(saturn *m)
{
    sh2_state *s = &m->cpu[0];
    uint32_t ready = m->scu.pending & ~m->scu.ims & 0x3FFF;
    int best = -1;
    if (!ready)
        return;
    for (int b = 0; b < 14; b++)
        if ((ready >> b) & 1 && SRC[b].name && (best < 0 || SRC[b].level > SRC[best].level))
            best = b;
    if (best < 0)
        return;
    if (sh2_interrupt(s, SRC[best].level, SRC[best].vector)) {
        m->scu.pending &= ~(1u << best);
        LOGT(SUB_SCU, "interrupt %s (vector %02X) taken at %08X", SRC[best].name, SRC[best].vector,
             (unsigned)s->pc);
    }
}

/* ---- DMA ------------------------------------------------------------------------------------------ */

static const uint32_t WRITE_ADD[8] = {0, 2, 4, 8, 16, 32, 64, 128};

static void copy_unit(saturn *m, uint32_t *src, uint32_t *dst, uint32_t bytes, uint32_t radd, uint32_t wadd)
{
    sh2_state *s = &m->cpu[0];
    /* transfer in 32-bit units where possible, otherwise bytes */
    for (uint32_t i = 0; i < bytes;) {
        if (!(*src & 3) && !(*dst & 1) && bytes - i >= 4) {
            uint32_t v = sh2_rd32(s, *src);
            if (wadd >= 4 || wadd == 0) sh2_wr32(s, *dst, v);
            else { sh2_wr16(s, *dst, (uint16_t)(v >> 16)); sh2_wr16(s, *dst + wadd, (uint16_t)v); }
            *src += radd ? 4 : 0;
            *dst += wadd >= 4 ? wadd : wadd * 2;
            i += 4;
        } else {
            sh2_wr8(s, *dst, sh2_rd8(s, *src));
            *src += radd ? 1 : 0;
            *dst += 1;
            i += 1;
        }
    }
}

static void run_dma(saturn *m, int level)
{
    uint32_t base = (uint32_t)level * 0x20 / 4;
    uint32_t *r = m->scu.regs;
    uint32_t src = r[base + 0], dst = r[base + 1], count = r[base + 2], add = r[base + 3];
    uint32_t mode = r[base + 5];
    uint32_t radd = (add >> 8) & 1, wadd = WRITE_ADD[add & 7];
    uint32_t mask = level == 0 ? 0xFFFFF : 0xFFF;

    m->scu.dma_count++;
    if (mode & (1u << 24)) { /* indirect: table at dst of (count, write addr, read addr | end bit) */
        uint32_t tbl = dst;
        for (int guard = 0; guard < 4096; guard++) {
            uint32_t c = sh2_rd32(&m->cpu[0], tbl) & mask;
            uint32_t w = sh2_rd32(&m->cpu[0], tbl + 4);
            uint32_t rd = sh2_rd32(&m->cpu[0], tbl + 8);
            uint32_t s0 = rd & 0x07FFFFFF;
            if (c == 0) c = mask + 1;
            copy_unit(m, &s0, &w, c, radd, wadd);
            tbl += 12;
            if (rd & 0x80000000u)
                break;
        }
    } else {
        if ((count & mask) == 0) count = mask + 1;
        copy_unit(m, &src, &dst, count & (mask | (mask + 1)), radd, wadd);
    }
    LOGD(SUB_SCU, "DMA level %d %s: %08X -> %08X, %u bytes", level, (mode & (1u << 24)) ? "indirect" : "direct",
         (unsigned)r[base], (unsigned)r[base + 1], (unsigned)(count & (mask | (mask + 1))));
    scu_raise(m, 11 - level); /* level 0 end = bit 11, 1 = 10, 2 = 9 */
    rt_bump_epoch();
}

void scu_trigger_dma(saturn *m, int factor)
{
    for (int level = 0; level < 3; level++) {
        uint32_t base = (uint32_t)level * 0x20 / 4;
        uint32_t en = m->scu.regs[base + 4], mode = m->scu.regs[base + 5];
        if ((en & 0x100) && (int)(mode & 7) == factor)
            run_dma(m, level);
    }
}

/* ---- registers ---------------------------------------------------------------------------------------- */

uint32_t scu_read(saturn *m, uint32_t off, int size)
{
    uint32_t v;
    off &= 0xFC;
    switch (off) {
    case 0xA0: v = m->scu.ims; break;
    case 0xA4: v = m->scu.ist; break;
    case 0xC8: v = 4; break;          /* version */
    case 0x7C: v = 0; break;          /* DMA status: idle */
    case 0x80: v = 0; break;          /* DSP program control: not running */
    default: v = m->scu.regs[off / 4]; break;
    }
    (void)size;
    return v;
}

void scu_write(saturn *m, uint32_t off, uint32_t v, int size)
{
    off &= 0xFC;
    (void)size;
    switch (off) {
    case 0xA0:
        m->scu.ims = v & 0xBFFF;
        break;
    case 0xA4: /* writing 0 bits clears them */
        m->scu.ist &= v;
        m->scu.pending &= v;
        break;
    case 0x10: case 0x30: case 0x50: /* DxEN */
        m->scu.regs[off / 4] = v;
        if ((v & 0x101) == 0x101 && (m->scu.regs[off / 4 + 1] & 7) == 7) /* enabled + start, start factor "now" */
            run_dma(m, (int)(off / 0x20));
        break;
    default:
        m->scu.regs[off / 4] = v;
        break;
    }
}
