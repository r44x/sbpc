#include "analysis/analyse.h"
#include "core/log.h"
#include "sh2/sh2_decode.h"

#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------------------- */

typedef struct {
    uint32_t pc;
    uint32_t val[16];
    uint16_t known; /* bit i: val[i] is a known constant */
    uint32_t from[16];
    uint16_t loaded; /* bit i: r[i] was loaded with "mov.l @rM,rI" from known address from[i] */
} path;

static const char *const SITE_NAMES[SITE_KIND_COUNT] = {
    "indirect-call", "indirect-jump", "external-call", "invalid-opcode",
    "branch-in-delay-slot", "code-runs-into-data", "runs-off-image", "trapa", "noreturn-call", "bios-call"};

static const char *const SOURCE_NAMES[] = {"entry", "bsr", "jsr-const", "jmp-const", "pointer", "table", "hint"};

const char *an_site_name(site_kind k) { return SITE_NAMES[k]; }
const char *an_source_name(fn_source s) { return SOURCE_NAMES[s]; }

void an_init(analysis *a, mem_image *im)
{
    memset(a, 0, sizeof *a);
    a->im = im;
}

void an_free(analysis *a)
{
    free(a->funcs);
    free(a->sites);
    free(a->jtables);
    memset(a, 0, sizeof *a);
}

static void add_site(analysis *a, uint32_t addr, uint32_t target, site_kind k)
{
    if (a->nsites == a->capsites) {
        int cap = a->capsites ? a->capsites * 2 : 256;
        an_site *n = realloc(a->sites, (size_t)cap * sizeof *n);
        if (!n)
            return;
        a->sites = n;
        a->capsites = cap;
    }
    a->sites[a->nsites++] = (an_site){addr, target, k};
    LOGT(SUB_HOOKS, "site %s at %08X -> %08X", SITE_NAMES[k], (unsigned)addr, (unsigned)target);
}

static int find_func(const analysis *a, uint32_t entry)
{
    for (int i = 0; i < a->nfuncs; i++)
        if (a->funcs[i].entry == entry)
            return i;
    return -1;
}

void an_add_entry(analysis *a, uint32_t addr, fn_source source)
{
    addr = img_phys(addr);
    /* The IP's security code: the BIOS stand-in (bios_hle -> ip_boot) takes over when the dispatcher reaches
     * it, so it must never be compiled. A front end that boots the game by calling it would otherwise skip the
     * stand-in and run the real code into the missing BIOS ROM. */
    if (addr == 0x06002100u)
        return;
    if (find_func(a, addr) >= 0)
        return;
    if (a->nfuncs == a->capfuncs) {
        int cap = a->capfuncs ? a->capfuncs * 2 : 256;
        an_function *n = realloc(a->funcs, (size_t)cap * sizeof *n);
        if (!n)
            return;
        a->funcs = n;
        a->capfuncs = cap;
    }
    a->funcs[a->nfuncs++] = (an_function){addr, addr, 0, source};
}

/* Call/jump to a constant: inside the image -> new function; outside -> external site. */
static void call_target(analysis *a, uint32_t from, uint32_t target, fn_source src)
{
    if ((target & 1) || !img_find(a->im, target, 2))
        add_site(a, from, target, SITE_EXTERNAL_CALL);
    else
        an_add_entry(a, target, src);
}

/* Tracks simple constants: MOV #imm, MOV Rm,Rn, ADD #imm, PC-relative loads, MOVA. Anything
 * else that writes a register forgets it. Good enough to resolve "mov.l @(lit),rN; jsr @rN". */
static void update_regs_inner(analysis *a, const sh2_insn *in, path *p);

static void update_regs(analysis *a, const sh2_insn *in, path *p)
{
    uint16_t before = p->known;
    bool is_load = strcmp(in->mnemonic, "mov.l") == 0 && strcmp(sh2_op_operands(in->op), "@r%m,r%n") == 0;
    uint32_t addr = p->val[in->m];
    bool addr_known = (p->known >> in->m) & 1;
    update_regs_inner(a, in, p);
    /* any register whose constant changed or was dropped loses its "loaded from" too */
    p->loaded &= (uint16_t)~(before ^ p->known);
    if (is_load) {
        p->loaded &= (uint16_t)~(1u << in->n);
        if (addr_known) {
            p->from[in->n] = addr;
            p->loaded |= (uint16_t)(1u << in->n);
        }
    } else if (!(in->flags & SH2F_BRANCH)) {
        /* conservative: an instruction writing Rn (dropped above) also invalidates the load */
        const char *ops = sh2_op_operands(in->op);
        size_t len = strlen(ops);
        if (len >= 3 && strcmp(ops + len - 3, "r%n") == 0)
            p->loaded &= (uint16_t)~(1u << in->n);
    }
}

static void update_regs_inner(analysis *a, const sh2_insn *in, path *p)
{
    const char *ops = sh2_op_operands(in->op);
    size_t len = strlen(ops);
    const char *mn = in->mnemonic;

    if (in->flags & SH2F_PCLOAD) {
        uint32_t v = 0;
        uint16_t w = 0;
        bool ok;
        if (in->size == 4) {
            ok = img_read32(a->im, in->target, &v);
        } else {
            ok = img_read16(a->im, in->target, &w);
            v = (uint32_t)(int32_t)(int16_t)w;
        }
        if (ok) { p->val[in->n] = v; p->known |= (uint16_t)(1u << in->n); }
        else p->known &= (uint16_t)~(1u << in->n);
        return;
    }
    if (in->flags & SH2F_PCADDR) { /* mova */
        p->val[0] = in->target;
        p->known |= 1;
        return;
    }
    if (strcmp(mn, "mov") == 0 && strcmp(ops, "#%s,r%n") == 0) {
        p->val[in->n] = (uint32_t)in->imm;
        p->known |= (uint16_t)(1u << in->n);
        return;
    }
    if (strcmp(mn, "mov") == 0 && strcmp(ops, "r%m,r%n") == 0) {
        p->val[in->n] = p->val[in->m];
        if (p->known & (1u << in->m)) p->known |= (uint16_t)(1u << in->n);
        else p->known &= (uint16_t)~(1u << in->n);
        return;
    }
    if (strcmp(mn, "add") == 0 && strcmp(ops, "#%s,r%n") == 0) {
        p->val[in->n] += (uint32_t)in->imm;
        return;
    }
    /* Generic: forget whatever this instruction writes. */
    if (len >= 3 && strcmp(ops + len - 3, "r%n") == 0) p->known &= (uint16_t)~(1u << in->n);
    if (len >= 3 && strcmp(ops + len - 3, ",r0") == 0) p->known &= 0xFFFEu;
    if (strstr(ops, "@r%n+") || strstr(ops, "@-r%n")) p->known &= (uint16_t)~(1u << in->n);
    if (strstr(ops, "@r%m+")) p->known &= (uint16_t)~(1u << in->m);
}

/* Decodes one instruction at pc and marks it as code. Returns false (with a site) if it can't. */
static bool fetch(analysis *a, uint32_t pc, sh2_insn *in, uint32_t from)
{
    uint16_t raw;
    uint8_t k;
    if ((pc & 1) || !img_read16(a->im, pc, &raw)) {
        add_site(a, from, pc, SITE_RUNS_OFF);
        return false;
    }
    k = img_kind(a->im, pc);
    if (k == BYTE_DATA) {
        add_site(a, pc, pc, SITE_CODE_DATA);
        return false;
    }
    if (!sh2_decode(raw, pc, in)) {
        add_site(a, pc, raw, SITE_INVALID_OPCODE);
        return false;
    }
    if (k == BYTE_UNKNOWN) {
        img_mark(a->im, pc, 1, BYTE_CODE);
        img_mark(a->im, pc + 1, 1, BYTE_CODE_CONT);
        a->total_insns++;
    }
    return true;
}

typedef struct {
    path *v;
    int top, cap;
} wstack;

static void push(wstack *w, const path *p, uint32_t pc)
{
    if (w->top == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        path *n = realloc(w->v, (size_t)cap * sizeof *n);
        if (!n)
            return;
        w->v = n;
        w->cap = cap;
    }
    w->v[w->top] = *p;
    w->v[w->top].pc = pc;
    w->top++;
}

static bool raw_at(analysis *a, uint32_t addr, uint16_t *w) { return img_read16(a->im, addr, w); }

static bool is_known_entry(const analysis *a, uint32_t addr) { return find_func(a, addr) >= 0; }

/* Finds an upper bound for a switch index by looking back from the jump for
 * "and #k,r0" (k+1 entries) or "mov #k,rL ... cmp/hi rL,rX" (k+1 entries). Returns 0 if none. */
static int switch_bound(analysis *a, uint32_t jmp_pc)
{
    for (uint32_t back = 2; back <= 40; back += 2) {
        uint16_t w;
        if (!raw_at(a, jmp_pc - back, &w))
            break;
        if ((w & 0xFF00) == 0xC900) { /* and #k,r0 */
            int k = w & 0xFF;
            if ((k & (k + 1)) == 0)
                return k + 1;
        }
        if ((w & 0xF00F) == 0x3006 || (w & 0xF00F) == 0x3002) { /* cmp/hi or cmp/hs rL,rX */
            int rl = (w >> 4) & 15;
            for (uint32_t b2 = back + 2; b2 <= back + 24; b2 += 2) {
                uint16_t v;
                if (!raw_at(a, jmp_pc - b2, &v))
                    break;
                if ((v & 0xF000) == 0xE000 && ((v >> 8) & 15) == rl) {
                    int k = (int8_t)(v & 0xFF);
                    int n = (w & 0xF) == 6 ? k + 1 : k; /* hi: idx<=k ; hs: idx<k */
                    return n > 0 && n <= 1024 ? n : 0;
                }
            }
            return 0;
        }
    }
    return 0;
}

static void add_jtable(analysis *a, const an_jtable *t)
{
    if (a->njtables == a->capjtables) {
        int cap = a->capjtables ? a->capjtables * 2 : 64;
        an_jtable *n = realloc(a->jtables, (size_t)cap * sizeof *n);
        if (!n)
            return;
        a->jtables = n;
        a->capjtables = cap;
    }
    a->jtables[a->njtables++] = *t;
}

uint32_t an_jtable_target(analysis *a, const an_jtable *t, int i)
{
    uint16_t w = 0;
    switch (t->kind) {
    case JT_OFFSET8: {
        uint32_t e = t->base + (uint32_t)i;
        uint16_t pair = 0;
        raw_at(a, e & ~1u, &pair);
        return t->base + (uint32_t)(int32_t)(int8_t)((e & 1) ? pair & 0xFF : pair >> 8);
    }
    case JT_OFFSET16:
        raw_at(a, t->base + 2u * (uint32_t)i, &w);
        return t->base + (uint32_t)(int32_t)(int16_t)w;
    case JT_ABS32: {
        uint32_t v = 0;
        img_read32(a->im, t->base + 4u * (uint32_t)i, &v);
        return img_phys(v);
    }
    case JT_BRAF16:
        raw_at(a, t->base + 2u * (uint32_t)i, &w);
        return t->jmp_pc + 4 + (uint32_t)(int32_t)(int16_t)w;
    case JT_CODE4: return t->base + 4u * (uint32_t)i;
    case JT_CODE2: return t->base + 2u * (uint32_t)i;
    }
    return 0;
}

/* Recognises compiler switch tables ending in "jmp @r0":
 *   mova tbl,r0 ; mov.{b,w} @(r0,rI),rI ; add rI,r0 ; jmp @r0      (offset tables)
 *   mova tbl,r0 ; shll2 rI / shll rI    ; add rI,r0 ; jmp @r0      (tables of code)   */
static bool resolve_switch(analysis *a, uint32_t pc, const path *p, wstack *w)
{
    uint16_t w1, w2, w3;
    uint32_t base;
    an_jtable t;
    int ri, bound, limit;
    uint32_t min_target = 0xFFFFFFFFu;

    if (!raw_at(a, pc - 2, &w1) || !raw_at(a, pc - 4, &w2) || !raw_at(a, pc - 6, &w3))
        return false;
    if ((w1 & 0xFF0F) != 0x300C) /* add rI,r0 */
        return false;
    ri = (w1 >> 4) & 15;
    {
        /* mova sits right before the load, or one further back with "add rI,rI" between. */
        uint32_t mova_pc = pc - 6;
        if ((w3 & 0xFF00) != 0xC700) {
            uint16_t w4;
            if (w3 != (0x300C | ri << 8 | ri << 4) || !raw_at(a, pc - 8, &w4) || (w4 & 0xFF00) != 0xC700)
                return false;
            w3 = w4;
            mova_pc = pc - 8;
        }
        base = (mova_pc & ~3u) + 4 + (w3 & 0xFF) * 4u;
    }

    if ((w2 & 0xF00F) == 0x000C || (w2 & 0xF00F) == 0x000D) {
        if (((w2 >> 8) & 15) != ri || ((w2 >> 4) & 15) != ri)
            return false;
        t.kind = (w2 & 0xF) == 0xC ? JT_OFFSET8 : JT_OFFSET16;
    } else if (w2 == (0x4008 | ri << 8)) {
        t.kind = JT_CODE4;
    } else if (w2 == (0x4000 | ri << 8)) {
        t.kind = JT_CODE2;
    } else {
        return false;
    }
    t.jmp_pc = pc;
    t.base = base;
    bound = switch_bound(a, pc);
    limit = bound ? bound : 256;

    for (t.count = 0; t.count < limit; t.count++) {
        uint32_t tgt, entry_addr;
        sh2_insn in;
        uint16_t raw;
        int esize = t.kind == JT_OFFSET8 ? 1 : 2;
        if (t.kind == JT_OFFSET8 || t.kind == JT_OFFSET16) {
            entry_addr = base + (uint32_t)(t.count * esize);
            if (entry_addr >= min_target || !img_find(a->im, entry_addr, (uint32_t)esize))
                break;
            if (!bound && img_kind(a->im, entry_addr) == BYTE_CODE)
                break;
        }
        tgt = an_jtable_target(a, &t, t.count);
        if ((tgt & 1) || !raw_at(a, tgt, &raw) || !sh2_decode(raw, tgt, &in) ||
            img_kind(a->im, tgt) == BYTE_DATA)
            break;
        if ((t.kind == JT_CODE4 || t.kind == JT_CODE2) && !bound && t.count > 0 &&
            (is_known_entry(a, tgt) || img_kind(a->im, tgt) == BYTE_CODE))
            break;
        if (tgt > pc && tgt < min_target)
            min_target = tgt;
    }
    if (t.count == 0)
        return false;

    if (t.kind == JT_OFFSET8 || t.kind == JT_OFFSET16)
        img_mark(a->im, base, (uint32_t)(t.count * (t.kind == JT_OFFSET8 ? 1 : 2)), BYTE_DATA);
    for (int i = 0; i < t.count; i++)
        push(w, p, an_jtable_target(a, &t, i));
    add_jtable(a, &t);
    LOGT(SUB_HOOKS, "switch at %08X: table %08X, %d entries%s", (unsigned)pc, (unsigned)base, t.count,
         bound ? "" : " (no bound found)");
    return true;
}

/* Bound for a table of absolute pointers indexed by a byte offset in r0:
 * "mov #k,rX ; cmp/hs r0,rX" (r0 <= k) or "mov #k,rX ; cmp/hi r0,rX" (r0 < k). */
static int abs32_bound(analysis *a, uint32_t from_pc)
{
    for (uint32_t back = 2; back <= 32; back += 2) {
        uint16_t w;
        if (!raw_at(a, from_pc - back, &w))
            break;
        if (((w & 0xF00F) == 0x3002 || (w & 0xF00F) == 0x3006) && ((w >> 4) & 15) == 0) {
            int rx = (w >> 8) & 15;
            for (uint32_t b2 = back + 2; b2 <= back + 16; b2 += 2) {
                uint16_t v;
                if (!raw_at(a, from_pc - b2, &v))
                    break;
                if ((v & 0xF000) == 0xE000 && ((v >> 8) & 15) == rx) {
                    int k = (int8_t)(v & 0xFF);
                    int n = (w & 0xF) == 2 ? k / 4 + 1 : (k + 3) / 4;
                    return n > 0 ? n : 0;
                }
            }
            return 0;
        }
    }
    return 0;
}

/* Recognises "mov.l @(lit),rT ; [..] mov.l @(r0,rT),rN ; [add #k,r0] ; jmp @rN". */
static bool resolve_abs_table(analysis *a, uint32_t pc, int rn, const path *p, wstack *w)
{
    an_jtable t;
    int bound;
    for (uint32_t back = 2; back <= 6; back += 2) {
        uint16_t ld, lit;
        int rt;
        if (!raw_at(a, pc - back, &ld))
            return false;
        if ((ld & 0xF00F) != 0x000E || ((ld >> 8) & 15) != rn)
            continue;
        rt = (ld >> 4) & 15;
        for (uint32_t b2 = back + 2; b2 <= back + 8; b2 += 2) {
            uint32_t q = pc - b2, base;
            if (!raw_at(a, q, &lit))
                return false;
            if ((lit & 0xF000) != 0xD000 || ((lit >> 8) & 15) != rt)
                continue;
            if (!img_read32(a->im, (q & ~3u) + 4 + (lit & 0xFF) * 4u, &base))
                return false;
            base = img_phys(base);
            t.jmp_pc = pc;
            t.base = base;
            t.kind = JT_ABS32;
            bound = abs32_bound(a, pc - back);
            for (t.count = 0; t.count < (bound ? bound : 512); t.count++) {
                uint32_t tgt;
                uint16_t raw;
                sh2_insn in;
                if (!img_find(a->im, base + 4u * (uint32_t)t.count, 4))
                    break;
                if (!bound && t.count > 0 && img_kind(a->im, base + 4u * (uint32_t)t.count) == BYTE_CODE)
                    break;
                tgt = an_jtable_target(a, &t, t.count);
                if ((tgt & 1) || !raw_at(a, tgt, &raw) || !sh2_decode(raw, tgt, &in) ||
                    img_kind(a->im, tgt) == BYTE_DATA)
                    break;
            }
            if (t.count == 0)
                return false;
            if (img_find(a->im, base, 4u * (uint32_t)t.count))
                img_mark(a->im, base, 4u * (uint32_t)t.count, BYTE_DATA);
            for (int i = 0; i < t.count; i++)
                push(w, p, an_jtable_target(a, &t, i));
            add_jtable(a, &t);
            return true;
        }
        return false;
    }
    return false;
}

/* "mova tbl,r0 ; mov.w @(r0,rI),r0 ; braf r0": offsets are relative to the braf + 4. */
static bool resolve_braf(analysis *a, uint32_t pc, const path *p, wstack *w)
{
    uint16_t ld, mv;
    an_jtable t;
    int bound;
    if (!raw_at(a, pc - 2, &ld) || !raw_at(a, pc - 4, &mv))
        return false;
    if ((ld & 0xFF0F) != 0x000D || (mv & 0xFF00) != 0xC700) /* mov.w @(r0,rI),r0 ; mova */
        return false;
    t.jmp_pc = pc;
    t.base = ((pc - 4) & ~3u) + 4 + (mv & 0xFF) * 4u;
    t.kind = JT_BRAF16;
    bound = switch_bound(a, pc);
    for (t.count = 0; t.count < (bound ? bound : 256); t.count++) {
        uint32_t e = t.base + 2u * (uint32_t)t.count, tgt;
        uint16_t raw;
        sh2_insn in;
        if (!img_find(a->im, e, 2) || (!bound && img_kind(a->im, e) == BYTE_CODE))
            break;
        tgt = an_jtable_target(a, &t, t.count);
        if ((tgt & 1) || !raw_at(a, tgt, &raw) || !sh2_decode(raw, tgt, &in) || img_kind(a->im, tgt) == BYTE_DATA)
            break;
        if (!bound && tgt > pc && tgt <= e)
            break; /* table runs into its own cases */
    }
    if (t.count == 0)
        return false;
    img_mark(a->im, t.base, 2u * (uint32_t)t.count, BYTE_DATA);
    for (int i = 0; i < t.count; i++)
        push(w, p, an_jtable_target(a, &t, i));
    add_jtable(a, &t);
    return true;
}

static void explore_function(analysis *a, int fi)
{
    wstack w = {0};
    uint32_t entry = a->funcs[fi].entry;
    path start = {0};
    push(&w, &start, entry);

    while (w.top > 0) {
        path p = w.v[--w.top];
        uint32_t last_call = 0; /* pc of a call we just returned from */
        for (;;) {
            sh2_insn in, slot;
            uint32_t pc = p.pc, target = 0;
            bool have_target = false;
            uint8_t kind = img_kind(a->im, pc);

            if (kind == BYTE_CODE && (pc != entry || a->funcs[fi].insns > 0))
                break; /* already explored */
            if (kind == BYTE_DATA && last_call) {
                add_site(a, last_call, pc, SITE_NORETURN_CALL);
                break;
            }
            last_call = 0;
            if (!fetch(a, pc, &in, pc))
                break;
            a->funcs[fi].insns++;
            if (pc + 2 > a->funcs[fi].end)
                a->funcs[fi].end = pc + 2;

            if (in.flags & SH2F_PCLOAD)
                img_mark(a->im, in.target, in.size, BYTE_DATA);

            if (!(in.flags & SH2F_BRANCH)) {
                update_regs(a, &in, &p);
                p.pc = pc + 2;
                continue;
            }

            /* Register-based targets are read before the delay slot executes. */
            if (in.flags & SH2F_INDIRECT) {
                if (p.known & (1u << in.n)) {
                    target = p.val[in.n];
                    if (strncmp(in.mnemonic, "bra", 3) == 0 || strncmp(in.mnemonic, "bsr", 3) == 0)
                        target += pc + 4; /* BRAF/BSRF are PC-relative */
                    have_target = true;
                }
            } else {
                target = in.target;
                have_target = !(in.flags & (SH2F_RETURN | SH2F_TRAP));
            }

            if (in.flags & SH2F_DELAY) {
                if (!fetch(a, pc + 2, &slot, pc))
                    break;
                a->funcs[fi].insns++;
                if (pc + 4 > a->funcs[fi].end)
                    a->funcs[fi].end = pc + 4;
                if (slot.flags & SH2F_NO_SLOT)
                    add_site(a, pc + 2, slot.raw, SITE_SLOT_ILLEGAL);
                if (slot.flags & SH2F_PCLOAD)
                    img_mark(a->im, slot.target, slot.size, BYTE_DATA);
                update_regs(a, &slot, &p);
            }

            if (in.flags & SH2F_TRAP) {
                add_site(a, pc, (uint32_t)in.imm, SITE_TRAPA);
                p.pc = pc + 2;
                continue;
            }
            if (in.flags & SH2F_RETURN)
                break;

            if (in.flags & SH2F_CALL) {
                if (have_target)
                    call_target(a, pc, target, (in.flags & SH2F_INDIRECT) ? FN_JSR_CONST : FN_BSR);
                else if ((in.flags & SH2F_INDIRECT) && ((p.loaded >> in.n) & 1) &&
                         img_phys(p.from[in.n]) >= 0x06000200u && img_phys(p.from[in.n]) < 0x06000800u)
                    add_site(a, pc, p.from[in.n], SITE_BIOS_CALL);
                else
                    add_site(a, pc, 0, SITE_INDIRECT_CALL);
                p.known &= 0xFF00; /* r0-r7 are caller-saved */
                p.loaded &= 0xFF00;
                p.pc = pc + 4;
                last_call = pc;
                continue;
            }

            /* Plain or conditional branch / jump */
            if (!have_target) {
                bool is_jmp = strcmp(in.mnemonic, "jmp") == 0;
                if (!(is_jmp && in.n == 0 && resolve_switch(a, pc, &p, &w)) &&
                    !(is_jmp && resolve_abs_table(a, pc, in.n, &p, &w)) &&
                    !(strcmp(in.mnemonic, "braf") == 0 && in.n == 0 && resolve_braf(a, pc, &p, &w)))
                    add_site(a, pc, 0, SITE_INDIRECT_JUMP);
                break;
            }
            if (strcmp(in.mnemonic, "jmp") == 0) { /* tail call to a constant */
                call_target(a, pc, target, FN_JMP_CONST);
                break;
            }
            if (!img_find(a->im, target, 2))
                add_site(a, pc, target, SITE_RUNS_OFF);
            else
                push(&w, &p, target);
            if (in.flags & SH2F_END)
                break;
            p.pc = pc + ((in.flags & SH2F_DELAY) ? 4 : 2); /* conditional: fall through */
        }
    }
    free(w.v);
}

static bool is_prologue(uint16_t w)
{
    return (w & 0xFF0F) == 0x2F06 /* mov.l rX,@-r15 */ || w == 0x4F22 /* sts.l pr,@-r15 */;
}

/* Pointer-found candidates must decode cleanly in a straight line until the first
 * unconditional end (with its delay slot) or for 24 instructions, and must not do odd
 * things to the stack pointer straight after the prologue. */
static bool plausible_function(analysis *a, uint32_t addr)
{
    for (int i = 0; i < 24; i++) {
        uint16_t raw;
        sh2_insn in;
        uint32_t pc = addr + 2u * (uint32_t)i;
        if (!raw_at(a, pc, &raw) || !sh2_decode(raw, pc, &in) || img_kind(a->im, pc) == BYTE_DATA)
            return false;
        if (i < 4 && in.n == 15 && !(in.flags & SH2F_BRANCH)) {
            const char *ops = sh2_op_operands(in.op);
            bool push = strstr(ops, "@-r%n") != NULL;
            bool sp_ok = push || strcmp(in.mnemonic, "add") == 0 || strstr(ops, "@r%m+") != NULL ||
                         strstr(ops, "@(") != NULL || strstr(ops, ",@r%n") != NULL;
            if (!sp_ok && strcmp(in.mnemonic, "mov") != 0)
                return false;
        }
        if ((in.flags & SH2F_END) && !(in.flags & SH2F_COND)) {
            if (in.flags & SH2F_DELAY) {
                if (!raw_at(a, pc + 2, &raw) || !sh2_decode(raw, pc + 2, &in) || (in.flags & SH2F_NO_SLOT))
                    return false;
            }
            return true;
        }
    }
    return true;
}

/* True when addr directly follows a literal pool, or explored code that ends unconditionally: rts,
 * rte, jmp, bra or braf at addr-4 with its delay slot at addr-2. Leaf methods called through tables often have no
 * prologue, but they still start where the previous function ended (USA kernel: the stream
 * readers at 0x06033DF0-0x06034350, listed in method tables at 0x060B52C0-0x060B5400). */
static bool after_function_end(analysis *a, uint32_t addr)
{
    uint16_t w;
    /* A literal pool (bytes explored code loads as data) also sits between functions. */
    if (img_kind(a->im, addr - 4) == BYTE_DATA && img_kind(a->im, addr - 1) == BYTE_DATA)
        return true;
    if (img_kind(a->im, addr - 4) != BYTE_CODE || img_kind(a->im, addr - 2) != BYTE_CODE ||
        !img_read16(a->im, addr - 4, &w))
        return false;
    return w == 0x000B /* rts */ || w == 0x002B /* rte */ || (w & 0xF0FF) == 0x402B /* jmp */ ||
           (w & 0xF000) == 0xA000 /* bra */ || (w & 0xF0FF) == 0x0023 /* braf */;
}

/* Aligned longwords that point at unexplored code starting with a prologue or right after the end
 * of a function: in literal pools (FN_POINTER) or anywhere not yet code, e.g. tables (FN_TABLE). */
static int pointer_pass(analysis *a)
{
    int added = 0, candidates = 0;
    for (int r = 0; r < a->im->count; r++) {
        img_region *rg = &a->im->regions[r];
        for (uint32_t off = 0; off + 4 <= rg->size; off += 4) {
            uint32_t v;
            uint16_t w;
            bool literal = rg->kind[off] == BYTE_DATA && rg->kind[off + 3] == BYTE_DATA;
            if (!literal && (rg->kind[off] != BYTE_UNKNOWN || rg->kind[off + 3] != BYTE_UNKNOWN))
                continue;
            img_read32(a->im, rg->base + off, &v);
            if ((v & 1) || !img_find(a->im, v, 2) || img_kind(a->im, v) != BYTE_UNKNOWN)
                continue;
            if (literal)
                candidates++;
            if (img_read16(a->im, v, &w) && (is_prologue(w) || after_function_end(a, v)) && find_func(a, img_phys(v)) < 0 &&
                plausible_function(a, v)) {
                int before = a->nfuncs;
                an_add_entry(a, v, literal ? FN_POINTER : FN_TABLE);
                added += a->nfuncs > before;   /* an address an_add_entry refuses must not count, or this never ends */
            }
        }
    }
    a->pointer_candidates = candidates;
    return added;
}

void an_run(analysis *a)
{
    int done = 0;
    do {
        while (done < a->nfuncs)
            explore_function(a, done++);
    } while (pointer_pass(a) > 0);

    /* After pointer passes some candidates became code; recount what remains. */
    pointer_pass(a);
    LOGI(SUB_DISC, "analysis: %d functions, %d instructions, %d sites", a->nfuncs, a->total_insns, a->nsites);
}

/* ---- reports ------------------------------------------------------------------------------ */

static int cmp_func(const void *x, const void *y)
{
    const an_function *a = x, *b = y;
    return a->entry < b->entry ? -1 : a->entry > b->entry;
}

void an_write_summary(analysis *a, FILE *f)
{
    int by_src[7] = {0}, by_site[SITE_KIND_COUNT] = {0};

    fprintf(f, "Regions:\n");
    for (int r = 0; r < a->im->count; r++) {
        img_region *rg = &a->im->regions[r];
        uint32_t code = 0, data = 0;
        for (uint32_t i = 0; i < rg->size; i++) {
            code += rg->kind[i] == BYTE_CODE || rg->kind[i] == BYTE_CODE_CONT;
            data += rg->kind[i] == BYTE_DATA;
        }
        fprintf(f, "  %-12s %08X-%08X %7u bytes: code %5.1f%%  literals %4.1f%%  unexplored %5.1f%%\n",
                rg->name, (unsigned)rg->base, (unsigned)(rg->base + rg->size - 1), (unsigned)rg->size,
                100.0 * code / rg->size, 100.0 * data / rg->size,
                100.0 * (rg->size - code - data) / rg->size);
    }

    for (int i = 0; i < a->nfuncs; i++) by_src[a->funcs[i].source]++;
    for (int i = 0; i < a->nsites; i++) by_site[a->sites[i].kind]++;

    fprintf(f, "\nFunctions: %d  (entry %d, bsr %d, jsr-const %d, jmp-const %d, pointer %d, table %d, hint %d)\n",
            a->nfuncs, by_src[0], by_src[1], by_src[2], by_src[3], by_src[4], by_src[5], by_src[6]);
    fprintf(f, "Instructions: %d\n", a->total_insns);
    {
        int entries = 0, unbounded = 0;
        for (int i = 0; i < a->njtables; i++) entries += a->jtables[i].count;
        (void)unbounded;
        fprintf(f, "Switch tables resolved: %d (%d cases)\n", a->njtables, entries);
    }
    fprintf(f, "Unexplored literal pointers into the image: %d\n", a->pointer_candidates);
    fprintf(f, "\nSites needing attention:\n");
    for (int k = 0; k < SITE_KIND_COUNT; k++) {
        int shown = 0;
        if (!by_site[k])
            continue;
        fprintf(f, "  %-22s %d\n", SITE_NAMES[k], by_site[k]);
        for (int i = 0; i < a->nsites && shown < 8; i++) {
            if (a->sites[i].kind != (site_kind)k)
                continue;
            fprintf(f, "      at %08X  -> %08X\n", (unsigned)a->sites[i].addr, (unsigned)a->sites[i].target);
            shown++;
        }
        if (by_site[k] > shown)
            fprintf(f, "      ... %d more (see listing)\n", by_site[k] - shown);
    }

    /* Largest unexplored gaps: likely code reached only through tables or callbacks. */
    fprintf(f, "\nLargest unexplored gaps:\n");
    for (int pass = 0; pass < 10; pass++) {
        uint32_t best = 0, best_at = 0, prev_best = 0xFFFFFFFFu;
        (void)prev_best;
        for (int r = 0; r < a->im->count; r++) {
            img_region *rg = &a->im->regions[r];
            for (uint32_t i = 0; i < rg->size;) {
                uint32_t j = i;
                if (rg->kind[i] != BYTE_UNKNOWN) { i++; continue; }
                while (j < rg->size && rg->kind[j] == BYTE_UNKNOWN) j++;
                if (j - i > best) { best = j - i; best_at = rg->base + i; }
                i = j;
            }
        }
        if (best < 16)
            break;
        fprintf(f, "  %08X  %u bytes\n", (unsigned)best_at, (unsigned)best);
        img_mark(a->im, best_at, best, 0xFF); /* temporarily hide it */
    }
    for (int r = 0; r < a->im->count; r++) /* restore hidden gaps */
        for (uint32_t i = 0; i < a->im->regions[r].size; i++)
            if (a->im->regions[r].kind[i] == 0xFF)
                a->im->regions[r].kind[i] = BYTE_UNKNOWN;
}

void an_write_functions(analysis *a, FILE *f)
{
    an_function *sorted = malloc((size_t)a->nfuncs * sizeof *sorted);
    if (!sorted)
        return;
    memcpy(sorted, a->funcs, (size_t)a->nfuncs * sizeof *sorted);
    qsort(sorted, (size_t)a->nfuncs, sizeof *sorted, cmp_func);
    fprintf(f, "# entry     end       insns  source\n");
    for (int i = 0; i < a->nfuncs; i++)
        fprintf(f, "%08X  %08X  %5d  %s\n", (unsigned)sorted[i].entry, (unsigned)sorted[i].end,
                sorted[i].insns, SOURCE_NAMES[sorted[i].source]);
    free(sorted);
}

void an_write_listing(analysis *a, FILE *f)
{
    for (int r = 0; r < a->im->count; r++) {
        img_region *rg = &a->im->regions[r];
        fprintf(f, "\n;; ===== %s  %08X-%08X =====\n", rg->name, (unsigned)rg->base,
                (unsigned)(rg->base + rg->size - 1));
        for (uint32_t i = 0; i < rg->size;) {
            uint32_t addr = rg->base + i;
            uint8_t k = rg->kind[i];
            if (k == BYTE_CODE) {
                sh2_insn in;
                char text[64];
                uint16_t raw;
                int fi = find_func(a, addr);
                img_read16(a->im, addr, &raw);
                sh2_decode(raw, addr, &in);
                sh2_disasm(&in, text, sizeof text);
                if (fi >= 0)
                    fprintf(f, "\nfunc_%08X:  ; %s\n", (unsigned)addr, SOURCE_NAMES[a->funcs[fi].source]);
                fprintf(f, "  %08X  %04X  %s\n", (unsigned)addr, raw, text);
                i += 2;
            } else if (k == BYTE_DATA) {
                uint32_t v;
                if (!(addr & 3) && i + 4 <= rg->size && rg->kind[i + 3] == BYTE_DATA && img_read32(a->im, addr, &v)) {
                    fprintf(f, "  %08X  .long 0x%08X\n", (unsigned)addr, (unsigned)v);
                    i += 4;
                } else {
                    uint16_t w = 0;
                    img_read16(a->im, addr, &w);
                    fprintf(f, "  %08X  .word 0x%04X\n", (unsigned)addr, w);
                    i += 2;
                }
            } else {
                uint32_t j = i;
                while (j < rg->size && rg->kind[j] != BYTE_CODE && rg->kind[j] != BYTE_DATA)
                    j++;
                fprintf(f, "  %08X  ; ---- %u unexplored bytes ----\n", (unsigned)addr, (unsigned)(j - i));
                i = j;
            }
        }
    }
}
