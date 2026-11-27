#include "sh2/sh2_interp.h"
#include "sh2/sh2_decode.h"
#include "sh2/sh2_ops.h"

#include <string.h>

typedef void (*exec_fn)(sh2_state *, unsigned, unsigned, int32_t, uint32_t);

/* Non-branch instructions; NULL entries are control flow handled in sh2_step. */
static const exec_fn EXEC[SH2_OP_COUNT] = {
#define X(id) [SH2_##id] = sh2_x_##id
    X(CLRT), X(CLRMAC), X(DIV0U), X(NOP), X(SETT), X(SLEEP),
    X(MOVT), X(STC_SR), X(STC_GBR), X(STC_VBR), X(STS_MACH), X(STS_MACL), X(STS_PR),
    X(DT), X(CMP_PZ), X(CMP_PL), X(TAS),
    X(ROTL), X(ROTR), X(ROTCL), X(ROTCR), X(SHAL), X(SHAR), X(SHLL), X(SHLR),
    X(SHLL2), X(SHLR2), X(SHLL8), X(SHLR8), X(SHLL16), X(SHLR16),
    X(LDC_SR), X(LDC_GBR), X(LDC_VBR), X(LDCL_SR), X(LDCL_GBR), X(LDCL_VBR),
    X(LDS_MACH), X(LDS_MACL), X(LDS_PR), X(LDSL_MACH), X(LDSL_MACL), X(LDSL_PR),
    X(STCL_SR), X(STCL_GBR), X(STCL_VBR), X(STSL_MACH), X(STSL_MACL), X(STSL_PR),
    X(MOVB_S0), X(MOVW_S0), X(MOVL_S0), X(MOVB_L0), X(MOVW_L0), X(MOVL_L0),
    X(MULL), X(MACL),
    X(MOVB_S), X(MOVW_S), X(MOVL_S), X(MOVB_M), X(MOVW_M), X(MOVL_M),
    X(DIV0S), X(TST), X(AND), X(XOR), X(OR), X(CMP_STR), X(XTRCT), X(MULU), X(MULS),
    X(CMP_EQ), X(CMP_HS), X(CMP_GE), X(DIV1), X(DMULU), X(CMP_HI), X(CMP_GT),
    X(SUB), X(SUBC), X(SUBV), X(ADD), X(DMULS), X(ADDC), X(ADDV), X(MACW),
    X(MOVB_L), X(MOVW_L), X(MOVL_L), X(MOV), X(MOVB_P), X(MOVW_P), X(MOVL_P),
    X(NOT), X(SWAPB), X(SWAPW), X(NEGC), X(NEG), X(EXTUB), X(EXTUW), X(EXTSB), X(EXTSW),
    X(MOVL_S4), X(MOVL_L4), X(ADDI), X(MOVW_PC), X(MOVL_PC), X(MOVI),
    X(MOVB_S4), X(MOVW_S4), X(MOVB_L4), X(MOVW_L4), X(CMP_EQI),
    X(MOVB_SG), X(MOVW_SG), X(MOVL_SG), X(MOVB_LG), X(MOVW_LG), X(MOVL_LG),
    X(MOVA), X(TSTI), X(ANDI), X(XORI), X(ORI), X(TSTB), X(ANDB), X(XORB), X(ORB),
#undef X
};

void sh2_reset_state(sh2_state *s)
{
    memset(s->r, 0, sizeof s->r);
    s->sr = SR_IMASK; /* interrupts masked at reset */
    s->gbr = s->vbr = s->mach = s->macl = s->pr = s->pc = 0;
    s->cycles = 0;
    s->sleeping = 0;
}

void sh2_exception(sh2_state *s, int vector, uint32_t return_pc)
{
    s->r[15] -= 4;
    sh2_wr32(s, s->r[15], s->sr);
    s->r[15] -= 4;
    sh2_wr32(s, s->r[15], return_pc);
    s->pc = sh2_rd32(s, s->vbr + (uint32_t)vector * 4);
    s->sleeping = 0;
}

int sh2_interrupt(sh2_state *s, int level, int vector)
{
    if (level <= (int)((s->sr & SR_IMASK) >> 4))
        return 0;
    sh2_exception(s, vector, s->pc);
    s->sr = (s->sr & ~SR_IMASK) | ((uint32_t)level << 4);
    return 1;
}

static void illegal(sh2_state *s, int vector, uint32_t pc, uint32_t return_pc)
{
    if (s->on_exception)
        s->on_exception(s, vector, pc);
    sh2_exception(s, vector, return_pc);
}

static void exec_insn(sh2_state *s, const sh2_insn *in)
{
    EXEC[in->op](s, in->n, in->m, in->imm + in->disp, in->target);
}

/* Executes the delay slot at pc+2. Returns 0 if the slot instruction is illegal there. */
static int slot(sh2_state *s, uint32_t branch_pc)
{
    sh2_insn in;
    uint32_t spc = branch_pc + 2;
    if (!sh2_decode(sh2_rd16(s, spc), spc, &in)) {
        illegal(s, SH2_VEC_ILLEGAL, spc, spc);
        return 0;
    }
    if (in.flags & SH2F_NO_SLOT) {
        illegal(s, SH2_VEC_SLOT_ILLEGAL, spc, branch_pc);
        return 0;
    }
    exec_insn(s, &in);
    return 1;
}

int sh2_step(sh2_state *s)
{
    sh2_insn in;
    uint32_t pc = s->pc, t;
    int taken;

    if (!sh2_decode(sh2_rd16(s, pc), pc, &in)) {
        illegal(s, SH2_VEC_ILLEGAL, pc, pc);
        return 1;
    }
    switch (in.op) {
    case SH2_BT: case SH2_BF:
        taken = (int)sh2_t(s) == (in.op == SH2_BT);
        s->pc = taken ? in.target : pc + 2;
        return taken ? 3 : 1;
    case SH2_BTS: case SH2_BFS:
        taken = (int)sh2_t(s) == (in.op == SH2_BTS);
        if (!slot(s, pc)) return 2;
        s->pc = taken ? in.target : pc + 4;
        return taken ? 2 : 1;
    case SH2_BRA:
        if (!slot(s, pc)) return 2;
        s->pc = in.target;
        return 2;
    case SH2_BSR:
        s->pr = pc + 4;
        if (!slot(s, pc)) return 2;
        s->pc = in.target;
        return 2;
    case SH2_BRAF:
        t = pc + 4 + s->r[in.n];
        if (!slot(s, pc)) return 2;
        s->pc = t;
        return 2;
    case SH2_BSRF:
        t = pc + 4 + s->r[in.n];
        s->pr = pc + 4;
        if (!slot(s, pc)) return 2;
        s->pc = t;
        return 2;
    case SH2_JMP:
        t = s->r[in.n];
        if (!slot(s, pc)) return 2;
        s->pc = t;
        return 2;
    case SH2_JSR:
        t = s->r[in.n];
        s->pr = pc + 4;
        if (!slot(s, pc)) return 2;
        s->pc = t;
        return 2;
    case SH2_RTS:
        t = s->pr;
        if (!slot(s, pc)) return 2;
        s->pc = t;
        return 2;
    case SH2_RTE:
        t = sh2_rd32(s, s->r[15]);
        s->r[15] += 4;
        s->sr = sh2_rd32(s, s->r[15]) & SR_MASK;
        s->r[15] += 4;
        if (!slot(s, pc)) return 4;
        s->pc = t;
        return 4;
    case SH2_TRAPA:
        sh2_exception(s, in.imm, pc + 2);
        return 8;
    default:
        exec_insn(s, &in);
        s->pc = pc + 2;
        return in.op == SH2_MACL || in.op == SH2_MACW || in.op == SH2_DMULS || in.op == SH2_DMULU ? 2 : 1;
    }
}

void sh2_run(sh2_state *s)
{
    while (s->cycles > 0 && !s->sleeping) {
        int c = sh2_step(s);
        s->cycles -= c;
        s->total_cycles += (uint64_t)c;
    }
}
