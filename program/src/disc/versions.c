#include "disc/versions.h"

#include <stddef.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const game_module DEFAULT_MODULES[] = {
    {"boot", NULL, 0, 0, "1st read file", NULL, 0},
};
const int DEFAULT_MODULE_COUNT = COUNT(DEFAULT_MODULES);

/* USA: /0 is a front-end. It copies a 140-byte routine (from 0x0604B4B0 in /0) to 0x00200000,
 * or just below 0x00300000, and jumps to it; that routine replaces high work RAM with
 * BOMSS/0KRNL.BIN at 0x06006000. The copied routine runs once from a moved address, so it is
 * left to the interpreter. The kernel's base comes from its own pointers: 7685 of them land on
 * function prologues at 0x06006000, next best 1696. FLD_KNL.BIN is a small program built for
 * 0x00200000 (23 pointer hits, next best 18); its first longword points at its entry. */
/* Kernel code nothing points at. 0x0600623A: the main loop, entered only by longjmp (setjmp at
 * 0x060061C4 saves its PR; longjmp at 0x060061E6 returns there), so it was all interpreted. */
/* 0x060B0E8E: an in-game routine (mov #68,r0; pushes) right after another function's rts that nothing
 * points at; the same routine in every version (Europe 0x060B09EE, Japan 0x060B1DF6). Found by the
 * parity matrix (SBPC_IHIST in Normal/Battle/Master on all three versions). */
static const uint32_t USA_KERNEL_HINTS[] = {0x0600623A, 0x060B0E8E};
static const uint32_t EUR_KERNEL_HINTS[] = {0x0600623A, 0x060B09EE};

static const game_module USA_MODULES[] = {
    {"boot", NULL, 0, 0, "front-end (1st read file /0)", NULL, 0},
    {"kernel", "/BOMSS/0KRNL.BIN", 0x06006000, 0x06006000, "game kernel", USA_KERNEL_HINTS, COUNT(USA_KERNEL_HINTS)},
    {"fld_knl", "/FLD_KNL.BIN", 0x00200000, 0x002001A4, "low-RAM program (who loads it: not yet known)", NULL, 0},
};

/* Europe (PAL): no NetLink front end; the 1st read file is /0KRNL.BIN itself, loaded by the BIOS at
 * 0x06006000 and entered there. Its code differs from the USA kernel from 0x06007382 on, but the start
 * matches, including the main loop entered by longjmp at 0x0600623A. */
static const game_module EUR_MODULES[] = {
    {"kernel", NULL, 0, 0, "game kernel (1st read file /0KRNL.BIN)", EUR_KERNEL_HINTS, COUNT(EUR_KERNEL_HINTS)},
};

/* Japan: the earliest release (1996-05-31). Like Europe, no NetLink front end: the 1st read file is
 * /0KRNL.BIN, linked for and loaded at 0x0600A000 (entry 0x0600A114). The main loop is found by the
 * USA bytes: setjmp 0x0600A164, longjmp 0x0600A186, main-loop function 0x0600A1CC, longjmp target
 * 0x0600A1DA. */
static const uint32_t JPN_KERNEL_HINTS[] = {0x0600A1DA, 0x0600AF6A, 0x060B1DF6};
static const game_module JPN_MODULES[] = {
    {"kernel", NULL, 0, 0, "game kernel (1st read file /0KRNL.BIN)", JPN_KERNEL_HINTS, COUNT(JPN_KERNEL_HINTS)},
};

/* To add a version: run `sbpc --cli identify <file.cue>` on a good dump (ideally Redump-verified)
 * and copy the printed product, version and data SHA-1 here. */
const game_version GAME_VERSIONS[] = {
    {"usa", "Saturn Bomberman (USA)", "MK-81070", "V1.003",
     "65a3bfeb654e5b3bb0f47105185298a62f50000e",
     "b95102652aac11a2ffd0460edd7972dc69df6643", 30, USA_MODULES, COUNT(USA_MODULES), 10, 0, "saturn-bomberman-usa"},
    {"eur", "Saturn Bomberman (Europe)", "MK-81070", "V1.000",
     "540c983bccea487f1f5aa602505fc2690f971eb2",
     "36d1cb71d4eb330dcaddd78d38477de074ed2143", 30, EUR_MODULES, COUNT(EUR_MODULES), 10, 1, "saturn-bomberman-europe"},
    {"jpn", "Saturn Bomberman (Japan)", "T-14302G", "V1.000",
     "09a5d7e2620c147a7b0d86d08d6ff111b5d19132",
     "", 30, JPN_MODULES, COUNT(JPN_MODULES), 10, 0, "saturn-bomberman-japan"},
    /* Japan demo (header date 1996-03-22, same product and version as the retail disc): PROGRAM.BIN, the
     * music and the movies are byte for byte the retail ones; only 176 bytes of the kernel differ (0x06031E7A-
     * 0x0603202D), so the retail layout and hints apply. */
    {"jdemo", "Saturn Bomberman (Japan) (Demo)", "T-14302G", "V1.000",
     "f79955c8d555841d66e58331e58bdc88540257e3",
     "", 30, JPN_MODULES, COUNT(JPN_MODULES), 10, 0, "saturn-bomberman-japan-demo"},
};

const int GAME_VERSION_COUNT = COUNT(GAME_VERSIONS);
