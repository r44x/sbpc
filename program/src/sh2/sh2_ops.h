/* SH-2 instruction semantics for every non-branch instruction, as inline functions with one
 * uniform signature:  sh2_x_<ID>(state, n, m, i, a)
 *   n, m : register fields
 *   i    : immediate or scaled displacement (whichever the instruction has)
 *   a    : PC-relative address (MOV.W/MOV.L @(disp,PC), MOVA)
 * The interpreter and the generated code both call these, so they can't disagree.
 * Semantics follow the SH-1/SH-2 programming manual. */
#ifndef SBPC_SH2_OPS_H
#define SBPC_SH2_OPS_H

#include "sh2_state.h"

#define SH2_OP(name) SH2_INLINE void sh2_x_##name(sh2_state *s, unsigned n, unsigned m, int32_t i, uint32_t a)
#define UNUSED_ARGS (void)n; (void)m; (void)i; (void)a
#define R s->r

/* ---- system / control ------------------------------------------------------------ */
SH2_OP(CLRT)   { UNUSED_ARGS; s->sr &= ~SR_T; }
SH2_OP(SETT)   { UNUSED_ARGS; s->sr |= SR_T; }
SH2_OP(CLRMAC) { UNUSED_ARGS; s->mach = s->macl = 0; }
SH2_OP(NOP)    { UNUSED_ARGS; (void)s; }
SH2_OP(SLEEP)  { UNUSED_ARGS; s->sleeping = 1; }
SH2_OP(DIV0U)  { UNUSED_ARGS; s->sr &= ~(SR_M | SR_Q | SR_T); }
SH2_OP(MOVT)   { UNUSED_ARGS; R[n] = s->sr & SR_T; }

SH2_OP(STC_SR)   { UNUSED_ARGS; R[n] = s->sr; }
SH2_OP(STC_GBR)  { UNUSED_ARGS; R[n] = s->gbr; }
SH2_OP(STC_VBR)  { UNUSED_ARGS; R[n] = s->vbr; }
SH2_OP(STS_MACH) { UNUSED_ARGS; R[n] = s->mach; }
SH2_OP(STS_MACL) { UNUSED_ARGS; R[n] = s->macl; }
SH2_OP(STS_PR)   { UNUSED_ARGS; R[n] = s->pr; }
SH2_OP(LDC_SR)   { UNUSED_ARGS; s->sr = R[n] & SR_MASK; }
SH2_OP(LDC_GBR)  { UNUSED_ARGS; s->gbr = R[n]; }
SH2_OP(LDC_VBR)  { UNUSED_ARGS; s->vbr = R[n]; }
SH2_OP(LDS_MACH) { UNUSED_ARGS; s->mach = R[n]; }
SH2_OP(LDS_MACL) { UNUSED_ARGS; s->macl = R[n]; }
SH2_OP(LDS_PR)   { UNUSED_ARGS; s->pr = R[n]; }
SH2_OP(LDCL_SR)  { UNUSED_ARGS; s->sr = sh2_rd32(s, R[n]) & SR_MASK; R[n] += 4; }
SH2_OP(LDCL_GBR) { UNUSED_ARGS; s->gbr = sh2_rd32(s, R[n]); R[n] += 4; }
SH2_OP(LDCL_VBR) { UNUSED_ARGS; s->vbr = sh2_rd32(s, R[n]); R[n] += 4; }
SH2_OP(LDSL_MACH){ UNUSED_ARGS; s->mach = sh2_rd32(s, R[n]); R[n] += 4; }
SH2_OP(LDSL_MACL){ UNUSED_ARGS; s->macl = sh2_rd32(s, R[n]); R[n] += 4; }
SH2_OP(LDSL_PR)  { UNUSED_ARGS; s->pr = sh2_rd32(s, R[n]); R[n] += 4; }
SH2_OP(STCL_SR)  { UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->sr); }
SH2_OP(STCL_GBR) { UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->gbr); }
SH2_OP(STCL_VBR) { UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->vbr); }
SH2_OP(STSL_MACH){ UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->mach); }
SH2_OP(STSL_MACL){ UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->macl); }
SH2_OP(STSL_PR)  { UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], s->pr); }

/* ---- compare / test --------------------------------------------------------------------- */
SH2_OP(DT)      { UNUSED_ARGS; R[n]--; sh2_set_t(s, R[n] == 0); }
SH2_OP(CMP_PZ)  { UNUSED_ARGS; sh2_set_t(s, (int32_t)R[n] >= 0); }
SH2_OP(CMP_PL)  { UNUSED_ARGS; sh2_set_t(s, (int32_t)R[n] > 0); }
SH2_OP(CMP_EQ)  { UNUSED_ARGS; sh2_set_t(s, R[n] == R[m]); }
SH2_OP(CMP_HS)  { UNUSED_ARGS; sh2_set_t(s, R[n] >= R[m]); }
SH2_OP(CMP_GE)  { UNUSED_ARGS; sh2_set_t(s, (int32_t)R[n] >= (int32_t)R[m]); }
SH2_OP(CMP_HI)  { UNUSED_ARGS; sh2_set_t(s, R[n] > R[m]); }
SH2_OP(CMP_GT)  { UNUSED_ARGS; sh2_set_t(s, (int32_t)R[n] > (int32_t)R[m]); }
SH2_OP(CMP_EQI) { UNUSED_ARGS; sh2_set_t(s, R[0] == (uint32_t)i); }
SH2_OP(CMP_STR) {
    uint32_t x = R[n] ^ R[m];
    UNUSED_ARGS;
    sh2_set_t(s, !(x & 0xFF000000u) || !(x & 0xFF0000u) || !(x & 0xFF00u) || !(x & 0xFFu));
}
SH2_OP(TST)  { UNUSED_ARGS; sh2_set_t(s, (R[n] & R[m]) == 0); }
SH2_OP(TSTI) { UNUSED_ARGS; sh2_set_t(s, (R[0] & (uint32_t)i) == 0); }
SH2_OP(TAS)  {
    uint8_t v = sh2_rd8(s, R[n]);
    UNUSED_ARGS;
    sh2_set_t(s, v == 0);
    sh2_wr8(s, R[n], (uint8_t)(v | 0x80));
}

/* ---- logic -------------------------------------------------------------------------------- */
SH2_OP(AND)  { UNUSED_ARGS; R[n] &= R[m]; }
SH2_OP(OR)   { UNUSED_ARGS; R[n] |= R[m]; }
SH2_OP(XOR)  { UNUSED_ARGS; R[n] ^= R[m]; }
SH2_OP(NOT)  { UNUSED_ARGS; R[n] = ~R[m]; }
SH2_OP(ANDI) { UNUSED_ARGS; R[0] &= (uint32_t)i; }
SH2_OP(ORI)  { UNUSED_ARGS; R[0] |= (uint32_t)i; }
SH2_OP(XORI) { UNUSED_ARGS; R[0] ^= (uint32_t)i; }
SH2_OP(TSTB) { UNUSED_ARGS; sh2_set_t(s, (sh2_rd8(s, s->gbr + R[0]) & (uint32_t)i) == 0); }
SH2_OP(ANDB) { uint32_t ad = s->gbr + R[0]; UNUSED_ARGS; sh2_wr8(s, ad, (uint8_t)(sh2_rd8(s, ad) & i)); }
SH2_OP(ORB)  { uint32_t ad = s->gbr + R[0]; UNUSED_ARGS; sh2_wr8(s, ad, (uint8_t)(sh2_rd8(s, ad) | i)); }
SH2_OP(XORB) { uint32_t ad = s->gbr + R[0]; UNUSED_ARGS; sh2_wr8(s, ad, (uint8_t)(sh2_rd8(s, ad) ^ i)); }

/* ---- shifts / rotates ---------------------------------------------------------------------- */
SH2_OP(ROTL)  { UNUSED_ARGS; sh2_set_t(s, R[n] >> 31); R[n] = R[n] << 1 | R[n] >> 31; }
SH2_OP(ROTR)  { UNUSED_ARGS; sh2_set_t(s, R[n] & 1); R[n] = R[n] >> 1 | R[n] << 31; }
SH2_OP(ROTCL) { uint32_t t = s->sr & SR_T; UNUSED_ARGS; sh2_set_t(s, R[n] >> 31); R[n] = R[n] << 1 | t; }
SH2_OP(ROTCR) { uint32_t t = s->sr & SR_T; UNUSED_ARGS; sh2_set_t(s, R[n] & 1); R[n] = R[n] >> 1 | t << 31; }
SH2_OP(SHAL)  { UNUSED_ARGS; sh2_set_t(s, R[n] >> 31); R[n] <<= 1; }
SH2_OP(SHLL)  { UNUSED_ARGS; sh2_set_t(s, R[n] >> 31); R[n] <<= 1; }
SH2_OP(SHAR)  { UNUSED_ARGS; sh2_set_t(s, R[n] & 1); R[n] = (R[n] >> 1) | (R[n] & 0x80000000u); }
SH2_OP(SHLR)  { UNUSED_ARGS; sh2_set_t(s, R[n] & 1); R[n] >>= 1; }
SH2_OP(SHLL2) { UNUSED_ARGS; R[n] <<= 2; }
SH2_OP(SHLR2) { UNUSED_ARGS; R[n] >>= 2; }
SH2_OP(SHLL8) { UNUSED_ARGS; R[n] <<= 8; }
SH2_OP(SHLR8) { UNUSED_ARGS; R[n] >>= 8; }
SH2_OP(SHLL16){ UNUSED_ARGS; R[n] <<= 16; }
SH2_OP(SHLR16){ UNUSED_ARGS; R[n] >>= 16; }

/* ---- arithmetic --------------------------------------------------------------------------- */
SH2_OP(ADD)  { UNUSED_ARGS; R[n] += R[m]; }
SH2_OP(ADDI) { UNUSED_ARGS; R[n] += (uint32_t)i; }
SH2_OP(SUB)  { UNUSED_ARGS; R[n] -= R[m]; }
SH2_OP(NEG)  { UNUSED_ARGS; R[n] = 0u - R[m]; }
SH2_OP(ADDC) {
    uint32_t t0 = R[n], t1 = R[n] + R[m];
    UNUSED_ARGS;
    R[n] = t1 + (s->sr & SR_T);
    sh2_set_t(s, t0 > t1 || t1 > R[n]);
}
SH2_OP(SUBC) {
    uint32_t t0 = R[n], t1 = R[n] - R[m];
    UNUSED_ARGS;
    R[n] = t1 - (s->sr & SR_T);
    sh2_set_t(s, t0 < t1 || t1 < R[n]);
}
SH2_OP(NEGC) {
    uint32_t t = 0u - R[m];
    UNUSED_ARGS;
    R[n] = t - (s->sr & SR_T);
    sh2_set_t(s, 0 < t || t < R[n]);
}
SH2_OP(ADDV) {
    uint32_t x = R[n], y = R[m], r = x + y;
    UNUSED_ARGS;
    R[n] = r;
    sh2_set_t(s, ((~(x ^ y) & (x ^ r)) >> 31) != 0);
}
SH2_OP(SUBV) {
    uint32_t x = R[n], y = R[m], r = x - y;
    UNUSED_ARGS;
    R[n] = r;
    sh2_set_t(s, (((x ^ y) & (x ^ r)) >> 31) != 0);
}
SH2_OP(EXTUB) { UNUSED_ARGS; R[n] = R[m] & 0xFFu; }
SH2_OP(EXTUW) { UNUSED_ARGS; R[n] = R[m] & 0xFFFFu; }
SH2_OP(EXTSB) { UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int8_t)R[m]; }
SH2_OP(EXTSW) { UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int16_t)R[m]; }
SH2_OP(SWAPB) { UNUSED_ARGS; R[n] = (R[m] & 0xFFFF0000u) | (R[m] & 0xFF) << 8 | (R[m] >> 8 & 0xFF); }
SH2_OP(SWAPW) { UNUSED_ARGS; R[n] = R[m] << 16 | R[m] >> 16; }
SH2_OP(XTRCT) { UNUSED_ARGS; R[n] = R[n] >> 16 | R[m] << 16; }

SH2_OP(MULL)  { UNUSED_ARGS; s->macl = R[n] * R[m]; }
SH2_OP(MULS)  { UNUSED_ARGS; s->macl = (uint32_t)((int32_t)(int16_t)R[n] * (int32_t)(int16_t)R[m]); }
SH2_OP(MULU)  { UNUSED_ARGS; s->macl = (R[n] & 0xFFFFu) * (R[m] & 0xFFFFu); }
SH2_OP(DMULS) {
    int64_t r = (int64_t)(int32_t)R[n] * (int64_t)(int32_t)R[m];
    UNUSED_ARGS;
    s->mach = (uint32_t)((uint64_t)r >> 32);
    s->macl = (uint32_t)r;
}
SH2_OP(DMULU) {
    uint64_t r = (uint64_t)R[n] * R[m];
    UNUSED_ARGS;
    s->mach = (uint32_t)(r >> 32);
    s->macl = (uint32_t)r;
}

/* MAC.L @Rm+,@Rn+ : with S set, the 64-bit result saturates to 48 bits. */
SH2_OP(MACL) {
    int64_t vn, vm, mac, prod;
    UNUSED_ARGS;
    vn = (int32_t)sh2_rd32(s, R[n]); R[n] += 4;
    vm = (int32_t)sh2_rd32(s, R[m]); R[m] += 4;
    prod = vn * vm;
    mac = (int64_t)((uint64_t)s->mach << 32 | s->macl);
    mac = (int64_t)((uint64_t)mac + (uint64_t)prod);
    if (s->sr & SR_S) {
        if (mac > (int64_t)0x00007FFFFFFFFFFFLL) mac = 0x00007FFFFFFFFFFFLL;
        else if (mac < -(int64_t)0x0000800000000000LL) mac = -(int64_t)0x0000800000000000LL;
    }
    s->mach = (uint32_t)((uint64_t)mac >> 32);
    s->macl = (uint32_t)mac;
}

/* MAC.W @Rm+,@Rn+ : with S set, 32-bit saturating add into MACL (MACH bit 0 flags overflow). */
SH2_OP(MACW) {
    int32_t vn, vm, prod;
    UNUSED_ARGS;
    vn = (int16_t)sh2_rd16(s, R[n]); R[n] += 2;
    vm = (int16_t)sh2_rd16(s, R[m]); R[m] += 2;
    prod = vn * vm;
    if (s->sr & SR_S) {
        int64_t r = (int64_t)(int32_t)s->macl + prod;
        if (r > 0x7FFFFFFF) { r = 0x7FFFFFFF; s->mach |= 1; }
        else if (r < -0x7FFFFFFF - 1) { r = -0x7FFFFFFF - 1; s->mach |= 1; }
        s->macl = (uint32_t)r;
    } else {
        uint64_t mac = (uint64_t)s->mach << 32 | s->macl;
        mac += (uint64_t)(int64_t)prod;
        s->mach = (uint32_t)(mac >> 32);
        s->macl = (uint32_t)mac;
    }
}

SH2_OP(DIV0S) {
    uint32_t q = R[n] >> 31, mm = R[m] >> 31;
    UNUSED_ARGS;
    s->sr = (s->sr & ~(SR_Q | SR_M | SR_T)) | (q ? SR_Q : 0) | (mm ? SR_M : 0) | (q != mm ? SR_T : 0);
}

/* One step of non-restoring division (manual algorithm). */
SH2_OP(DIV1) {
    uint32_t old_q = (s->sr & SR_Q) ? 1 : 0, M = (s->sr & SR_M) ? 1 : 0, Q, t0, t1, rm = R[m];
    UNUSED_ARGS;
    Q = R[n] >> 31;
    R[n] = R[n] << 1 | (s->sr & SR_T);
    t0 = R[n];
    if (old_q == M) {
        R[n] -= rm;
        t1 = R[n] > t0;
    } else {
        R[n] += rm;
        t1 = R[n] < t0;
    }
    /* new Q = Q xor M xor carry/borrow, written out as in the manual's case table */
    if (old_q == 0) {
        if (M == 0) Q = Q ? (t1 == 0) : t1;
        else        Q = Q ? t1 : (t1 == 0);
    } else {
        if (M == 0) Q = Q ? (t1 == 0) : t1;
        else        Q = Q ? t1 : (t1 == 0);
    }
    s->sr = (s->sr & ~(SR_Q | SR_T)) | (Q ? SR_Q : 0) | (Q == M ? SR_T : 0);
}

/* ---- data transfer --------------------------------------------------------------------------- */
SH2_OP(MOV)    { UNUSED_ARGS; R[n] = R[m]; }
SH2_OP(MOVI)   { UNUSED_ARGS; R[n] = (uint32_t)i; }
SH2_OP(MOVA)   { UNUSED_ARGS; R[0] = a; }
SH2_OP(MOVW_PC){ UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, a); }
SH2_OP(MOVL_PC){ UNUSED_ARGS; R[n] = sh2_rd32(s, a); }

SH2_OP(MOVB_S) { UNUSED_ARGS; sh2_wr8(s, R[n], (uint8_t)R[m]); }
SH2_OP(MOVW_S) { UNUSED_ARGS; sh2_wr16(s, R[n], (uint16_t)R[m]); }
SH2_OP(MOVL_S) { UNUSED_ARGS; sh2_wr32(s, R[n], R[m]); }
SH2_OP(MOVB_L) { UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int8_t)sh2_rd8(s, R[m]); }
SH2_OP(MOVW_L) { UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, R[m]); }
SH2_OP(MOVL_L) { UNUSED_ARGS; R[n] = sh2_rd32(s, R[m]); }
/* Pre-decrement stores: the manual decrements Rn first, then writes Rm (so Rn == Rm stores
 * the decremented value). */
SH2_OP(MOVB_M) { UNUSED_ARGS; R[n] -= 1; sh2_wr8(s, R[n], (uint8_t)R[m]); }
SH2_OP(MOVW_M) { UNUSED_ARGS; R[n] -= 2; sh2_wr16(s, R[n], (uint16_t)R[m]); }
SH2_OP(MOVL_M) { UNUSED_ARGS; R[n] -= 4; sh2_wr32(s, R[n], R[m]); }
SH2_OP(MOVB_P) { uint32_t v = (uint32_t)(int32_t)(int8_t)sh2_rd8(s, R[m]); UNUSED_ARGS; if (n != m) R[m] += 1; R[n] = v; }
SH2_OP(MOVW_P) { uint32_t v = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, R[m]); UNUSED_ARGS; if (n != m) R[m] += 2; R[n] = v; }
SH2_OP(MOVL_P) { uint32_t v = sh2_rd32(s, R[m]); UNUSED_ARGS; if (n != m) R[m] += 4; R[n] = v; }
SH2_OP(MOVB_S0){ UNUSED_ARGS; sh2_wr8(s, R[n] + R[0], (uint8_t)R[m]); }
SH2_OP(MOVW_S0){ UNUSED_ARGS; sh2_wr16(s, R[n] + R[0], (uint16_t)R[m]); }
SH2_OP(MOVL_S0){ UNUSED_ARGS; sh2_wr32(s, R[n] + R[0], R[m]); }
SH2_OP(MOVB_L0){ UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int8_t)sh2_rd8(s, R[m] + R[0]); }
SH2_OP(MOVW_L0){ UNUSED_ARGS; R[n] = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, R[m] + R[0]); }
SH2_OP(MOVL_L0){ UNUSED_ARGS; R[n] = sh2_rd32(s, R[m] + R[0]); }
SH2_OP(MOVL_S4){ UNUSED_ARGS; sh2_wr32(s, R[n] + (uint32_t)i, R[m]); }
SH2_OP(MOVL_L4){ UNUSED_ARGS; R[n] = sh2_rd32(s, R[m] + (uint32_t)i); }
SH2_OP(MOVB_S4){ UNUSED_ARGS; sh2_wr8(s, R[m] + (uint32_t)i, (uint8_t)R[0]); }
SH2_OP(MOVW_S4){ UNUSED_ARGS; sh2_wr16(s, R[m] + (uint32_t)i, (uint16_t)R[0]); }
SH2_OP(MOVB_L4){ UNUSED_ARGS; R[0] = (uint32_t)(int32_t)(int8_t)sh2_rd8(s, R[m] + (uint32_t)i); }
SH2_OP(MOVW_L4){ UNUSED_ARGS; R[0] = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, R[m] + (uint32_t)i); }
SH2_OP(MOVB_SG){ UNUSED_ARGS; sh2_wr8(s, s->gbr + (uint32_t)i, (uint8_t)R[0]); }
SH2_OP(MOVW_SG){ UNUSED_ARGS; sh2_wr16(s, s->gbr + (uint32_t)i, (uint16_t)R[0]); }
SH2_OP(MOVL_SG){ UNUSED_ARGS; sh2_wr32(s, s->gbr + (uint32_t)i, R[0]); }
SH2_OP(MOVB_LG){ UNUSED_ARGS; R[0] = (uint32_t)(int32_t)(int8_t)sh2_rd8(s, s->gbr + (uint32_t)i); }
SH2_OP(MOVW_LG){ UNUSED_ARGS; R[0] = (uint32_t)(int32_t)(int16_t)sh2_rd16(s, s->gbr + (uint32_t)i); }
SH2_OP(MOVL_LG){ UNUSED_ARGS; R[0] = sh2_rd32(s, s->gbr + (uint32_t)i); }

#undef R
#undef UNUSED_ARGS

#endif
