/* VDP2 compositor, VDP1 command list, frame-buffer swap and the PNG writer, with synthetic state. */
#include "core/png.h"
#include "saturn/saturn.h"
#include "test.h"

static void w16(uint8_t *b, uint32_t o, uint16_t v) { b[o] = (uint8_t)(v >> 8); b[o + 1] = (uint8_t)v; }

static uint32_t pix[704 * 512];

int main(void)
{
    saturn *m = saturn_create();
    int w, h;
    CHECK(m != NULL);
    if (!m) TEST_DONE();

    /* display off -> black; on -> back screen colour (single colour, at VRAM 0xE000) */
    saturn_render(m, pix, &w, &h);
    CHECK(w == 320 && h == 224);
    CHECK(pix[0] == 0xFF000000u);
    w16(m->vdp2_regs, 0x00, 0x8000);
    w16(m->vdp2_regs, 0xAE, 0x7000);                   /* BKTAL: address 0xE000 */
    w16(m->vdp2_vram, 0xE000, 0x7C00);                 /* blue */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF0000FFu);

    /* NBG0: 8x8 cells, 16 colours, one-word names; plane A at 0x2000; char 1 at 0x20 */
    w16(m->vdp2_regs, 0x20, 0x0001);                   /* BGON: NBG0 on */
    w16(m->vdp2_regs, 0x30, 0x8000);                   /* PNCN0: one word */
    w16(m->vdp2_regs, 0x40, 0x0001);                   /* MPABN0: plane A -> 0x2000 */
    w16(m->vdp2_regs, 0xF8, 0x0003);                   /* NBG0 priority 3 */
    w16(m->vdp2_vram, 0x2000, 0x0001);                 /* name (0,0): char 1, palette 0 */
    m->vdp2_vram[0x20] = 0x12;                         /* char 1 row 0: pixels 1, 2 */
    w16(m->vdp2_cram, 2, 0x001F);                      /* colour 1 = red */
    w16(m->vdp2_cram, 4, 0x03E0);                      /* colour 2 = green */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFFFF0000u);
    CHECK(pix[1] == 0xFF00FF00u);
    CHECK(pix[2] == 0xFF0000FFu);                      /* transparent -> back screen */
    /* horizontal scroll by 1 moves the picture left */
    w16(m->vdp2_regs, 0x70, 1);
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF00FF00u);
    w16(m->vdp2_regs, 0x70, 0);

    /* widescreen: 10 extra pixels per side; the game's x 0 lands at 10, the left side shows the plane
     * continuing (here: transparent cells, so the back screen) */
    m->wide_extra = 10;
    saturn_render(m, pix, &w, &h);
    CHECK(w == 340 && m->render_extra == 10);
    CHECK(pix[10] == 0xFFFF0000u && pix[11] == 0xFF00FF00u && pix[9] == 0xFF0000FFu);
    m->wide_extra = 0;
    saturn_render(m, pix, &w, &h);
    CHECK(w == 320 && pix[0] == 0xFFFF0000u);

    /* colour calculation: NBG0 per character (PNC supplement bit for one-word names), ratio 15
     * mixes red over the blue back screen half and half */
    w16(m->vdp2_regs, 0xEC, 0x0001);                   /* CCCTL: NBG0 */
    w16(m->vdp2_regs, 0xEE, 0x0001);                   /* SFCCMD: NBG0 per character */
    w16(m->vdp2_regs, 0x108, 15);                      /* CCRNA: NBG0 ratio 15 */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFFFF0000u);                      /* cell bit clear: opaque */
    w16(m->vdp2_regs, 0x30, 0x8100);                   /* PNCN0: special colour calculation bit */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF7F007Fu);
    CHECK(pix[2] == 0xFF0000FFu);                      /* transparent dots aren't mixed */
    w16(m->vdp2_regs, 0xEC, 0x0101);                   /* CCMD: add */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFFFF00FFu);
    w16(m->vdp2_regs, 0xEC, 0); w16(m->vdp2_regs, 0xEE, 0); w16(m->vdp2_regs, 0x108, 0);
    w16(m->vdp2_regs, 0x30, 0x8000);

    /* window 0 = pixel column 0 (horizontal coordinates are in half pixels at 320 wide) */
    w16(m->vdp2_regs, 0xC0, 0); w16(m->vdp2_regs, 0xC4, 1);
    w16(m->vdp2_regs, 0xC2, 0); w16(m->vdp2_regs, 0xC6, 223);
    w16(m->vdp2_regs, 0xD0, 0x0002);                   /* WCTLA: NBG0 window 0, hidden inside */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF0000FFu && pix[1] == 0xFF00FF00u);
    w16(m->vdp2_regs, 0xD0, 0x0003);                   /* hidden outside */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFFFF0000u && pix[1] == 0xFF0000FFu);
    w16(m->vdp2_regs, 0xD0, 0x0083 | 0x08);            /* AND with window 1, which is empty (start > end) */
    w16(m->vdp2_regs, 0xC8, 10); w16(m->vdp2_regs, 0xCC, 2);
    saturn_render(m, pix, &w, &h);
    CHECK(pix[1] == 0xFF00FF00u);                      /* outside w0 but not inside w1: shown */
    w16(m->vdp2_regs, 0xD0, 0);

    /* VDP1: system clip, local coordinates, a green polygon; shown through the sprite layer */
    {
        uint8_t *c = m->vdp1_vram;
        w16(c, 0x00, 0x0009); w16(c, 0x14, 319); w16(c, 0x16, 223);             /* system clip */
        w16(c, 0x20, 0x000A); w16(c, 0x2C, 0);   w16(c, 0x2E, 0);               /* local coords */
        w16(c, 0x40, 0x0004); w16(c, 0x46, 0x83E0);                              /* polygon, green */
        w16(c, 0x4C, 10); w16(c, 0x4E, 10); w16(c, 0x50, 20); w16(c, 0x52, 10);
        w16(c, 0x54, 20); w16(c, 0x56, 20); w16(c, 0x58, 10); w16(c, 0x5A, 20);
        w16(c, 0x60, 0x8000);                                                    /* end */
        vdp1_draw(m);
        CHECK(m->vdp1_cmds == 3);
        CHECK(vdp1_draw_fb(m)[(15 * 512 + 15) * 2] == 0x83 && vdp1_draw_fb(m)[(15 * 512 + 15) * 2 + 1] == 0xE0);
        CHECK(vdp1_draw_fb(m)[(5 * 512 + 5) * 2] == 0);
    }
    w16(m->vdp1_regs, 2, 0x0000);                      /* FBCR: 1-cycle mode */
    vdp1_frame_end(m);                                  /* swap: the drawn buffer is now displayed */
    w16(m->vdp2_regs, 0xE0, 0x0020);                   /* SPCTL: SPCLMD, so MSB-set dots are RGB */
    w16(m->vdp2_regs, 0xF0, 0x0006);                   /* sprite priority 6 */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[15 * 320 + 15] == 0xFF00FF00u);
    CHECK(pix[5 * 320 + 5] == 0xFF0000FFu);

    /* switching to widescreen while paused (nothing redrawn): the frame buffer was drawn unshifted, so the
     * polygon must still land at game x 15, i.e. 15 + 53 in the wider picture, not 53 further right */
    m->wide_extra = 53;
    saturn_render(m, pix, &w, &h);
    CHECK(w == 426);
    CHECK(pix[15 * 426 + 15 + 53] == 0xFF00FF00u);
    CHECK(pix[15 * 426 + 15 + 106] != 0xFF00FF00u);
    m->wide_extra = 0;
    saturn_render(m, pix, &w, &h);
    CHECK(pix[15 * 320 + 15] == 0xFF00FF00u);

    /* sprite colour calculation: a type-0 palette dot (CC bits = 1, colour 1 = red) at (30,30)
     * mixed with the blue back screen by CCRSA ratio 1 = 15 when its priority meets SPCCCS */
    {
        uint8_t *shown = m->vdp1_fb + (size_t)(m->fb_draw ^ 1) * 0x40000;
        w16(shown, (30 * 512 + 30) * 2, 0x0801);
        w16(m->vdp2_regs, 0xEC, 0x0040);               /* CCCTL: SPCCEN */
        w16(m->vdp2_regs, 0x100, 15 << 8);             /* CCRSA: ratio 1 = 15 */
        w16(m->vdp2_regs, 0xE0, 0x0720);               /* SPCTL: type 0, CC number 7, priority <= 7 */
        saturn_render(m, pix, &w, &h);
        CHECK(pix[30 * 320 + 30] == 0xFF7F007Fu);
        CHECK(pix[15 * 320 + 15] == 0xFF00FF00u);     /* RGB sprite dots aren't mixed */
        w16(m->vdp2_regs, 0xE0, 0x1520);               /* priority == 5: priority 6 doesn't qualify */
        saturn_render(m, pix, &w, &h);
        CHECK(pix[30 * 320 + 30] == 0xFFFF0000u);
        w16(m->vdp2_regs, 0xE0, 0x0020); w16(m->vdp2_regs, 0xEC, 0); w16(m->vdp2_regs, 0x100, 0);
        w16(shown, (30 * 512 + 30) * 2, 0);
    }

    /* priorities: raise NBG0 above the sprites and the cell (0,0) stays, sprite hidden behind a cell */
    w16(m->vdp2_regs, 0xF8, 0x0007);
    w16(m->vdp2_regs, 0xF0, 0x0001);
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFFFF0000u);
    CHECK(pix[15 * 320 + 15] == 0xFF00FF00u);          /* NBG0 is transparent there */

    /* line colour screen (LNCLEN): NBG0's colour calculation mixes with the line colour (green, index 2 read
     * from the table at 0xF000) instead of the blue back screen under it */
    w16(m->vdp2_regs, 0xEC, 0x0001);                   /* CCCTL: NBG0, ratio mode */
    w16(m->vdp2_regs, 0x108, 15);                      /* CCRNA: ratio 15 */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF7F007Fu);                      /* red over blue */
    w16(m->vdp2_regs, 0xE8, 0x0001);                   /* LNCLEN: NBG0 */
    w16(m->vdp2_regs, 0xA8, 0x0000); w16(m->vdp2_regs, 0xAA, 0x7800);   /* LCTA/LCTB: 0xF000 */
    w16(m->vdp2_vram, 0xF000, 2);
    saturn_render(m, pix, &w, &h);
    CHECK(pix[0] == 0xFF7F7F00u);                      /* red over the green line colour */
    w16(m->vdp2_regs, 0xE8, 0); w16(m->vdp2_regs, 0xEC, 0); w16(m->vdp2_regs, 0x108, 0);

    /* special priority per cell (SFPRMD): NBG0 at priority 4 against the sprite at 4 (sprites win ties); a cell
     * whose SPR bit (PNC supplement bit 9 with one-word names) is set gets priority 5 and covers the sprite */
    w16(m->vdp2_vram, 0x2000 + (1 * 64 + 2) * 2, 0x0001);   /* cell (2,1): char 1 */
    m->vdp2_vram[0x20 + 7 * 4] = 0x12;                       /* char 1 row 7: dots at (16,15) and (17,15) */
    w16(m->vdp2_regs, 0xF8, 0x0004);                   /* NBG0 priority 4 */
    w16(m->vdp2_regs, 0xF0, 0x0004);                   /* sprites priority 4 */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[15 * 320 + 16] == 0xFF00FF00u);          /* the sprite (tie) */
    w16(m->vdp2_regs, 0xEA, 0x0001);                   /* SFPRMD: NBG0 per cell */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[15 * 320 + 16] == 0xFF00FF00u);          /* SPR clear: priority 4, the sprite still wins */
    w16(m->vdp2_regs, 0x30, 0x8200);                   /* PNCN0: SPR set for every cell */
    saturn_render(m, pix, &w, &h);
    CHECK(pix[15 * 320 + 16] == 0xFFFF0000u);          /* priority 5: the cell covers the sprite */
    w16(m->vdp2_regs, 0xEA, 0); w16(m->vdp2_regs, 0x30, 0x8000);
    w16(m->vdp2_regs, 0xF8, 0x0007); w16(m->vdp2_regs, 0xF0, 0x0001);

    /* PNG writer */
    {
        const char *path = "test_video.png";
        FILE *f;
        uint8_t sig[8];
        CHECK(png_write(path, pix, 320, 224));
        f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            CHECK(fread(sig, 1, 8, f) == 8);
            CHECK(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
            fseek(f, -12, SEEK_END);
            CHECK(fread(sig, 1, 8, f) == 8);
            CHECK(memcmp(sig + 4, "IEND", 4) == 0);
            fclose(f);
            remove(path);
        }
    }

    saturn_destroy(m);
    TEST_DONE();
}
