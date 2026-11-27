/* Runs guest code: recompiled functions where they exist, the interpreter everywhere else. */
#ifndef SBPC_RUNTIME_DISPATCH_H
#define SBPC_RUNTIME_DISPATCH_H

#include "sh2/sbpc_rt.h"

#include <stdbool.h>

typedef struct {
    sbpc_entry *entries;   /* sorted by addr, merged from all loaded modules */
    int count;
    uint64_t interp_steps; /* instructions run by the interpreter */
    uint64_t native_calls; /* entries into recompiled code */
    uint64_t code_mismatches; /* recompiled functions skipped because memory holds other code */
    /* Optional: handles addresses that aren't real code (the BIOS replacement). Returns 1 if
     * it dealt with s->pc. */
    int (*hle)(sh2_state *s);
    void (*probe)(sh2_state *s);  /* developer hook: called before each step when set */
    void (*on_interp)(sh2_state *s); /* developer hook: called before each interpreted instruction */
} rt_dispatch;

void rt_dispatch_init(rt_dispatch *d);
void rt_dispatch_free(rt_dispatch *d);
/* Adds a module's entry points (later modules win on duplicate addresses). */
bool rt_dispatch_add(rt_dispatch *d, const sbpc_module *m);
sbpc_fn rt_dispatch_find(const rt_dispatch *d, uint32_t addr);

/* Code-check epoch: bump it whenever guest code may have changed (each frame, after DMA/loads). */
void rt_bump_epoch(void);
uint32_t rt_epoch(void);

/* The table handed to generated modules. */
const sbpc_rt *rt_api(void);

/* Runs s (with s->dispatch set) until the time slice ends or the CPU sleeps. */
void rt_run(sh2_state *s);
/* Runs until s->pc == stop or the time slice ends. */
void rt_run_until(sh2_state *s, uint32_t stop);

#endif
