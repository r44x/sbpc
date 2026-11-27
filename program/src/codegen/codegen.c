#include "codegen/codegen.h"
#include "core/fs.h"
#include "core/log.h"
#include "sh2/sh2_decode.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Names of the sh2_x_* functions, indexed by instruction id (NULL = control flow). */
static const char *const OPNAME[SH2_OP_COUNT] = {
#define X(id) [SH2_##id] = #id
    X(CLRT), X(CLRMAC), X(DIV0U), X(NOP), X(SETT), X(SLEEP),
    X(MOVT), X(STC_SR), X(STC_GBR), X(STC_VBR), X(STS_MACH), X(STS_MACL), X(STS_PR),
    X(DT), X(CMP_PZ), X(CMP_PL), X(TAS),
    X(ROTL), X(ROTR), X(ROTCL), X(ROTCR), X(SHAL), X(SHAR), X(SHLL), X(SHLR),
    X(SHLL2), X(SHLR2), X(SHLL8), X(SHLR8), X(SHLL16), X(SHLR16),
    X(LDC_SR), X(LDC_GBR), X(LDC_VBR), X(LDCL_SR), X(LDCL_GBR), X(LDCL_VBR),
    X(LDS_MACH), X(LDS_MACL), X(LDS_PR), X(LDSL_MACH), X(LDSL_MACL), X(LDSL_PR),
    X(STCL_SR), X(STCL_GBR), X(STCL_VBR), X(STSL_MACH), X(STSL_MACL), X(STSL_PR),
    X(MOVB_S0), X(MOVW_S0), X(MOVL_S0), X(MOVB_L0), X(MOVW_L0), X(MOVL_L0),
    X(MULL), X(MACL),
    X(MOVB_S), X(MOVW_S), X(MOVL_S), X(MOVB_M), X(MOVW_M), X(MOVL_M),
    X(DIV0S), X(TST), X(AND), X(XOR), X(OR), X(CMP_STR), X(XTRCT), X(MULU), X(MULS),
    X(CMP_EQ), X(CMP_HS), X(CMP_GE), X(DIV1), X(DMULU), X(CMP_HI), X(CMP_GT),
    X(SUB), X(SUBC), X(SUBV), X(ADD), X(DMULS), X(ADDC), X(ADDV), X(MACW),
    X(MOVB_L), X(MOVW_L), X(MOVL_L), X(MOV), X(MOVB_P), X(MOVW_P), X(MOVL_P),
    X(NOT), X(SWAPB), X(SWAPW), X(NEGC), X(NEG), X(EXTUB), X(EXTUW), X(EXTSB), X(EXTSW),
    X(MOVL_S4), X(MOVL_L4), X(ADDI), X(MOVW_PC), X(MOVL_PC), X(MOVI),
    X(MOVB_S4), X(MOVW_S4), X(MOVB_L4), X(MOVW_L4), X(CMP_EQI),
    X(MOVB_SG), X(MOVW_SG), X(MOVL_SG), X(MOVB_LG), X(MOVW_LG), X(MOVL_LG),
    X(MOVA), X(TSTI), X(ANDI), X(XORI), X(ORI), X(TSTB), X(ANDB), X(XORB), X(ORB),
#undef X
};

/* ---- sorted address sets ----------------------------------------------------------------- */

typedef struct {
    uint32_t *v;
    int n, cap;
} aset;

static void aset_add(aset *s, uint32_t a)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        uint32_t *nv = realloc(s->v, (size_t)cap * sizeof *nv);
        if (!nv)
            return;
        s->v = nv;
        s->cap = cap;
    }
    s->v[s->n++] = a;
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

static void aset_finish(aset *s) /* sort + unique */
{
    int o = 0;
    qsort(s->v, (size_t)s->n, sizeof *s->v, cmp_u32);
    for (int i = 0; i < s->n; i++)
        if (o == 0 || s->v[o - 1] != s->v[i])
            s->v[o++] = s->v[i];
    s->n = o;
}

static bool aset_has(const aset *s, uint32_t a)
{
    return bsearch(&a, s->v, (size_t)s->n, sizeof *s->v, cmp_u32) != NULL;
}

/* FNV-1a over guest bytes; the runtime computes the same over memory (sbpc_rt.h). */
static uint32_t sbpc_code_hash_image(mem_image *im, uint32_t start, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t w = 0;
        img_read16(im, start + i, &w);
        h = (h ^ (w >> 8)) * 16777619u;
        h = (h ^ (w & 0xFF)) * 16777619u;
    }
    return h;
}

/* ---- per-function walk --------------------------------------------------------------------- */

typedef struct {
    uint32_t start, len, hash;
} code_range;

typedef struct {
    analysis *a;
    aset entries;        /* all function entry points */
    code_range *ranges;  /* per function, for the runtime's code check */
    FILE *f;
    cg_stats *st;
} cg;

typedef struct {
    uint32_t entry;
    aset body;           /* instruction addresses (branch + its slot both included) */
    aset labels;         /* addresses that need a label */
    aset resume;         /* addresses the entry switch can jump to */
} fn_ctx;

static bool decode_at(cg *c, uint32_t pc, sh2_insn *in)
{
    uint16_t raw;
    return !(pc & 1) && img_read16(c->a->im, pc, &raw) && sh2_decode(raw, pc, in) &&
           img_kind(c->a->im, pc) != BYTE_DATA;
}

static const an_jtable *jtable_at(cg *c, uint32_t pc)
{
    for (int i = 0; i < c->a->njtables; i++)
        if (c->a->jtables[i].jmp_pc == pc)
            return &c->a->jtables[i];
    return NULL;
}

/* A target that is another function's entry is a tail call, not part of this body. */
static bool is_other_entry(cg *c, const fn_ctx *fc, uint32_t t)
{
    return t != fc->entry && aset_has(&c->entries, t);
}

static void walk(cg *c, fn_ctx *fc)
{
    aset work = {0};
    aset seen = {0};
    aset_add(&work, fc->entry);
    while (work.n > 0) {
        uint32_t pc = work.v[--work.n];
        for (;;) {
            sh2_insn in;
            bool dup = false;
            for (int i = 0; i < seen.n && !dup; i++) dup = seen.v[i] == pc; /* small per-function */
            if (dup || !decode_at(c, pc, &in))
                break;
            aset_add(&seen, pc);
            aset_add(&fc->body, pc);
            if (!(in.flags & SH2F_BRANCH)) {
                if (in.op == SH2_SLEEP || in.op == SH2_LDC_SR || in.op == SH2_LDCL_SR) {
                    aset_add(&fc->resume, pc + 2); /* we return to the dispatcher after these */
                    aset_add(&fc->labels, pc + 2);
                }
                pc += 2;
                continue;
            }
            if (in.flags & SH2F_DELAY)
                aset_add(&fc->body, pc + 2);
            if (in.op == SH2_TRAPA) {
                aset_add(&fc->resume, pc + 2);
                aset_add(&fc->labels, pc + 2);
                pc += 2;
                continue;
            }
            if (in.flags & SH2F_CALL) {
                aset_add(&fc->resume, pc + 4);
                aset_add(&fc->labels, pc + 4);
                pc += 4;
                continue;
            }
            if (in.flags & SH2F_RETURN)
                break;
            if (in.op == SH2_JMP || in.op == SH2_BRAF) {
                const an_jtable *t = jtable_at(c, pc);
                if (t)
                    for (int i = 0; i < t->count; i++) {
                        uint32_t tg = an_jtable_target(c->a, t, i);
                        aset_add(&fc->resume, tg);
                        aset_add(&fc->labels, tg);
                        aset_add(&work, tg);
                    }
                break;
            }
            /* bt/bf/bt.s/bf.s/bra */
            if (img_find(c->a->im, in.target, 2) && !is_other_entry(c, fc, in.target)) {
                aset_add(&fc->labels, in.target);
                if (in.target <= pc)
                    aset_add(&fc->resume, in.target); /* yield point */
                aset_add(&work, in.target);
            }
            if (in.op == SH2_BRA)
                break;
            pc += (in.flags & SH2F_DELAY) ? 4 : 2;
            aset_add(&fc->labels, pc); /* conditional fall-through */
        }
    }
    free(work.v);
    free(seen.v);
    aset_add(&fc->resume, fc->entry);
    aset_add(&fc->labels, fc->entry);
    aset_finish(&fc->body);
    aset_finish(&fc->resume);
    aset_finish(&fc->labels);
}

/* ---- emission -------------------------------------------------------------------------------- */

static void out(cg *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(c->f, fmt, ap);
    va_end(ap);
}

static void emit_simple(cg *c, const sh2_insn *in, int cycles)
{
    out(c, "    s->cycles -= %d; sh2_x_%s(s, %u, %u, %d, 0x%08Xu);\n", cycles, OPNAME[in->op],
        in->n, in->m, (int)(in->imm + in->disp), (unsigned)in->target);
}

/* Emits the delay slot of the branch at pc. Returns false if it can't be generated. */
static bool emit_slot(cg *c, uint32_t pc)
{
    sh2_insn sl;
    if (!decode_at(c, pc + 2, &sl) || (sl.flags & SH2F_NO_SLOT) || !OPNAME[sl.op])
        return false;
    emit_simple(c, &sl, 1);
    return true;
}

static void emit_fallback(cg *c, uint32_t pc)
{
    out(c, "    s->pc = 0x%08Xu; return; /* interpreter */\n", (unsigned)pc);
    c->st->fallbacks++;
}

/* Jump to t: inside the body -> goto (with a yield check when going backwards);
 * another function's entry -> tail call; otherwise leave to the dispatcher. */
static void emit_goto(cg *c, const fn_ctx *fc, uint32_t pc, uint32_t t)
{
    if (aset_has(&fc->body, t) && aset_has(&fc->labels, t)) {
        if (t <= pc)
            out(c, "    if (s->cycles <= 0) { s->pc = 0x%08Xu; return; }\n", (unsigned)t);
        out(c, "    goto L_%08X;\n", (unsigned)t);
    } else if (aset_has(&c->entries, t)) {
        out(c, "    s->pc = 0x%08Xu; f_%08X(s); return;\n", (unsigned)t, (unsigned)t);
    } else {
        out(c, "    s->pc = 0x%08Xu; return;\n", (unsigned)t);
    }
}

/* Looks back a few instructions for "mov.l @(lit),rN" to guess an indirect call target. */
static bool guess_reg_const(cg *c, const fn_ctx *fc, uint32_t pc, int reg, uint32_t *out_v)
{
    for (uint32_t back = 2; back <= 12; back += 2) {
        sh2_insn in;
        if (!aset_has(&fc->body, pc - back) || !decode_at(c, pc - back, &in))
            return false;
        if (in.op == SH2_MOVL_PC && in.n == reg)
            return img_read32(c->a->im, in.target, out_v);
        if (in.flags & SH2F_BRANCH)
            return false;
        if (in.n == reg && in.op != SH2_MOVL_S && in.op != SH2_MOVW_S && in.op != SH2_MOVB_S &&
            in.op != SH2_MOVL_S0 && in.op != SH2_MOVL_S4 && in.op != SH2_TST && in.op != SH2_CMP_EQ)
            return false; /* register overwritten (conservatively) */
    }
    return false;
}

static void emit_call(cg *c, const fn_ctx *fc, uint32_t pc, const char *target_expr, bool direct,
                      bool known, uint32_t k)
{
    uint32_t ret = pc + 4;
    out(c, "    { uint32_t t = %s; s->pr = 0x%08Xu;\n", target_expr, (unsigned)ret);
    if (!emit_slot(c, pc)) {
        out(c, "    }\n");
        emit_fallback(c, pc);
        return;
    }
    out(c, "    s->cycles -= 2; s->pc = t;\n");
    if (known && aset_has(&c->entries, k)) {
        if (direct) {
            out(c, "    f_%08X(s); }\n", (unsigned)k);
            c->st->direct_calls++;
        } else {
            out(c, "    if (t == 0x%08Xu) f_%08X(s); else sbpc_rt_api->call(s, 0x%08Xu); }\n",
                (unsigned)k, (unsigned)k, (unsigned)ret);
            c->st->guarded_calls++;
        }
    } else {
        out(c, "    sbpc_rt_api->call(s, 0x%08Xu); }\n", (unsigned)ret);
        c->st->dynamic_calls++;
    }
    out(c, "    if (s->pc != 0x%08Xu) return;\n", (unsigned)ret);
    (void)fc;
}

static void emit_function(cg *c, fn_ctx *fc, int idx)
{
    bool has_dyn = false;
    for (int i = 0; i < fc->body.n && !has_dyn; i++) {
        sh2_insn in;
        has_dyn = decode_at(c, fc->body.v[i], &in) && (in.op == SH2_JMP || in.op == SH2_BRAF);
    }
    out(c, "\nvoid f_%08X(sh2_state *s)\n{\n    SBPC_CHECK(%d);\n", (unsigned)fc->entry, idx);
    out(c, "%s    switch (s->pc) {\n", has_dyn ? "dispatch:\n" : "");
    for (int i = 0; i < fc->resume.n; i++)
        if (aset_has(&fc->body, fc->resume.v[i]))
            out(c, "    case 0x%08Xu: goto L_%08X;\n", (unsigned)fc->resume.v[i], (unsigned)fc->resume.v[i]);
    out(c, "    default: return;\n    }\n");

    for (int i = 0; i < fc->body.n; i++) {
        uint32_t pc = fc->body.v[i];
        sh2_insn in;
        char expr[64];
        uint32_t k = 0;
        bool known;

        if (aset_has(&fc->labels, pc))
            out(c, "L_%08X:\n", (unsigned)pc);
        if (!decode_at(c, pc, &in)) {
            emit_fallback(c, pc);
            continue;
        }
        c->st->instructions++;

        if (!(in.flags & SH2F_BRANCH)) {
            emit_simple(c, &in, (in.op == SH2_MACL || in.op == SH2_MACW || in.op == SH2_DMULS ||
                                 in.op == SH2_DMULU) ? 2 : 1);
            if (in.op == SH2_SLEEP || in.op == SH2_LDC_SR || in.op == SH2_LDCL_SR) {
                out(c, "    s->pc = 0x%08Xu; return;\n", (unsigned)(pc + 2));
                continue;
            }
            /* fall through: if the next emitted instruction isn't pc+2, jump there */
            if (i + 1 >= fc->body.n || fc->body.v[i + 1] != pc + 2)
                emit_goto(c, fc, pc, pc + 2);
            continue;
        }

        /* Skip the delay slot entry in the body list: it's emitted with its branch. */
        if ((in.flags & SH2F_DELAY) && i + 1 < fc->body.n && fc->body.v[i + 1] == pc + 2 &&
            !aset_has(&fc->labels, pc + 2))
            i++;

        switch (in.op) {
        case SH2_BT: case SH2_BF:
            out(c, "    s->cycles -= 1;\n    if (%s(s->sr & 1)) { s->cycles -= 2;\n", in.op == SH2_BT ? "" : "!");
            emit_goto(c, fc, pc, in.target);
            out(c, "    }\n");
            if (i + 1 >= fc->body.n || fc->body.v[i + 1] != pc + 2)
                emit_goto(c, fc, pc, pc + 2);
            break;
        case SH2_BTS: case SH2_BFS:
            out(c, "    { int taken = %s(s->sr & 1);\n", in.op == SH2_BTS ? "" : "!");
            if (!emit_slot(c, pc)) { out(c, "    (void)taken; }\n"); emit_fallback(c, pc); break; }
            out(c, "    s->cycles -= 1;\n    if (taken) { s->cycles -= 1;\n");
            emit_goto(c, fc, pc, in.target);
            out(c, "    } }\n");
            if (i + 1 >= fc->body.n || fc->body.v[i + 1] != pc + 4)
                emit_goto(c, fc, pc, pc + 4);
            break;
        case SH2_BRA:
            if (!emit_slot(c, pc)) { emit_fallback(c, pc); break; }
            out(c, "    s->cycles -= 2;\n");
            emit_goto(c, fc, pc, in.target);
            break;
        case SH2_BSR:
            snprintf(expr, sizeof expr, "0x%08Xu", (unsigned)in.target);
            emit_call(c, fc, pc, expr, true, true, in.target);
            if (i + 1 >= fc->body.n || fc->body.v[i + 1] != pc + 4)
                emit_goto(c, fc, pc, pc + 4);
            break;
        case SH2_JSR: case SH2_BSRF:
            if (in.op == SH2_JSR)
                snprintf(expr, sizeof expr, "s->r[%u]", in.n);
            else
                snprintf(expr, sizeof expr, "0x%08Xu + s->r[%u]", (unsigned)(pc + 4), in.n);
            known = in.op == SH2_JSR && guess_reg_const(c, fc, pc, in.n, &k);
            if (known)
                k &= 0x1FFFFFFFu;
            emit_call(c, fc, pc, expr, false, known, k);
            if (i + 1 >= fc->body.n || fc->body.v[i + 1] != pc + 4)
                emit_goto(c, fc, pc, pc + 4);
            break;
        case SH2_JMP: case SH2_BRAF:
            if (in.op == SH2_JMP)
                out(c, "    { uint32_t t = s->r[%u];\n", in.n);
            else
                out(c, "    { uint32_t t = 0x%08Xu + s->r[%u];\n", (unsigned)(pc + 4), in.n);
            if (!emit_slot(c, pc)) { out(c, "    (void)t; }\n"); emit_fallback(c, pc); break; }
            known = in.op == SH2_JMP && !jtable_at(c, pc) && guess_reg_const(c, fc, pc, in.n, &k) &&
                    aset_has(&c->entries, k & 0x1FFFFFFFu);
            if (known)
                out(c, "    s->cycles -= 2; s->pc = t;\n    if (t == 0x%08Xu) { f_%08X(s); return; }\n"
                       "    if (s->cycles <= 0) return;\n    goto dispatch; }\n",
                    (unsigned)(k & 0x1FFFFFFFu), (unsigned)(k & 0x1FFFFFFFu));
            else
                out(c, "    s->cycles -= 2; s->pc = t;\n    if (s->cycles <= 0) return;\n    goto dispatch; }\n");
            c->st->dynamic_jumps++;
            break;
        case SH2_RTS:
            out(c, "    { uint32_t t = s->pr;\n");
            if (!emit_slot(c, pc)) { out(c, "    (void)t; }\n"); emit_fallback(c, pc); break; }
            out(c, "    s->cycles -= 2; s->pc = t; return; }\n");
            break;
        case SH2_RTE:
            out(c, "    { uint32_t t = sh2_rd32(s, s->r[15]); s->r[15] += 4;\n"
                   "    s->sr = sh2_rd32(s, s->r[15]) & SR_MASK; s->r[15] += 4;\n");
            if (!emit_slot(c, pc)) { out(c, "    (void)t; }\n"); emit_fallback(c, pc); break; }
            out(c, "    s->cycles -= 4; s->pc = t; return; }\n");
            break;
        case SH2_TRAPA:
            out(c, "    s->cycles -= 8; sbpc_rt_api->trap(s, %d, 0x%08Xu); return;\n", (int)in.imm,
                (unsigned)(pc + 2));
            break;
        default:
            emit_fallback(c, pc);
            break;
        }
    }
    out(c, "}\n");
}

/* ---- driver -------------------------------------------------------------------------------- */

typedef struct {
    uint32_t addr;
    uint32_t fn;
} resume_entry;

static int cmp_resume(const void *x, const void *y)
{
    const resume_entry *a = x, *b = y;
    if (a->addr != b->addr)
        return a->addr < b->addr ? -1 : 1;
    /* prefer the function whose entry is this address */
    return (a->fn != a->addr) - (b->fn != b->addr);
}

bool cg_generate(analysis *a, const cg_options *opt, const char *out_dir, cg_stats *st)
{
    cg c;
    char path[SBPC_PATH_MAX], name[96];
    FILE *list, *hdr;
    int per_file = opt->funcs_per_file > 0 ? opt->funcs_per_file : 400;
    resume_entry *res = NULL;
    int nres = 0, capres = 0, file_no = 0;

    memset(&c, 0, sizeof c);
    memset(st, 0, sizeof *st);
    c.a = a;
    c.st = st;
    for (int i = 0; i < a->nfuncs; i++)
        aset_add(&c.entries, a->funcs[i].entry);
    aset_finish(&c.entries);
    c.ranges = calloc((size_t)c.entries.n + 1, sizeof *c.ranges);
    if (!c.ranges)
        return false;

    if (!fs_mkdirs(out_dir))
        return false;

    /* header with prototypes */
    snprintf(name, sizeof name, "gen_%s.h", opt->module);
    if (!fs_join(path, out_dir, name) || !(hdr = fs_open(path, "w")))
        return false;
    fprintf(hdr, "/* Generated by SBPC from your own disc. Do not distribute. */\n"
                 "#include \"sh2_ops.h\"\n#include \"sbpc_rt.h\"\n"
                 "extern const sbpc_rt *sbpc_rt_api;\n"
                 "#define sbpc_ok sbpc_ok_%s\n#define sbpc_code_tab sbpc_code_tab_%s\n"
                 "extern uint32_t sbpc_ok[];\nextern const sbpc_code sbpc_code_tab[];\n", opt->module, opt->module);
    for (int i = 0; i < c.entries.n; i++)
        fprintf(hdr, "void f_%08X(sh2_state *s);\n", (unsigned)c.entries.v[i]);
    fclose(hdr);

    snprintf(name, sizeof name, "gen_%s.files", opt->module);
    if (!fs_join(path, out_dir, name) || !(list = fs_open(path, "w")))
        return false;

    for (int i = 0; i < c.entries.n; i++) {
        fn_ctx fc;
        if (i % per_file == 0) {
            if (c.f)
                fclose(c.f);
            snprintf(name, sizeof name, "gen_%s_%03d.c", opt->module, file_no++);
            if (!fs_join(path, out_dir, name) || !(c.f = fs_open(path, "w"))) {
                fclose(list);
                return false;
            }
            fprintf(list, "%s\n", name);
            out(&c, "/* Generated by SBPC from your own disc. Do not distribute. */\n#include \"gen_%s.h\"\n",
                opt->module);
            st->files++;
        }
        memset(&fc, 0, sizeof fc);
        fc.entry = c.entries.v[i];
        walk(&c, &fc);
        {   /* the bytes this function was generated from: the runtime checks them before running it */
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (int k = 0; k < fc.body.n; k++) {
                if (fc.body.v[k] < lo) lo = fc.body.v[k];
                if (fc.body.v[k] + 2 > hi) hi = fc.body.v[k] + 2;
            }
            if (lo > hi) lo = hi = fc.entry;
            c.ranges[i].start = lo;
            c.ranges[i].len = hi - lo;
            c.ranges[i].hash = sbpc_code_hash_image(a->im, lo, hi - lo);
        }
        emit_function(&c, &fc, i);
        st->functions++;
        for (int r = 0; r < fc.resume.n; r++) {
            if (!aset_has(&fc.body, fc.resume.v[r]))
                continue;
            if (nres == capres) {
                capres = capres ? capres * 2 : 1024;
                res = realloc(res, (size_t)capres * sizeof *res);
                if (!res)
                    return false;
            }
            res[nres++] = (resume_entry){fc.resume.v[r], fc.entry};
        }
        free(fc.body.v);
        free(fc.labels.v);
        free(fc.resume.v);
    }
    if (c.f)
        fclose(c.f);

    /* table: one entry per resumable address (first function wins on duplicates) */
    qsort(res, (size_t)nres, sizeof *res, cmp_resume);
    snprintf(name, sizeof name, "gen_%s_table.c", opt->module);
    if (!fs_join(path, out_dir, name) || !(c.f = fs_open(path, "w"))) {
        free(res);
        fclose(list);
        return false;
    }
    fprintf(list, "%s\n", name);
    fclose(list);
    out(&c, "/* Generated by SBPC from your own disc. Do not distribute. */\n#include \"gen_%s.h\"\n"
            "#if defined(_WIN32)\n#define SBPC_EXPORT __declspec(dllexport)\n#else\n"
            "#define SBPC_EXPORT __attribute__((visibility(\"default\")))\n#endif\n"
            "const sbpc_rt *sbpc_rt_api;\nstatic const sbpc_entry ENTRIES[] = {\n", opt->module);
    for (int i = 0; i < nres; i++) {
        if (i > 0 && res[i].addr == res[i - 1].addr)
            continue;
        out(&c, "    {0x%08Xu, f_%08X},\n", (unsigned)res[i].addr, (unsigned)res[i].fn);
        st->resume_points++;
    }
    out(&c, "};\nuint32_t sbpc_ok[%d];\nconst sbpc_code sbpc_code_tab[] = {\n", c.entries.n + 1);
    for (int i = 0; i < c.entries.n; i++)
        out(&c, "    {0x%08Xu, %uu, 0x%08Xu},\n", (unsigned)c.ranges[i].start, (unsigned)c.ranges[i].len,
            (unsigned)c.ranges[i].hash);
    out(&c, "    {0, 0, 0}\n");
    out(&c, "};\nstatic const sbpc_module MODULE = {SBPC_MODULE_ABI, \"%s\", \"%s\",\n"
            "    (int)(sizeof ENTRIES / sizeof ENTRIES[0]), ENTRIES};\n"
            "SBPC_EXPORT const sbpc_module *sbpc_module_%s(const sbpc_rt *rt)\n"
            "{\n    sbpc_rt_api = rt;\n    return &MODULE;\n}\n",
        opt->module, opt->source_hash ? opt->source_hash : "", opt->module);
    fclose(c.f);
    st->files++;
    free(res);
    free(c.ranges);
    free(c.entries.v);
    LOGI(SUB_CORE, "codegen %s: %d functions, %d instructions, %d files", opt->module, st->functions,
         st->instructions, st->files);
    return true;
}
