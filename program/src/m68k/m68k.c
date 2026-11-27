#include "m68k/m68k.h"

#include <string.h>

/* ---- bus ------------------------------------------------------------------------------------------- */

static uint8_t rd8(m68k *c, uint32_t a) { c->cycles -= 4; return c->bus.read8(c->bus.user, a & 0xFFFFFF); }
static uint16_t rd16(m68k *c, uint32_t a) { c->cycles -= 4; return c->bus.read16(c->bus.user, a & 0xFFFFFE); }
static uint32_t rd32(m68k *c, uint32_t a) { uint32_t hi = rd16(c, a); return hi << 16 | rd16(c, a + 2); }
static void wr8(m68k *c, uint32_t a, uint32_t v) { c->cycles -= 4; c->bus.write8(c->bus.user, a & 0xFFFFFF, (uint8_t)v); }
static void wr16(m68k *c, uint32_t a, uint32_t v) { c->cycles -= 4; c->bus.write16(c->bus.user, a & 0xFFFFFE, (uint16_t)v); }
static void wr32(m68k *c, uint32_t a, uint32_t v) { wr16(c, a, v >> 16); wr16(c, a + 2, v); }

static uint16_t fetch16(m68k *c) { uint16_t v = rd16(c, c->pc); c->pc += 2; return v; }
static uint32_t fetch32(m68k *c) { uint32_t hi = fetch16(c); return hi << 16 | fetch16(c); }

static void push16(m68k *c, uint32_t v) { c->a[7] -= 2; wr16(c, c->a[7], v); }
static void push32(m68k *c, uint32_t v) { c->a[7] -= 4; wr32(c, c->a[7], v); }
static uint16_t pop16(m68k *c) { uint16_t v = rd16(c, c->a[7]); c->a[7] += 2; return v; }
static uint32_t pop32(m68k *c) { uint32_t v = rd32(c, c->a[7]); c->a[7] += 4; return v; }

/* ---- flags & sizes ----------------------------------------------------------------------------------- */

enum { FC = 1, FV = 2, FZ = 4, FN = 8, FX = 16 };

static const uint32_t MASK[3] = {0xFF, 0xFFFF, 0xFFFFFFFFu};
static const uint32_t MSB[3] = {0x80, 0x8000, 0x80000000u};
static const int BYTES[3] = {1, 2, 4};

static int32_t sext(uint32_t v, int sz)
{
    return sz == 0 ? (int32_t)(int8_t)v : sz == 1 ? (int32_t)(int16_t)v : (int32_t)v;
}

void m68k_set_sr(m68k *c, uint16_t sr)
{
    sr &= 0xA71F;
    if ((sr ^ c->sr) & M68K_SR_S) {
        uint32_t t = c->a[7];
        c->a[7] = c->other_sp;
        c->other_sp = t;
    }
    c->sr = sr;
}

static void set_nz(m68k *c, uint32_t v, int sz)
{
    c->sr &= (uint16_t)~(FN | FZ | FV | FC);
    if (!(v & MASK[sz])) c->sr |= FZ;
    if (v & MSB[sz]) c->sr |= FN;
}

static void set_flag(m68k *c, int f, bool on) { if (on) c->sr |= (uint16_t)f; else c->sr &= (uint16_t)~f; }

static uint32_t do_add(m68k *c, uint32_t s, uint32_t d, int sz, int x, bool set_x, bool keep_z)
{
    uint64_t r64 = (uint64_t)(s & MASK[sz]) + (d & MASK[sz]) + (uint64_t)x;
    uint32_t r = (uint32_t)r64 & MASK[sz];
    bool carry = (r64 >> (BYTES[sz] * 8)) & 1;
    bool ovf = ((s ^ r) & (d ^ r) & MSB[sz]) != 0;
    bool zero = keep_z ? (r == 0 && (c->sr & FZ)) : r == 0;
    c->sr &= (uint16_t)~(FN | FZ | FV | FC);
    if (r & MSB[sz]) c->sr |= FN;
    if (zero) c->sr |= FZ;
    if (ovf) c->sr |= FV;
    if (carry) c->sr |= FC;
    if (set_x) set_flag(c, FX, carry);
    return r;
}

/* d - s - x */
static uint32_t do_sub(m68k *c, uint32_t s, uint32_t d, int sz, int x, bool set_x, bool keep_z)
{
    uint32_t sm = s & MASK[sz], dm = d & MASK[sz];
    uint32_t r = (dm - sm - (uint32_t)x) & MASK[sz];
    bool borrow = (uint64_t)sm + (uint64_t)x > dm;
    bool ovf = ((s ^ d) & (r ^ d) & MSB[sz]) != 0;
    bool zero = keep_z ? (r == 0 && (c->sr & FZ)) : r == 0;
    c->sr &= (uint16_t)~(FN | FZ | FV | FC);
    if (r & MSB[sz]) c->sr |= FN;
    if (zero) c->sr |= FZ;
    if (ovf) c->sr |= FV;
    if (borrow) c->sr |= FC;
    if (set_x) set_flag(c, FX, borrow);
    return r;
}

static bool cond(m68k *c, int cc)
{
    uint16_t f = c->sr;
    bool C = f & FC, V = f & FV, Z = f & FZ, N = f & FN;
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !C && !Z;      /* HI */
    case 3: return C || Z;        /* LS */
    case 4: return !C;            /* CC */
    case 5: return C;             /* CS */
    case 6: return !Z;            /* NE */
    case 7: return Z;             /* EQ */
    case 8: return !V;            /* VC */
    case 9: return V;             /* VS */
    case 10: return !N;           /* PL */
    case 11: return N;            /* MI */
    case 12: return N == V;       /* GE */
    case 13: return N != V;       /* LT */
    case 14: return !Z && N == V; /* GT */
    default: return Z || N != V;  /* LE */
    }
}

/* ---- exceptions ---------------------------------------------------------------------------------------- */

static void exception(m68k *c, int vector)
{
    uint16_t old = c->sr;
    m68k_set_sr(c, (uint16_t)((old | M68K_SR_S) & ~0x8000u));
    push32(c, c->pc);
    push16(c, old);
    c->pc = rd32(c, (uint32_t)vector * 4);
    c->cycles -= 18;
}

static void privilege(m68k *c, uint32_t insn_pc) { c->pc = insn_pc; exception(c, 8); }

/* ---- effective addresses ----------------------------------------------------------------------------------- */

typedef struct {
    int kind;      /* 0 Dn, 1 An, 2 memory, 3 immediate */
    int reg;
    uint32_t addr; /* memory address, or the immediate value */
} loc;

static uint32_t index_ext(m68k *c, uint32_t base)
{
    uint16_t ext = fetch16(c);
    int r = (ext >> 12) & 7;
    uint32_t xn = (ext & 0x8000) ? c->a[r] : c->d[r];
    if (!(ext & 0x800)) xn = (uint32_t)(int32_t)(int16_t)xn;
    c->cycles -= 2;
    return base + xn + (uint32_t)(int32_t)(int8_t)ext;
}

static loc ea(m68k *c, int mode, int reg, int sz)
{
    loc l = {2, reg, 0};
    switch (mode) {
    case 0: l.kind = 0; break;
    case 1: l.kind = 1; break;
    case 2: l.addr = c->a[reg]; break;
    case 3:
        l.addr = c->a[reg];
        c->a[reg] += (sz == 0 && reg == 7) ? 2 : (uint32_t)BYTES[sz];
        break;
    case 4:
        c->a[reg] -= (sz == 0 && reg == 7) ? 2 : (uint32_t)BYTES[sz];
        l.addr = c->a[reg];
        c->cycles -= 2;
        break;
    case 5: l.addr = c->a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(c); break;
    case 6: l.addr = index_ext(c, c->a[reg]); break;
    default:
        switch (reg) {
        case 0: l.addr = (uint32_t)(int32_t)(int16_t)fetch16(c); break;
        case 1: l.addr = fetch32(c); break;
        case 2: { uint32_t base = c->pc; l.addr = base + (uint32_t)(int32_t)(int16_t)fetch16(c); break; }
        case 3: { uint32_t base = c->pc; l.addr = index_ext(c, base); break; }
        default:
            l.kind = 3;
            l.addr = sz == 2 ? fetch32(c) : (fetch16(c) & MASK[sz]);
            break;
        }
    }
    return l;
}

static uint32_t rd_loc(m68k *c, loc l, int sz)
{
    switch (l.kind) {
    case 0: return c->d[l.reg] & MASK[sz];
    case 1: return c->a[l.reg] & MASK[sz];
    case 3: return l.addr & MASK[sz];
    default: return sz == 0 ? rd8(c, l.addr) : sz == 1 ? rd16(c, l.addr) : rd32(c, l.addr);
    }
}

static void wr_loc(m68k *c, loc l, int sz, uint32_t v)
{
    switch (l.kind) {
    case 0: c->d[l.reg] = (c->d[l.reg] & ~MASK[sz]) | (v & MASK[sz]); break;
    case 1: c->a[l.reg] = v; break;
    case 3: break;
    default:
        if (sz == 0) wr8(c, l.addr, v);
        else if (sz == 1) wr16(c, l.addr, v);
        else wr32(c, l.addr, v);
    }
}

static uint32_t read_ea(m68k *c, int mode, int reg, int sz) { return rd_loc(c, ea(c, mode, reg, sz), sz); }

/* ---- instruction groups ----------------------------------------------------------------------------------- */

static void illegal(m68k *c, uint32_t insn_pc, int vector)
{
    c->illegal_count++;
    c->pc = insn_pc;
    exception(c, vector);
}

static void op_bitops(m68k *c, uint16_t op, uint32_t ipc);
static void op_misc(m68k *c, uint16_t op, uint32_t ipc);

/* 0x0xxx: immediate ops, bit ops, MOVEP */
static void group0(m68k *c, uint16_t op, uint32_t ipc)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 6) & 3;
    if ((op & 0x0100) || (op & 0x0F00) == 0x0800) {
        op_bitops(c, op, ipc);
        return;
    }
    if (sz == 3) { illegal(c, ipc, 4); return; }
    {
        int kind = (op >> 9) & 7;   /* 0 ORI 1 ANDI 2 SUBI 3 ADDI 5 EORI 6 CMPI */
        uint32_t imm = sz == 2 ? fetch32(c) : (fetch16(c) & MASK[sz]);
        loc l;
        uint32_t d, r;
        if (mode == 7 && reg == 4 && (kind == 0 || kind == 1 || kind == 5)) {   /* to CCR / SR */
            uint16_t cur = c->sr, v;
            if (sz == 1 && !(c->sr & M68K_SR_S)) { privilege(c, ipc); return; }
            if (kind == 0) v = (uint16_t)(cur | imm);
            else if (kind == 1) v = (uint16_t)(cur & (sz == 0 ? (imm | 0xFF00) : imm));
            else v = (uint16_t)(cur ^ imm);
            if (sz == 0) v = (uint16_t)((cur & 0xFF00) | (v & 0x1F));
            m68k_set_sr(c, v);
            return;
        }
        l = ea(c, mode, reg, sz);
        d = rd_loc(c, l, sz);
        switch (kind) {
        case 0: r = d | imm; set_nz(c, r, sz); break;
        case 1: r = d & imm; set_nz(c, r, sz); break;
        case 2: r = do_sub(c, imm, d, sz, 0, true, false); break;
        case 3: r = do_add(c, imm, d, sz, 0, true, false); break;
        case 5: r = d ^ imm; set_nz(c, r, sz); break;
        case 6: do_sub(c, imm, d, sz, 0, false, false); return;
        default: illegal(c, ipc, 4); return;
        }
        wr_loc(c, l, sz, r);
    }
}

static void op_bitops(m68k *c, uint16_t op, uint32_t ipc)
{
    int mode = (op >> 3) & 7, reg = op & 7, type = (op >> 6) & 3;
    uint32_t bit;
    (void)ipc;
    if ((op & 0x0138) == 0x0108) {                        /* MOVEP */
        int dr = (op >> 9) & 7, opmode = (op >> 6) & 7;
        uint32_t a = c->a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(c);
        switch (opmode) {
        case 4: c->d[dr] = (c->d[dr] & 0xFFFF0000u) | (uint32_t)rd8(c, a) << 8 | rd8(c, a + 2); break;
        case 5: c->d[dr] = (uint32_t)rd8(c, a) << 24 | (uint32_t)rd8(c, a + 2) << 16 | (uint32_t)rd8(c, a + 4) << 8 | rd8(c, a + 6); break;
        case 6: wr8(c, a, c->d[dr] >> 8); wr8(c, a + 2, c->d[dr]); break;
        default: wr8(c, a, c->d[dr] >> 24); wr8(c, a + 2, c->d[dr] >> 16); wr8(c, a + 4, c->d[dr] >> 8); wr8(c, a + 6, c->d[dr]); break;
        }
        return;
    }
    bit = (op & 0x0100) ? c->d[(op >> 9) & 7] : fetch16(c);
    if (mode == 0) {
        uint32_t m = 1u << (bit & 31);
        set_flag(c, FZ, !(c->d[reg] & m));
        if (type == 1) c->d[reg] ^= m;
        else if (type == 2) c->d[reg] &= ~m;
        else if (type == 3) c->d[reg] |= m;
        c->cycles -= 2;
    } else {
        loc l = ea(c, mode, reg, 0);
        uint32_t v = rd_loc(c, l, 0), m = 1u << (bit & 7);
        set_flag(c, FZ, !(v & m));
        if (type == 1) wr_loc(c, l, 0, v ^ m);
        else if (type == 2) wr_loc(c, l, 0, v & ~m);
        else if (type == 3) wr_loc(c, l, 0, v | m);
    }
}

/* 0x1xxx-0x3xxx: MOVE / MOVEA */
static void op_move(m68k *c, uint16_t op)
{
    static const int SZ[4] = {-1, 0, 2, 1};
    int sz = SZ[op >> 12];
    int smode = (op >> 3) & 7, sreg = op & 7, dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    uint32_t v = read_ea(c, smode, sreg, sz);
    if (dmode == 1) {                                     /* MOVEA: sign-extended, flags untouched */
        c->a[dreg] = (uint32_t)sext(v, sz);
        return;
    }
    wr_loc(c, ea(c, dmode, dreg, sz), sz, v);
    set_nz(c, v, sz);
}

static void movem(m68k *c, uint16_t op)
{
    int sz = (op & 0x40) ? 2 : 1, mode = (op >> 3) & 7, reg = op & 7;
    uint16_t list = fetch16(c);
    if (op & 0x0400) {                                    /* memory -> registers */
        uint32_t a;
        if (mode == 3) a = c->a[reg];
        else { loc l = ea(c, mode, reg, sz); a = l.addr; }
        for (int i = 0; i < 16; i++) {
            if (!(list & (1 << i))) continue;
            uint32_t v = sz == 2 ? rd32(c, a) : (uint32_t)(int32_t)(int16_t)rd16(c, a);
            if (i < 8) c->d[i] = v; else c->a[i - 8] = v;
            a += (uint32_t)BYTES[sz];
        }
        if (mode == 3) c->a[reg] = a;
    } else if (mode == 4) {                               /* registers -> -(An): list is reversed */
        uint32_t a = c->a[reg];
        for (int i = 0; i < 16; i++) {
            if (!(list & (1 << i))) continue;
            int r = 15 - i;
            uint32_t v = r < 8 ? c->d[r] : c->a[r - 8];
            a -= (uint32_t)BYTES[sz];
            if (sz == 2) wr32(c, a, v); else wr16(c, a, v);
        }
        c->a[reg] = a;
    } else {
        loc l = ea(c, mode, reg, sz);
        uint32_t a = l.addr;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1 << i))) continue;
            uint32_t v = i < 8 ? c->d[i] : c->a[i - 8];
            if (sz == 2) wr32(c, a, v); else wr16(c, a, v);
            a += (uint32_t)BYTES[sz];
        }
    }
}

static uint32_t bcd_add(m68k *c, uint32_t s, uint32_t d)
{
    int x = (c->sr & FX) ? 1 : 0;
    int lo = (int)(s & 0xF) + (int)(d & 0xF) + x;
    int hi = (int)(s & 0xF0) + (int)(d & 0xF0);
    int r;
    if (lo > 9) lo += 6;
    r = hi + lo;
    if (r > 0x99) r += 0x60;
    set_flag(c, FC, r > 0xFF);
    set_flag(c, FX, r > 0xFF);
    if (r & 0xFF) c->sr &= (uint16_t)~FZ;
    set_flag(c, FN, r & 0x80);
    return (uint32_t)r & 0xFF;
}

static uint32_t bcd_sub(m68k *c, uint32_t s, uint32_t d)
{
    int x = (c->sr & FX) ? 1 : 0;
    int lo = (int)(d & 0xF) - (int)(s & 0xF) - x;
    int hi = (int)(d & 0xF0) - (int)(s & 0xF0);
    int r;
    bool borrow;
    if (lo < 0) { lo += 10; hi -= 0x10; }
    if (hi < 0) hi -= 0x60;
    r = hi + lo;
    borrow = (int)(d & 0xFF) - (int)(s & 0xFF) - x < 0 || r < 0;
    set_flag(c, FC, borrow);
    set_flag(c, FX, borrow);
    if (r & 0xFF) c->sr &= (uint16_t)~FZ;
    set_flag(c, FN, r & 0x80);
    return (uint32_t)r & 0xFF;
}

/* 0x4xxx */
static void op_misc(m68k *c, uint16_t op, uint32_t ipc)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 6) & 3;
    bool super = (c->sr & M68K_SR_S) != 0;

    if ((op & 0xF1C0) == 0x41C0) {                        /* LEA */
        c->a[(op >> 9) & 7] = ea(c, mode, reg, 2).addr;
        return;
    }
    if ((op & 0xF1C0) == 0x4180) {                        /* CHK.W */
        int16_t bound = (int16_t)read_ea(c, mode, reg, 1);
        int16_t v = (int16_t)c->d[(op >> 9) & 7];
        if (v < 0 || v > bound) {
            set_flag(c, FN, v < 0);
            exception(c, 6);
        }
        return;
    }
    switch (op & 0x0FC0) {
    case 0x00C0: {                                        /* MOVE from SR */
        wr_loc(c, ea(c, mode, reg, 1), 1, c->sr);
        return;
    }
    case 0x04C0:                                          /* MOVE to CCR */
        m68k_set_sr(c, (uint16_t)((c->sr & 0xFF00) | (read_ea(c, mode, reg, 1) & 0x1F)));
        return;
    case 0x06C0:                                          /* MOVE to SR */
        if (!super) { privilege(c, ipc); return; }
        m68k_set_sr(c, (uint16_t)read_ea(c, mode, reg, 1));
        return;
    case 0x0800:                                          /* NBCD */
        { loc l = ea(c, mode, reg, 0); wr_loc(c, l, 0, bcd_sub(c, rd_loc(c, l, 0), 0)); }
        return;
    case 0x0840:
        if (mode == 0) {                                  /* SWAP */
            c->d[reg] = c->d[reg] << 16 | c->d[reg] >> 16;
            set_nz(c, c->d[reg], 2);
        } else {                                          /* PEA */
            uint32_t a = ea(c, mode, reg, 2).addr;
            push32(c, a);
        }
        return;
    case 0x0880: case 0x08C0:
        if (mode == 0) {                                  /* EXT.W / EXT.L */
            if (op & 0x40) c->d[reg] = (uint32_t)(int32_t)(int16_t)c->d[reg];
            else c->d[reg] = (c->d[reg] & 0xFFFF0000u) | ((uint32_t)(int32_t)(int8_t)c->d[reg] & 0xFFFF);
            set_nz(c, c->d[reg], (op & 0x40) ? 2 : 1);
        } else {
            movem(c, op);
        }
        return;
    case 0x0C80: case 0x0CC0:
        movem(c, op);
        return;
    case 0x0AC0:
        if (op == 0x4AFC) { illegal(c, ipc, 4); return; }  /* ILLEGAL */
        {                                                 /* TAS */
            loc l = ea(c, mode, reg, 0);
            uint32_t v = rd_loc(c, l, 0);
            set_nz(c, v, 0);
            wr_loc(c, l, 0, v | 0x80);
        }
        return;
    default: break;
    }
    if (sz != 3 && ((op & 0xFF00) == 0x4000 || (op & 0xFF00) == 0x4200 || (op & 0xFF00) == 0x4400 ||
                    (op & 0xFF00) == 0x4600 || (op & 0xFF00) == 0x4A00)) {
        loc l = ea(c, mode, reg, sz);
        uint32_t v = rd_loc(c, l, sz), r;
        switch (op & 0xFF00) {
        case 0x4000: r = do_sub(c, v, 0, sz, (c->sr & FX) ? 1 : 0, true, true); break;   /* NEGX */
        case 0x4200: r = 0; set_nz(c, 0, sz); break;                                      /* CLR */
        case 0x4400: r = do_sub(c, v, 0, sz, 0, true, false); break;                       /* NEG */
        case 0x4600: r = ~v; set_nz(c, r, sz); break;                                       /* NOT */
        default: set_nz(c, v, sz); return;                                                  /* TST */
        }
        wr_loc(c, l, sz, r);
        return;
    }
    if ((op & 0xFFF0) == 0x4E40) { exception(c, 32 + (op & 15)); return; }   /* TRAP */
    if ((op & 0xFFF8) == 0x4E50) {                        /* LINK */
        int16_t disp = (int16_t)fetch16(c);
        push32(c, c->a[reg]);
        c->a[reg] = c->a[7];
        c->a[7] += (uint32_t)(int32_t)disp;
        return;
    }
    if ((op & 0xFFF8) == 0x4E58) {                        /* UNLK */
        c->a[7] = c->a[reg];
        c->a[reg] = pop32(c);
        return;
    }
    if ((op & 0xFFF0) == 0x4E60) {                        /* MOVE USP */
        if (!super) { privilege(c, ipc); return; }
        if (op & 8) c->a[reg] = c->other_sp; else c->other_sp = c->a[reg];
        return;
    }
    switch (op) {
    case 0x4E70: if (!super) privilege(c, ipc); else c->cycles -= 128; return;   /* RESET */
    case 0x4E71: return;                                                          /* NOP */
    case 0x4E72: {                                                                /* STOP */
        uint16_t v = fetch16(c);
        if (!super) { privilege(c, ipc); return; }
        m68k_set_sr(c, v);
        c->stopped = true;
        return;
    }
    case 0x4E73: {                                                                /* RTE */
        uint16_t sr;
        uint32_t pc;
        if (!super) { privilege(c, ipc); return; }
        sr = pop16(c);
        pc = pop32(c);
        m68k_set_sr(c, sr);
        c->pc = pc;
        return;
    }
    case 0x4E75: c->pc = pop32(c); return;                                        /* RTS */
    case 0x4E76: if (c->sr & FV) exception(c, 7); return;                         /* TRAPV */
    case 0x4E77: {                                                                /* RTR */
        uint16_t ccr = pop16(c);
        m68k_set_sr(c, (uint16_t)((c->sr & 0xFF00) | (ccr & 0x1F)));
        c->pc = pop32(c);
        return;
    }
    default: break;
    }
    if ((op & 0xFFC0) == 0x4E80) {                        /* JSR */
        uint32_t a = ea(c, mode, reg, 2).addr;
        push32(c, c->pc);
        c->pc = a;
        return;
    }
    if ((op & 0xFFC0) == 0x4EC0) {                        /* JMP */
        c->pc = ea(c, mode, reg, 2).addr;
        return;
    }
    illegal(c, ipc, 4);
}

/* 0x5xxx: ADDQ, SUBQ, Scc, DBcc */
static void group5(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op >> 6) & 3;
    if (sz == 3) {
        int cc = (op >> 8) & 15;
        if (mode == 1) {                                  /* DBcc */
            uint32_t base = c->pc;
            int16_t disp = (int16_t)fetch16(c);
            if (!cond(c, cc)) {
                uint16_t cnt = (uint16_t)(c->d[reg] - 1);
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | cnt;
                if (cnt != 0xFFFF) c->pc = base + (uint32_t)(int32_t)disp;
            }
            return;
        }
        wr_loc(c, ea(c, mode, reg, 0), 0, cond(c, cc) ? 0xFF : 0);   /* Scc */
        return;
    }
    {
        uint32_t q = (uint32_t)((op >> 9) & 7);
        if (q == 0) q = 8;
        if (mode == 1) {                                  /* to An: whole register, no flags */
            c->a[reg] = (op & 0x100) ? c->a[reg] - q : c->a[reg] + q;
            return;
        }
        {
            loc l = ea(c, mode, reg, sz);
            uint32_t d = rd_loc(c, l, sz);
            wr_loc(c, l, sz, (op & 0x100) ? do_sub(c, q, d, sz, 0, true, false) : do_add(c, q, d, sz, 0, true, false));
        }
    }
}

/* 0x6xxx: BRA, BSR, Bcc */
static void group6(m68k *c, uint16_t op)
{
    uint32_t base = c->pc;
    int32_t disp = (int8_t)(op & 0xFF);
    int cc = (op >> 8) & 15;
    if (disp == 0) disp = (int16_t)fetch16(c);
    if (cc == 1) {                                        /* BSR */
        push32(c, c->pc);
        c->pc = base + (uint32_t)disp;
    } else if (cond(c, cc)) {
        c->pc = base + (uint32_t)disp;
        c->cycles -= 2;
    }
}

static void divide(m68k *c, uint16_t op, bool is_signed)
{
    int dr = (op >> 9) & 7;
    uint32_t src = read_ea(c, (op >> 3) & 7, op & 7, 1);
    c->cycles -= is_signed ? 150 : 130;
    if ((src & 0xFFFF) == 0) { exception(c, 5); return; }
    c->sr &= (uint16_t)~FC;
    if (is_signed) {
        int32_t dd = (int32_t)c->d[dr], ds = (int16_t)src;
        int32_t q, r;
        if (dd == INT32_MIN && ds == -1) { c->sr |= FV; return; }
        q = dd / ds;
        r = dd % ds;
        if (q > 32767 || q < -32768) { c->sr |= FV; return; }
        c->d[dr] = ((uint32_t)r & 0xFFFF) << 16 | ((uint32_t)q & 0xFFFF);
        set_nz(c, (uint32_t)q, 1);
    } else {
        uint32_t q = c->d[dr] / (src & 0xFFFF), r = c->d[dr] % (src & 0xFFFF);
        if (q > 0xFFFF) { c->sr |= FV; return; }
        c->d[dr] = r << 16 | q;
        set_nz(c, q, 1);
    }
}

/* 0x8xxx OR/DIVx/SBCD, 0xCxxx AND/MULx/ABCD/EXG */
static void group_logic(m68k *c, uint16_t op, bool is_and)
{
    int mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, opmode = (op >> 6) & 7;
    if (opmode == 3 || opmode == 7) {
        if (!is_and) { divide(c, op, opmode == 7); return; }
        {                                                 /* MULU / MULS */
            uint32_t s = read_ea(c, mode, reg, 1), r;
            if (opmode == 7) r = (uint32_t)((int32_t)(int16_t)s * (int32_t)(int16_t)c->d[dr]);
            else r = (s & 0xFFFF) * (c->d[dr] & 0xFFFF);
            c->d[dr] = r;
            set_nz(c, r, 2);
            c->cycles -= 54;
            return;
        }
    }
    if ((op & 0x01F0) == 0x0100) {                        /* ABCD / SBCD */
        if (op & 8) {
            uint32_t s, d;
            c->a[reg] -= reg == 7 ? 2 : 1;
            s = rd8(c, c->a[reg]);
            c->a[dr] -= dr == 7 ? 2 : 1;
            d = rd8(c, c->a[dr]);
            wr8(c, c->a[dr], is_and ? bcd_add(c, s, d) : bcd_sub(c, s, d));
        } else {
            uint32_t r = is_and ? bcd_add(c, c->d[reg], c->d[dr]) : bcd_sub(c, c->d[reg], c->d[dr]);
            c->d[dr] = (c->d[dr] & ~0xFFu) | r;
        }
        return;
    }
    if (is_and && (op & 0x0130) == 0x0100 && ((op >> 3) & 0x1F) != 0) {   /* EXG */
        int m = (op >> 3) & 0x1F;
        uint32_t t;
        if (m == 0x08) { t = c->d[dr]; c->d[dr] = c->d[reg]; c->d[reg] = t; return; }
        if (m == 0x09) { t = c->a[dr]; c->a[dr] = c->a[reg]; c->a[reg] = t; return; }
        if (m == 0x11) { t = c->d[dr]; c->d[dr] = c->a[reg]; c->a[reg] = t; return; }
    }
    {
        int sz = opmode & 3;
        if (opmode & 4) {                                 /* Dn op <ea> -> <ea> */
            loc l = ea(c, mode, reg, sz);
            uint32_t r = is_and ? (rd_loc(c, l, sz) & c->d[dr]) : (rd_loc(c, l, sz) | c->d[dr]);
            wr_loc(c, l, sz, r);
            set_nz(c, r, sz);
        } else {
            uint32_t r = read_ea(c, mode, reg, sz);
            r = is_and ? (r & c->d[dr]) : (r | c->d[dr]);
            c->d[dr] = (c->d[dr] & ~MASK[sz]) | (r & MASK[sz]);
            set_nz(c, r, sz);
        }
    }
}

/* 0x9xxx SUB/SUBA/SUBX, 0xDxxx ADD/ADDA/ADDX */
static void group_arith(m68k *c, uint16_t op, bool is_add)
{
    int mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, opmode = (op >> 6) & 7;
    if (opmode == 3 || opmode == 7) {                     /* ADDA / SUBA */
        int sz = opmode == 7 ? 2 : 1;
        uint32_t s = (uint32_t)sext(read_ea(c, mode, reg, sz), sz);
        c->a[dr] = is_add ? c->a[dr] + s : c->a[dr] - s;
        return;
    }
    {
        int sz = opmode & 3;
        int x = (c->sr & FX) ? 1 : 0;
        if ((opmode & 4) && mode <= 1) {                  /* ADDX / SUBX */
            if (mode == 0) {
                uint32_t r = is_add ? do_add(c, c->d[reg], c->d[dr], sz, x, true, true)
                                    : do_sub(c, c->d[reg], c->d[dr], sz, x, true, true);
                c->d[dr] = (c->d[dr] & ~MASK[sz]) | r;
            } else {
                uint32_t s, d, step;
                step = (sz == 0 && reg == 7) ? 2 : (uint32_t)BYTES[sz];
                c->a[reg] -= step;
                s = rd_loc(c, (loc){2, 0, c->a[reg]}, sz);
                step = (sz == 0 && dr == 7) ? 2 : (uint32_t)BYTES[sz];
                c->a[dr] -= step;
                d = rd_loc(c, (loc){2, 0, c->a[dr]}, sz);
                wr_loc(c, (loc){2, 0, c->a[dr]}, sz,
                       is_add ? do_add(c, s, d, sz, x, true, true) : do_sub(c, s, d, sz, x, true, true));
            }
            return;
        }
        if (opmode & 4) {                                 /* Dn op <ea> -> <ea> */
            loc l = ea(c, mode, reg, sz);
            uint32_t d = rd_loc(c, l, sz);
            wr_loc(c, l, sz, is_add ? do_add(c, c->d[dr], d, sz, 0, true, false) : do_sub(c, c->d[dr], d, sz, 0, true, false));
        } else {
            uint32_t s = read_ea(c, mode, reg, sz);
            uint32_t r = is_add ? do_add(c, s, c->d[dr], sz, 0, true, false) : do_sub(c, s, c->d[dr], sz, 0, true, false);
            c->d[dr] = (c->d[dr] & ~MASK[sz]) | r;
        }
    }
}

/* 0xBxxx CMP/CMPA/CMPM/EOR */
static void groupB(m68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, opmode = (op >> 6) & 7;
    if (opmode == 3 || opmode == 7) {                     /* CMPA */
        int sz = opmode == 7 ? 2 : 1;
        uint32_t s = (uint32_t)sext(read_ea(c, mode, reg, sz), sz);
        do_sub(c, s, c->a[dr], 2, 0, false, false);
        return;
    }
    if (opmode < 3) {                                     /* CMP */
        do_sub(c, read_ea(c, mode, reg, opmode), c->d[dr], opmode, 0, false, false);
        return;
    }
    {
        int sz = opmode & 3;
        if (mode == 1) {                                  /* CMPM (Ay)+,(Ax)+ */
            uint32_t s = read_ea(c, 3, reg, sz);
            uint32_t d = read_ea(c, 3, dr, sz);
            do_sub(c, s, d, sz, 0, false, false);
            return;
        }
        {                                                 /* EOR Dn,<ea> */
            loc l = ea(c, mode, reg, sz);
            uint32_t r = rd_loc(c, l, sz) ^ c->d[dr];
            wr_loc(c, l, sz, r);
            set_nz(c, r, sz);
        }
    }
}

/* shift/rotate one value; type 0 AS, 1 LS, 2 ROX, 3 RO */
static uint32_t shift(m68k *c, int type, bool left, uint32_t v, int count, int sz)
{
    uint32_t m = MASK[sz], top = MSB[sz];
    int bits = BYTES[sz] * 8;
    bool carry = false, ovf = false;
    v &= m;
    c->cycles -= 2 * count;
    if (count == 0) {
        c->sr &= (uint16_t)~(FN | FZ | FV | FC);
        if (type == 2 && (c->sr & FX)) c->sr |= FC;
        if (!v) c->sr |= FZ;
        if (v & top) c->sr |= FN;
        return v;
    }
    for (int i = 0; i < count; i++) {
        if (left) {
            carry = (v & top) != 0;
            switch (type) {
            case 0: { uint32_t nv = (v << 1) & m; if ((nv ^ v) & top) ovf = true; v = nv; break; }
            case 1: v = (v << 1) & m; break;
            case 2: v = ((v << 1) | ((c->sr & FX) ? 1 : 0)) & m; break;
            default: v = ((v << 1) | (carry ? 1 : 0)) & m; break;
            }
        } else {
            carry = v & 1;
            switch (type) {
            case 0: v = (v >> 1) | (v & top); break;
            case 1: v >>= 1; break;
            case 2: v = (v >> 1) | ((c->sr & FX) ? top : 0); break;
            default: v = (v >> 1) | (carry ? top : 0); break;
            }
        }
        if (type != 3) set_flag(c, FX, carry);
    }
    (void)bits;
    c->sr &= (uint16_t)~(FN | FZ | FV | FC);
    if (carry) c->sr |= FC;
    if (ovf) c->sr |= FV;
    if (!v) c->sr |= FZ;
    if (v & top) c->sr |= FN;
    return v;
}

static void groupE(m68k *c, uint16_t op)
{
    int sz = (op >> 6) & 3, reg = op & 7;
    bool left = (op & 0x100) != 0;
    if (sz == 3) {                                        /* memory, by one, word */
        int type = (op >> 9) & 3;
        loc l = ea(c, (op >> 3) & 7, reg, 1);
        wr_loc(c, l, 1, shift(c, type, left, rd_loc(c, l, 1), 1, 1));
        return;
    }
    {
        int type = (op >> 3) & 3;
        int cnt = (op >> 9) & 7;
        if (op & 0x20) cnt = (int)(c->d[cnt] & 63);
        else if (cnt == 0) cnt = 8;
        c->d[reg] = (c->d[reg] & ~MASK[sz]) | shift(c, type, left, c->d[reg], cnt, sz);
    }
}

/* ---- main loop ------------------------------------------------------------------------------------------------ */

static void step(m68k *c)
{
    uint32_t ipc = c->pc;
    uint16_t op = fetch16(c);
    switch (op >> 12) {
    case 0x0: group0(c, op, ipc); break;
    case 0x1: case 0x2: case 0x3: op_move(c, op); break;
    case 0x4: op_misc(c, op, ipc); break;
    case 0x5: group5(c, op); break;
    case 0x6: group6(c, op); break;
    case 0x7:
        if (op & 0x100) { illegal(c, ipc, 4); break; }
        c->d[(op >> 9) & 7] = (uint32_t)(int32_t)(int8_t)op;
        set_nz(c, c->d[(op >> 9) & 7], 2);
        break;
    case 0x8: group_logic(c, op, false); break;
    case 0x9: group_arith(c, op, false); break;
    case 0xA: illegal(c, ipc, 10); break;
    case 0xB: groupB(c, op); break;
    case 0xC: group_logic(c, op, true); break;
    case 0xD: group_arith(c, op, true); break;
    case 0xE: groupE(c, op); break;
    default: illegal(c, ipc, 11); break;
    }
}

static void check_irq(m68k *c)
{
    int mask = (c->sr >> 8) & 7;
    if (c->irq_level > 0 && (c->irq_level > mask || c->irq_level == 7)) {
        int lvl = c->irq_level;
        c->stopped = false;
        exception(c, 24 + lvl);
        c->sr = (uint16_t)((c->sr & ~0x0700u) | (unsigned)(lvl << 8));
        if (lvl == 7) c->irq_level = 0;   /* edge triggered */
    }
}

void m68k_reset(m68k *c)
{
    m68k_bus bus = c->bus;
    memset(c, 0, sizeof *c);
    c->bus = bus;
    c->sr = 0x2700;
    c->a[7] = rd32(c, 0);
    c->pc = rd32(c, 4);
    c->cycles = 0;
}

void m68k_set_irq(m68k *c, int level) { c->irq_level = level & 7; }

int m68k_run(m68k *c, int cycles)
{
    int32_t start;
    c->cycles += cycles;
    start = c->cycles;
    while (c->cycles > 0 && !c->halted) {
        check_irq(c);
        if (c->stopped) { c->cycles = 0; break; }
        step(c);
    }
    c->total_cycles += (uint64_t)(start - c->cycles);
    return start - c->cycles;
}
