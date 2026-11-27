/* Build-time tool for the differential test: assembles random SH-2 programs, runs the analyser
 * and the code generator on them, and writes
 *   <out>/diff_prog.h          program bytes + entry points
 *   <out>/gen_diff*.{h,c}      generated C
 * The programs are random but deterministic (fixed seeds). No game data involved. */
#include "analysis/analyse.h"
#include "codegen/codegen.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE 0x06010000u
#define PROG_SPAN 0x1000u
#define NPROG 64
#define DATA_BASE 0x06080000u
#define GBR_VALUE (DATA_BASE + 0x800u)

/* ---- tiny assembler -------------------------------------------------------------------------- */

enum { FX_BR8, FX_BR12, FX_PCL, FX_MOVA, FX_OFF16 };

typedef struct { int kind; int at; int label; int tbl; } fixup;

typedef struct {
    uint16_t w[PROG_SPAN / 2];
    int n;
    uint32_t base;
    int label_at[256];
    int nlabels;
    fixup fx[512];
    int nfx;
    uint32_t pool[128];
    int npool;
} asmb;

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int pick(int n) { return (int)(rnd() % (uint32_t)n); }

static void emit(asmb *a, uint16_t w) { a->w[a->n++] = w; }
static int new_label(asmb *a) { a->label_at[a->nlabels] = -1; return a->nlabels++; }
static void bind(asmb *a, int l) { a->label_at[l] = a->n; }
static void fix(asmb *a, int kind, int label) { a->fx[a->nfx++] = (fixup){kind, a->n, label, 0}; }
static void fix_tbl(asmb *a, int label, int tbl_label) { a->fx[a->nfx++] = (fixup){FX_OFF16, a->n, label, tbl_label}; }
static int pool_add(asmb *a, uint32_t v) { a->pool[a->npool] = v; return a->npool++; }

/* mov.l @(pool[i]),rN : the pool index is stored as a negative label (-(i+1)). */
static void movl_pool(asmb *a, int reg, uint32_t value)
{
    int i = pool_add(a, value);
    a->fx[a->nfx++] = (fixup){FX_PCL, a->n, -(i + 1), 0};
    emit(a, (uint16_t)(0xD000 | reg << 8));
}

static void finish(asmb *a)
{
    int pool_at;
    if (a->n & 1)
        emit(a, 0x0009); /* align the pool to 4 bytes */
    pool_at = a->n;
    for (int i = 0; i < a->npool; i++) {
        emit(a, (uint16_t)(a->pool[i] >> 16));
        emit(a, (uint16_t)a->pool[i]);
    }
    for (int i = 0; i < a->nfx; i++) {
        fixup *f = &a->fx[i];
        uint32_t pc = a->base + (uint32_t)f->at * 2;
        int32_t d;
        uint32_t target;
        if (f->kind == FX_PCL) {
            target = a->base + (uint32_t)(pool_at + 2 * (-f->label - 1)) * 2;
            d = (int32_t)(target - ((pc & ~3u) + 4)) / 4;
            if (d < 0 || d > 255) { fprintf(stderr, "pool too far\n"); exit(1); }
            a->w[f->at] |= (uint16_t)d;
            continue;
        }
        target = a->base + (uint32_t)a->label_at[f->label] * 2;
        switch (f->kind) {
        case FX_BR8:
            d = (int32_t)(target - (pc + 4)) / 2;
            if (d < -128 || d > 127) { fprintf(stderr, "br8 out of range\n"); exit(1); }
            a->w[f->at] |= (uint16_t)(d & 0xFF);
            break;
        case FX_BR12:
            d = (int32_t)(target - (pc + 4)) / 2;
            a->w[f->at] |= (uint16_t)(d & 0xFFF);
            break;
        case FX_MOVA:
            d = (int32_t)(target - ((pc & ~3u) + 4)) / 4;
            if ((target & 3) || d < 0 || d > 255) { fprintf(stderr, "bad mova\n"); exit(1); }
            a->w[f->at] |= (uint16_t)d;
            break;
        case FX_OFF16:
            a->w[f->at] = (uint16_t)(int16_t)(target - (a->base + (uint32_t)a->label_at[f->tbl] * 2));
            break;
        }
    }
}

/* ---- random instructions --------------------------------------------------------------------- */

static int dreg(void) { return pick(8); }       /* data registers r0-r7 */
static int preg(void) { return 8 + pick(4); }   /* pointer registers r8-r11 */

static void random_op(asmb *a)
{
    int n = dreg(), m = dreg(), p = preg(), q, k;
    static const uint16_t two[] = {0x300C, 0x300E, 0x300F, 0x3008, 0x300A, 0x300B, 0x2009, 0x200B,
                                   0x200A, 0x6007, 0x600B, 0x600A, 0x600C, 0x600D, 0x600E, 0x600F,
                                   0x6008, 0x6009, 0x200D, 0x6003, 0x3000, 0x3002, 0x3003, 0x3006,
                                   0x3007, 0x200C, 0x2008, 0x0007, 0x200E, 0x200F, 0x300D, 0x3005,
                                   0x2007, 0x3004};
    static const uint16_t one[] = {0x4000, 0x4001, 0x4004, 0x4005, 0x4008, 0x4009, 0x4018, 0x4019,
                                   0x4020, 0x4021, 0x4024, 0x4025, 0x4028, 0x4029, 0x4010, 0x4011,
                                   0x4015, 0x0029, 0x000A, 0x001A};
    static const uint16_t none[] = {0x0008, 0x0018, 0x0028, 0x0019};
    switch (pick(16)) {
    case 0: case 1: case 2: case 3:
        emit(a, (uint16_t)(two[pick((int)(sizeof two / 2))] | n << 8 | m << 4));
        break;
    case 4: case 5:
        emit(a, (uint16_t)(one[pick((int)(sizeof one / 2))] | n << 8));
        break;
    case 6:
        k = pick(5);
        if (k == 0) emit(a, (uint16_t)(0xE000 | n << 8 | (rnd() & 0xFF)));
        else if (k == 1) emit(a, (uint16_t)(0x7000 | n << 8 | (rnd() & 0xFF)));
        else if (k == 2) emit(a, (uint16_t)(0x8800 | (rnd() & 0xFF)));
        else emit(a, (uint16_t)((0xC800 + 0x100 * pick(4)) | (rnd() & 0xFF)));
        break;
    case 7:
        emit(a, none[pick(4)]);
        break;
    case 8: /* @Rn / @Rm loads and stores through pointers */
        k = pick(3);
        if (pick(2)) emit(a, (uint16_t)(0x2000 | p << 8 | m << 4 | k));
        else emit(a, (uint16_t)(0x6000 | n << 8 | p << 4 | k));
        break;
    case 9: /* pre-decrement / post-increment */
        k = pick(3);
        if (pick(2)) emit(a, (uint16_t)(0x2004 | p << 8 | m << 4 | k));
        else emit(a, (uint16_t)(0x6004 | n << 8 | p << 4 | k));
        break;
    case 10: /* displacement forms */
        switch (pick(6)) {
        case 0: emit(a, (uint16_t)(0x1000 | p << 8 | m << 4 | pick(16))); break;
        case 1: emit(a, (uint16_t)(0x5000 | n << 8 | p << 4 | pick(16))); break;
        case 2: emit(a, (uint16_t)(0x8000 | p << 4 | pick(16))); break;
        case 3: emit(a, (uint16_t)(0x8100 | p << 4 | pick(16))); break;
        case 4: emit(a, (uint16_t)(0x8400 | p << 4 | pick(16))); break;
        default: emit(a, (uint16_t)(0x8500 | p << 4 | pick(16))); break;
        }
        break;
    case 11: /* @(r0,Rn): small r0 first */
        emit(a, (uint16_t)(0xE000 | (pick(16) * 4)));
        k = pick(3);
        if (pick(2)) emit(a, (uint16_t)(0x0004 | p << 8 | m << 4 | k));
        else emit(a, (uint16_t)(0x000C | n << 8 | p << 4 | k));
        break;
    case 12: /* GBR forms */
        k = pick(10);
        if (k < 6) emit(a, (uint16_t)((0xC000 + 0x100 * (k < 3 ? k : k + 1)) | (rnd() & 0xFF)));
        else {
            emit(a, (uint16_t)(0xE000 | pick(64)));
            emit(a, (uint16_t)((0xCC00 + 0x100 * (k - 6)) | (rnd() & 0xFF)));
        }
        break;
    case 13: /* mac.l / mac.w / tas.b */
        q = preg();
        while (q == p) q = preg();
        k = pick(3);
        if (k == 0) emit(a, (uint16_t)(0x000F | p << 8 | q << 4));
        else if (k == 1) emit(a, (uint16_t)(0x400F | p << 8 | q << 4));
        else emit(a, (uint16_t)(0x401B | p << 8));
        break;
    case 14: /* literal loads, balanced stack push/pop */
        if (pick(2)) movl_pool(a, n, rnd());
        else { emit(a, (uint16_t)(0x4F02 | 0x10 * pick(2))); emit(a, (uint16_t)(0x4F06 | 0x10 * pick(2))); }
        break;
    default: /* toggle the S bit (exercises ldc sr and the return-to-dispatcher path) */
        emit(a, 0x0002);                       /* stc sr,r0 */
        emit(a, pick(2) ? 0xCB02 : 0xC9F1);    /* or #2,r0  /  and #0xF1,r0 (clears S, keeps T/imask) */
        emit(a, 0x400E);                       /* ldc r0,sr */
        break;
    }
}

static void ops(asmb *a, int lo, int hi)
{
    int k = lo + pick(hi - lo + 1);
    for (int i = 0; i < k; i++)
        random_op(a);
}

/* A simple instruction for delay slots (no branches, no PC-relative). */
static void slot_op(asmb *a)
{
    static const uint16_t two[] = {0x300C, 0x2009, 0x600B, 0x6003};
    switch (pick(4)) {
    case 0: emit(a, (uint16_t)(0x4000 | dreg() << 8)); break;   /* shll rN */
    case 1: emit(a, pick(2) ? 0x0008 : 0x0018); break;          /* clrt / sett */
    default: emit(a, (uint16_t)(two[pick(4)] | dreg() << 8 | dreg() << 4)); break;
    }
}

static void make_program(asmb *a, uint32_t base, uint32_t seed)
{
    int sub = 0, sub_lbl;
    memset(a, 0, sizeof *a);
    a->base = base;
    rng_state = seed * 2654435761u + 1;
    sub_lbl = new_label(a);

    emit(a, 0x4F22);                 /* sts.l pr,@-r15 */
    emit(a, 0x2FE6);                 /* mov.l r14,@-r15 */
    for (int r = 8; r < 12; r++)
        movl_pool(a, r, DATA_BASE + 0x400u + (uint32_t)(r - 8) * 0x200u);
    movl_pool(a, 12, GBR_VALUE);
    emit(a, 0x4C1E);                 /* ldc r12,gbr */
    for (int r = 0; r < 8; r++)
        movl_pool(a, r, rnd());

    for (int b = 0; b < 10; b++) {
        int l, l2, join, tbl, cases[4];
        ops(a, 3, 7);
        switch (pick(8)) {
        case 0: /* forward bt/bf */
            l = new_label(a);
            fix(a, FX_BR8, l); emit(a, pick(2) ? 0x8900 : 0x8B00);
            ops(a, 1, 3);
            bind(a, l);
            break;
        case 1: /* forward bt/s / bf/s with a slot */
            l = new_label(a);
            fix(a, FX_BR8, l); emit(a, pick(2) ? 0x8D00 : 0x8F00);
            slot_op(a);
            ops(a, 1, 3);
            bind(a, l);
            break;
        case 2: /* counted loop */
            emit(a, (uint16_t)(0xED00 | (2 + pick(4))));  /* mov #c,r13 */
            l = new_label(a);
            bind(a, l);
            ops(a, 1, 3);
            emit(a, 0x4D10);                              /* dt r13 */
            if (pick(2)) { fix(a, FX_BR8, l); emit(a, 0x8B00); }          /* bf */
            else { fix(a, FX_BR8, l); emit(a, 0x8F00); slot_op(a); }     /* bf/s */
            break;
        case 3: /* bsr */
            fix(a, FX_BR12, sub_lbl); emit(a, 0xB000);
            slot_op(a);
            sub = 1;
            break;
        case 4: /* jsr through a literal */
            a->fx[a->nfx++] = (fixup){FX_PCL, a->n, -(pool_add(a, 0) + 1), 0};
            a->pool[a->npool - 1] = 0xFFFFFFFFu; /* patched after layout (sub address) */
            emit(a, 0xDC00);   /* mov.l @(lit),r12 */
            emit(a, 0x4C0B);   /* jsr @r12 */
            slot_op(a);
            sub = 1;
            break;
        case 5: /* switch through a 16-bit offset table */
            join = new_label(a);
            tbl = new_label(a);
            emit(a, 0x6013);   /* mov r1,r0 */
            emit(a, 0xC903);   /* and #3,r0 */
            emit(a, 0x6603);   /* mov r0,r6 */
            emit(a, 0x366C);   /* add r6,r6 */
            if ((a->base + (uint32_t)a->n * 2 + 2 + 2 + 2 + 2 + 2) & 2) emit(a, 0x0009); /* align table */
            fix(a, FX_MOVA, tbl); emit(a, 0xC700);  /* mova tbl,r0 */
            emit(a, 0x066D);   /* mov.w @(r0,r6),r6 */
            emit(a, 0x306C);   /* add r6,r0 */
            emit(a, 0x402B);   /* jmp @r0 */
            emit(a, 0x0009);   /* nop */
            bind(a, tbl);
            for (int c = 0; c < 4; c++) { cases[c] = new_label(a); fix_tbl(a, cases[c], tbl); emit(a, 0); }
            for (int c = 0; c < 4; c++) {
                bind(a, cases[c]);
                ops(a, 1, 2);
                fix(a, FX_BR12, join); emit(a, 0xA000);
                emit(a, 0x0009);
            }
            bind(a, join);
            break;
        case 6: /* bra over a few instructions */
            l2 = new_label(a);
            fix(a, FX_BR12, l2); emit(a, 0xA000);
            slot_op(a);
            ops(a, 1, 2);
            bind(a, l2);
            break;
        default:
            break;
        }
    }
    emit(a, 0x6EF6);   /* mov.l @r15+,r14 */
    emit(a, 0x4F26);   /* lds.l @r15+,pr */
    emit(a, 0x000B);   /* rts */
    slot_op(a);

    bind(a, sub_lbl);  /* subroutine (always emitted so labels resolve) */
    ops(a, 2, 5);
    emit(a, 0x000B);
    slot_op(a);
    (void)sub;

    for (int i = 0; i < a->npool; i++)
        if (a->pool[i] == 0xFFFFFFFFu)
            a->pool[i] = base + (uint32_t)a->label_at[sub_lbl] * 2;
    finish(a);
    if ((uint32_t)a->n * 2 > PROG_SPAN) { fprintf(stderr, "program too large\n"); exit(1); }
}

int main(int argc, char **argv)
{
    static asmb progs[NPROG];
    static uint8_t image[NPROG * PROG_SPAN];
    mem_image im;
    analysis an;
    cg_options opt = {"diff", "test", 100000};
    cg_stats st;
    FILE *h;
    char path[SBPC_PATH_MAX];

    if (argc < 2) { fprintf(stderr, "usage: diffgen <outdir>\n"); return 2; }
    memset(image, 0, sizeof image);
    for (int p = 0; p < NPROG; p++) {
        make_program(&progs[p], CODE_BASE + (uint32_t)p * PROG_SPAN, (uint32_t)p + 1);
        for (int i = 0; i < progs[p].n; i++) {
            image[p * PROG_SPAN + i * 2] = (uint8_t)(progs[p].w[i] >> 8);
            image[p * PROG_SPAN + i * 2 + 1] = (uint8_t)progs[p].w[i];
        }
    }

    img_init(&im);
    img_add(&im, "diff", CODE_BASE, image, sizeof image);
    an_init(&an, &im);
    for (int p = 0; p < NPROG; p++)
        an_add_entry(&an, CODE_BASE + (uint32_t)p * PROG_SPAN, FN_ENTRY);
    an_run(&an);
    if (!cg_generate(&an, &opt, argv[1], &st)) { fprintf(stderr, "codegen failed\n"); return 1; }

    if (!fs_join(path, argv[1], "diff_prog.h") || !(h = fs_open(path, "w"))) return 1;
    fprintf(h, "/* Generated by diffgen: random test programs (no game data). */\n");
    fprintf(h, "#define DIFF_NPROG %d\n#define DIFF_CODE_BASE 0x%08Xu\n#define DIFF_PROG_SPAN 0x%Xu\n",
            NPROG, CODE_BASE, PROG_SPAN);
    fprintf(h, "#define DIFF_DATA_BASE 0x%08Xu\n#define DIFF_SWITCHES %d\n#define DIFF_FUNCS %d\n",
            DATA_BASE, an.njtables, an.nfuncs);
    fprintf(h, "static const unsigned char DIFF_IMAGE[%u] = {", (unsigned)sizeof image);
    for (size_t i = 0; i < sizeof image; i++)
        fprintf(h, "%s%u,", i % 32 ? "" : "\n", image[i]);
    fprintf(h, "\n};\n");
    fclose(h);
    printf("diffgen: %d programs, %d functions, %d switch tables, %d instructions generated, %d fallbacks\n",
           NPROG, an.nfuncs, an.njtables, st.instructions, st.fallbacks);
    an_free(&an);
    img_free(&im);
    return 0;
}
