/* Saturn memory map, on-chip SH-2 peripherals, VDP status stubs, scheduler and reporting. */
#include "saturn/saturn.h"
#include "saturn/saturn_bup.h"
#include "core/fs.h"
#include "analysis/boot.h"
#include "core/log.h"
#include "sh2/sh2_interp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1u << 20)

static saturn *M(sh2_state *s) { return (saturn *)s->user; }

/* ---- I/O access log --------------------------------------------------------------------------- */

static const char *region_name(uint32_t a)
{
    if (a >= 0xFFFFFE00u) return "SH-2 on-chip";
    if (a >= 0xC0000000u) return "SH-2 cache RAM";
    if (a >= 0x40000000u) return "SH-2 cache control";
    if (a < 0x00100000u) return "BIOS ROM";
    if (a < 0x00180000u) return "SMPC";
    if (a < 0x00200000u) return "backup RAM";
    if (a < 0x01000000u) return "work RAM low";
    if (a < 0x01800000u) return "MINIT";
    if (a < 0x02000000u) return "SINIT";
    if (a < 0x05800000u) return "A-bus (cartridge)";
    if (a < 0x05900000u) return "CD block";
    if (a < 0x05A00000u) return "A-bus";
    if (a < 0x05B00000u) return "sound RAM";
    if (a < 0x05C00000u) return "SCSP";
    if (a < 0x05C80000u) return "VDP1 VRAM";
    if (a < 0x05D00000u) return "VDP1 framebuffer";
    if (a < 0x05E00000u) return "VDP1 registers";
    if (a < 0x05F00000u) return "VDP2 VRAM";
    if (a < 0x05F80000u) return "VDP2 CRAM";
    if (a < 0x05FE0000u) return "VDP2 registers";
    if (a < 0x06000000u) return "SCU";
    return "work RAM high";
}

void io_note(saturn *m, sh2_state *s, uint32_t addr, int write, int size, uint32_t value)
{
    uint32_t h = (addr * 2654435761u ^ (uint32_t)write) & (IOLOG_SIZE - 1);
    for (int probe = 0; probe < IOLOG_SIZE; probe++) {
        io_stat *e = &m->iolog[(h + (uint32_t)probe) & (IOLOG_SIZE - 1)];
        if (e->count && e->addr == addr && e->write == write) {
            e->count++;
            e->last_value = value;
            return;
        }
        if (!e->count) {
            e->addr = addr;
            e->write = (uint8_t)write;
            e->size = (uint8_t)size;
            e->count = 1;
            e->first_pc = s->pc;
            e->last_value = value;
            LOGD(SUB_IO, "%s %-18s %08X .%c = %08X  (pc %08X, %s)", write ? "W" : "R", region_name(addr),
                 (unsigned)addr, size == 1 ? 'b' : size == 2 ? 'w' : 'l', (unsigned)value, (unsigned)s->pc,
                 s->id ? "slave" : "master");
            return;
        }
    }
}

/* ---- simple big-endian buffers -------------------------------------------------------------------- */

static uint32_t buf_rd(const uint8_t *b, uint32_t off, int size)
{
    switch (size) {
    case 1: return b[off];
    case 2: return (uint32_t)(b[off] << 8 | b[off + 1]);
    default: return (uint32_t)b[off] << 24 | (uint32_t)b[off + 1] << 16 | (uint32_t)b[off + 2] << 8 | b[off + 3];
    }
}

static void buf_wr(uint8_t *b, uint32_t off, uint32_t v, int size)
{
    switch (size) {
    case 1: b[off] = (uint8_t)v; break;
    case 2: b[off] = (uint8_t)(v >> 8); b[off + 1] = (uint8_t)v; break;
    default:
        b[off] = (uint8_t)(v >> 24); b[off + 1] = (uint8_t)(v >> 16);
        b[off + 2] = (uint8_t)(v >> 8); b[off + 3] = (uint8_t)v;
        break;
    }
}

/* ---- SH-2 on-chip peripherals (0xFFFFFE00-0xFFFFFFFF) ----------------------------------------------- */

static uint32_t frt_div(saturn *m, int id)
{
    static const uint32_t d[4] = {8, 32, 128, 8};
    return d[m->onchip[id][0x16] & 3];
}

/* Free-running timer: advances FRC, handles output-compare A (flag + interrupt). Called once per line. */
static void frt_update(saturn *m, sh2_state *s)
{
    int id = s->id;
    uint8_t *r = m->onchip[id];
    uint64_t now = s->total_cycles / frt_div(m, id);
    uint32_t delta = (uint32_t)(now - m->frt_last[id]);
    uint16_t ocra = (uint16_t)(r[0x14] << 8 | r[0x15]);
    uint32_t to_match = (uint16_t)(ocra - m->frc[id]);
    m->frt_last[id] = now;
    if (to_match == 0) to_match = 65536;
    if (delta >= to_match) {
        r[0x11] |= 0x08;                                   /* OCFA */
        m->frc[id] = (r[0x11] & 1) ? (uint16_t)(delta - to_match) : (uint16_t)(m->frc[id] + delta);
    } else {
        m->frc[id] = (uint16_t)(m->frc[id] + delta);
    }
    if ((r[0x11] & 0x80) && (r[0x10] & 0x80)) {            /* ICIE: SINIT */
        int level = (r[0x60] & 0x0F);
        int vector = r[0x66] & 0x7F;
        if (vector == 0) vector = 0x64;
        if (level > 0)
            sh2_interrupt(s, level, vector);
    }
    if ((r[0x11] & 0x08) && (r[0x10] & 0x08)) {            /* OCIAE */
        int level = (r[0x60] & 0x0F);
        int vector = r[0x68] & 0x7F;
        if (vector == 0) vector = 0x65;                    /* FOCV: the BIOS default */
        if (level > 0)
            sh2_interrupt(s, level, vector);
    }
}

/* Watchdog timer in interval mode: WTCNT counts up at phi/CKS; on overflow OVF is set and the ITI
 * interrupt (level IPRA bits 7-4, vector VCRWDT bits 14-8) is requested until OVF is cleared. The CD
 * library runs its file server from this interrupt. Called once per line, like the FRT. */
static const uint32_t wdt_divs[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};

static void wdt_sync(saturn *m, sh2_state *s)
{
    int id = s->id;
    uint8_t *r = m->onchip[id];
    uint64_t now = s->total_cycles / wdt_divs[r[0x80] & 7];
    uint64_t delta = now - m->wdt_last[id];
    m->wdt_last[id] = now;
    if (!(r[0x80] & 0x20))                                 /* TME off: counter stopped */
        return;
    if ((m->wdt_cnt[id] & 0xFF) + delta >= 256)
        r[0x80] |= 0x80;                                   /* OVF */
    m->wdt_cnt[id] = (uint32_t)((m->wdt_cnt[id] + delta) & 0xFF);
}

static void wdt_update(saturn *m, sh2_state *s)
{
    uint8_t *r = m->onchip[s->id];
    wdt_sync(m, s);
    if ((r[0x80] & 0xE0) == 0xA0) {                        /* OVF, interval mode, running */
        int level = r[0xE3] >> 4;
        int vector = r[0xE4] & 0x7F;
        if (vector == 0) vector = 0x68;                    /* the BIOS default for ITI */
        if (level > 0)
            sh2_interrupt(s, level, vector);
    }
}

static uint32_t onchip_read(saturn *m, sh2_state *s, uint32_t a, int size)
{
    uint32_t off = a & 0x1FF;
    uint8_t *r = m->onchip[s->id];
    if (off == 0x012 || off == 0x013) { /* FRC */
        uint16_t frc = (uint16_t)(m->frc[s->id] + (s->total_cycles / frt_div(m, s->id) - m->frt_last[s->id]));
        if (size == 2) return frc;
        return off == 0x012 ? frc >> 8 : frc & 0xFF;
    }
    if (off == 0x080 || off == 0x081 || (off >= 0x088 && off < 0x08A)) {   /* WTCSR / WTCNT (read side) */
        wdt_sync(m, s);
        if (size == 2) return (uint32_t)((r[0x80] | 0x18) << 8 | (m->wdt_cnt[s->id] & 0xFF));
        return (off & 1) ? (m->wdt_cnt[s->id] & 0xFF) : (uint32_t)(r[0x80] | 0x18);
    }
    return buf_rd(r, off & ~(uint32_t)(size - 1), size);
}

/* DIVU: 32/32 starts on a write to DVDNT (0x104), 64/32 on a write to DVDNTL (0x114). */
static void divu(saturn *m, sh2_state *s, int is64)
{
    uint8_t *r = m->onchip[s->id];
    int32_t dvsr = (int32_t)buf_rd(r, 0x100, 4);
    if (!is64) {
        int32_t dvdnt = (int32_t)buf_rd(r, 0x104, 4);
        if (dvsr == 0) {
            buf_wr(r, 0x108, buf_rd(r, 0x108, 4) | 1, 4); /* OVF */
            buf_wr(r, 0x104, dvdnt < 0 ? 0x80000000u : 0x7FFFFFFFu, 4);
        } else if (dvdnt == INT32_MIN && dvsr == -1) {
            buf_wr(r, 0x104, 0x80000000u, 4);
            buf_wr(r, 0x110, 0, 4);
        } else {
            buf_wr(r, 0x104, (uint32_t)(dvdnt / dvsr), 4);
            buf_wr(r, 0x110, (uint32_t)(dvdnt % dvsr), 4);
        }
        buf_wr(r, 0x114, buf_rd(r, 0x104, 4), 4);
    } else {
        int64_t dvdnt = (int64_t)((uint64_t)buf_rd(r, 0x110, 4) << 32 | buf_rd(r, 0x114, 4));
        int64_t q;
        if (dvsr == 0 || (q = dvdnt / dvsr) > INT32_MAX || q < INT32_MIN) {
            buf_wr(r, 0x108, buf_rd(r, 0x108, 4) | 1, 4);
            buf_wr(r, 0x114, dvdnt < 0 ? 0x80000000u : 0x7FFFFFFFu, 4);
        } else {
            buf_wr(r, 0x110, (uint32_t)(int32_t)(dvdnt % dvsr), 4);
            buf_wr(r, 0x114, (uint32_t)(int32_t)q, 4);
        }
        buf_wr(r, 0x104, buf_rd(r, 0x114, 4), 4);
    }
    /* mirrors at +0x20 */
    memcpy(r + 0x120, r + 0x100, 0x18);
}


/* On-chip DMA controller (two channels). Auto-request transfers run to completion as soon as a
 * channel is enabled (CHCR.DE, DMAOR.DME, TE clear); external-request channels are treated the same,
 * which covers the CD block data register. TE is set at the end; with IE the interrupt (level IPRA
 * bits 11-8, vector VCRDMAn) is requested until the program clears TE. */
static void dmac_try(saturn *m, sh2_state *s, int ch)
{
    uint8_t *r = m->onchip[s->id];
    uint32_t base = 0x180 + (uint32_t)ch * 0x10;
    uint32_t chcr = buf_rd(r, base + 0xC, 4), dmaor = buf_rd(r, 0x1B0, 4);
    uint32_t sar, dar, n, unit;
    int dm, sm;
    if (!(chcr & 1) || (chcr & 2) || !(dmaor & 1) || (dmaor & 6))
        return;
    sar = buf_rd(r, base, 4);
    dar = buf_rd(r, base + 4, 4);
    n = buf_rd(r, base + 8, 4) & 0xFFFFFF;
    if (n == 0) n = 0x1000000;
    dm = (int)(chcr >> 14) & 3;
    sm = (int)(chcr >> 12) & 3;
    switch ((chcr >> 10) & 3) {
    case 0: unit = 1; break;
    case 1: unit = 2; break;
    default: unit = 4; break;      /* long, and 16-byte units done as longs */
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = unit == 1 ? sh2_rd8(s, sar) : unit == 2 ? sh2_rd16(s, sar) : sh2_rd32(s, sar);
        if (unit == 1) sh2_wr8(s, dar, (uint8_t)v);
        else if (unit == 2) sh2_wr16(s, dar, (uint16_t)v);
        else sh2_wr32(s, dar, v);
        if (sm == 1) sar += unit; else if (sm == 2) sar -= unit;
        if (dm == 1) dar += unit; else if (dm == 2) dar -= unit;
    }
    buf_wr(r, base, sar, 4);
    buf_wr(r, base + 4, dar, 4);
    buf_wr(r, base + 8, 0, 4);
    buf_wr(r, base + 0xC, chcr | 2, 4);
    rt_bump_epoch();
    s->cycles -= (int32_t)n;
    LOGT(SUB_IO, "%s DMAC%d: %u x %u bytes to %08X", s->id ? "slave" : "master", ch, (unsigned)n,
         (unsigned)unit, (unsigned)(dar - n * unit));
}

static void dmac_update(saturn *m, sh2_state *s)
{
    uint8_t *r = m->onchip[s->id];
    for (int ch = 0; ch < 2; ch++) {
        uint32_t chcr = buf_rd(r, 0x18C + (uint32_t)ch * 0x10, 4);
        if ((chcr & 6) == 6) {
            int level = (r[0xE2] >> 0) & 0x0F;   /* IPRA bits 11-8 */
            int vector = (int)(buf_rd(r, 0x1A0 + (uint32_t)ch * 8, 4) & 0x7F);
            if (level > 0)
                sh2_interrupt(s, level, vector);
        }
    }
}

static void onchip_write(saturn *m, sh2_state *s, uint32_t a, uint32_t v, int size)
{
    uint32_t off = a & 0x1FF;
    uint8_t *r = m->onchip[s->id];
    if (off >= 0x120 && off < 0x138)
        off -= 0x20; /* DIVU mirror */
    if (off == 0x080 && size == 2) {                       /* WDT: A5xx writes WTCSR, 5Axx writes WTCNT */
        wdt_sync(m, s);
        if ((v >> 8) == 0xA5) {
            uint8_t nv = (uint8_t)v;
            r[0x80] = (uint8_t)((r[0x80] & nv & 0x80) | (nv & 0x67));
            m->wdt_last[s->id] = s->total_cycles / wdt_divs[r[0x80] & 7];
        } else if ((v >> 8) == 0x5A) {
            m->wdt_cnt[s->id] = v & 0xFF;
        }
        return;
    }
    if (off == 0x080 || off == 0x081)                      /* byte writes are ignored by the hardware */
        return;
    if (off == 0x011 && size == 1) {                       /* FTCSR: flags clear when written 0 */
        r[0x11] = (uint8_t)((r[0x11] & v & 0x8E) | (v & 1));
        return;
    }
    if ((off == 0x012 || off == 0x013) && size == 1) {     /* FRC byte write */
        uint16_t cur = (uint16_t)(m->frc[s->id] + (s->total_cycles / frt_div(m, s->id) - m->frt_last[s->id]));
        m->frt_last[s->id] = s->total_cycles / frt_div(m, s->id);
        m->frc[s->id] = off == 0x012 ? (uint16_t)((v << 8) | (cur & 0xFF)) : (uint16_t)((cur & 0xFF00) | v);
        return;
    }
    if (off == 0x016 && size == 1) {                       /* TCR: new clock divider, resync */
        m->frc[s->id] = (uint16_t)(m->frc[s->id] + (s->total_cycles / frt_div(m, s->id) - m->frt_last[s->id]));
        r[0x16] = (uint8_t)v;
        m->frt_last[s->id] = s->total_cycles / frt_div(m, s->id);
        return;
    }
    if ((off == 0x18C || off == 0x19C) && size == 4) {   /* CHCR: TE only clears (after being read as 1) */
        uint32_t old = buf_rd(r, off, 4);
        v = (v & ~2u) | (old & v & 2u);
    }
    buf_wr(r, off & ~(uint32_t)(size - 1), v, size);
    if ((off >= 0x180 && off < 0x1A0) || off == 0x1B0) { dmac_try(m, s, 0); dmac_try(m, s, 1); }
    if (off == 0x104 && size == 4) divu(m, s, 0);
    else if (off == 0x114 && size == 4) divu(m, s, 1);
}

/* ---- VDP status stubs ---------------------------------------------------------------------------- */

int saturn_lines(const saturn *m) { return m->pal ? 313 : 263; }
int saturn_fps(const saturn *m) { return m->pal ? 50 : 60; }

/* Master clock / (lines * frames): NTSC 26.85 / 28.64 MHz, PAL 26.69 / 28.44 MHz. */
void saturn_set_clock(saturn *m, bool mode352)
{
    m->cycles_per_line = m->pal ? (mode352 ? 1817 : 1705) : (mode352 ? 1820 : 1706);
}

static uint32_t vdp2_reg_read(saturn *m, uint32_t off, int size)
{
    off &= 0x1FF;
    if (off == 0x004 || off == 0x005) { /* TVSTAT */
        uint16_t st = (uint16_t)((m->vblank ? 8 : 0) | (m->hblank ? 4 : 0) | ((m->frame & 1) ? 2 : 0) | (m->pal ? 1 : 0));
        return size == 1 ? (off == 4 ? st >> 8 : st & 0xFF) : st;
    }
    if (off == 0x008 && size == 2) /* HCNT */
        return 0;
    if (off == 0x00A && size == 2) /* VCNT */
        return (uint32_t)m->line;
    return buf_rd(m->vdp2_regs, off, size);
}

static uint32_t vdp1_reg_read(saturn *m, uint32_t off, int size)
{
    off &= 0x1F;
    if (off == 0x10 || off == 0x11) /* EDSR: drawing always "finished" for now */
        return size == 1 ? (off == 0x10 ? 0 : 3) : 3;
    return buf_rd(m->vdp1_regs, off, size);
}

static void vdp1_reg_write(saturn *m, uint32_t off, uint32_t v, int size)
{
    off &= 0x1F;
    buf_wr(m->vdp1_regs, off, v, size);
    if (off == 0x02 || (off == 0x03 && size == 1)) vdp1_fbcr_write(m);
    if (off == 0x04 && (v & 3) == 1) { /* PTMR = 1: draw now -> report "draw end" straight away */
        vdp1_draw(m);
        scu_raise(m, 13);
    }
}

/* ---- the bus ----------------------------------------------------------------------------------------- */

static uint32_t bus_read(sh2_state *s, uint32_t addr, int size)
{
    saturn *m = M(s);
    uint32_t a, v;
    if (addr >= 0xFFFFFE00u) {
        v = onchip_read(m, s, addr, size);
        io_note(m, s, addr, 0, size, v);
        return v;
    }
    if (addr >= 0xC0000000u)
        return buf_rd(m->cache_data[s->id], addr & 0xFFF, size);
    if (addr >= 0x40000000u)
        return 0; /* cache address array / purge */
    a = addr & 0x1FFFFFFFu;

    if (a >= 0x00100000u && a < 0x00180000u) {          /* SMPC: odd bytes */
        v = size == 1 ? smpc_read(m, a & 0x7F) : smpc_read(m, (a & 0x7F) | 1);
        if (size == 2) v &= 0xFF;
        if (size == 4) v = (smpc_read(m, (a + 1) & 0x7F) << 16) | smpc_read(m, (a + 3) & 0x7F);
    } else if (a >= 0x00180000u && a < 0x00200000u) {   /* backup RAM: odd bytes */
        uint32_t off = (a & 0xFFFF) >> 1;
        v = (a & 1) ? m->backup[off] : 0xFF;
        if (size > 1) v = m->backup[off] | 0xFF00u;
        return v;
    } else if (a >= 0x05800000u && a < 0x05900000u) {   /* CD block */
        if (size == 1) {
            v = cdb_read(m, a & 0xFFFFE);
            v = (a & 1) ? (v & 0xFF) : (v >> 8);
        } else {
            v = cdb_read(m, a & 0xFFFFF);
            if (size == 4) v = (v << 16) | cdb_read(m, (a + 2) & 0xFFFFF);
        }
    } else if (a >= 0x05A00000u && a < 0x05B00000u) {
        return buf_rd(m->sound_ram, a & 0x7FFFF, size);
    } else if (a >= 0x05B00000u && a < 0x05C00000u) {
        if (size == 1) { uint16_t w = scsp_read16(m, a & 0xFFF); v = (a & 1) ? (w & 0xFF) : (w >> 8u); }
        else if (size == 2) v = scsp_read16(m, a & 0xFFE);
        else v = (uint32_t)scsp_read16(m, a & 0xFFC) << 16 | scsp_read16(m, (a & 0xFFC) + 2);
    } else if (a >= 0x05C00000u && a < 0x05C80000u) {
        return buf_rd(m->vdp1_vram, a & 0x7FFFF, size);
    } else if (a >= 0x05C80000u && a < 0x05D00000u) {
        return buf_rd(vdp1_draw_fb(m), a & 0x3FFFF, size);
    } else if (a >= 0x05D00000u && a < 0x05E00000u) {
        v = vdp1_reg_read(m, a, size);
    } else if (a >= 0x05E00000u && a < 0x05F00000u) {
        return buf_rd(m->vdp2_vram, a & 0x7FFFF, size);
    } else if (a >= 0x05F00000u && a < 0x05F80000u) {
        return buf_rd(m->vdp2_cram, a & 0xFFF, size);
    } else if (a >= 0x05F80000u && a < 0x05FE0000u) {
        v = vdp2_reg_read(m, a, size);
    } else if (a >= 0x05FE0000u && a < 0x05FF0000u) {
        v = scu_read(m, a & 0xFF, size);
    } else if (a >= 0x02000000u && a < 0x05000000u) {    /* no cartridge */
        v = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
    } else {
        v = 0;                                           /* BIOS ROM, MINIT/SINIT, unmapped */
    }
    io_note(m, s, a, 0, size, v);
    return v;
}

static void bus_write(sh2_state *s, uint32_t addr, uint32_t v, int size)
{
    saturn *m = M(s);
    uint32_t a;
    if (addr >= 0xFFFFFE00u) {
        io_note(m, s, addr, 1, size, v);
        onchip_write(m, s, addr, v, size);
        return;
    }
    if (addr >= 0xC0000000u) {
        buf_wr(m->cache_data[s->id], addr & 0xFFF, v, size);
        return;
    }
    if (addr >= 0x40000000u)
        return;
    a = addr & 0x1FFFFFFFu;

    if (a >= 0x05A00000u && a < 0x05B00000u) { buf_wr(m->sound_ram, a & 0x7FFFF, v, size); return; }
    if (a >= 0x05C00000u && a < 0x05C80000u) { buf_wr(m->vdp1_vram, a & 0x7FFFF, v, size); return; }
    if (a >= 0x05C80000u && a < 0x05D00000u) { buf_wr(vdp1_draw_fb(m), a & 0x3FFFF, v, size); return; }
    if (a >= 0x05E00000u && a < 0x05F00000u) { buf_wr(m->vdp2_vram, a & 0x7FFFF, v, size); return; }
    if (a >= 0x05F00000u && a < 0x05F80000u) { buf_wr(m->vdp2_cram, a & 0xFFF, v, size); return; }
    if (a >= 0x00180000u && a < 0x00200000u) {
        if (a & 1 || size > 1) {
            uint8_t *b = &m->backup[(a & 0xFFFF) >> 1];
            if (*b != (uint8_t)v) { *b = (uint8_t)v; m->backup_dirty = true; m->backup_quiet = 0; }
        }
        return;
    }

    io_note(m, s, a, 1, size, v);
    if (a >= 0x00100000u && a < 0x00180000u) {
        if (size == 1) smpc_write(m, a & 0x7F, (uint8_t)v);
        else smpc_write(m, (a & 0x7F) | 1, (uint8_t)v);
    } else if (a >= 0x05800000u && a < 0x05900000u) {
        if (size == 1) {
            uint16_t old = cdb_read(m, a & 0xFFFFE);
            cdb_write(m, a & 0xFFFFE, (a & 1) ? (uint16_t)((old & 0xFF00) | (v & 0xFF)) : (uint16_t)((old & 0xFF) | (v << 8)));
        } else if (size == 4) { cdb_write(m, a & 0xFFFFF, (uint16_t)(v >> 16)); cdb_write(m, (a + 2) & 0xFFFFF, (uint16_t)v); }
        else cdb_write(m, a & 0xFFFFF, (uint16_t)v);
    } else if (a >= 0x05B00000u && a < 0x05C00000u) {
        if (size == 1) scsp_write16(m, a & 0xFFF, (a & 1) ? (uint16_t)(v & 0xFF) : (uint16_t)(v << 8), (a & 1) ? 0x00FF : 0xFF00);
        else if (size == 2) scsp_write16(m, a & 0xFFE, (uint16_t)v, 0xFFFF);
        else { scsp_write16(m, a & 0xFFC, (uint16_t)(v >> 16), 0xFFFF); scsp_write16(m, (a & 0xFFC) + 2, (uint16_t)v, 0xFFFF); }
    } else if (a >= 0x05D00000u && a < 0x05E00000u) {
        vdp1_reg_write(m, a, v, size);
    } else if (a >= 0x05F80000u && a < 0x05FE0000u) {
        buf_wr(m->vdp2_regs, a & 0x1FF & ~(uint32_t)(size - 1), v, size);
    } else if (a >= 0x05FE0000u && a < 0x05FF0000u) {
        scu_write(m, a & 0xFF, v, size);
    } else if (a >= 0x01000000u && a < 0x01800000u) {
        LOGD(SUB_IO, "MINIT (master -> slave interrupt) from %08X", (unsigned)s->pc);
        if (m->slave_on) {            /* the slave BIOS idles until MINIT, then runs the vector-0x94 handler */
            sh2_state *sl = &m->cpu[1];
            m->onchip[1][0x11] |= 0x80;   /* slave FRT input capture flag (games poll it) */
            sl->sleeping = 0;
            sl->sr &= ~0xF0u;
            sh2_interrupt(sl, 15, 0x94);
        }
    } else if (a >= 0x01800000u && a < 0x02000000u) {
        LOGD(SUB_IO, "SINIT (slave -> master interrupt) from %08X", (unsigned)s->pc);
        m->onchip[0][0x11] |= 0x80;   /* master FRT input capture flag */
    }
}

static uint8_t rd8(sh2_state *s, uint32_t a) { return (uint8_t)bus_read(s, a, 1); }
static uint16_t rd16(sh2_state *s, uint32_t a) { return (uint16_t)bus_read(s, a & ~1u, 2); }
static uint32_t rd32(sh2_state *s, uint32_t a) { return bus_read(s, a & ~3u, 4); }
static void wr8(sh2_state *s, uint32_t a, uint8_t v) { bus_write(s, a, v, 1); }
static void wr16(sh2_state *s, uint32_t a, uint16_t v) { bus_write(s, a & ~1u, v, 2); }
static void wr32(sh2_state *s, uint32_t a, uint32_t v) { bus_write(s, a & ~3u, v, 4); }
static const sh2_bus BUS = {rd8, rd16, rd32, wr8, wr16, wr32};

/* ---- lifetime ------------------------------------------------------------------------------------------- */

static int hle_hook(sh2_state *s) { return bios_hle(M(s), s); }

saturn *saturn_create(void)
{
    saturn *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->wram_l = calloc(1, MB);
    m->wram_h = calloc(1, MB);
    m->vdp1_vram = calloc(1, 512 * 1024);
    m->vdp1_fb = calloc(1, 512 * 1024);
    m->vdp1_field = calloc(1, 256 * 1024);
    m->fb_field[0] = m->fb_field[1] = m->field_parity = -1;
    m->rtc_start = (int64_t)time(NULL);
    m->vdp2_vram = calloc(1, 512 * 1024);
    m->vdp2_cram = calloc(1, 4096);
    m->sound_ram = calloc(1, 512 * 1024);
    m->backup = calloc(1, 32 * 1024);
    if (!m->wram_l || !m->wram_h || !m->vdp1_vram || !m->vdp1_fb || !m->vdp1_field || !m->vdp2_vram || !m->vdp2_cram ||
        !m->sound_ram || !m->backup) {
        saturn_destroy(m);
        return NULL;
    }
    bup_format(m->backup);   /* the BIOS formats a blank internal memory at power-on */
    rt_dispatch_init(&m->dispatch);
    m->dispatch.hle = hle_hook;
    for (int c = 0; c < 2; c++) {
        sh2_state *s = &m->cpu[c];
        sh2_reset_state(s);
        s->id = c;
        s->user = m;
        s->bus = BUS;
        s->dispatch = &m->dispatch;
        s->page[0x002] = s->page[0x003] = m->wram_l;           /* low work RAM (+ mirror) */
        for (uint32_t p = 0x060; p < 0x080; p++)
            s->page[p] = m->wram_h;                            /* high work RAM and mirrors */
    }
    m->cycles_per_line = 1706; /* 26.8 MHz / (263 * 59.94) */
    memset(m->smpc.pad, 0xFF, sizeof m->smpc.pad);
    m->smpc.port_mode[0] = SMPC_PORT_PAD;     /* one pad on port 1 until the front end sets a layout */
    cdb_init(m);
    scsp_init(m);
    return m;
}

void saturn_destroy(saturn *m)
{
    if (!m)
        return;
    saturn_backup_flush(m);
    free(m->wram_l); free(m->wram_h); free(m->vdp1_vram); free(m->vdp1_fb); free(m->vdp1_field);
    free(m->vdp2_vram); free(m->vdp2_cram); free(m->sound_ram); free(m->backup);
    rt_dispatch_free(&m->dispatch);
    free(m);
}

bool saturn_boot(saturn *m, iso_fs *fs, char *err, size_t errlen)
{
    mem_image im;
    boot_info bi;
    sh2_state *ms = &m->cpu[0];
    m->fs = fs;
    img_init(&im);
    if (!boot_load(fs, &im, &bi, err, errlen)) {
        img_free(&im);
        return false;
    }
    /* copy the IP and the 1st read file into work RAM where the BIOS would have put them */
    for (int r = 0; r < im.count; r++)
        for (uint32_t i = 0; i < im.regions[r].size; i++)
            sh2_wr8(ms, im.regions[r].base + i, im.regions[r].bytes[i]);
    img_free(&im);
    m->pal = m->version && m->version->pal;   /* Europe: 50 Hz */
    saturn_set_clock(m, false);

    bios_install(m);
    sh2_reset_state(ms);
    ms->vbr = 0x06000000u;
    ms->sr = 0x0F0;  /* interrupts masked; the game enables them */
    ms->r[15] = bi.header.master_stack ? bi.header.master_stack : 0x06002000u;
    ms->pc = bi.first_addr;
    ms->pr = BIOS_TRAP_SLOT; /* returning from the game lands in a BIOS trap */
    LOGI(SUB_BIOS, "boot: %s at %08X, master stack %08X", bi.first_file, (unsigned)ms->pc, (unsigned)ms->r[15]);
    return true;
}

/* ---- scheduler -------------------------------------------------------------------------------------------- */

static void sample_pc(saturn *m, sh2_state *s)
{
    m->recent_pc[s->id][m->recent_pos[s->id]++ & 31] = s->pc;
}

static void run_cpu(saturn *m, sh2_state *s, int cycles)
{
    if (s->sleeping)
        return;
    s->cycles += cycles;
    rt_run(s);
    sample_pc(m, s);
}

/* Diagnostics: VDP2 features the renderer doesn't draw yet, checked once per frame, so a long
 * scripted run shows which ones the game needs (and from which frame). See saturn_report. */
static const char *const FEATURE_NAMES[] = {
    "RBG0/RBG1 rotation", "line windows", "line/vertical-cell scroll", "NBG zoom", "mosaic", "shadow",
    "sprite shadow (normal/MSB)", "colour calc per-dot/MSB", "line colour screen", "extended colour calc",
    "VDP1 non-replace colour modes"};
#define FEATURE_COUNT ((int)(sizeof FEATURE_NAMES / sizeof FEATURE_NAMES[0]))

static void vdp2_feature_survey(saturn *m)
{
#define R(o) ((uint16_t)(m->vdp2_regs[o] << 8 | m->vdp2_regs[(o) + 1]))
    static uint64_t vdp1_cc_last;
    uint16_t bgon = R(0x20), ccctl = R(0xEC), sfccmd = R(0xEE);
    bool cc_dot = false, on[16];
    uint64_t vdp1_cc = 0;
    for (int i = 0; i < 4; i++)
        cc_dot |= ((ccctl >> i) & 1) && ((sfccmd >> (i * 2)) & 3) >= 2 && ((bgon >> i) & 1);
    for (int i = 1; i < 8; i++) vdp1_cc += m->vdp1_ccmode[i];
    memset(on, 0, sizeof on);
    on[0] = (bgon & 0x30) != 0;
    on[1] = ((R(0xD8) | R(0xDC)) & 0x8000) != 0 && ((R(0xD0) | R(0xD2) | R(0xD4) | R(0xD6)) & 0x0A0A) != 0;
    on[2] = R(0x9A) != 0;
    on[3] = (R(0x98) & 0x0303) != 0;
    on[4] = (R(0x22) & 0x1F) != 0;
    on[5] = (R(0xE2) & 0x13F) != 0;
    on[6] = (R(0xE0) & 0xF) >= 2 && (R(0xE0) & 0xF) <= 7 && !(R(0xE0) & 0x10) && (R(0xE2) & 0x100) != 0;
    on[7] = cc_dot;
    on[8] = (ccctl & 0x20) != 0 && R(0xE8) != 0;
    on[9] = (ccctl & 0x400) != 0;
    on[10] = vdp1_cc != vdp1_cc_last;
    vdp1_cc_last = vdp1_cc;
    if (!(R(0x00) & 0x8000))
        return;   /* display off: nothing is shown */
    for (int i = 0; i < FEATURE_COUNT; i++)
        if (on[i]) {
            if (!m->feat_first[i]) m->feat_first[i] = m->frame + 1;
            m->feat_frames[i]++;
        }
#undef R
}

bool saturn_backup_attach(saturn *m, const char *path, char *err, size_t errlen)
{
    static uint8_t buf[2 * BUP_SIZE];
    int64_t size = fs_size(path);
    snprintf(m->backup_path, sizeof m->backup_path, "%s", path);
    if (size < 0) {
        /* Blank and formatted, but not written: the file appears only once the game saves something.
           (Saturn Bomberman has no save feature; it only looks for its file.) */
        bup_format(m->backup);
        m->backup_dirty = false;
        LOGI(SUB_BIOS, "backup RAM: blank; saved to %s only if the game writes to it", path);
        return true;
    }
    if (size == BUP_SIZE || size == 2 * BUP_SIZE) {
        FILE *f = fs_open(path, "rb");
        bool ok = f && fread(buf, 1, (size_t)size, f) == (size_t)size;
        if (f) fclose(f);
        if (ok) {
            for (int i = 0; i < BUP_SIZE; i++)    /* 64 KB images keep the data in the odd bytes */
                m->backup[i] = size == BUP_SIZE ? buf[i] : buf[i * 2 + 1];
            if (!bup_is_formatted(m->backup)) {
                LOGW(SUB_BIOS, "backup RAM: %s isn't formatted; starting blank", path);
                bup_format(m->backup);
                m->backup_dirty = true;
            }
            LOGI(SUB_BIOS, "backup RAM: loaded %s (%d of %d blocks free)", path, bup_free_blocks(m->backup), BUP_BLOCKS);
            return true;
        }
    }
    snprintf(err, errlen, "can't use the save file %.400s (expected 32 KB)", path);
    m->backup_path[0] = 0;   /* leave a file we don't understand alone */
    return false;
}

void saturn_backup_flush(saturn *m)
{
    char tmp[sizeof m->backup_path + 8];
    FILE *f;
    if (!m->backup_dirty || !m->backup_path[0])
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp", m->backup_path);
    if (fs_dirname(tmp, m->backup_path) && tmp[0])
        fs_mkdirs(tmp);                       /* the saves folder is made on the first real save */
    snprintf(tmp, sizeof tmp, "%s.tmp", m->backup_path);
    f = fs_open(tmp, "wb");
    if (!f || fwrite(m->backup, 1, BUP_SIZE, f) != BUP_SIZE) {
        if (f) fclose(f);
        LOGE(SUB_BIOS, "backup RAM: can't write %s", tmp);
        return;
    }
    if (fclose(f) != 0 || !fs_rename(tmp, m->backup_path)) {
        LOGE(SUB_BIOS, "backup RAM: can't replace %s", m->backup_path);
        return;
    }
    m->backup_dirty = false;
    LOGI(SUB_BIOS, "backup RAM saved to %s", m->backup_path);
}

void saturn_run_frame(saturn *m)
{
    /* Save half a second after the last change, so a burst of writes becomes one file write. */
    if (m->backup_dirty && m->backup_path[0] && ++m->backup_quiet >= 30)
        saturn_backup_flush(m);
    int lines = saturn_lines(m);
    for (m->line = 0; m->line < lines; m->line++) {
        if (m->line == 0) {
            m->vblank = false;
            cdb_tick(m);
            vdp1_frame_end(m);
            if ((m->vdp1_regs[5] & 3) == 2)
                vdp1_draw(m);          /* PTMR = 2: draw automatically at the start of each frame */
            scu_raise(m, 1);           /* VBlank-OUT */
            scu_trigger_dma(m, 1);
        }
        if (m->line == 224) {
            vdp2_feature_survey(m);
            m->vblank = true;
            scu_raise(m, 0);           /* VBlank-IN */
            scu_trigger_dma(m, 0);
            if ((m->vdp1_regs[5] & 3) == 2)
                scu_raise(m, 13);      /* VDP1 auto-draw "ends" at once for now */
        }
        m->hblank = true;
        scu_raise(m, 2);               /* HBlank-IN */
        {   /* SCU timer 0: compare against the line counter */
            uint32_t t1md = m->scu.regs[0x98 / 4];
            if ((t1md & 1) && (uint32_t)m->line == (m->scu.regs[0x90 / 4] & 0x3FF))
                scu_raise(m, 3);
        }
        scu_deliver(m);
        m->hblank = false;

        run_cpu(m, &m->cpu[0], m->cycles_per_line);
        scsp_run_line(m, (double)lines * saturn_fps(m));
        frt_update(m, &m->cpu[0]);
        wdt_update(m, &m->cpu[0]);
        dmac_update(m, &m->cpu[0]);
        if (m->slave_on) {
            run_cpu(m, &m->cpu[1], m->cycles_per_line);
            frt_update(m, &m->cpu[1]);
            wdt_update(m, &m->cpu[1]);
            dmac_update(m, &m->cpu[1]);
        }
        if (m->npc_samples < 1024 && (m->line & 7) == 0)
            m->pc_samples[m->npc_samples++] = m->cpu[0].pc;
        else if ((m->line & 7) == 0)
            m->pc_samples[(m->frame * 33 + (uint64_t)m->line) % 1024] = m->cpu[0].pc;
    }
    m->frame++;
    rt_bump_epoch();   /* recompiled code is re-checked against memory every frame */
}

/* ---- report --------------------------------------------------------------------------------------------- */

static int cmp_io(const void *x, const void *y)
{
    const io_stat *a = x, *b = y;
    if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
    return a->write - b->write;
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

void saturn_report(saturn *m, FILE *f)
{
    sh2_state *ms = &m->cpu[0];
    io_stat *sorted;
    int n = 0;
    const char *last_region = "";

    fprintf(f, "Video: TVMD %04X BGON %04X CHCTLA %04X CHCTLB %04X RAMCTL %04X SPCTL %04X PRINA %04X BKTA %04X%04X\n",
            (m->vdp2_regs[0] << 8) | m->vdp2_regs[1], (m->vdp2_regs[0x20] << 8) | m->vdp2_regs[0x21],
            (m->vdp2_regs[0x28] << 8) | m->vdp2_regs[0x29], (m->vdp2_regs[0x2A] << 8) | m->vdp2_regs[0x2B],
            (m->vdp2_regs[0x0E] << 8) | m->vdp2_regs[0x0F], (m->vdp2_regs[0xE0] << 8) | m->vdp2_regs[0xE1],
            (m->vdp2_regs[0xF8] << 8) | m->vdp2_regs[0xF9], (m->vdp2_regs[0xAC] << 8) | m->vdp2_regs[0xAD],
            (m->vdp2_regs[0xAE] << 8) | m->vdp2_regs[0xAF]);
    for (int v = 0; v < 128; v++)
        if (m->bios.uint_handler[v])
            fprintf(f, "Interrupt handler: vector %02X -> %08X\n", v, (unsigned)m->bios.uint_handler[v]);
    {
        long nz2 = 0, nz1 = 0, nzc = 0;
        uint32_t h1 = 2166136261u, h2 = 2166136261u;
        for (int i = 0; i < 0x80000; i++) {
            nz2 += m->vdp2_vram[i] != 0; nz1 += m->vdp1_vram[i] != 0;
            h1 = (h1 ^ m->vdp1_vram[i]) * 16777619u; h2 = (h2 ^ m->vdp2_vram[i]) * 16777619u;
        }
        fprintf(f, "Video: VRAM hashes VDP1 %08X VDP2 %08X\n", (unsigned)h1, (unsigned)h2);
        if (getenv("SBPC_DUMP_VDP2")) {   /* developer: VRAM + CRAM + registers to a file */
            FILE *d = fopen(getenv("SBPC_DUMP_VDP2"), "wb");
            if (d) { fwrite(m->vdp2_vram, 1, 0x80000, d); fwrite(m->vdp2_cram, 1, 0x1000, d); fwrite(m->vdp2_regs, 1, 0x200, d); fclose(d); }
        }
        fprintf(f, "VDP2 registers:");
        for (int i = 0; i < 0x120; i += 2) {
            if (i % 32 == 0) fprintf(f, "\n  %03X:", i);
            fprintf(f, " %04X", (unsigned)(m->vdp2_regs[i] << 8 | m->vdp2_regs[i + 1]));
        }
        fprintf(f, "\n");
        for (int i = 0; i < 0x1000; i++) nzc += m->vdp2_cram[i] != 0;
        fprintf(f, "Video: non-zero bytes VDP2 VRAM %ld, CRAM %ld, VDP1 VRAM %ld; VDP1 commands last draw %d; "
                   "VDP1 TVMR %04X FBCR %04X PTMR %04X\n\n", nz2, nzc, nz1, m->vdp1_cmds,
                (m->vdp1_regs[0] << 8) | m->vdp1_regs[1], (m->vdp1_regs[2] << 8) | m->vdp1_regs[3],
                (m->vdp1_regs[4] << 8) | m->vdp1_regs[5]);
        fprintf(f, "VDP1 colour modes drawn (replace, shadow, half-lum, half-trans, gouraud, -, gouraud+half-lum, "
                   "gouraud+half-trans): %llu %llu %llu %llu %llu %llu %llu %llu; mesh %llu\n\n",
                (unsigned long long)m->vdp1_ccmode[0], (unsigned long long)m->vdp1_ccmode[1],
                (unsigned long long)m->vdp1_ccmode[2], (unsigned long long)m->vdp1_ccmode[3],
                (unsigned long long)m->vdp1_ccmode[4], (unsigned long long)m->vdp1_ccmode[5],
                (unsigned long long)m->vdp1_ccmode[6], (unsigned long long)m->vdp1_ccmode[7],
                (unsigned long long)m->vdp1_mesh);
        {   /* shown frame buffer: type 2-7 sprite dots with the SD bit (shadow/window) and normal-shadow codes */
            const uint8_t *fb = m->vdp1_fb + (size_t)(m->fb_draw ^ 1) * 0x40000;
            long sd = 0, nshadow = 0, nonzero = 0;
            for (int i = 0; i < 512 * 256; i++) {
                uint16_t d = (uint16_t)(fb[i * 2] << 8 | fb[i * 2 + 1]);
                nonzero += d != 0;
                sd += (d & 0x8000) != 0;
                nshadow += (d & 0x3FF) == 0x3FE || (d & 0x7FF) == 0x7FE;
            }
            fprintf(f, "VDP1 shown frame: %ld dots, %ld with bit 15, %ld normal-shadow codes\n", nonzero, sd, nshadow);
        }
        fprintf(f, "Features not drawn yet, as enabled by the game:");
        {
            bool any = false;
            for (int i = 0; i < FEATURE_COUNT; i++)
                if (m->feat_first[i]) {
                    fprintf(f, "\n  %-32s first at frame %llu, on for %llu frames", FEATURE_NAMES[i],
                            (unsigned long long)(m->feat_first[i] - 1), (unsigned long long)m->feat_frames[i]);
                    any = true;
                }
            fprintf(f, "%s\n\n", any ? "" : " none");
        }
    }

    fprintf(f, "Frames: %llu   master cycles: %llu   slave: %s\n", (unsigned long long)m->frame,
            (unsigned long long)ms->total_cycles, m->slave_on ? "running" : "off");
    fprintf(f, "Interpreted instructions: %llu   recompiled entries: %llu   code-check mismatches: %llu\n",
            (unsigned long long)m->dispatch.interp_steps, (unsigned long long)m->dispatch.native_calls,
            (unsigned long long)m->dispatch.code_mismatches);
    if (m->slave_on)
        fprintf(f, "Slave now: pc %08X pr %08X sr %03X r15 %08X%s\n", (unsigned)m->cpu[1].pc, (unsigned)m->cpu[1].pr,
                (unsigned)(m->cpu[1].sr & 0xFFF), (unsigned)m->cpu[1].r[15], m->cpu[1].sleeping ? " (sleeping)" : "");
    fprintf(f, "Master now: pc %08X pr %08X sr %03X r15 %08X%s\n", (unsigned)ms->pc, (unsigned)ms->pr,
            (unsigned)ms->sr, (unsigned)ms->r[15], ms->sleeping ? " (sleeping)" : "");

    {   const scsp_state *p = &m->scsp;
        fprintf(f, "Sound CPU: %s  pc %06X sr %04X a7 %06X  cycles %llu  illegal %u%s\n", p->cpu_on ? "on" : "off",
                (unsigned)p->cpu.pc, (unsigned)p->cpu.sr, (unsigned)p->cpu.a[7],
                (unsigned long long)p->cpu.total_cycles, (unsigned)p->cpu.illegal_count, p->cpu.stopped ? " (STOP)" : "");
        fprintf(f, "SCSP: SCIEB %03X SCIPD %03X MCIEB %03X MCIPD %03X  TIMA %04X TIMB %04X TIMC %04X  "
                   "levels %02X/%02X/%02X  DMA %u  keyed slots %u\n",
                (unsigned)p->r[0x41E / 2], (unsigned)p->scipd & 0x7FF, (unsigned)p->r[0x42A / 2], (unsigned)p->mcipd & 0x7FF,
                (unsigned)p->r[0x418 / 2], (unsigned)p->r[0x41A / 2], (unsigned)p->r[0x41C / 2],
                (unsigned)p->r[0x424 / 2] & 0xFF, (unsigned)p->r[0x426 / 2] & 0xFF, (unsigned)p->r[0x428 / 2] & 0xFF,
                (unsigned)p->dma_count, (unsigned)p->keyed);
        {
            int mpro = 0;
            for (int i = 0x800 / 2; i < 0xC00 / 2; i++) mpro += p->r[i] != 0;   /* DSP program words */
            fprintf(f, "SCSP effect returns (slot: EFSDL/EFPAN):");
            for (int k = 0; k < 18; k++)
                if ((p->r[k * 16 + 11] >> 5) & 7) fprintf(f, " %d:%d/%02X", k, (p->r[k * 16 + 11] >> 5) & 7, p->r[k * 16 + 11] & 0x1F);
            fprintf(f, "\nSCSP DSP sends (slot: ISEL/IMXL):");
            for (int k = 0; k < 32; k++)
                if (p->r[k * 16 + 10] & 7) fprintf(f, " %d:%d/%d", k, (p->r[k * 16 + 10] >> 3) & 15, p->r[k * 16 + 10] & 7);
            fprintf(f, "\nSCSP RBP/RBL %04X, COEF0-7:", p->r[0x402 / 2]);
            for (int k = 0; k < 8; k++) fprintf(f, " %04X", p->r[0x700 / 2 + k]);
            fprintf(f, "\n");
            fprintf(f, "SCSP use: %llu key-ons; attack<31 %llu, D1R %llu, D2R %llu, KRS %llu, EGHOLD %llu, LPSLNK %llu; "
                       "pitch LFO %llu, amp LFO %llu, FM %llu, effect return %llu, 8-bit %llu; loops off/fwd/rev/alt "
                       "%llu/%llu/%llu/%llu; DSP program words %d\n",
                    (unsigned long long)p->keyons, (unsigned long long)p->use_eg[0], (unsigned long long)p->use_eg[1],
                    (unsigned long long)p->use_eg[2], (unsigned long long)p->use_eg[3], (unsigned long long)p->use_eg[4],
                    (unsigned long long)p->use_eg[5], (unsigned long long)p->use_lfo_p, (unsigned long long)p->use_lfo_a,
                    (unsigned long long)p->use_fm, (unsigned long long)p->use_efsdl, (unsigned long long)p->use_pcm8,
                    (unsigned long long)p->use_loop[0], (unsigned long long)p->use_loop[1], (unsigned long long)p->use_loop[2],
                    (unsigned long long)p->use_loop[3], mpro);
        }
        for (int k = 0; k < 32; k++) {
            const uint16_t *sr = &p->r[k * 16];
            if (!p->slot[k].active) continue;
            fprintf(f, "  slot %2d: SA %05X LSA %04X LEA %04X ctl %04X OCT/FNS %04X TL %02X DISDL/PAN %04X EG %04X/%04X env %d%s\n",
                    k, (unsigned)((sr[0] & 0xF) << 16 | sr[1]), sr[2], sr[3], sr[0], sr[8], sr[6] & 0xFF, sr[11], sr[4], sr[5],
                    p->slot[k].env, p->slot[k].released ? " released" : "");
        }
    }
    /* where the master spends its time */
    {
        uint32_t tmp[1024];
        int k = m->npc_samples;
        memcpy(tmp, m->pc_samples, (size_t)k * sizeof *tmp);
        qsort(tmp, (size_t)k, sizeof *tmp, cmp_u32);
        fprintf(f, "\nMost sampled master PCs:\n");
        for (int shown = 0; shown < 8; shown++) {
            int best = 0, best_at = -1;
            for (int i = 0; i < k;) {
                int j = i;
                while (j < k && tmp[j] == tmp[i]) j++;
                if (j - i > best && tmp[i] != 0xFFFFFFFFu) { best = j - i; best_at = i; }
                i = j;
            }
            if (best_at < 0) break;
            fprintf(f, "  %08X  %d%%\n", (unsigned)tmp[best_at], 100 * best / (k ? k : 1));
            for (int i = best_at; i < best_at + best; i++) tmp[i] = 0xFFFFFFFFu;
        }
    }

    fprintf(f, "\nBIOS calls by slot:\n");
    for (int i = 0; i < 0x400 / 4; i++)
        if (m->bios.calls[i])
            fprintf(f, "  06000%03X  %d\n", (unsigned)(i * 4), m->bios.calls[i]);
    if (m->bios.rom_calls)
        fprintf(f, "  calls into BIOS ROM: %d\n", m->bios.rom_calls);
    for (int v = 0; v < 256; v++)
        if (m->bios.unhandled[v])
            fprintf(f, "  unhandled exception/interrupt vector 0x%02X: %d\n", v, m->bios.unhandled[v]);

    fprintf(f, "\nSMPC commands:");
    for (int i = 0; i < 32; i++)
        if (m->smpc.commands[i]) fprintf(f, " %02X x%d", i, m->smpc.commands[i]);
    fprintf(f, "\nCD block commands:");
    for (int i = 0; i < 256; i++)
        if (m->cdb.command_count[i]) fprintf(f, " %02X x%d", i, m->cdb.command_count[i]);
    fprintf(f, "\nSCU: IMS %08X IST %08X, DMA transfers %u\n", (unsigned)m->scu.ims, (unsigned)m->scu.ist,
            (unsigned)m->scu.dma_count);

    sorted = malloc(sizeof m->iolog);
    if (sorted) {
        for (int i = 0; i < IOLOG_SIZE; i++)
            if (m->iolog[i].count) sorted[n++] = m->iolog[i];
        qsort(sorted, (size_t)n, sizeof *sorted, cmp_io);
        fprintf(f, "\nRegister accesses (%d distinct):\n", n);
        for (int i = 0; i < n; i++) {
            const char *rg = region_name(sorted[i].addr);
            if (strcmp(rg, last_region) != 0) {
                fprintf(f, "  [%s]\n", rg);
                last_region = rg;
            }
            fprintf(f, "    %c %08X .%c x%-7u last %08X  first pc %08X\n", sorted[i].write ? 'W' : 'R',
                    (unsigned)sorted[i].addr, sorted[i].size == 1 ? 'b' : sorted[i].size == 2 ? 'w' : 'l',
                    (unsigned)sorted[i].count, (unsigned)sorted[i].last_value, (unsigned)sorted[i].first_pc);
        }
        free(sorted);
    }
    fprintf(f, "\nLast master PCs (per line):");
    for (int i = 0; i < 32; i++)
        fprintf(f, "%s%08X", i % 8 ? " " : "\n  ", (unsigned)m->recent_pc[0][(m->recent_pos[0] + i) & 31]);
    fprintf(f, "\n");
}
