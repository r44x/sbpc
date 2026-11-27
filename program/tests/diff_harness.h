/* Shared by test_diff (generated code compiled into the test) and test_tcc (the same code built by
 * the bundled TinyCC and loaded as a module): every random program from diffgen is run
 *   A) entirely in the interpreter,
 *   B) through the generated code with one large time slice,
 *   C) through the generated code with tiny time slices (yield/resume everywhere),
 * and the CPU state and all of RAM must match exactly. Include after test.h. */
#ifndef SBPC_TESTS_DIFF_HARNESS_H
#define SBPC_TESTS_DIFF_HARNESS_H

#include "runtime/dispatch.h"
#include "sh2/sh2_interp.h"
#include "diff_prog.h"

const sbpc_module *sbpc_module_diff(const sbpc_rt *rt);

#define STOP 0x06000F00u
static uint8_t ram[3][1 << 20];
static int bad_access;

/* Anything outside RAM is a test bug: record it and return 0. */
static uint8_t bad8(sh2_state *s, uint32_t a) { (void)s; (void)a; bad_access++; return 0; }
static uint16_t bad16(sh2_state *s, uint32_t a) { (void)s; (void)a; bad_access++; return 0; }
static uint32_t bad32(sh2_state *s, uint32_t a) { (void)s; (void)a; bad_access++; return 0; }
static void badw8(sh2_state *s, uint32_t a, uint8_t v) { (void)s; (void)a; (void)v; bad_access++; }
static void badw16(sh2_state *s, uint32_t a, uint16_t v) { (void)s; (void)a; (void)v; bad_access++; }
static void badw32(sh2_state *s, uint32_t a, uint32_t v) { (void)s; (void)a; (void)v; bad_access++; }
static const sh2_bus BAD_BUS = {bad8, bad16, bad32, badw8, badw16, badw32};

static void setup(sh2_state *s, uint8_t *mem, int prog, rt_dispatch *d)
{
    uint32_t x = 0x12345u + (uint32_t)prog * 977u;
    memset(s, 0, sizeof *s);
    memset(mem, 0, 1 << 20);
    for (uint32_t p = 0x060; p < 0x080; p++)
        s->page[p] = mem;
    memcpy(mem + (DIFF_CODE_BASE - 0x06000000u), DIFF_IMAGE, sizeof DIFF_IMAGE);
    for (uint32_t i = 0; i < 0x1000; i++) { /* deterministic data window */
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        mem[(DIFF_DATA_BASE - 0x06000000u) + i] = (uint8_t)x;
    }
    sh2_reset_state(s);
    s->bus = BAD_BUS;
    s->sr = 0x0F0;
    s->pc = DIFF_CODE_BASE + (uint32_t)prog * DIFF_PROG_SPAN;
    s->pr = STOP;
    s->r[15] = 0x06090000u;
    s->dispatch = d;
}

static int same(const sh2_state *a, const uint8_t *ma, const sh2_state *b, const uint8_t *mb,
                const char *what, int prog)
{
    int ok = 1;
    for (int i = 0; i < 16; i++)
        if (a->r[i] != b->r[i]) { fprintf(stderr, "prog %d %s: r%d %08X vs %08X\n", prog, what, i, a->r[i], b->r[i]); ok = 0; }
    if (a->sr != b->sr) { fprintf(stderr, "prog %d %s: sr %03X vs %03X\n", prog, what, a->sr, b->sr); ok = 0; }
    if (a->gbr != b->gbr || a->mach != b->mach || a->macl != b->macl || a->pr != b->pr || a->pc != b->pc) {
        fprintf(stderr, "prog %d %s: gbr/mach/macl/pr/pc differ\n", prog, what);
        ok = 0;
    }
    if (memcmp(ma, mb, 1 << 20) != 0) {
        for (int i = 0; i < (1 << 20); i++)
            if (ma[i] != mb[i]) { fprintf(stderr, "prog %d %s: RAM differs first at %08X\n", prog, what, 0x06000000 + i); break; }
        ok = 0;
    }
    return ok;
}

/* Runs every program three ways with the module(s) already added to d. */
static void diff_run_all(rt_dispatch *d)
{
    static sh2_state A, B, C;
    uint64_t native = 0, interp = 0;
    {   volatile int switches = DIFF_SWITCHES;   /* volatile: not a constant condition */
        CHECK(switches >= 1);
    }

    for (int p = 0; p < DIFF_NPROG; p++) {
        int steps = 0;
        /* A: interpreter */
        setup(&A, ram[0], p, NULL);
        while (A.pc != STOP && steps++ < 1000000) {
            int c = sh2_step(&A);
            A.total_cycles += (uint64_t)c;
        }
        CHECK(A.pc == STOP);

        /* B: generated code, one big slice */
        setup(&B, ram[1], p, d);
        d->native_calls = d->interp_steps = 0;
        B.cycles = 100000000;
        rt_run_until(&B, STOP);
        native += d->native_calls;
        interp += d->interp_steps;
        CHECK(same(&A, ram[0], &B, ram[1], "big slice", p));

        /* C: generated code, 5-cycle slices */
        setup(&C, ram[2], p, d);
        for (int guard = 0; C.pc != STOP && guard < 1000000; guard++) {
            C.cycles = 5;
            rt_run_until(&C, STOP);
        }
        CHECK(same(&A, ram[0], &C, ram[2], "small slices", p));
    }
    CHECK(bad_access == 0);
    printf("%d programs, %d functions, %d switch tables; native entries %llu, interpreted steps %llu\n",
           DIFF_NPROG, DIFF_FUNCS, DIFF_SWITCHES, (unsigned long long)native, (unsigned long long)interp);
    /* With one big slice the generated code should do nearly all the work. */
    CHECK(interp * 20 < native + interp + 1 || interp < 200);
}

#endif
