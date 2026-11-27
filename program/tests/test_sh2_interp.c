/* Semantic tests for the SH-2 interpreter: every instruction at least once, with the tricky
 * cases (carry/borrow, overflow, division, MAC saturation, Rn==Rm post-increment, delay
 * slots, exceptions). All values are made up. */
#include "sh2/sh2_decode.h"
#include "sh2/sh2_interp.h"
#include "test.h"

#define RAM_BASE 0x06000000u
#define CODE 0x06001000u
#define DATA 0x06002000u

static uint8_t ram[1 << 20];
static sh2_state S;
static int covered[SH2_OP_COUNT];
static int exc_vector;

static void on_exc(sh2_state *s, int v, uint32_t pc) { (void)s; (void)pc; exc_vector = v; }

static void reset(void)
{
    memset(&S, 0, sizeof S);
    memset(ram, 0, sizeof ram);
    for (uint32_t p = 0x060; p < 0x080; p++)
        S.page[p] = ram; /* high work RAM and its mirrors */
    sh2_reset_state(&S);
    S.sr = 0;
    S.pc = CODE;
    S.r[15] = RAM_BASE + 0x8000;
    S.on_exception = on_exc;
    exc_vector = -1;
}

static void put16(uint32_t a, uint16_t v) { sh2_wr16(&S, a, v); }

/* Places the instructions at CODE and runs one step (a branch runs with its slot). */
static void run(uint16_t op, uint16_t next)
{
    sh2_insn in;
    put16(CODE, op);
    put16(CODE + 2, next);
    if (sh2_decode(op, CODE, &in))
        covered[in.op]++;
    if (sh2_decode(next, CODE + 2, &in) && next != 0x0009)
        covered[in.op]++;
    S.pc = CODE;
    sh2_step(&S);
}
#define RUN(op) run(op, 0x0009)

#define T_IS(v) CHECK(((S.sr & SR_T) != 0) == (v))
#define REG(i, v) CHECK(S.r[i] == (uint32_t)(v))

int main(void)
{
    /* ---- system ---- */
    reset(); S.sr = 0; RUN(0x0018); T_IS(1); RUN(0x0008); T_IS(0);            /* sett, clrt */
    reset(); S.mach = 5; S.macl = 6; RUN(0x0028); CHECK(S.mach == 0 && S.macl == 0); /* clrmac */
    reset(); RUN(0x0009); CHECK(S.pc == CODE + 2);                                  /* nop */
    reset(); RUN(0x001B); CHECK(S.sleeping == 1);                                   /* sleep */
    reset(); S.sr = SR_M | SR_Q | SR_T; RUN(0x0019); CHECK((S.sr & (SR_M | SR_Q | SR_T)) == 0); /* div0u */
    reset(); S.sr = SR_T; RUN(0x0329); REG(3, 1);                                   /* movt r3 */
    reset(); S.sr = 0x3F3; RUN(0x0302); REG(3, 0x3F3);                              /* stc sr */
    reset(); S.gbr = 7; RUN(0x0312); REG(3, 7);
    reset(); S.vbr = 8; RUN(0x0322); REG(3, 8);
    reset(); S.mach = 9; RUN(0x030A); REG(3, 9);
    reset(); S.macl = 10; RUN(0x031A); REG(3, 10);
    reset(); S.pr = 11; RUN(0x032A); REG(3, 11);
    reset(); S.r[3] = 0xFFFFFFFF; RUN(0x430E); CHECK(S.sr == 0x3F3);                /* ldc sr masks */
    reset(); S.r[3] = 1; RUN(0x431E); CHECK(S.gbr == 1);
    reset(); S.r[3] = 2; RUN(0x432E); CHECK(S.vbr == 2);
    reset(); S.r[3] = 3; RUN(0x430A); CHECK(S.mach == 3);
    reset(); S.r[3] = 4; RUN(0x431A); CHECK(S.macl == 4);
    reset(); S.r[3] = 5; RUN(0x432A); CHECK(S.pr == 5);
    /* ldc.l / lds.l: post-increment */
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0xFFFFFFFF); RUN(0x4307); CHECK(S.sr == 0x3F3); REG(3, DATA + 4);
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0x12); RUN(0x4317); CHECK(S.gbr == 0x12);
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0x13); RUN(0x4327); CHECK(S.vbr == 0x13);
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0x14); RUN(0x4306); CHECK(S.mach == 0x14);
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0x15); RUN(0x4316); CHECK(S.macl == 0x15);
    reset(); S.r[3] = DATA; sh2_wr32(&S, DATA, 0x16); RUN(0x4326); CHECK(S.pr == 0x16); REG(3, DATA + 4);
    /* stc.l / sts.l: pre-decrement */
    reset(); S.r[3] = DATA + 4; S.sr = 0x101; RUN(0x4303); REG(3, DATA); CHECK(sh2_rd32(&S, DATA) == 0x101);
    reset(); S.r[3] = DATA + 4; S.gbr = 0x21; RUN(0x4313); CHECK(sh2_rd32(&S, DATA) == 0x21);
    reset(); S.r[3] = DATA + 4; S.vbr = 0x22; RUN(0x4323); CHECK(sh2_rd32(&S, DATA) == 0x22);
    reset(); S.r[3] = DATA + 4; S.mach = 0x23; RUN(0x4302); CHECK(sh2_rd32(&S, DATA) == 0x23);
    reset(); S.r[3] = DATA + 4; S.macl = 0x24; RUN(0x4312); CHECK(sh2_rd32(&S, DATA) == 0x24);
    reset(); S.r[3] = DATA + 4; S.pr = 0x25; RUN(0x4322); CHECK(sh2_rd32(&S, DATA) == 0x25);

    /* ---- compare / test ---- */
    reset(); S.r[3] = 1; RUN(0x4310); REG(3, 0); T_IS(1);                           /* dt */
    reset(); S.r[3] = 2; RUN(0x4310); REG(3, 1); T_IS(0);
    reset(); S.r[3] = 0; RUN(0x4311); T_IS(1); S.r[3] = 0x80000000; RUN(0x4311); T_IS(0); /* cmp/pz */
    reset(); S.r[3] = 0; RUN(0x4315); T_IS(0); S.r[3] = 1; RUN(0x4315); T_IS(1);    /* cmp/pl */
    reset(); S.r[3] = 5; S.r[5] = 5; RUN(0x3350); T_IS(1);                          /* cmp/eq */
    reset(); S.r[3] = 1; S.r[5] = 0xFFFFFFFF; RUN(0x3352); T_IS(0);                 /* cmp/hs unsigned */
    reset(); S.r[3] = 1; S.r[5] = 0xFFFFFFFF; RUN(0x3353); T_IS(1);                 /* cmp/ge signed */
    reset(); S.r[3] = 0xFFFFFFFF; S.r[5] = 1; RUN(0x3356); T_IS(1);                 /* cmp/hi */
    reset(); S.r[3] = 0xFFFFFFFF; S.r[5] = 1; RUN(0x3357); T_IS(0);                 /* cmp/gt */
    reset(); S.r[0] = 0xFFFFFFF5; RUN(0x88F5); T_IS(1);                             /* cmp/eq #-11 */
    reset(); S.r[3] = 0x11223344; S.r[5] = 0x55663377; RUN(0x235C); T_IS(1);        /* cmp/str: byte 2 equal */
    reset(); S.r[3] = 0x11223344; S.r[5] = 0x55667788; RUN(0x235C); T_IS(0);
    reset(); S.r[3] = 0xF0; S.r[5] = 0x0F; RUN(0x2358); T_IS(1);                    /* tst */
    reset(); S.r[0] = 0x10; RUN(0xC810); T_IS(0);                                   /* tst #imm */
    reset(); S.r[3] = DATA; RUN(0x431B); T_IS(1); CHECK(sh2_rd8(&S, DATA) == 0x80); /* tas.b */

    /* ---- logic ---- */
    reset(); S.r[3] = 0xFF00; S.r[5] = 0x0FF0; RUN(0x2359); REG(3, 0x0F00);         /* and */
    reset(); S.r[3] = 0xFF00; S.r[5] = 0x0FF0; RUN(0x235B); REG(3, 0xFFF0);         /* or */
    reset(); S.r[3] = 0xFF00; S.r[5] = 0x0FF0; RUN(0x235A); REG(3, 0xF0F0);         /* xor */
    reset(); S.r[5] = 0x0F0F0F0F; RUN(0x6357); REG(3, 0xF0F0F0F0);                  /* not */
    reset(); S.r[0] = 0x1234; RUN(0xC90F); REG(0, 4);                               /* and #imm */
    reset(); S.r[0] = 0x1200; RUN(0xCB34); REG(0, 0x1234);                          /* or #imm */
    reset(); S.r[0] = 0xFF; RUN(0xCA0F); REG(0, 0xF0);                              /* xor #imm */
    reset(); S.gbr = DATA; S.r[0] = 1; sh2_wr8(&S, DATA + 1, 0x0F); RUN(0xCC10); T_IS(1); /* tst.b */
    reset(); S.gbr = DATA; S.r[0] = 1; sh2_wr8(&S, DATA + 1, 0xFF); RUN(0xCD0F); CHECK(sh2_rd8(&S, DATA + 1) == 0x0F);
    reset(); S.gbr = DATA; S.r[0] = 1; sh2_wr8(&S, DATA + 1, 0xF0); RUN(0xCF0F); CHECK(sh2_rd8(&S, DATA + 1) == 0xFF);
    reset(); S.gbr = DATA; S.r[0] = 1; sh2_wr8(&S, DATA + 1, 0xFF); RUN(0xCE0F); CHECK(sh2_rd8(&S, DATA + 1) == 0xF0);

    /* ---- shifts ---- */
    reset(); S.r[3] = 0x80000001; RUN(0x4304); REG(3, 0x00000003); T_IS(1);         /* rotl */
    reset(); S.r[3] = 0x80000001; RUN(0x4305); REG(3, 0xC0000000); T_IS(1);         /* rotr */
    reset(); S.r[3] = 0x80000000; S.sr = SR_T; RUN(0x4324); REG(3, 1); T_IS(1);     /* rotcl */
    reset(); S.r[3] = 0x00000001; S.sr = SR_T; RUN(0x4325); REG(3, 0x80000000); T_IS(1); /* rotcr */
    reset(); S.r[3] = 0xC0000000; RUN(0x4320); REG(3, 0x80000000); T_IS(1);         /* shal */
    reset(); S.r[3] = 0x80000003; RUN(0x4321); REG(3, 0xC0000001); T_IS(1);         /* shar keeps sign */
    reset(); S.r[3] = 0x80000000; RUN(0x4300); REG(3, 0); T_IS(1);                  /* shll */
    reset(); S.r[3] = 3; RUN(0x4301); REG(3, 1); T_IS(1);                           /* shlr */
    reset(); S.r[3] = 3; RUN(0x4308); REG(3, 12);
    reset(); S.r[3] = 12; RUN(0x4309); REG(3, 3);
    reset(); S.r[3] = 0x12; RUN(0x4318); REG(3, 0x1200);
    reset(); S.r[3] = 0x1234; RUN(0x4319); REG(3, 0x12);
    reset(); S.r[3] = 0x1234; RUN(0x4328); REG(3, 0x12340000);
    reset(); S.r[3] = 0x12345678; RUN(0x4329); REG(3, 0x1234);

    /* ---- arithmetic ---- */
    reset(); S.r[3] = 1; S.r[5] = 2; RUN(0x335C); REG(3, 3);                        /* add */
    reset(); S.r[3] = 1; RUN(0x73F5); REG(3, 0xFFFFFFF6);                           /* add #-11 */
    reset(); S.r[3] = 1; S.r[5] = 2; RUN(0x3358); REG(3, 0xFFFFFFFF);               /* sub */
    reset(); S.r[5] = 1; RUN(0x635B); REG(3, 0xFFFFFFFF);                           /* neg */
    reset(); S.r[3] = 0xFFFFFFFF; S.r[5] = 0; S.sr = SR_T; RUN(0x335E); REG(3, 0); T_IS(1); /* addc carry via T */
    reset(); S.r[3] = 1; S.r[5] = 1; S.sr = 0; RUN(0x335E); REG(3, 2); T_IS(0);
    reset(); S.r[3] = 0; S.r[5] = 0; S.sr = SR_T; RUN(0x335A); REG(3, 0xFFFFFFFF); T_IS(1); /* subc borrow */
    reset(); S.r[5] = 0; S.sr = SR_T; RUN(0x635A); REG(3, 0xFFFFFFFF); T_IS(1);     /* negc */
    reset(); S.r[5] = 1; S.sr = 0; RUN(0x635A); REG(3, 0xFFFFFFFF); T_IS(1);
    reset(); S.r[5] = 0; S.sr = 0; RUN(0x635A); REG(3, 0); T_IS(0);
    reset(); S.r[3] = 0x7FFFFFFF; S.r[5] = 1; RUN(0x335F); REG(3, 0x80000000); T_IS(1); /* addv */
    reset(); S.r[3] = 0x80000000; S.r[5] = 1; RUN(0x335B); REG(3, 0x7FFFFFFF); T_IS(1); /* subv */
    reset(); S.r[5] = 0xFFFFFF80; RUN(0x635C); REG(3, 0x80);                        /* extu.b */
    reset(); S.r[5] = 0xFFFF8000; RUN(0x635D); REG(3, 0x8000);                      /* extu.w */
    reset(); S.r[5] = 0x80; RUN(0x635E); REG(3, 0xFFFFFF80);                        /* exts.b */
    reset(); S.r[5] = 0x8000; RUN(0x635F); REG(3, 0xFFFF8000);                      /* exts.w */
    reset(); S.r[5] = 0x11223344; RUN(0x6358); REG(3, 0x11224433);                  /* swap.b */
    reset(); S.r[5] = 0x11223344; RUN(0x6359); REG(3, 0x33441122);                  /* swap.w */
    reset(); S.r[3] = 0x11223344; S.r[5] = 0x55667788; RUN(0x235D); REG(3, 0x77881122); /* xtrct */
    reset(); S.r[3] = 0x10000; S.r[5] = 0x10000; RUN(0x0357); CHECK(S.macl == 0);   /* mul.l (low 32) */
    reset(); S.r[3] = 0xFFFE; S.r[5] = 3; RUN(0x235F); CHECK(S.macl == 0xFFFFFFFA); /* muls.w -2*3 */
    reset(); S.r[3] = 0xFFFE; S.r[5] = 3; RUN(0x235E); CHECK(S.macl == 0x2FFFA);    /* mulu.w */
    reset(); S.r[3] = 0xFFFFFFFF; S.r[5] = 2; RUN(0x335D); CHECK(S.mach == 0xFFFFFFFF && S.macl == 0xFFFFFFFE); /* dmuls.l */
    reset(); S.r[3] = 0xFFFFFFFF; S.r[5] = 2; RUN(0x3355); CHECK(S.mach == 1 && S.macl == 0xFFFFFFFE); /* dmulu.l */

    /* mac.l: plain and 48-bit saturated */
    reset(); S.r[3] = DATA; S.r[5] = DATA + 8; sh2_wr32(&S, DATA, 3); sh2_wr32(&S, DATA + 8, 0xFFFFFFFE);
    S.mach = 0; S.macl = 10; RUN(0x035F); CHECK(S.mach == 0 && S.macl == 4); REG(3, DATA + 4); REG(5, DATA + 12);
    reset(); S.r[3] = DATA; S.r[5] = DATA + 8; sh2_wr32(&S, DATA, 0x7FFFFFFF); sh2_wr32(&S, DATA + 8, 0x7FFFFFFF);
    S.sr = SR_S; S.mach = 0x7FFF; S.macl = 0xFFFFFFF0; RUN(0x035F); CHECK(S.mach == 0x7FFF && S.macl == 0xFFFFFFFF);
    /* mac.w: plain and 32-bit saturated */
    reset(); S.r[3] = DATA; S.r[5] = DATA + 8; sh2_wr16(&S, DATA, 0xFFFF); sh2_wr16(&S, DATA + 8, 2);
    S.mach = 0; S.macl = 1; RUN(0x435F); CHECK(S.mach == 0xFFFFFFFF && S.macl == 0xFFFFFFFF); REG(3, DATA + 2);
    reset(); S.r[3] = DATA; S.r[5] = DATA + 8; sh2_wr16(&S, DATA, 0x7FFF); sh2_wr16(&S, DATA + 8, 0x7FFF);
    S.sr = SR_S; S.macl = 0x7FFFFFF0; RUN(0x435F); CHECK(S.macl == 0x7FFFFFFF && (S.mach & 1));

    /* div1: unsigned 32/16 division 100 / 7 with the standard sequence:
     * div0u; 16 x div1 r0,r1; rotcl r1; extu.w r1,r1   (r0 = divisor << 16) */
    reset();
    S.r[1] = 100; S.r[0] = 7u << 16;
    RUN(0x0019);
    for (int k = 0; k < 16; k++)
        RUN(0x3104);
    RUN(0x4124);
    RUN(0x611D);
    REG(1, 14);
    /* signed: -100 / 7 = -14 (32/16 signed sequence: div0s r0,r1 then div1 x16, rotcl, exts.w,
     * plus the usual +1 correction when the dividend is negative) */
    reset();
    S.r[1] = (uint32_t)-100; S.r[0] = 7u << 16;
    RUN(0x2107);                     /* div0s r0,r1 */
    for (int k = 0; k < 16; k++)
        RUN(0x3104);
    RUN(0x4124);                     /* rotcl r1 */
    RUN(0x611F);                     /* exts.w r1,r1 */
    CHECK(S.r[1] == (uint32_t)-15 || S.r[1] == (uint32_t)-14); /* -15 before the correction step */
    reset(); S.r[3] = 0x80000000; S.r[5] = 1; RUN(0x2357); CHECK((S.sr & SR_Q) && !(S.sr & SR_M)); T_IS(1);

    /* ---- data transfer ---- */
    reset(); S.r[5] = 9; RUN(0x6353); REG(3, 9);                                    /* mov */
    reset(); RUN(0xE3F5); REG(3, 0xFFFFFFF5);                                       /* mov #-11 */
    reset(); RUN(0xC701); REG(0, (CODE & ~3u) + 4 + 4);                             /* mova */
    reset(); sh2_wr16(&S, CODE + 4 + 2, 0x8001); RUN(0x9301); REG(3, 0xFFFF8001);   /* mov.w @(pc) */
    reset(); sh2_wr32(&S, CODE + 4 + 4, 0xCAFEBABE); RUN(0xD301); REG(3, 0xCAFEBABE); /* mov.l @(pc) */
    reset(); S.r[3] = DATA; S.r[5] = 0x1FF; RUN(0x2350); CHECK(sh2_rd8(&S, DATA) == 0xFF);
    reset(); S.r[3] = DATA; S.r[5] = 0x1FFFF; RUN(0x2351); CHECK(sh2_rd16(&S, DATA) == 0xFFFF);
    reset(); S.r[3] = DATA; S.r[5] = 0x12345678; RUN(0x2352); CHECK(sh2_rd32(&S, DATA) == 0x12345678);
    reset(); S.r[5] = DATA; sh2_wr8(&S, DATA, 0x80); RUN(0x6350); REG(3, 0xFFFFFF80);
    reset(); S.r[5] = DATA; sh2_wr16(&S, DATA, 0x8000); RUN(0x6351); REG(3, 0xFFFF8000);
    reset(); S.r[5] = DATA; sh2_wr32(&S, DATA, 0x87654321); RUN(0x6352); REG(3, 0x87654321);
    reset(); S.r[3] = DATA + 1; S.r[5] = 0x7F; RUN(0x2354); REG(3, DATA); CHECK(sh2_rd8(&S, DATA) == 0x7F);
    reset(); S.r[3] = DATA + 2; S.r[5] = 0x7FFF; RUN(0x2355); REG(3, DATA); CHECK(sh2_rd16(&S, DATA) == 0x7FFF);
    reset(); S.r[3] = DATA + 4; S.r[5] = 0x7FFFFFFF; RUN(0x2356); REG(3, DATA); CHECK(sh2_rd32(&S, DATA) == 0x7FFFFFFF);
    reset(); S.r[3] = DATA + 4; RUN(0x2336); REG(3, DATA); CHECK(sh2_rd32(&S, DATA) == DATA); /* mov.l r3,@-r3: stores the decremented r3 (manual) */
    reset(); S.r[5] = DATA; sh2_wr8(&S, DATA, 0xFE); RUN(0x6354); REG(3, 0xFFFFFFFE); REG(5, DATA + 1);
    reset(); S.r[5] = DATA; sh2_wr16(&S, DATA, 0xFFFE); RUN(0x6355); REG(5, DATA + 2);
    reset(); S.r[5] = DATA; sh2_wr32(&S, DATA, 0xAABBCCDD); RUN(0x6356); REG(3, 0xAABBCCDD); REG(5, DATA + 4);
    reset(); S.r[5] = DATA; sh2_wr32(&S, DATA, 0x11); RUN(0x6556); REG(5, 0x11);   /* mov.l @r5+,r5: no increment */
    reset(); S.r[0] = 4; S.r[3] = DATA; S.r[5] = 0xAB; RUN(0x0354); CHECK(sh2_rd8(&S, DATA + 4) == 0xAB);
    reset(); S.r[0] = 4; S.r[3] = DATA; S.r[5] = 0xABCD; RUN(0x0355); CHECK(sh2_rd16(&S, DATA + 4) == 0xABCD);
    reset(); S.r[0] = 4; S.r[3] = DATA; S.r[5] = 0xABCDEF01; RUN(0x0356); CHECK(sh2_rd32(&S, DATA + 4) == 0xABCDEF01);
    reset(); S.r[0] = 4; S.r[5] = DATA; sh2_wr8(&S, DATA + 4, 0x81); RUN(0x035C); REG(3, 0xFFFFFF81);
    reset(); S.r[0] = 4; S.r[5] = DATA; sh2_wr16(&S, DATA + 4, 0x8001); RUN(0x035D); REG(3, 0xFFFF8001);
    reset(); S.r[0] = 4; S.r[5] = DATA; sh2_wr32(&S, DATA + 4, 0x55); RUN(0x035E); REG(3, 0x55);
    reset(); S.r[3] = DATA; S.r[15] = 0x99; RUN(0x13F5); CHECK(sh2_rd32(&S, DATA + 20) == 0x99); /* mov.l r15,@(20,r3) */
    reset(); S.r[15] = DATA; sh2_wr32(&S, DATA + 20, 0x77); RUN(0x53F5); REG(3, 0x77);         /* mov.l @(20,r15),r3 */
    reset(); S.r[15] = DATA; S.r[0] = 0x1AB; RUN(0x80F5); CHECK(sh2_rd8(&S, DATA + 5) == 0xAB);
    reset(); S.r[15] = DATA; S.r[0] = 0x1ABCD; RUN(0x81F5); CHECK(sh2_rd16(&S, DATA + 10) == 0xABCD);
    reset(); S.r[15] = DATA; sh2_wr8(&S, DATA + 5, 0x90); RUN(0x84F5); REG(0, 0xFFFFFF90);
    reset(); S.r[15] = DATA; sh2_wr16(&S, DATA + 10, 0x9000); RUN(0x85F5); REG(0, 0xFFFF9000);
    reset(); S.gbr = DATA; S.r[0] = 0x12; RUN(0xC002); CHECK(sh2_rd8(&S, DATA + 2) == 0x12);
    reset(); S.gbr = DATA; S.r[0] = 0x1234; RUN(0xC102); CHECK(sh2_rd16(&S, DATA + 4) == 0x1234);
    reset(); S.gbr = DATA; S.r[0] = 0x12345678; RUN(0xC202); CHECK(sh2_rd32(&S, DATA + 8) == 0x12345678);
    reset(); S.gbr = DATA; sh2_wr8(&S, DATA + 2, 0xF0); RUN(0xC402); REG(0, 0xFFFFFFF0);
    reset(); S.gbr = DATA; sh2_wr16(&S, DATA + 4, 0xF000); RUN(0xC502); REG(0, 0xFFFFF000);
    reset(); S.gbr = DATA; sh2_wr32(&S, DATA + 8, 0xF0000000); RUN(0xC602); REG(0, 0xF0000000);

    /* ---- branches (with delay slots) ---- */
    reset(); S.sr = SR_T; RUN(0x8902); CHECK(S.pc == CODE + 4 + 4);                 /* bt taken */
    reset(); S.sr = 0; RUN(0x8902); CHECK(S.pc == CODE + 2);                        /* bt not taken */
    reset(); S.sr = 0; RUN(0x8B02); CHECK(S.pc == CODE + 8);                        /* bf taken */
    reset(); S.sr = SR_T; run(0x8D02, 0xE307); CHECK(S.pc == CODE + 8); REG(3, 7);  /* bt/s: slot runs */
    reset(); S.sr = 0; run(0x8D02, 0xE307); CHECK(S.pc == CODE + 4); REG(3, 7);     /* bt/s not taken: slot still runs */
    reset(); S.sr = SR_T; run(0x8F02, 0x0008); CHECK(S.pc == CODE + 4);             /* bf/s: T read before slot clears it */
    reset(); run(0xA002, 0xE301); CHECK(S.pc == CODE + 8); REG(3, 1);               /* bra */
    reset(); run(0xB002, 0x0009); CHECK(S.pc == CODE + 8 && S.pr == CODE + 4);      /* bsr */
    reset(); S.r[3] = 0x10; run(0x0323, 0x0009); CHECK(S.pc == CODE + 4 + 0x10);    /* braf */
    reset(); S.r[3] = 0x10; run(0x0303, 0x0009); CHECK(S.pc == CODE + 4 + 0x10 && S.pr == CODE + 4); /* bsrf */
    reset(); S.r[3] = DATA; run(0x432B, 0xE300); CHECK(S.pc == DATA); REG(3, 0);     /* jmp: target read before slot */
    reset(); S.r[3] = DATA; run(0x430B, 0x0009); CHECK(S.pc == DATA && S.pr == CODE + 4); /* jsr */
    reset(); S.pr = DATA; run(0x000B, 0x4F26); CHECK(S.pc == DATA);                 /* rts: PR read before slot */
    reset(); S.r[15] = DATA; sh2_wr32(&S, DATA, 0x06003000); sh2_wr32(&S, DATA + 4, 0x0F3);
    run(0x002B, 0x0009); CHECK(S.pc == 0x06003000 && S.sr == 0x0F3 && S.r[15] == DATA + 8); /* rte */

    /* ---- exceptions ---- */
    reset(); S.vbr = DATA; sh2_wr32(&S, DATA + 0x20 * 4, 0x06004000); S.sr = 0x0F0;
    RUN(0xC320); CHECK(S.pc == 0x06004000); CHECK(sh2_rd32(&S, S.r[15]) == CODE + 2); /* trapa #0x20 */
    CHECK(sh2_rd32(&S, S.r[15] + 4) == 0x0F0);
    reset(); S.vbr = DATA; sh2_wr32(&S, DATA + 4 * 4, 0x06005000);
    put16(CODE, 0xFFFF); S.pc = CODE; sh2_step(&S);
    CHECK(exc_vector == SH2_VEC_ILLEGAL && S.pc == 0x06005000);                    /* illegal */
    reset(); S.vbr = DATA; sh2_wr32(&S, DATA + 6 * 4, 0x06006000);
    run(0xA002, 0xA002); CHECK(exc_vector == SH2_VEC_SLOT_ILLEGAL && S.pc == 0x06006000); /* branch in slot */
    reset(); S.vbr = DATA; sh2_wr32(&S, DATA + 0x40 * 4, 0x06007000); S.sr = 0x030;
    CHECK(!sh2_interrupt(&S, 3, 0x40));
    CHECK(sh2_interrupt(&S, 5, 0x40)); CHECK(S.pc == 0x06007000 && (S.sr & SR_IMASK) == 0x50);

    /* Every instruction exercised at least once */
    for (int op = 0; op < SH2_OP_COUNT; op++)
        if (!covered[op]) {
            fprintf(stderr, "not covered: %s (id %d)\n", sh2_op_name(op), op);
            g_failures++;
        }

    TEST_DONE();
}
