/* SH-2 CPU state and memory access, shared by the interpreter, the runtime and the
 * generated code (which is compiled on the user's PC with this header). */
#ifndef SBPC_SH2_STATE_H
#define SBPC_SH2_STATE_H

#include <stdint.h>

/* SR bits */
#define SR_T 0x001u
#define SR_S 0x002u
#define SR_IMASK 0x0F0u
#define SR_Q 0x100u
#define SR_M 0x200u
#define SR_MASK 0x3F3u

typedef struct sh2_state sh2_state;

/* Slow-path bus: anything not in a fast RAM page (I/O registers, VDP, sound, CD block...). */
typedef struct {
    uint8_t (*rd8)(sh2_state *s, uint32_t a);
    uint16_t (*rd16)(sh2_state *s, uint32_t a);
    uint32_t (*rd32)(sh2_state *s, uint32_t a);
    void (*wr8)(sh2_state *s, uint32_t a, uint8_t v);
    void (*wr16)(sh2_state *s, uint32_t a, uint16_t v);
    void (*wr32)(sh2_state *s, uint32_t a, uint32_t v);
} sh2_bus;

#define SH2_PAGE_SHIFT 20                          /* 1 MB pages */
#define SH2_PAGES (0x20000000u >> SH2_PAGE_SHIFT)  /* covers the 512 MB physical space */

struct sh2_state {
    uint32_t r[16];
    uint32_t sr, gbr, vbr, mach, macl, pr, pc;
    int32_t cycles;            /* remaining in the current time slice; code yields at <= 0 */
    uint64_t total_cycles;
    uint8_t *page[SH2_PAGES];  /* big-endian RAM for each 1 MB page, or NULL for the slow path */
    sh2_bus bus;
    void *user;                /* owner (the Saturn machine) */
    void *dispatch;            /* runtime dispatcher (recompiled function lookup) */
    int id;                    /* 0 = master, 1 = slave */
    int sleeping;
    /* Called for TRAPA, illegal instructions and similar events the runtime must handle. */
    void (*on_exception)(sh2_state *s, int vector, uint32_t pc);
};

#if defined(_MSC_VER)
#define SH2_INLINE static __inline
#else
#define SH2_INLINE static inline
#endif

/* Cache-through mirror (0x2xxxxxxx) maps onto the same memory. Addresses at or above
 * 0x40000000 (cache control, on-chip registers) always take the slow path. */
SH2_INLINE uint8_t *sh2_fast(sh2_state *s, uint32_t a)
{
    uint8_t *p;
    if (a >= 0x40000000u)
        return 0;
    a &= 0x1FFFFFFFu;
    p = s->page[a >> SH2_PAGE_SHIFT];
    return p ? p + (a & ((1u << SH2_PAGE_SHIFT) - 1)) : 0;
}

SH2_INLINE uint8_t sh2_rd8(sh2_state *s, uint32_t a)
{
    uint8_t *p = sh2_fast(s, a);
    return p ? p[0] : s->bus.rd8(s, a);
}
SH2_INLINE uint16_t sh2_rd16(sh2_state *s, uint32_t a)
{
    uint8_t *p = sh2_fast(s, a);
    return p ? (uint16_t)(p[0] << 8 | p[1]) : s->bus.rd16(s, a);
}
SH2_INLINE uint32_t sh2_rd32(sh2_state *s, uint32_t a)
{
    uint8_t *p = sh2_fast(s, a);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : s->bus.rd32(s, a);
}
SH2_INLINE void sh2_wr8(sh2_state *s, uint32_t a, uint8_t v)
{
    uint8_t *p = sh2_fast(s, a);
    if (p) p[0] = v;
    else s->bus.wr8(s, a, v);
}
SH2_INLINE void sh2_wr16(sh2_state *s, uint32_t a, uint16_t v)
{
    uint8_t *p = sh2_fast(s, a);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else s->bus.wr16(s, a, v);
}
SH2_INLINE void sh2_wr32(sh2_state *s, uint32_t a, uint32_t v)
{
    uint8_t *p = sh2_fast(s, a);
    if (p) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    else s->bus.wr32(s, a, v);
}

SH2_INLINE uint32_t sh2_t(const sh2_state *s) { return s->sr & SR_T; }
SH2_INLINE void sh2_set_t(sh2_state *s, int t) { s->sr = (s->sr & ~SR_T) | (t ? SR_T : 0); }

#endif
