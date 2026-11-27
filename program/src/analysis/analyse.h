/* Static code discovery: finds SH-2 functions by recursive descent from known entry points. */
#ifndef SBPC_ANALYSIS_ANALYSE_H
#define SBPC_ANALYSIS_ANALYSE_H

#include <stdio.h>

#include "analysis/image.h"

typedef enum {
    FN_ENTRY,      /* given entry point */
    FN_BSR,        /* target of BSR */
    FN_JSR_CONST,  /* JSR/BSRF to a register holding a known constant */
    FN_JMP_CONST,  /* tail jump (JMP) to a known constant */
    FN_POINTER,    /* code pointer found in a literal pool (prologue or function boundary) */
    FN_TABLE,      /* code pointer found anywhere else, e.g. method tables (prologue or boundary) */
    FN_HINT        /* per-version hint: code reached only at runtime (longjmp, computed jumps) */
} fn_source;

typedef enum {
    SITE_INDIRECT_CALL,   /* JSR/BSRF with unknown target */
    SITE_INDIRECT_JUMP,   /* JMP/BRAF with unknown target (switch tables, callbacks) */
    SITE_EXTERNAL_CALL,   /* call/jump to an address outside the loaded image (BIOS, other files) */
    SITE_INVALID_OPCODE,
    SITE_SLOT_ILLEGAL,    /* branch in a delay slot */
    SITE_CODE_DATA,       /* execution runs into bytes already used as a literal */
    SITE_RUNS_OFF,        /* execution leaves the loaded image */
    SITE_TRAPA,
    SITE_NORETURN_CALL,   /* call followed directly by literals: the callee never returns (info) */
    SITE_BIOS_CALL,       /* call through a BIOS vector (0x06000200-0x060007FF); runtime provides it */
    SITE_KIND_COUNT
} site_kind;

/* A resolved switch: `jmp @r0` through a table next to the code. */
typedef enum {
    JT_OFFSET8,   /* signed byte offsets from the table base */
    JT_OFFSET16,  /* signed 16-bit offsets from the table base */
    JT_CODE4,     /* the table is code: entry i starts at base + 4*i */
    JT_CODE2,     /* entry i starts at base + 2*i */
    JT_ABS32,     /* absolute 32-bit addresses: mov.l @(r0,rT),rN ; jmp @rN */
    JT_BRAF16     /* 16-bit offsets from the braf: mova tbl,r0 ; mov.w @(r0,rI),r0 ; braf r0 */
} jtable_kind;

typedef struct {
    uint32_t jmp_pc;       /* the jmp / braf */
    uint32_t base;
    jtable_kind kind;
    int count;
} an_jtable;

typedef struct {
    uint32_t entry;
    uint32_t end;        /* one past the highest instruction reached */
    int insns;
    fn_source source;
} an_function;

typedef struct {
    uint32_t addr;       /* instruction address */
    uint32_t target;     /* where relevant */
    site_kind kind;
} an_site;

typedef struct {
    mem_image *im;
    int nfuncs, capfuncs;
    an_function *funcs;
    int nsites, capsites;
    an_site *sites;
    int njtables, capjtables;
    an_jtable *jtables;
    int total_insns;
    int pointer_candidates;
} analysis;

void an_init(analysis *a, mem_image *im);
void an_free(analysis *a);
/* Adds an entry point; analysis happens in an_run. */
void an_add_entry(analysis *a, uint32_t addr, fn_source source);
/* Explores everything reachable, then looks for pointer-to-prologue candidates, repeating
 * until nothing new is found. */
void an_run(analysis *a);

const char *an_site_name(site_kind k);
const char *an_source_name(fn_source s);

/* Target of switch-table entry i. */
uint32_t an_jtable_target(analysis *a, const an_jtable *t, int i);

/* Reports: summary (human), function list, and a full address-ordered listing. */
void an_write_summary(analysis *a, FILE *f);
void an_write_functions(analysis *a, FILE *f);
void an_write_listing(analysis *a, FILE *f);

#endif
