/* VDP2 software compositor: NBG0-3 (cell and bitmap), VDP1 sprite layer, back screen, colour
 * calculation for NBG layers (ratio and add, per screen or per character), colour offset.
 * Sprite colour calculation too (SPCCCS conditions, CCRSA-D ratios picked by the dot's CC bits).
 * Windows: rectangle windows 0/1 and the sprite window, AND/OR logic, for NBG0-3, sprites and
 * colour calculation.
 * Special priority (SFPRMD) per cell and per dot; the line colour screen as the colour calculation partner (LNCLEN).
 * Not yet: rotation backgrounds (RBG0/1), zoom, line scroll, line windows, shadow, per-dot and
 * colour-MSB colour calculation modes for NBG layers. Double-density interlace shows both fields
 * at once (448 lines; background layers are read line by line, sprites from the matching VDP1 field). */
#include "saturn/saturn.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rg(const saturn *m, uint32_t o) { return (uint16_t)(m->vdp2_regs[o] << 8 | m->vdp2_regs[o + 1]); }
static uint16_t vw(const saturn *m, uint32_t a) { a &= 0x7FFFE; return (uint16_t)(m->vdp2_vram[a] << 8 | m->vdp2_vram[a + 1]); }

/* ---- colour RAM ---------------------------------------------------------------------------------- */

static uint32_t rgb555(uint16_t c)
{
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return 0xFF000000u | (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
}

static uint32_t cram_color(const saturn *m, uint32_t idx)
{
    int mode = (rg(m, 0x0E) >> 12) & 3;
    const uint8_t *c = m->vdp2_cram;
    if (mode == 2 || mode == 3) {
        uint32_t o = (idx & 0x3FF) * 4;
        return 0xFF000000u | (uint32_t)c[o + 3] << 16 | (uint32_t)c[o + 2] << 8 | c[o + 1];
    }
    uint32_t o = (mode == 0 ? (idx & 0x3FF) : (idx & 0x7FF)) * 2;
    return rgb555((uint16_t)(c[o] << 8 | c[o + 1]));
}

/* Bit 15 of a 16-bit CRAM entry (CRAM modes 0 and 1): a colour calculation condition. */
static bool cram_msb(const saturn *m, uint32_t idx)
{
    int mode = (rg(m, 0x0E) >> 12) & 3;
    uint32_t o;
    if (mode >= 2) return (m->vdp2_cram[(idx & 0x3FF) * 4] & 0x80) != 0;
    o = (mode == 0 ? (idx & 0x3FF) : (idx & 0x7FF)) * 2;
    return (m->vdp2_cram[o] & 0x80) != 0;
}

/* ---- scroll layers --------------------------------------------------------------------------------- */

typedef struct {
    bool on, bitmap, two_word, big_char, trans_on;
    int ccn, prio, bmw, bmh;
    uint32_t plane[4], bmbase;
    int pw, ph;               /* plane size in pages */
    int scx, scy;
    int pal_off;              /* CRAM offset in colours */
    uint16_t supp;            /* PNC supplement bits 9:0 */
    bool cnsm;
    int bmpal;
    int bmspr;                /* BMPNA special priority bit for a bitmap layer */
    int sprmode;              /* SFPRMD: 0 the register priority, 1 per cell (SPR bit), 2 per dot (SPR bit and code) */
    uint8_t sfcode;           /* SFCODE A or B (SFSEL): bit n = dots whose low 4 bits are 2n or 2n+1 */
} nbg;

static void setup(const saturn *m, nbg *n, int i)
{
    uint16_t bgon = rg(m, 0x20), chctla = rg(m, 0x28), chctlb = rg(m, 0x2A);
    uint16_t pnc = rg(m, 0x30 + i * 2), plsz = rg(m, 0x3A), mpofn = rg(m, 0x3C);
    memset(n, 0, sizeof *n);
    n->on = (bgon >> i) & 1;
    n->trans_on = (bgon >> (8 + i)) & 1;
    if (i == 0) {
        n->big_char = chctla & 1; n->bitmap = (chctla >> 1) & 1; int bs = (chctla >> 2) & 3; n->ccn = (chctla >> 4) & 7;
        n->bmw = (bs & 2) ? 1024 : 512; n->bmh = (bs & 1) ? 512 : 256;
    } else if (i == 1) {
        n->big_char = (chctla >> 8) & 1; n->bitmap = (chctla >> 9) & 1; int bs = (chctla >> 10) & 3; n->ccn = (chctla >> 12) & 3;
        n->bmw = (bs & 2) ? 1024 : 512; n->bmh = (bs & 1) ? 512 : 256;
    } else if (i == 2) {
        n->big_char = chctlb & 1; n->ccn = (chctlb >> 1) & 1;
    } else {
        n->big_char = (chctlb >> 4) & 1; n->ccn = (chctlb >> 5) & 1;
    }
    n->two_word = !(pnc & 0x8000);
    n->cnsm = (pnc & 0x4000) != 0;
    n->supp = pnc & 0x3FF;
    int sz = (plsz >> (i * 2)) & 3;
    n->pw = (sz & 1) ? 2 : 1;
    n->ph = (sz & 2) ? 2 : 1;
    if (sz == 3) { n->pw = 2; n->ph = 2; }
    uint32_t off = (uint32_t)((mpofn >> (i * 4)) & 7) << 6;
    for (int p = 0; p < 4; p++) {
        uint16_t reg = rg(m, 0x40 + i * 4 + (p >> 1) * 2);   /* MPABNi, MPCDNi: 4 bytes per layer */
        uint32_t v = ((p & 1) ? (reg >> 8) : reg) & 0x3F;
        uint32_t tmp = v | off;
        int deca = n->pw + n->ph - 2;
        uint32_t multi = (uint32_t)(n->pw * n->ph);
        uint32_t page = n->two_word ? (n->big_char ? 0x1000u : 0x4000u) : (n->big_char ? 0x800u : 0x2000u);
        n->plane[p] = (tmp >> deca) * (multi * page);
    }
    n->bmbase = (uint32_t)(mpofn >> (i * 4) & 7) << 17;
    n->bmpal = (rg(m, i == 0 ? 0x2C : 0x2C) >> (i == 0 ? 0 : 8)) & 7;
    n->bmspr = (rg(m, 0x2C) >> (i == 0 ? 5 : 13)) & 1;
    static const uint16_t scr[4][2] = {{0x70, 0x74}, {0x80, 0x84}, {0x90, 0x92}, {0x94, 0x96}};
    n->scx = rg(m, scr[i][0]) & 0x7FF;
    n->scy = rg(m, scr[i][1]) & 0x7FF;
    uint16_t pr = rg(m, i < 2 ? 0xF8 : 0xFA);
    n->prio = (pr >> ((i & 1) * 8)) & 7;
    n->pal_off = (int)((rg(m, 0xE4) >> (i * 4)) & 7) << 8;
    n->sprmode = (rg(m, 0xEA) >> (i * 2)) & 3;
    n->sfcode = (uint8_t)(rg(m, 0x26) >> (((rg(m, 0x24) >> i) & 1) ? 8 : 0));
}

/* Special colour calculation bit of the last cell nbg_pixel() read (pattern name bit 12, or the
 * PNC supplement bit for one-word pattern names). Bitmaps never set it. */
static bool g_cell_cc;
/* Special priority bit (pattern name bit 13, PNC supplement bit 9, or BMPNA's bitmap bit) and the colour
 * code of the last dot nbg_pixel() read, for SFPRMD's per-cell and per-dot modes. */
static bool g_cell_spr;
static uint32_t g_dot;

/* Returns false when the pixel is transparent; *prio_extra unused. */
static bool nbg_pixel(const saturn *m, const nbg *n, int x, int y, uint32_t *col)
{
    int sx = x + n->scx, sy = y + n->scy;
    uint32_t cellbytes;
    uint32_t addr;
    int cx, cy;
    uint32_t dot = 0;
    int ccn = n->ccn;

    g_cell_cc = false;
    g_cell_spr = false;
    if (n->bitmap) {
        sx &= n->bmw - 1; sy &= n->bmh - 1;
        uint32_t pix = (uint32_t)(sy * n->bmw + sx);
        uint32_t pal = (uint32_t)n->bmpal << 8;
        g_cell_spr = (n->bmspr) != 0;
        switch (ccn) {
        case 0: { uint8_t b = m->vdp2_vram[(n->bmbase + pix / 2) & 0x7FFFF]; dot = (pix & 1) ? (b & 15) : (b >> 4);
                  if (!dot && !n->trans_on) { return false; } g_dot = dot; *col = cram_color(m, n->pal_off + (pal & 0x700) + dot); return true; }
        case 1: { dot = m->vdp2_vram[(n->bmbase + pix) & 0x7FFFF];
                  if (!dot && !n->trans_on) { return false; } g_dot = dot; *col = cram_color(m, n->pal_off + (pal & 0x700) + dot); return true; }
        case 2: { dot = vw(m, n->bmbase + pix * 2) & 0x7FF;
                  if (!dot && !n->trans_on) { return false; } *col = cram_color(m, n->pal_off + dot); return true; }
        case 3: { uint16_t w = vw(m, n->bmbase + pix * 2);
                  if (!(w & 0x8000) && !n->trans_on) { return false; } *col = rgb555(w); return true; }
        default: { uint32_t o = n->bmbase + pix * 4; uint16_t hi = vw(m, o), lo = vw(m, o + 2);
                  if (!(hi & 0x8000) && !n->trans_on) return false;
                  *col = 0xFF000000u | (uint32_t)(hi & 0xFF) << 16 | (uint32_t)(lo >> 8) << 8 | (lo & 0xFF);
                  return true; }
        }
    }

    {
        int cw = n->big_char ? 16 : 8;
        int plane_px_w = n->pw * 512, plane_px_h = n->ph * 512;
        sx &= plane_px_w * 2 - 1;
        sy &= plane_px_h * 2 - 1;
        int plane = (sx / plane_px_w) + (sy / plane_px_h) * 2;
        int px = sx % plane_px_w, py = sy % plane_px_h;
        int page = (px / 512) + (py / 512) * n->pw;
        int nx = (px % 512) / cw, ny = (py % 512) / cw;
        int cols = n->big_char ? 32 : 64;
        uint32_t pagebytes = n->two_word ? (uint32_t)(cols * cols * 4) : (uint32_t)(cols * cols * 2);
        uint32_t pn = n->plane[plane] + (uint32_t)page * pagebytes + (uint32_t)(ny * cols + nx) * (n->two_word ? 4u : 2u);
        uint32_t chara, paladdr;
        bool hf = false, vf = false;
        cx = (px % 512) % cw;
        cy = (py % 512) % cw;
        if (!n->two_word) {
            uint16_t t = vw(m, pn);
            g_cell_cc = (n->supp & 0x100) != 0;
            g_cell_spr = (n->supp & 0x200) != 0;
            paladdr = ccn == 0 ? (uint32_t)(((t & 0xF000) >> 12) | ((n->supp & 0xE0) >> 1)) : (uint32_t)((t & 0x7000) >> 8);
            hf = (t & 0x400) != 0; vf = (t & 0x800) != 0;
            if (!n->cnsm) {
                chara = n->big_char ? (uint32_t)(((t & 0x3FF) << 2) | (n->supp & 3) | ((n->supp & 0x1C) << 10))
                                    : (uint32_t)((t & 0x3FF) | ((n->supp & 0x1F) << 10));
            } else {
                hf = vf = false;
                chara = n->big_char ? (uint32_t)(((t & 0xFFF) << 2) | (n->supp & 3) | ((n->supp & 0x10) << 10))
                                    : (uint32_t)((t & 0xFFF) | ((n->supp & 0x1C) << 10));
            }
        } else {
            uint16_t t1 = vw(m, pn), t2 = vw(m, pn + 2);
            g_cell_cc = (t1 & 0x1000) != 0;
            g_cell_spr = (t1 & 0x2000) != 0;
            chara = t2 & 0x7FFF;
            paladdr = ccn == 0 ? (uint32_t)(t1 & 0x7F) : (uint32_t)(t1 & 0x70);
            hf = (t1 & 0x4000) != 0; vf = (t1 & 0x8000) != 0;
        }
        if (hf) cx = cw - 1 - cx;
        if (vf) cy = cw - 1 - cy;
        int cell = 0;
        if (n->big_char) { cell = (cy / 8) * 2 + (cx / 8); cx &= 7; cy &= 7; }
        switch (ccn) { case 0: cellbytes = 32; break; case 1: cellbytes = 64; break; case 2: case 3: cellbytes = 128; break; default: cellbytes = 256; }
        addr = chara * 32u;
        if (n->big_char && !n->two_word) addr = (chara & ~3u) * 32u;
        addr += (uint32_t)cell * cellbytes;
        if (n->big_char && n->two_word) { /* char number points at the top-left 8x8 cell; neighbours follow */ }
        switch (ccn) {
        case 0: { uint8_t b = m->vdp2_vram[(addr + (uint32_t)(cy * 4 + cx / 2)) & 0x7FFFF]; dot = (cx & 1) ? (b & 15) : (b >> 4);
                  if (!dot && !n->trans_on) { return false; } g_dot = dot; *col = cram_color(m, (uint32_t)n->pal_off + (paladdr << 4) + dot); return true; }
        case 1: { dot = m->vdp2_vram[(addr + (uint32_t)(cy * 8 + cx)) & 0x7FFFF];
                  if (!dot && !n->trans_on) { return false; } g_dot = dot; *col = cram_color(m, (uint32_t)n->pal_off + (paladdr << 4) + dot); return true; }
        case 2: { dot = vw(m, addr + (uint32_t)(cy * 16 + cx * 2)) & 0x7FF;
                  if (!dot && !n->trans_on) { return false; } g_dot = dot; *col = cram_color(m, (uint32_t)n->pal_off + dot); return true; }
        case 3: { uint16_t w = vw(m, addr + (uint32_t)(cy * 16 + cx * 2));
                  if (!(w & 0x8000) && !n->trans_on) { return false; } *col = rgb555(w); return true; }
        default: { uint32_t o = addr + (uint32_t)(cy * 32 + cx * 4); uint16_t hi = vw(m, o), lo = vw(m, o + 2);
                  if (!(hi & 0x8000) && !n->trans_on) return false;
                  *col = 0xFF000000u | (uint32_t)(hi & 0xFF) << 16 | (uint32_t)(lo >> 8) << 8 | (lo & 0xFF);
                  return true; }
        }
    }
}

/* The priority of the dot nbg_pixel() just returned: PRINA/PRINB, with its LSB from the special priority
 * function (SFPRMD) when that is on: per cell = the cell's SPR bit; per dot = SPR bit and the dot's colour
 * code in SFCODE. Normal Game stages use the per-cell mode on all four layers (scenery over the bombers). */
static int layer_prio(const nbg *n)
{
    bool lsb;
    switch (n->sprmode) {
    case 1: lsb = g_cell_spr; break;
    case 2: lsb = g_cell_spr && ((n->sfcode >> ((g_dot & 15) >> 1)) & 1); break;
    default: return n->prio;
    }
    return (n->prio & 6) | (lsb ? 1 : 0);
}

/* ---- sprite layer (VDP1 frame buffer) ------------------------------------------------------------ */

/* Sprite colour calculation for the last dot sprite_pixel() returned: whether it qualifies
 * (CCCTL SPCCEN plus the SPCTL SPCCCS condition) and its ratio (CCRSA-D, picked by the dot's CC bits). */
static bool g_spr_cc;
static int g_spr_ratio;

/* The frame buffer row behind display line sy. Double-density interlace (TVMD LSMD = 3) shows 448 lines:
 * the shown frame buffer holds one parity (two lines per row) and the previous field the other. */
static const uint8_t *sprite_row(const saturn *m, int sy, int *row)
{
    const uint8_t *fb = m->vdp1_fb + (size_t)(m->fb_draw ^ 1) * 0x40000;
    int shown = m->fb_field[m->fb_draw ^ 1];
    if (((rg(m, 0x00) >> 6) & 3) != 3) {
        *row = sy;
        return fb;
    }
    *row = sy >> 1;
    if (shown >= 0 && (sy & 1) != shown && m->field_parity == (sy & 1))
        return m->vdp1_field;
    return fb;
}

/* Frame buffer column behind display x: the 8-bit frame buffer (1024 wide, TVMR TVM) maps 1:1 in hi-res;
 * the 16-bit one (512 wide) is shown at half resolution there. */
static int sprite_col(bool b8, bool hires, int sx) { return hires && !b8 ? sx / 2 : sx; }

/* Display x -> frame buffer x when the shown frame buffer was drawn with another widescreen shift than this
 * picture uses (the setting changed while the game was paused, so nothing has been redrawn yet). */
static int sprite_shift(const saturn *m, int sx)
{
    int now = (m->vdp1_regs[1] & 1) ? 0 : m->wide_extra;   /* the shift VDP1 would draw with today (vdp1.c gctx_init) */
    return sx + m->fb_xoff[m->fb_draw ^ 1] - now;
}

static bool sprite_pixel(const saturn *m, int sx, int sy, bool hires, uint32_t *col, int *prio)
{
    bool b8 = (m->vdp1_regs[1] & 1) != 0;
    uint16_t spctl = rg(m, 0xE0);
    int type = spctl & 15;
    uint32_t data;
    int fx = sprite_col(b8, hires, sprite_shift(m, sx));
    const uint8_t *fb = sprite_row(m, sy, &sy);
    if (sy >= 256 || fx < 0) return false;
    if (b8) { if (fx >= 1024) return false; data = fb[(size_t)sy * 1024 + fx]; }
    else { if (fx >= 512) return false; data = (uint32_t)(fb[((size_t)sy * 512 + fx) * 2] << 8 | fb[((size_t)sy * 512 + fx) * 2 + 1]); }
    g_spr_cc = false;
    if (!b8 && (spctl & 0x20) && (data & 0x8000)) {     /* SPCLMD: MSB set = direct RGB sprite */
        *col = rgb555((uint16_t)data);
        *prio = (rg(m, 0xF0) & 7);
        return true;
    }
    int pri_idx, dot, cc_idx = 0;
    if (!b8) {
        /* types 2-7 have a shadow / sprite-window bit (SD) at bit 15 */
        if (type >= 2 && (data & 0x8000) && (spctl & 0x10)) return false;   /* sprite window dot */
        switch (type) {
        case 0: pri_idx = (int)((data >> 14) & 3); cc_idx = (int)((data >> 11) & 7); dot = (int)(data & 0x7FF); break;
        case 1: pri_idx = (int)((data >> 13) & 7); cc_idx = (int)((data >> 11) & 3); dot = (int)(data & 0x7FF); break;
        case 2: pri_idx = (int)((data >> 14) & 1); cc_idx = (int)((data >> 11) & 7); dot = (int)(data & 0x7FF); break;
        case 3: pri_idx = (int)((data >> 13) & 3); cc_idx = (int)((data >> 11) & 3); dot = (int)(data & 0x7FF); break;
        case 4: pri_idx = (int)((data >> 13) & 3); cc_idx = (int)((data >> 10) & 7); dot = (int)(data & 0x3FF); break;
        case 5: pri_idx = (int)((data >> 12) & 7); cc_idx = (int)((data >> 11) & 1); dot = (int)(data & 0x7FF); break;
        case 6: pri_idx = (int)((data >> 12) & 7); cc_idx = (int)((data >> 10) & 3); dot = (int)(data & 0x3FF); break;
        default: pri_idx = (int)((data >> 12) & 7); cc_idx = (int)((data >> 9) & 7); dot = (int)(data & 0x1FF); break;
        }
    } else {
        switch (type) {
        case 8: pri_idx = (int)((data >> 7) & 1); dot = (int)(data & 0x7F); break;
        case 9: pri_idx = (int)((data >> 7) & 1); cc_idx = (int)((data >> 6) & 1); dot = (int)(data & 0x3F); break;
        case 10: pri_idx = (int)((data >> 6) & 3); dot = (int)(data & 0x3F); break;
        case 11: pri_idx = 0; cc_idx = (int)((data >> 6) & 3); dot = (int)(data & 0x3F); break;
        case 12: pri_idx = (int)((data >> 7) & 1); dot = (int)(data & 0xFF); break;
        case 13: pri_idx = (int)((data >> 7) & 1); cc_idx = (int)((data >> 6) & 1); dot = (int)(data & 0xFF); break;
        case 14: pri_idx = (int)((data >> 6) & 3); dot = (int)(data & 0xFF); break;
        default: pri_idx = 0; cc_idx = (int)((data >> 6) & 3); dot = (int)(data & 0xFF); break;
        }
    }
    if (dot == 0) return false;
    if (pri_idx > 7) pri_idx = 7;
    uint16_t pr = rg(m, 0xF0 + (pri_idx >> 1) * 2);
    uint32_t cidx = (uint32_t)(((rg(m, 0xE6) >> 4) & 7) << 8) + (uint32_t)dot;
    *prio = (pr >> ((pri_idx & 1) * 8)) & 7;
    *col = cram_color(m, cidx);
    if (rg(m, 0xEC) & 0x40) {                          /* SPCCEN */
        int ccn = (spctl >> 8) & 7, cond = (spctl >> 12) & 3;
        uint16_t rr = rg(m, 0x100 + (cc_idx >> 1) * 2);
        g_spr_cc = cond == 0 ? *prio <= ccn : cond == 1 ? *prio == ccn : cond == 2 ? *prio >= ccn : cram_msb(m, cidx);
        g_spr_ratio = (rr >> ((cc_idx & 1) * 8)) & 31;
    }
    return true;
}

/* ---- composition --------------------------------------------------------------------------------------- */

static int s9(uint16_t v) { return (v & 0x100) ? (int)(v & 0x1FF) - 512 : (int)(v & 0x1FF); }

static uint32_t offset_color(const saturn *m, uint32_t c)
{
    uint16_t clofen = rg(m, 0x110);
    int sel = (rg(m, 0x112) & 1) ? 1 : 0;       /* backdrop-only handling is approximated by "all" */
    (void)sel;
    if (!(clofen & 0x40) && !(clofen & 0x3F)) return c;
    int r = s9(rg(m, sel ? 0x11A : 0x114)), g = s9(rg(m, sel ? 0x11C : 0x116)), b = s9(rg(m, sel ? 0x11E : 0x118));
    int rr = (int)((c >> 16) & 255) + r, gg = (int)((c >> 8) & 255) + g, bb = (int)(c & 255) + b;
    rr = rr < 0 ? 0 : rr > 255 ? 255 : rr; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
    return 0xFF000000u | (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
}

/* Colour calculation (CCCTL 0xEC, SFCCMD 0xEE, CCRNA/B 0x108/0x10A). Ratio mode mixes the top
 * pixel with the one under it: top * (31 - r) / 32 + under * (r + 1) / 32; add mode saturates.
 * USA battle stages: NBG0 explosion flames are per-character, ratio 8. Normal Game sprites: SPCTL
 * 0x0616 (type 6, condition priority <= 6), CCRSA 0x1300 (ratio 0 or 19 by the dot's CC bits). */
typedef struct {
    bool layer_on[4], add;
    int mode[4], ratio[4];
    uint8_t lncl;             /* LNCLEN: bit 0-3 NBG0-3, 4 RBG0, 5 sprites insert the line colour screen */
    uint32_t lct;             /* line colour table address (LCTA/LCTB), per line when lct_line */
    bool lct_line;
} cc_regs;

static void cc_setup(const saturn *m, cc_regs *cc)
{
    uint16_t ccctl = rg(m, 0xEC), sfccmd = rg(m, 0xEE), rna = rg(m, 0x108), rnb = rg(m, 0x10A);
    cc->add = (ccctl & 0x100) != 0;
    for (int i = 0; i < 4; i++) {
        cc->layer_on[i] = (ccctl >> i) & 1;
        cc->mode[i] = (sfccmd >> (i * 2)) & 3;
        if (cc->mode[i] >= 2) cc->layer_on[i] = false;   /* per-dot / colour-MSB modes: not yet */
    }
    cc->ratio[0] = rna & 31; cc->ratio[1] = (rna >> 8) & 31;
    cc->ratio[2] = rnb & 31; cc->ratio[3] = (rnb >> 8) & 31;
    cc->lncl = (uint8_t)(rg(m, 0xE8) & 0x3F);
    cc->lct = ((uint32_t)(rg(m, 0xA8) & 7) << 16 | rg(m, 0xAA)) * 2;
    cc->lct_line = (rg(m, 0xA8) & 0x8000) != 0;
}

/* The line colour screen on line y: a colour RAM index read from the line colour table (one entry, or one
 * per line). When a layer's LNCLEN bit is set, colour calculation on that layer's dots mixes them with this
 * colour instead of the screen below. Normal Game stage select (after clearing a stage): LNCLEN 0x2F, black
 * line colour, NBG ratio 23: the whole stage dims evenly behind the map; mixing with the layer below made it
 * look see-through (the user's report). */
static uint32_t line_color(const saturn *m, const cc_regs *cc, int y)
{
    return cram_color(m, vw(m, cc->lct + (cc->lct_line ? (uint32_t)y * 2 : 0)) & 0x7FF);
}

static uint32_t color_calc(const cc_regs *cc, uint32_t a, uint32_t b, int r)
{
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 255), cb = (int)((b >> sh) & 255), v;
        v = cc->add ? ca + cb : (ca * (31 - r) + cb * (r + 1)) / 32;
        out |= (uint32_t)(v > 255 ? 255 : v) << sh;
    }
    return out;
}

/* Windows (WPSX0..WPEY1 0xC0-0xCE, WCTLA-D 0xD0-0xD6). A layer's control byte: bit 0/1 window 0
 * area/enable, bit 2/3 window 1, bit 4/5 sprite window, bit 7 AND (else OR). The area bit picks
 * where the layer is hidden: 0 = inside the window, 1 = outside. USA Battle Game setup: NBG2 (bomb
 * pattern) shows only inside window 0, NBG0 (theme park) only outside window 1. */
typedef struct {
    int x0[2], x1[2], y0[2], y1[2];
    uint8_t ctl[6];           /* NBG0-3, sprite, colour calculation */
} win_regs;

static void win_setup(const saturn *m, win_regs *wr, bool hires)
{
    for (int i = 0; i < 2; i++) {
        int sx = rg(m, 0xC0 + i * 8) & 0x3FF, ex = rg(m, 0xC4 + i * 8) & 0x3FF;
        wr->x0[i] = hires ? sx : sx >> 1;
        wr->x1[i] = hires ? ex : ex >> 1;
        wr->y0[i] = rg(m, 0xC2 + i * 8) & 0x1FF;
        wr->y1[i] = rg(m, 0xC6 + i * 8) & 0x1FF;
    }
    wr->ctl[0] = (uint8_t)rg(m, 0xD0); wr->ctl[1] = (uint8_t)(rg(m, 0xD0) >> 8);
    wr->ctl[2] = (uint8_t)rg(m, 0xD2); wr->ctl[3] = (uint8_t)(rg(m, 0xD2) >> 8);
    wr->ctl[4] = (uint8_t)(rg(m, 0xD4) >> 8);
    wr->ctl[5] = (uint8_t)(rg(m, 0xD6) >> 8);
}

/* True when the layer with control byte c is hidden at (x, y). sw = the sprite window covers it. */
static bool win_hidden(const win_regs *wr, uint8_t c, int x, int y, bool sw)
{
    bool any = false, all = true;
    if (!(c & 0x2A))
        return false;
    for (int i = 0; i < 2; i++) {
        if (!(c & (2 << (i * 2)))) continue;
        bool in = x >= wr->x0[i] && x <= wr->x1[i] && y >= wr->y0[i] && y <= wr->y1[i];
        bool hit = (c & (1 << (i * 2))) ? !in : in;
        any |= hit; all &= hit;
    }
    if (c & 0x20) {
        bool hit = (c & 0x10) ? !sw : sw;
        any |= hit; all &= hit;
    }
    return (c & 0x80) ? all : any;
}

/* Sprite window: dots with the SD bit set when SPCTL SPWINEN is on (sprite types 2-7). */
static bool sprite_window(const saturn *m, int sx, int sy, bool hires)
{
    uint16_t spctl = rg(m, 0xE0);
    int fx = sprite_col(false, hires, sprite_shift(m, sx)), type = spctl & 15;
    const uint8_t *fb = sprite_row(m, sy, &sy);
    if (!(spctl & 0x10) || type < 2 || type > 7 || (m->vdp1_regs[1] & 1) || sy >= 256 || fx < 0 || fx >= 512)
        return false;
    return (fb[((size_t)sy * 512 + fx) * 2] & 0x80) != 0;
}

void saturn_render(saturn *m, uint32_t *out, int *w, int *h)
{
    uint16_t tvmd = rg(m, 0x00);
    static const int widths[4] = {320, 352, 640, 704};
    static const int heights[4] = {224, 240, 256, 256};
    int W = widths[tvmd & 3];
    int H = heights[(tvmd >> 4) & 3] * (((tvmd >> 6) & 3) == 3 ? 2 : 1);   /* double-density interlace: 448/480/512 */
    bool hires = (tvmd & 2) != 0;
    bool disp = (tvmd & 0x8000) != 0;
    nbg bg[4];
    cc_regs cc;
    win_regs wr;
    bool show_sprites = true, windows;
    uint16_t bktau = rg(m, 0xAC), bktal = rg(m, 0xAE);
    uint32_t bkaddr = ((uint32_t)(bktau & 7) << 16 | bktal) * 2;
    bool per_line = (bktau & 0x8000) != 0;

    /* widescreen: ext extra pixels left and right. Background layers just draw further out (gx can be
     * negative), the VDP1 frame buffer already holds the picture ext pixels to the right. */
    int ext = (!hires && !(m->vdp1_regs[1] & 1)) ? m->wide_extra : 0;
    int OW = W + 2 * ext;
    m->render_extra = ext;
    *w = OW; *h = H;
    for (int i = 0; i < 4; i++) setup(m, &bg[i], i);
    cc_setup(m, &cc);
    win_setup(m, &wr, hires);
    windows = (wr.ctl[0] | wr.ctl[1] | wr.ctl[2] | wr.ctl[3] | wr.ctl[4] | wr.ctl[5]) & 0x2A;
    {   /* developer: SBPC_LAYERS=hex mask (bit 0-3 NBG0-3, bit 4 sprites) hides layers */
        static int mask = -2;
        if (mask == -2) { const char *e = getenv("SBPC_LAYERS"); mask = e ? (int)strtol(e, NULL, 16) : -1; }
        if (mask >= 0) {
            for (int i = 0; i < 4; i++) if (!((mask >> i) & 1)) bg[i].on = false;
            show_sprites = (mask & 0x10) != 0;
        }
    }

    for (int y = 0; y < H; y++) {
        uint32_t back = rgb555(vw(m, per_line ? bkaddr + (uint32_t)y * 2 : bkaddr));
        for (int x = 0; x < OW; x++) {
            uint32_t col = back;
            int best = 0, gx = x - ext;   /* gx: the game's x; x: the frame buffer's */
            if (!disp && !(tvmd & 0x100)) { out[y * OW + x] = 0xFF000000u; continue; }
            if (!disp) { out[y * OW + x] = offset_color(m, back); continue; }
            /* later (lower-numbered) layers win ties: draw NBG3..0 then the sprite layer with >=.
             * The runner-up is kept for colour calculation. */
            uint32_t second = back;
            int best2 = 0, top = -1, ratio_top = 0;
            bool cc_top = false, sw = windows && sprite_window(m, x, y, hires);
            for (int i = 3; i >= 0; i--) {
                uint32_t c;
                int pr;
                /* special priority can only raise the layer by one (LSB) */
                if (!bg[i].on || bg[i].prio == 0 || (bg[i].prio | (bg[i].sprmode ? 1 : 0)) < best2) continue;
                if (windows && win_hidden(&wr, wr.ctl[i], gx, y, sw)) continue;
                if (!nbg_pixel(m, &bg[i], gx, y, &c)) continue;
                pr = layer_prio(&bg[i]);
                if (pr == 0) continue;
                if (pr >= best) {
                    second = col; best2 = best;
                    best = pr; col = c; top = i;
                    cc_top = cc.layer_on[i] && (cc.mode[i] == 0 || (cc.mode[i] == 1 && g_cell_cc));
                    ratio_top = cc.ratio[i];
                } else {
                    second = c; best2 = pr;
                }
            }
            {
                uint32_t c; int p;
                if (show_sprites && !(windows && win_hidden(&wr, wr.ctl[4], gx, y, sw)) &&
                    sprite_pixel(m, x, y, hires, &c, &p) && p > 0) {
                    if (p >= best) {
                        second = col; best2 = best; best = p; col = c; top = 4;
                        cc_top = g_spr_cc; ratio_top = g_spr_ratio;
                    }
                    else if (p >= best2) { second = c; best2 = p; }
                }
            }
            if (cc_top && top >= 0 && !(windows && win_hidden(&wr, wr.ctl[5], gx, y, sw))) {
                if ((cc.lncl >> (top == 4 ? 5 : top)) & 1)
                    second = line_color(m, &cc, y);
                col = color_calc(&cc, col, second, ratio_top);
            }
            out[y * OW + x] = offset_color(m, col);
        }
    }
}