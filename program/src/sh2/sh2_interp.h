/* SH-2 interpreter: runs code that wasn't recompiled, and is the reference that the
 * generated code is checked against. */
#ifndef SBPC_SH2_INTERP_H
#define SBPC_SH2_INTERP_H

#include "sh2/sh2_state.h"

/* Exception vectors (SH7604) */
#define SH2_VEC_ILLEGAL       4
#define SH2_VEC_SLOT_ILLEGAL  6

void sh2_reset_state(sh2_state *s);

/* Executes one instruction (a delayed branch together with its slot). Returns cycles used. */
int sh2_step(sh2_state *s);

/* Runs until s->cycles <= 0 or the CPU sleeps. */
void sh2_run(sh2_state *s);

/* Takes an exception / interrupt through VBR: pushes SR and PC, jumps to the vector. */
void sh2_exception(sh2_state *s, int vector, uint32_t return_pc);
/* Accepts an interrupt if level > SR.IMASK. Returns 1 if taken. */
int sh2_interrupt(sh2_state *s, int level, int vector);

#endif
