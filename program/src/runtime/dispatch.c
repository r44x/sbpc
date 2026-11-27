#include "runtime/dispatch.h"
#include "sh2/sh2_interp.h"

#include <stdlib.h>
#include <string.h>

void rt_dispatch_init(rt_dispatch *d) { memset(d, 0, sizeof *d); }

void rt_dispatch_free(rt_dispatch *d)
{
    free(d->entries);
    memset(d, 0, sizeof *d);
}

typedef struct {
    sbpc_entry e;
    int order; /* lower = added later = wins */
} tagged;

static int cmp_tagged(const void *x, const void *y)
{
    const tagged *a = x, *b = y;
    if (a->e.addr != b->e.addr)
        return a->e.addr < b->e.addr ? -1 : 1;
    return a->order - b->order;
}

bool rt_dispatch_add(rt_dispatch *d, const sbpc_module *m)
{
    tagged *t;
    int total, o = 0;
    if (!m || m->abi != SBPC_MODULE_ABI)
        return false;
    total = d->count + m->count;
    t = malloc((size_t)total * sizeof *t);
    if (!t)
        return false;
    for (int i = 0; i < m->count; i++)
        t[i] = (tagged){m->entries[i], 0};
    for (int i = 0; i < d->count; i++)
        t[m->count + i] = (tagged){d->entries[i], 1};
    qsort(t, (size_t)total, sizeof *t, cmp_tagged);
    free(d->entries);
    d->entries = malloc((size_t)total * sizeof *d->entries);
    if (!d->entries) {
        free(t);
        d->count = 0;
        return false;
    }
    /* Duplicates are kept (the most recently added module first): two modules can cover the
     * same address with different code (overlays); the code check picks the right one. */
    for (int i = 0; i < total; i++)
        if (o == 0 || d->entries[o - 1].addr != t[i].e.addr || d->entries[o - 1].fn != t[i].e.fn)
            d->entries[o++] = t[i].e;
    d->count = o;
    free(t);
    return true;
}

/* Index of the first entry for addr, or -1. */
static int find_first(const rt_dispatch *d, uint32_t addr)
{
    int lo = 0, hi = d->count - 1, found = -1;
    addr &= 0x1FFFFFFFu;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (d->entries[mid].addr >= addr) {
            if (d->entries[mid].addr == addr) found = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return found;
}

sbpc_fn rt_dispatch_find(const rt_dispatch *d, uint32_t addr)
{
    int i = find_first(d, addr);
    return i >= 0 ? d->entries[i].fn : NULL;
}

/* One unit of progress: a recompiled function (which returns when it leaves its code)
 * or a single interpreted instruction. */
static void advance(sh2_state *s)
{
    rt_dispatch *d = s->dispatch;
    sbpc_fn fn;
    uint32_t before = s->pc;
    if (d && d->probe)
        d->probe(s);
    if (d && d->hle && ((s->pc & 0x1FFFFFFFu) < 0x00100000u || s->pc == 0x06002100u) && d->hle(s))
        return;
    if (d && d->count) {
        int i = find_first(d, s->pc);
        for (; i >= 0 && i < d->count && d->entries[i].addr == (before & 0x1FFFFFFFu); i++) {
            int32_t c0 = s->cycles;
            fn = d->entries[i].fn;
            d->native_calls++;
            fn(s);
            s->total_cycles += (uint64_t)(int64_t)(c0 - s->cycles);   /* timers run off total_cycles */
            if (s->pc != before || s->cycles <= 0)
                return;
            /* it didn't run (code check failed, or not resumable here): try the next module */
        }
        /* nothing ran: the interpreter does one step */
    }
    {
        int c;
        if (d && d->on_interp)
            d->on_interp(s);
        c = sh2_step(s);
        s->cycles -= c;
        s->total_cycles += (uint64_t)c;
        if (d)
            d->interp_steps++;
    }
}

void rt_run(sh2_state *s)
{
    while (s->cycles > 0 && !s->sleeping)
        advance(s);
}

void rt_run_until(sh2_state *s, uint32_t stop)
{
    while (s->pc != stop && s->cycles > 0 && !s->sleeping)
        advance(s);
}

static void api_call(sh2_state *s, uint32_t ret) { rt_run_until(s, ret); }

static void api_trap(sh2_state *s, int imm, uint32_t return_pc) { sh2_exception(s, imm, return_pc); }

static uint32_t g_epoch = 1;

void rt_bump_epoch(void) { g_epoch = (g_epoch + 1) & 0x7FFFFFFFu; if (!g_epoch) g_epoch = 1; }

uint32_t rt_epoch(void) { return g_epoch; }

static int api_verify(sh2_state *s, const sbpc_code *c, uint32_t *ok)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < c->len; i += 2) {
        uint16_t w = sh2_rd16(s, c->start + i);
        h = (h ^ (w >> 8)) * 16777619u;
        h = (h ^ (w & 0xFF)) * 16777619u;
    }
    if (h == c->hash) { *ok = g_epoch; return 1; }
    *ok = g_epoch | SBPC_BAD;
    if (s->dispatch) ((rt_dispatch *)s->dispatch)->code_mismatches++;
    return 0;
}

static const sbpc_rt API = {api_call, api_trap, &g_epoch, api_verify};

const sbpc_rt *rt_api(void) { return &API; }
