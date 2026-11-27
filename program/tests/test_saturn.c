/* Machine-level tests: BIOS replacement calls, SCU interrupt delivery and DMA, SMPC INTBACK,
 * SH-2 on-chip divider. No game data. */
#include "saturn/saturn.h"
#include "sh2/sh2_interp.h"
#include "test.h"

#define VT 0x06000000u

/* Calls a BIOS slot the way game code does: pc = slot trap, pr = a return address. */
static void bios_call(saturn *m, uint32_t slot, uint32_t a, uint32_t b)
{
    sh2_state *s = &m->cpu[0];
    s->r[4] = a;
    s->r[5] = b;
    s->pr = 0x06004000u;
    s->pc = sh2_rd32(s, VT + slot);
    CHECK(bios_hle(m, s));
    CHECK(s->pc == 0x06004000u);
}

static void cd_cmd(saturn *m, uint16_t r0, uint16_t r1, uint16_t r2, uint16_t r3)
{
    cdb_write(m, 0x90018, r0);   /* CR1-CR4 at 0x25890018.. */
    cdb_write(m, 0x9001C, r1);
    cdb_write(m, 0x90020, r2);
    cdb_write(m, 0x90024, r3);   /* CR4 issues it */
}

/* The game's own pause: seek 0xFFFFFF (pause here), then play 0xFFFFFF/0xFFFFFF (carry on). The music
 * must resume where it was, not jump past its end. No disc needed: positions are absolute FADs. */
static void test_cd_pause_resume(saturn *m)
{
    cd_cmd(m, 0x1080, 0x03E8, 0x0F80, 0x01F4);   /* play FAD 1000, 500 sectors, repeat forever */
    CHECK(m->cdb.playing && m->cdb.fad == 1000 && m->cdb.play_end == 1500);
    m->cdb.fad = 1234;                           /* part-way through */
    cd_cmd(m, 0x11FF, 0xFFFF, 0, 0);             /* pause */
    CHECK(!m->cdb.playing && m->cdb.fad == 1234);
    cd_cmd(m, 0x10FF, 0xFFFF, 0xFFFF, 0xFFFF);   /* resume */
    CHECK(m->cdb.playing && m->cdb.fad == 1234 && m->cdb.play_end == 1500);
    cd_cmd(m, 0x1180, 0x0400, 0, 0);             /* seek to FAD 1024 still works */
    CHECK(!m->cdb.playing && m->cdb.fad == 1024);
}

int main(void)
{
    saturn *m = saturn_create();
    sh2_state *s;
    CHECK(m != NULL);
    if (!m) TEST_DONE();
    s = &m->cpu[0];
    bios_install(m);

    /* SCU mask calls: 0x340 sets, 0x344 ANDs then ORs; 0x348 mirrors the mask */
    bios_call(m, 0x340, 0x1234, 0);
    CHECK(m->scu.ims == 0x1234 && sh2_rd32(s, VT + 0x348) == 0x1234);
    bios_call(m, 0x344, ~0x0004u, 0x8000);
    CHECK(m->scu.ims == ((0x1234 & ~0x0004u) | 0x8000));

    /* semaphores: first TASSEM acquires (non-zero), second fails, CLRSEM releases */
    bios_call(m, 0x330, 7, 0); CHECK(s->r[0] != 0);
    bios_call(m, 0x330, 7, 0); CHECK(s->r[0] == 0);
    bios_call(m, 0x334, 7, 0);
    bios_call(m, 0x330, 7, 0); CHECK(s->r[0] != 0);

    /* SETSINT installs a raw handler; GETSINT reads it back */
    bios_call(m, 0x310, 0x41, 0x06005000u);
    CHECK(sh2_rd32(s, VT + 0x41 * 4) == 0x06005000u);
    bios_call(m, 0x314, 0x41, 0); CHECK(s->r[0] == 0x06005000u);

    /* SETUINT: a C-style handler runs through the wrapper and comes back via rte,
     * with r0-r7 restored */
    bios_call(m, 0x300, 0x40, 0x06006000u);
    s->vbr = VT;
    s->sr = 0x000;
    s->r[15] = 0x06008000u;
    s->pc = 0x06007000u;
    s->r[3] = 0x33;
    m->scu.ims = 0;
    scu_raise(m, 0);                       /* VBlank-IN */
    scu_deliver(m);
    CHECK((s->sr & SR_IMASK) == 0xF0);     /* level 15 */
    CHECK(bios_hle(m, s) && s->pc == 0x06006000u);
    s->r[3] = 0x99;                        /* handler clobbers r3 */
    s->pc = s->pr;                         /* handler returns (rts) */
    CHECK(bios_hle(m, s));
    CHECK(s->pc == 0x06007000u && s->r[3] == 0x33 && (s->sr & SR_IMASK) == 0);

    /* masked interrupts aren't delivered */
    m->scu.ims = 1;
    scu_raise(m, 0);
    s->pc = 0x06007000u;
    scu_deliver(m);
    CHECK(s->pc == 0x06007000u);

    /* SCU DMA level 0, direct, started immediately */
    for (int i = 0; i < 16; i++) sh2_wr8(s, 0x06010000u + (uint32_t)i, (uint8_t)(i + 1));
    sh2_wr32(s, 0x25FE0000u, 0x06010000u);  /* D0R */
    sh2_wr32(s, 0x25FE0004u, 0x06020000u);  /* D0W */
    sh2_wr32(s, 0x25FE0008u, 16);           /* D0C */
    sh2_wr32(s, 0x25FE000Cu, 0x102);        /* read +4, write +4 */
    sh2_wr32(s, 0x25FE0014u, 7);            /* start factor: immediate */
    sh2_wr32(s, 0x25FE0010u, 0x101);        /* enable + start */
    CHECK(sh2_rd8(s, 0x06020000u) == 1 && sh2_rd8(s, 0x0602000Fu) == 16);
    CHECK(m->scu.ist & (1u << 11));         /* level 0 end */

    /* SMPC INTBACK peripheral-only: a standard pad on port 1 with start pressed, port 2 empty */
    m->smpc.pad[0] = 0xF7FF;
    sh2_wr8(s, 0x20100001u, 0x00);          /* IREG0: no status */
    sh2_wr8(s, 0x20100003u, 0x08);          /* IREG1: peripheral data */
    sh2_wr8(s, 0x2010001Fu, 0x10);          /* INTBACK */
    CHECK(sh2_rd8(s, 0x20100063u) == 0);    /* SF cleared */
    CHECK(sh2_rd8(s, 0x20100021u) == 0xF1 && sh2_rd8(s, 0x20100023u) == 0x02);
    CHECK(sh2_rd8(s, 0x20100025u) == 0xF7 && sh2_rd8(s, 0x20100027u) == 0xFF);
    CHECK(sh2_rd8(s, 0x20100029u) == 0xF0);  /* port 2: nothing plugged in */
    CHECK(m->scu.ist & (1u << 7));

    /* 10 players, the game's maximum: 10 pads on two multitaps (6 + 4) make 38 bytes, two
     * blocks via INTBACK continue. */
    {
        int slot[SMPC_SLOTS];
#define OREG(n) sh2_rd8(s, 0x20100021u + 2u * (n))
        CHECK(smpc_layout(m, 10, PORTS_AUTO, slot) == 10);
        CHECK(slot[0] == 0 && slot[5] == 5 && slot[6] == 6 && slot[9] == 9 && slot[10] == -1);
        for (int i = 0; i < SMPC_SLOTS; i++) m->smpc.pad[i] = 0xFFFF;
        m->smpc.pad[9] = 0xF7FF;                /* player 10 holds Start */
        sh2_wr8(s, 0x20100001u, 0x00);
        sh2_wr8(s, 0x20100003u, 0x08);
        sh2_wr8(s, 0x2010001Fu, 0x10);
        CHECK(OREG(0) == 0x16 && OREG(1) == 0x02 && OREG(2) == 0xFF);       /* port 1 multitap */
        CHECK(OREG(19) == 0x16 && OREG(20) == 0x02);                         /* port 2 multitap */
        CHECK(OREG(29) == 0x02 && OREG(30) == 0xF7 && OREG(31) == 0xFF);     /* its 4th pad */
        CHECK(sh2_rd8(s, 0x20100061u) == 0xE0);                              /* first block, more follows */
        sh2_wr8(s, 0x20100001u, 0x80);                                       /* continue: bit 7 toggles */
        CHECK(OREG(0) == 0xFF && OREG(1) == 0xFF);                           /* two empty connectors */
        CHECK(sh2_rd8(s, 0x20100061u) == 0x80);                              /* last block */

        /* the way the kernel does it: status first, then continue with 0x80, then 0x00 */
        sh2_wr8(s, 0x20100001u, 0x01);
        sh2_wr8(s, 0x2010001Fu, 0x10);
        CHECK(sh2_rd8(s, 0x20100061u) == 0x60);                              /* status, peripherals pending */
        sh2_wr8(s, 0x20100001u, 0x80);
        CHECK(OREG(0) == 0x16 && sh2_rd8(s, 0x20100061u) == 0xE0);
        sh2_wr8(s, 0x20100001u, 0x00);
        CHECK(OREG(0) == 0xFF && sh2_rd8(s, 0x20100061u) == 0x80);

        /* 8 players: a multitap on each port (6 + 2); 30 bytes, one block */
        CHECK(smpc_layout(m, 8, PORTS_AUTO, slot) == 8 && slot[6] == 6 && slot[7] == 7);
        CHECK(m->smpc.port_mode[0] == SMPC_PORT_TAP && m->smpc.tap_pads[0] == 6 && m->smpc.tap_pads[1] == 2);
        m->smpc.pad[7] = 0xF7FF;                /* player 8 holds Start */
        sh2_wr8(s, 0x20100001u, 0x00);
        sh2_wr8(s, 0x2010001Fu, 0x10);
        CHECK(OREG(19) == 0x16 && OREG(23) == 0x02 && OREG(24) == 0xF7 && OREG(26) == 0xFF && OREG(29) == 0xFF);
        CHECK(sh2_rd8(s, 0x20100061u) == 0xC0);                              /* first and last block */
        m->smpc.pad[7] = 0xFFFF;

        /* 2 players: a direct pad on each port; 1 player: port 2 empty again */
        CHECK(smpc_layout(m, 2, PORTS_AUTO, slot) == 2 && slot[1] == 6);
        sh2_wr8(s, 0x20100001u, 0x00);
        sh2_wr8(s, 0x2010001Fu, 0x10);
        CHECK(OREG(0) == 0xF1 && OREG(4) == 0xF1 && OREG(5) == 0x02);
        CHECK(smpc_layout(m, 1, PORTS_AUTO, slot) == 1);
        CHECK(smpc_layout(m, 3, PORTS_TAP1, slot) == 3 && slot[2] == 2);
        CHECK(smpc_layout(m, 1, PORTS_AUTO, slot) == 1 && m->smpc.port_mode[1] == SMPC_PORT_NONE);
        m->smpc.pad[0] = 0xFFFF;
#undef OREG
    }

    /* SMPC INTBACK status: area code for USA */
    sh2_wr8(s, 0x20100001u, 0x01);
    sh2_wr8(s, 0x20100003u, 0x00);
    sh2_wr8(s, 0x2010001Fu, 0x10);
    CHECK(sh2_rd8(s, 0x20100021u + 9 * 2) == 0x04);

    /* SH-2 on-chip divider: 32/32 and 64/32 */
    sh2_wr32(s, 0xFFFFFF00u, 7);
    sh2_wr32(s, 0xFFFFFF04u, (uint32_t)-100);
    CHECK(sh2_rd32(s, 0xFFFFFF04u) == (uint32_t)-14);
    sh2_wr32(s, 0xFFFFFF00u, 10);
    sh2_wr32(s, 0xFFFFFF10u, 1);            /* 0x1_00000000 / 10 */
    sh2_wr32(s, 0xFFFFFF14u, 0);
    CHECK(sh2_rd32(s, 0xFFFFFF14u) == 429496729u && sh2_rd32(s, 0xFFFFFF10u) == 6);

    /* VDP2 TVSTAT reports vertical blank */
    m->vblank = true;
    CHECK(sh2_rd16(s, 0x25F80004u) & 8);

    test_cd_pause_resume(m);
    saturn_destroy(m);
    TEST_DONE();
}
