/* VDP1: command-list interpreter drawing into the sprite frame buffer (software).
 * Supported: normal/scaled/distorted sprites, polygons, polylines, lines, clipping, local
 * coordinates, flips, all texture colour modes, frame-buffer change/erase.
 * Double-density interlace (FBCR DIE/DIL): a frame draws only the lines of one parity, at row y/2.
 * Not yet: gouraud shading, shadow / half-luminance / half-transparent, mesh, end-code line stop.
 * Those draw as plain replacement. */
#include "core/log.h"
#include "saturn/saturn.h"

#include <stdlib.h>
#include <string.h>

static uint16_t r16(const uint8_t *b, uint32_t o) { return (uint16_t)(b[o] << 8 | b[o + 1]); }

uint8_t *vdp1_draw_fb(saturn *m) { return m->vdp1_fb + (size_t)m->fb_draw * 0x40000; }
uint8_t *vdp1_display_fb(saturn *m) { return m->vdp1_fb + (size_t)(m->fb_draw ^ 1) * 0x40000; }

static bool fb_8bit(saturn *m) { return (r16(m->vdp1_regs, 0) & 1) != 0; }

/* ---- frame buffer change / erase ------------------------------------------------------------ */

static void erase_draw_fb(saturn *m)
{
    uint8_t *fb = vdp1_draw_fb(m);
    uint16_t ewdr = r16(m->vdp1_regs, 6), ewlr = r16(m->vdp1_regs, 8), ewrr = r16(m->vdp1_regs, 10);
    int x1 = ((ewlr >> 9) & 0x3F) * 8, y1 = ewlr & 0x1FF;
    int x2 = ((ewrr >> 9) & 0x7F) * 8, y2 = ewrr & 0x1FF;
    bool b8 = fb_8bit(m);
    int stride = b8 ? 1024 : 512;
    if (m->wide_extra && !b8) {          /* widescreen: the picture sits wide_extra to the right; clear the sides too */
        x1 = 0;
        x2 += 2 * m->wide_extra;
    }
    if (y2 > 255) y2 = 255;
    if (x2 > stride) x2 = stride;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x < x2; x++) {
            uint8_t *p = fb + ((size_t)y * stride + x) * (b8 ? 1 : 2);
            if (b8) *p = (x & 1) ? (uint8_t)ewdr : (uint8_t)(ewdr >> 8);
            else { p[0] = (uint8_t)(ewdr >> 8); p[1] = (uint8_t)ewdr; }
        }
}

void vdp1_fbcr_write(saturn *m)
{
    uint16_t fbcr = r16(m->vdp1_regs, 2);
    if ((fbcr & 2) && !(fbcr & 1))
        m->fb_erase_pending = true;
}

/* Before a swap: the frame leaving the display is the other field of an interlaced picture. Keep its
 * lines so VDP2 can show both fields (an interlaced TV shows the previous field's lines too). */
static void keep_field(saturn *m)
{
    int shown = m->fb_draw ^ 1;
    m->field_parity = m->fb_field[shown];
    if (m->field_parity >= 0)
        memcpy(m->vdp1_field, m->vdp1_fb + (size_t)shown * 0x40000, 0x40000);
}

void vdp1_frame_end(saturn *m)
{
    uint16_t fbcr = r16(m->vdp1_regs, 2), tvmr = r16(m->vdp1_regs, 0);
    bool fcm = (fbcr & 2) != 0, fct = (fbcr & 1) != 0;
    if (!fcm) {                         /* 1-cycle mode: swap and erase every frame */
        keep_field(m);
        m->fb_draw ^= 1;
        erase_draw_fb(m);
    } else if (fct) {                   /* manual change */
        keep_field(m);
        m->fb_draw ^= 1;
        if (m->fb_erase_pending || (tvmr & 8))
            erase_draw_fb(m);
        m->fb_erase_pending = false;
        m->vdp1_regs[3] = (uint8_t)(m->vdp1_regs[3] & 0xFE);
    } else if (m->fb_erase_pending) {   /* manual erase */
        erase_draw_fb(m);
        m->fb_erase_pending = false;
    }
}

/* ---- drawing ---------------------------------------------------------------------------------- */

typedef struct {
    saturn *m;
    int lx, ly;                         /* local coordinates */
    int sx2, sy2;                       /* system clip */
    int ux1, uy1, ux2, uy2;             /* user clip */
    bool uclip_on, uclip_out;
    bool b8;
    int dil;                            /* double interlace: the line parity drawn (FBCR DIL), -1 = off */
    int xoff;                           /* widescreen: drawing offset into the frame buffer */
    uint8_t *fb;                        /* where to draw */
    const uint8_t *vram;                /* textures from here */
    bool stats;                         /* count commands in the machine's diagnostics */
    /* current command */
    uint16_t pmod, colr;
    uint32_t src;                       /* texture address in VDP1 VRAM */
    int tw, th, cmode;
    bool spd, ecd;
} gctx;

static void put(gctx *g, int x, int y, uint16_t val)
{
    /* widescreen: a full-width system clip is widened by xoff on each side, so sprites can be drawn
     * into the side areas (the game itself still thinks in 320/352 wide coordinates) */
    int wide = g->xoff && g->sx2 >= 319 ? g->xoff : 0;
    if (x < -wide || y < 0 || x > g->sx2 + wide || y > g->sy2)
        return;
    if (g->uclip_on) {
        bool in = x >= g->ux1 && x <= g->ux2 && y >= g->uy1 && y <= g->uy2;
        if (in == g->uclip_out)
            return;
    }
    if (g->dil >= 0) {                  /* double interlace: this field's lines only, two lines per row */
        if ((y & 1) != g->dil)
            return;
        y >>= 1;
    }
    x += g->xoff;
    uint8_t *fb = g->fb;
    if (g->b8) {
        if (x < 1024 && y < 256) fb[(size_t)y * 1024 + x] = (uint8_t)val;
    } else if (x < 512 && y < 256) {
        uint8_t *p = fb + ((size_t)y * 512 + x) * 2;
        p[0] = (uint8_t)(val >> 8);
        p[1] = (uint8_t)val;
    }
}

static int iabs(int v) { return v < 0 ? -v : v; }
static int imax(int a, int b) { return a > b ? a : b; }

/* Texel (u,v) -> colour value for the frame buffer; returns false if transparent. */
static bool texel(gctx *g, int u, int v, uint16_t *out)
{
    const uint8_t *vr = g->vram;
    uint32_t mask = 0x7FFFF;
    uint32_t a;
    uint32_t dot;
    switch (g->cmode) {
    case 0: case 1:                      /* 16 colours */
        a = (g->src + (uint32_t)(v * (g->tw / 2)) + (uint32_t)(u / 2)) & mask;
        dot = (u & 1) ? (vr[a] & 0xF) : (vr[a] >> 4);
        if (dot == 0 && !g->spd) return false;
        if (dot == 0xF && !g->ecd) return false;
        if (g->cmode == 0) *out = (uint16_t)((g->colr & 0xFFF0) | dot);
        else {
            uint32_t t = ((uint32_t)g->colr * 8 + dot * 2) & mask;
            *out = r16(vr, t);
        }
        return true;
    case 2: case 3: case 4:              /* 64 / 128 / 256 colours */
        a = (g->src + (uint32_t)(v * g->tw) + (uint32_t)u) & mask;
        dot = vr[a];
        if (dot == 0 && !g->spd) return false;
        if (dot == 0xFF && !g->ecd) return false;
        if (g->cmode == 2) *out = (uint16_t)((g->colr & 0xFFC0) | (dot & 0x3F));
        else if (g->cmode == 3) *out = (uint16_t)((g->colr & 0xFF80) | (dot & 0x7F));
        else *out = (uint16_t)((g->colr & 0xFF00) | dot);
        return true;
    default:                             /* RGB */
        a = (g->src + (uint32_t)(v * g->tw * 2) + (uint32_t)(u * 2)) & mask;
        dot = r16(vr, a);
        if (dot == 0 && !g->spd) return false;
        *out = (uint16_t)dot;
        return true;
    }
}

typedef struct { int x, y; } pt;

static void draw_line(gctx *g, pt a, pt b, uint16_t color)
{
    int n = imax(iabs(b.x - a.x), iabs(b.y - a.y));
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (b.x - a.x) * i / n : a.x;
        int y = n ? a.y + (b.y - a.y) * i / n : a.y;
        put(g, x, y, color);
    }
}

/* Textured (tex != 0) or flat quad with corners A B C D in order. */
static void draw_quad(gctx *g, const pt q[4], bool tex, bool hflip, bool vflip)
{
    int n = imax(imax(iabs(q[3].x - q[0].x), iabs(q[3].y - q[0].y)),
                 imax(iabs(q[2].x - q[1].x), iabs(q[2].y - q[1].y)));
    for (int i = 0; i <= n; i++) {
        pt l = {n ? q[0].x + (q[3].x - q[0].x) * i / n : q[0].x, n ? q[0].y + (q[3].y - q[0].y) * i / n : q[0].y};
        pt r = {n ? q[1].x + (q[2].x - q[1].x) * i / n : q[1].x, n ? q[1].y + (q[2].y - q[1].y) * i / n : q[1].y};
        int mlen = imax(iabs(r.x - l.x), iabs(r.y - l.y));
        int v = tex ? (i * g->th) / (n + 1) : 0;
        if (tex && vflip) v = g->th - 1 - v;
        for (int j = 0; j <= mlen; j++) {
            int x = mlen ? l.x + (r.x - l.x) * j / mlen : l.x;
            int y = mlen ? l.y + (r.y - l.y) * j / mlen : l.y;
            uint16_t c;
            if (tex) {
                int u = (j * g->tw) / (mlen + 1);
                if (hflip) u = g->tw - 1 - u;
                if (!texel(g, u, v, &c)) continue;
            } else {
                c = g->colr;
            }
            put(g, x, y, c);
        }
    }
}

/* One command (32 bytes at c): draws, or sets clipping / local coordinates. */
static void draw_command(gctx *g, const uint8_t *c)
{
    uint16_t ctrl = r16(c, 0);
    int cmd = ctrl & 0xF;
    int xa = (int16_t)r16(c, 0xC), ya = (int16_t)r16(c, 0xE);
    int xb = (int16_t)r16(c, 0x10), yb = (int16_t)r16(c, 0x12);
    int xc = (int16_t)r16(c, 0x14), yc = (int16_t)r16(c, 0x16);
    int xd = (int16_t)r16(c, 0x18), yd = (int16_t)r16(c, 0x1A);
    bool hf = (ctrl & 0x10) != 0, vf = (ctrl & 0x20) != 0;
    pt q[4];
    g->pmod = r16(c, 4);
    g->colr = r16(c, 6);
    g->src = (uint32_t)r16(c, 8) * 8;
    g->tw = ((r16(c, 0xA) >> 8) & 0x3F) * 8;
    g->th = r16(c, 0xA) & 0xFF;
    g->cmode = (g->pmod >> 3) & 7;
    g->spd = (g->pmod & 0x40) != 0;
    g->ecd = (g->pmod & 0x80) != 0;
    /* CMDPMOD: bit 10 = user clip enable, bit 9 = clip outside */
    g->uclip_on = (g->pmod & 0x400) != 0;
    g->uclip_out = (g->pmod & 0x200) != 0;
    if (g->stats) {
        g->m->vdp1_cmds++;
        if (cmd <= 6 && cmd != 3) { g->m->vdp1_ccmode[g->pmod & 7]++; if (g->pmod & 0x100) g->m->vdp1_mesh++; }
    }
    switch (cmd) {
    case 0:                                     /* normal sprite */
        if (g->tw && g->th) {
            int x = xa + g->lx, y = ya + g->ly;
            q[0] = (pt){x, y}; q[1] = (pt){x + g->tw - 1, y};
            q[2] = (pt){x + g->tw - 1, y + g->th - 1}; q[3] = (pt){x, y + g->th - 1};
            draw_quad(g, q, true, hf, vf);
        }
        break;
    case 1: {                                   /* scaled sprite */
        int zp = (ctrl >> 8) & 0xF, x0, y0, x1, y1;
        if (g->tw && g->th) {
            if (zp == 0) { x0 = xa; y0 = ya; x1 = xc; y1 = yc; }
            else {
                int w = xb, h = yb, hx = zp & 3, vy = (zp >> 2) & 3;
                x0 = xa; y0 = ya;
                if (hx == 2) x0 = xa - w / 2; else if (hx == 3) x0 = xa - w;
                if (vy == 2) y0 = ya - h / 2; else if (vy == 3) y0 = ya - h;
                x1 = x0 + w; y1 = y0 + h;
            }
            x0 += g->lx; x1 += g->lx; y0 += g->ly; y1 += g->ly;
            q[0] = (pt){x0, y0}; q[1] = (pt){x1, y0}; q[2] = (pt){x1, y1}; q[3] = (pt){x0, y1};
            draw_quad(g, q, true, hf, vf);
        }
        break;
    }
    case 2:                                     /* distorted sprite */
        q[0] = (pt){xa + g->lx, ya + g->ly}; q[1] = (pt){xb + g->lx, yb + g->ly};
        q[2] = (pt){xc + g->lx, yc + g->ly}; q[3] = (pt){xd + g->lx, yd + g->ly};
        if (g->tw && g->th) draw_quad(g, q, true, hf, vf);
        break;
    case 4:                                     /* polygon */
        q[0] = (pt){xa + g->lx, ya + g->ly}; q[1] = (pt){xb + g->lx, yb + g->ly};
        q[2] = (pt){xc + g->lx, yc + g->ly}; q[3] = (pt){xd + g->lx, yd + g->ly};
        draw_quad(g, q, false, false, false);
        break;
    case 5: case 6: {                           /* polyline / line */
        pt p[4] = {{xa + g->lx, ya + g->ly}, {xb + g->lx, yb + g->ly}, {xc + g->lx, yc + g->ly}, {xd + g->lx, yd + g->ly}};
        if (cmd == 6) draw_line(g, p[0], p[1], g->colr);
        else for (int i = 0; i < 4; i++) draw_line(g, p[i], p[(i + 1) & 3], g->colr);
        break;
    }
    case 8:                                     /* user clip */
        g->ux1 = xa; g->uy1 = ya; g->ux2 = xc; g->uy2 = yc;
        break;
    case 9:                                     /* system clip */
        g->sx2 = xc; g->sy2 = yc;
        break;
    case 10:                                    /* local coordinates */
        g->lx = xa; g->ly = ya;
        break;
    default:
        LOGD(SUB_VDP1, "unknown command %X", cmd);
        break;
    }
}

static void gctx_init(gctx *g, saturn *m, uint8_t *fb, const uint8_t *vram)
{
    memset(g, 0, sizeof *g);
    g->m = m;
    g->fb = fb;
    g->vram = vram;
    g->b8 = fb_8bit(m);
    g->dil = (r16(m->vdp1_regs, 2) & 8) ? (r16(m->vdp1_regs, 2) >> 2) & 1 : -1;
    g->xoff = g->b8 ? 0 : m->wide_extra;
    g->sx2 = g->b8 ? 1023 : 511;
    g->sy2 = g->dil >= 0 ? 511 : 255;
    g->ux2 = g->sx2; g->uy2 = g->sy2;
}

void vdp1_draw(saturn *m)
{
    const uint8_t *vr = m->vdp1_vram;
    gctx g;
    uint32_t addr = 0, ret = 0;
    bool have_ret = false;
    int count = 0;

    gctx_init(&g, m, vdp1_draw_fb(m), m->vdp1_vram);
    g.stats = true;
    m->fb_field[m->fb_draw] = g.dil;
    m->fb_xoff[m->fb_draw] = g.xoff;
    m->vdp1_cmds = 0;

    while (count++ < 100000) {
        const uint8_t *c = vr + (addr & 0x7FFE0);
        uint16_t ctrl = r16(c, 0), link = r16(c, 2);
        uint32_t next = addr + 0x20;
        if (ctrl & 0x8000)
            break;
        if (!(ctrl & 0x4000)) {                         /* not skipped */
            draw_command(&g, c);
        }
        switch ((ctrl >> 12) & 3) {
        case 0: break;
        case 1: next = (uint32_t)link * 8; break;
        case 2:
            if (!have_ret) { ret = addr + 0x20; have_ret = true; }
            next = (uint32_t)link * 8;
            break;
        default:
            if (have_ret) { next = ret; have_ret = false; }
            break;
        }
        addr = next;
    }
    LOGD(SUB_VDP1, "draw: %d commands", count - 1);
}

