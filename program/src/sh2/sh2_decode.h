/* SH-2 (SH7604) instruction decoder and disassembler. All 142 instructions. */
#ifndef SBPC_SH2_DECODE_H
#define SBPC_SH2_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Instruction flags */
enum {
    SH2F_BRANCH    = 1 << 0,  /* changes control flow (branch, jump, call, return, trap) */
    SH2F_DELAY     = 1 << 1,  /* has a delay slot */
    SH2F_COND      = 1 << 2,  /* conditional (BT/BF and their /S forms) */
    SH2F_CALL      = 1 << 3,  /* BSR, BSRF, JSR */
    SH2F_RETURN    = 1 << 4,  /* RTS, RTE */
    SH2F_INDIRECT  = 1 << 5,  /* target comes from a register (JMP, JSR, BRAF, BSRF) */
    SH2F_END       = 1 << 6,  /* execution never falls through (after the delay slot) */
    SH2F_PCLOAD    = 1 << 7,  /* reads a PC-relative literal: target = literal address */
    SH2F_PCADDR    = 1 << 8,  /* MOVA: target = an address computed from PC (not read) */
    SH2F_TRAP      = 1 << 9,  /* TRAPA */
    SH2F_NO_SLOT   = 1 << 10  /* illegal in a delay slot */
};

typedef struct {
    uint16_t raw;
    uint16_t op;       /* index into the instruction table; SH2_OP_INVALID if none */
    uint8_t n, m;      /* register fields */
    int32_t imm;       /* immediate (sign-extended where the instruction does so) */
    int32_t disp;      /* scaled displacement for @(disp,Rn) / @(disp,GBR) forms */
    uint32_t target;   /* branch target / literal address (PC-relative forms only) */
    uint8_t size;      /* memory access size for loads/stores of literals: 1, 2, 4 */
    uint16_t flags;
    const char *mnemonic;
} sh2_insn;

#define SH2_OP_INVALID 0xFFFF

/* Instruction ids: same order as the decoder's table (checked by test_sh2_decode).
 * Suffixes: _S0/_L0 = @(r0,Rn) store/load, _S/_L = @Rn, _M = @-Rn, _P = @Rm+,
 * _S4/_L4 = @(disp,Rn), _SG/_LG = @(disp,GBR), _PC = PC-relative, I = immediate,
 * B (on logic ops) = .B @(r0,GBR). */
enum {
    SH2_CLRT, SH2_CLRMAC, SH2_DIV0U, SH2_NOP, SH2_RTE, SH2_RTS, SH2_SETT, SH2_SLEEP,
    SH2_MOVT, SH2_STC_SR, SH2_STC_GBR, SH2_STC_VBR, SH2_STS_MACH, SH2_STS_MACL, SH2_STS_PR,
    SH2_BRAF, SH2_BSRF, SH2_DT, SH2_CMP_PZ, SH2_CMP_PL, SH2_TAS,
    SH2_ROTL, SH2_ROTR, SH2_ROTCL, SH2_ROTCR, SH2_SHAL, SH2_SHAR, SH2_SHLL, SH2_SHLR,
    SH2_SHLL2, SH2_SHLR2, SH2_SHLL8, SH2_SHLR8, SH2_SHLL16, SH2_SHLR16,
    SH2_JMP, SH2_JSR, SH2_LDC_SR, SH2_LDC_GBR, SH2_LDC_VBR, SH2_LDCL_SR, SH2_LDCL_GBR, SH2_LDCL_VBR,
    SH2_LDS_MACH, SH2_LDS_MACL, SH2_LDS_PR, SH2_LDSL_MACH, SH2_LDSL_MACL, SH2_LDSL_PR,
    SH2_STCL_SR, SH2_STCL_GBR, SH2_STCL_VBR, SH2_STSL_MACH, SH2_STSL_MACL, SH2_STSL_PR,
    SH2_MOVB_S0, SH2_MOVW_S0, SH2_MOVL_S0, SH2_MOVB_L0, SH2_MOVW_L0, SH2_MOVL_L0,
    SH2_MULL, SH2_MACL,
    SH2_MOVB_S, SH2_MOVW_S, SH2_MOVL_S, SH2_MOVB_M, SH2_MOVW_M, SH2_MOVL_M,
    SH2_DIV0S, SH2_TST, SH2_AND, SH2_XOR, SH2_OR, SH2_CMP_STR, SH2_XTRCT, SH2_MULU, SH2_MULS,
    SH2_CMP_EQ, SH2_CMP_HS, SH2_CMP_GE, SH2_DIV1, SH2_DMULU, SH2_CMP_HI, SH2_CMP_GT,
    SH2_SUB, SH2_SUBC, SH2_SUBV, SH2_ADD, SH2_DMULS, SH2_ADDC, SH2_ADDV, SH2_MACW,
    SH2_MOVB_L, SH2_MOVW_L, SH2_MOVL_L, SH2_MOV, SH2_MOVB_P, SH2_MOVW_P, SH2_MOVL_P,
    SH2_NOT, SH2_SWAPB, SH2_SWAPW, SH2_NEGC, SH2_NEG, SH2_EXTUB, SH2_EXTUW, SH2_EXTSB, SH2_EXTSW,
    SH2_MOVL_S4, SH2_MOVL_L4, SH2_ADDI, SH2_MOVW_PC, SH2_BRA, SH2_BSR, SH2_MOVL_PC, SH2_MOVI,
    SH2_MOVB_S4, SH2_MOVW_S4, SH2_MOVB_L4, SH2_MOVW_L4, SH2_CMP_EQI,
    SH2_BT, SH2_BF, SH2_BTS, SH2_BFS,
    SH2_MOVB_SG, SH2_MOVW_SG, SH2_MOVL_SG, SH2_TRAPA, SH2_MOVB_LG, SH2_MOVW_LG, SH2_MOVL_LG,
    SH2_MOVA, SH2_TSTI, SH2_ANDI, SH2_XORI, SH2_ORI, SH2_TSTB, SH2_ANDB, SH2_XORB, SH2_ORB,
    SH2_OP_COUNT
};

/* Decodes the instruction `raw` located at address `pc`. Returns false for invalid opcodes. */
bool sh2_decode(uint16_t raw, uint32_t pc, sh2_insn *out);

/* Writes e.g. "mov.l @(0x06010040),r3" into buf. */
void sh2_disasm(const sh2_insn *in, char *buf, size_t len);

/* Number of instruction forms in the table (142 for SH-2). */
int sh2_op_count(void);
const char *sh2_op_name(int op);
uint16_t sh2_op_mask(int op);
uint16_t sh2_op_match(int op);
const char *sh2_op_operands(int op); /* operand template, e.g. "r%m,@(%d,r%n)" */

#endif
