/* Interface between generated game code and the runtime. Generated modules are compiled on
 * the user's PC (TinyCC or a system compiler) and loaded as a .dll/.so, so they reach the
 * runtime only through this table of function pointers. */
#ifndef SBPC_RT_H
#define SBPC_RT_H

#include "sh2_state.h"

#define SBPC_MODULE_ABI 2

typedef void (*sbpc_fn)(sh2_state *s);

typedef struct {
    uint32_t addr;   /* guest address the function can be entered at */
    sbpc_fn fn;
} sbpc_entry;

/* The guest code a generated function was made from (for the runtime's check). */
typedef struct {
    uint32_t start, len, hash;   /* hash: FNV-1a over the bytes */
} sbpc_code;

typedef struct {
    /* Runs guest code at s->pc (recompiled or interpreted) until it returns to `ret`
     * (s->pc == ret) or the time slice ends. Used for calls whose target isn't known
     * at generation time. */
    void (*call)(sh2_state *s, uint32_t ret);
    /* TRAPA #imm: takes the exception; afterwards s->pc is the handler. */
    void (*trap)(sh2_state *s, int imm, uint32_t return_pc);
    /* Code check. Memory can hold other code than what was recompiled (overlays, a different
     * program loaded at the same address), so every generated function starts with SBPC_CHECK:
     * its bytes are compared with memory once per epoch (the runtime bumps *epoch every frame
     * and after DMA / loads). On a mismatch the function returns without running and the
     * interpreter takes over at that address. */
    const uint32_t *epoch;
    int (*verify)(sh2_state *s, const sbpc_code *c, uint32_t *ok);
} sbpc_rt;

#define SBPC_BAD 0x80000000u
#define SBPC_CHECK(i) do { uint32_t ok_ = sbpc_ok[i], ep_ = *sbpc_rt_api->epoch; \
    if (ok_ != ep_ && (ok_ == (ep_ | SBPC_BAD) || !sbpc_rt_api->verify(s, &sbpc_code_tab[i], &sbpc_ok[i]))) return; } while (0)

/* What a generated module exports. */
typedef struct {
    int abi;                 /* SBPC_MODULE_ABI */
    const char *name;        /* module name, e.g. "kernel" */
    const char *source_hash; /* hash of the disc data it was generated from */
    int count;
    const sbpc_entry *entries; /* sorted by addr */
} sbpc_module;

#endif
