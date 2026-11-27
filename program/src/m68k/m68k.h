/* Motorola 68000 interpreter: the Saturn's sound CPU (11.29 MHz, runs the sound driver from sound RAM).
 * Plain 68000 (no 010/020 additions). Timing is approximate: 4 cycles per bus word plus the documented
 * extra cost of multiply, divide and shifts, which is what a sound driver's timing loops care about. */
#ifndef SBPC_M68K_H
#define SBPC_M68K_H

#include <stdbool.h>
#include <stdint.h>

typedef struct m68k_bus {
    void *user;
    uint8_t (*read8)(void *user, uint32_t addr);
    uint16_t (*read16)(void *user, uint32_t addr);
    void (*write8)(void *user, uint32_t addr, uint8_t v);
    void (*write16)(void *user, uint32_t addr, uint16_t v);
} m68k_bus;

typedef struct m68k {
    uint32_t d[8];
    uint32_t a[8];       /* a[7] is the active stack pointer */
    uint32_t other_sp;   /* the inactive one: USP in supervisor mode, SSP in user mode */
    uint32_t pc;
    uint16_t sr;         /* T . S . . I2 I1 I0 . . . X N Z V C */
    int irq_level;       /* level being requested on IPL0-2 (0 = none); autovectored */
    bool stopped;        /* STOP: waiting for an interrupt */
    bool halted;         /* double fault */
    int32_t cycles;      /* budget left in the current run */
    uint64_t total_cycles;
    uint32_t illegal_count;
    m68k_bus bus;
} m68k;

#define M68K_SR_S 0x2000u

/* Reads SSP from address 0 and PC from address 4. */
void m68k_reset(m68k *c);

/* Runs until at least `cycles` cycles have been used. Returns the cycles used. */
int m68k_run(m68k *c, int cycles);

/* Sets the interrupt level on the IPL pins (0-7). Level 7 is non-maskable. */
void m68k_set_irq(m68k *c, int level);

/* Writes SR, switching stack pointers when the S bit changes. */
void m68k_set_sr(m68k *c, uint16_t sr);

#endif
