/* 68000 interpreter tests: small hand-assembled programs, checked against known results. */
#include "m68k/m68k.h"
#include "test.h"

static uint8_t mem[0x10000];

static uint8_t r8(void *u, uint32_t a) { (void)u; return mem[a & 0xFFFF]; }
static uint16_t r16(void *u, uint32_t a) { (void)u; a &= 0xFFFF; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void w8(void *u, uint32_t a, uint8_t v) { (void)u; mem[a & 0xFFFF] = v; }
static void w16(void *u, uint32_t a, uint16_t v) { (void)u; a &= 0xFFFF; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }

static void put16(uint32_t a, uint16_t v) { w16(NULL, a, v); }
static void put32(uint32_t a, uint32_t v) { put16(a, (uint16_t)(v >> 16)); put16(a + 2, (uint16_t)v); }

/* Loads `code` at 0x400 (SSP 0x8000), resets and runs until STOP or the budget runs out. */
static void run(m68k *c, const uint16_t *code, int n, int budget)
{
    memset(mem, 0, sizeof mem);
    put32(0, 0x8000);
    put32(4, 0x400);
    for (int i = 0; i < n; i++)
        put16(0x400 + (uint32_t)i * 2, code[i]);
    c->bus = (m68k_bus){NULL, r8, r16, w8, w16};
    m68k_reset(c);
    m68k_run(c, budget);
}

#define RUN(c, budget, ...) do { static const uint16_t p_[] = {__VA_ARGS__}; \
    run(c, p_, (int)(sizeof p_ / sizeof p_[0]), budget); } while (0)

#define STOP 0x4E72, 0x2700
#define HALT 0x60FE   /* BRA.S * : keeps the flags, unlike STOP */

int main(void)
{
    m68k c;

    /* sum 10..1 with ADD/SUBQ/BNE */
    RUN(&c, 2000, 0x700A, 0x7200, 0xD280, 0x5380, 0x66FA, STOP);
    CHECK(c.d[1] == 55);
    CHECK(c.stopped);

    /* DIVU #7: 100 / 7 = 14 r 2 ; MULS #-3 * 5 = -15 */
    RUN(&c, 2000, 0x7064, 0x80FC, 0x0007, 0x7205, 0xC3FC, 0xFFFD, STOP);
    CHECK(c.d[0] == 0x0002000E);
    CHECK(c.d[1] == (uint32_t)-15);

    /* MOVE.L #$12345678,D2 ; LSL.L #4,D2 -> 23456780, C=1 */
    RUN(&c, 2000, 0x243C, 0x1234, 0x5678, 0xE98A, HALT);
    CHECK(c.d[2] == 0x23456780);
    CHECK(c.sr & 1);

    /* MOVEQ #1,D3 ; ROR.W #1,D3 -> 8000, C=1, N=1 */
    RUN(&c, 2000, 0x7601, 0xE25B, HALT);
    CHECK(c.d[3] == 0x8000);
    CHECK((c.sr & 0x9) == 0x9);

    /* ADD overflow: MOVE.L #$7FFFFFFF,D0 ; ADDQ.L #1,D0 -> N V set, C clear */
    RUN(&c, 2000, 0x203C, 0x7FFF, 0xFFFF, 0x5280, HALT);
    CHECK(c.d[0] == 0x80000000u);
    CHECK((c.sr & 0xF) == 0xA);

    /* ABCD: MOVE #0,CCR ; MOVEQ #$19,D0 ; MOVEQ #$28,D1 ; ABCD D0,D1 -> 47 */
    RUN(&c, 2000, 0x44FC, 0x0000, 0x7019, 0x7228, 0xC300, STOP);
    CHECK((c.d[1] & 0xFF) == 0x47);

    /* DBRA: MOVEQ #4,D0 ; MOVEQ #0,D1 ; loop: ADDQ.W #2,D1 ; DBRA D0,loop -> D1 = 10, D0.w = FFFF */
    RUN(&c, 2000, 0x7004, 0x7200, 0x5441, 0x51C8, 0xFFFC, STOP);
    CHECK(c.d[1] == 10);
    CHECK((c.d[0] & 0xFFFF) == 0xFFFF);

    /* JSR/RTS + MOVEM: LEA $1000,A0 ; MOVEQ #7,D5 ; MOVEQ #9,D6 ; MOVEM.L D5-D6,-(A7) ;
     * MOVEQ #0,D5 ; MOVEQ #0,D6 ; MOVEM.L (A7)+,D5-D6 ; BSR sub ; STOP ; sub: MOVEQ #1,D7 ; RTS */
    RUN(&c, 4000, 0x41F8, 0x1000, 0x7A07, 0x7C09, 0x48E7, 0x0600, 0x7A00, 0x7C00, 0x4CDF, 0x0060,
        0x6104, STOP, 0x7E01, 0x4E75);
    CHECK(c.a[0] == 0x1000);
    CHECK(c.d[5] == 7 && c.d[6] == 9);
    CHECK(c.d[7] == 1);
    CHECK(c.a[7] == 0x8000);

    /* byte push on A7 keeps it even: MOVE.B #$AB,-(A7) -> A7 = 7FFE, byte at 7FFE */
    RUN(&c, 2000, 0x1F3C, 0x00AB, STOP);
    CHECK(c.a[7] == 0x7FFE);
    CHECK(mem[0x7FFE] == 0xAB);

    /* autovectored interrupt (level 2 -> vector 26 at $68): handler ADDQ.L #1,D7 ; RTE
     * main: MOVE #$2000,SR ; BRA.S * */
    RUN(&c, 0, 0x46FC, 0x2000, 0x60FE);
    put32(0x68, 0x500);
    put16(0x500, 0x5287);
    put16(0x502, 0x4E73);
    m68k_run(&c, 100);
    CHECK(c.d[7] == 0);
    m68k_set_irq(&c, 2);
    m68k_run(&c, 100);
    m68k_set_irq(&c, 0);
    m68k_run(&c, 100);
    CHECK(c.d[7] >= 1);
    CHECK((c.sr & 0x0700) == 0);
    CHECK(c.pc >= 0x404 && c.pc <= 0x406);

    /* STOP is left by an interrupt */
    RUN(&c, 0, 0x4E72, 0x2000, 0x7401, 0x4E72, 0x2700);
    put32(0x68, 0x500);
    put16(0x500, 0x4E73);
    m68k_run(&c, 100);
    CHECK(c.stopped);
    m68k_set_irq(&c, 2);
    m68k_run(&c, 30);           /* takes the interrupt (the mask rises to 2 inside the handler) */
    m68k_set_irq(&c, 0);        /* the device drops its request */
    m68k_run(&c, 200);
    if (c.d[2] != 1) fprintf(stderr, "pc %X sr %X stopped %d\n", (unsigned)c.pc, (unsigned)c.sr, c.stopped);
    CHECK(c.d[2] == 1);

    /* CMP + Bcc signed: MOVEQ #-1,D0 ; MOVEQ #1,D1 ; CMP.L D1,D0 ; BLT.S +2 ; MOVEQ #5,D2 ; STOP */
    RUN(&c, 2000, 0x70FF, 0x7201, 0xB081, 0x6D02, 0x7405, STOP);
    CHECK(c.d[2] == 0);

    /* EXT, SWAP, EXG, NEG */
    RUN(&c, 2000, 0x70F0, 0x4880, 0x4840, 0x7203, 0xC141, 0x4481, STOP);
    CHECK(c.d[1] == 0x000F0001u);
    CHECK(c.d[0] == 3);

    /* privilege violation in user mode (vector 8 handler at $600: MOVEQ #8,D4 ; STOP) */
    RUN(&c, 0, 0x46FC, 0x0000, 0x46FC, 0x2700, STOP);
    put32(0x20, 0x600);
    put16(0x600, 0x7808);
    put16(0x602, 0x4E72);
    put16(0x604, 0x2700);
    m68k_run(&c, 400);
    CHECK(c.d[4] == 8);
    CHECK(c.sr & M68K_SR_S);

    TEST_DONE();
}
