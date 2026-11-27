#include "sh2/sh2_decode.h"

#include <stdio.h>
#include <string.h>

/* How an instruction's 8/12-bit field is interpreted. */
typedef enum {
    K_NONE,
    K_D4,   /* disp4 * scale  (@(disp,Rn)) */
    K_D8,   /* disp8 * scale  (@(disp,GBR)) */
    K_IS,   /* signed imm8 */
    K_IU,   /* unsigned imm8 */
    K_PCW,  /* PC + 4 + disp8*2       (MOV.W @(disp,PC)) */
    K_PCL,  /* (PC & ~3) + 4 + disp8*4 (MOV.L @(disp,PC), MOVA) */
    K_BR8,  /* PC + 4 + sext(disp8)*2 */
    K_BR12  /* PC + 4 + sext(disp12)*2 */
} field_kind;

typedef struct {
    uint16_t mask, match;
    const char *name; /* mnemonic */
    const char *fmt;  /* operands: %n Rn, %m Rm, %s signed imm, %u unsigned imm, %d disp, %t target */
    uint8_t kind;
    uint8_t scale;
    uint16_t flags;
} op_def;

#define BR   (SH2F_BRANCH | SH2F_NO_SLOT)
#define DS   SH2F_DELAY
#define END  SH2F_END

static const op_def OPS[] = {
    /* ---- no operands ---------------------------------------------------------- */
    {0xFFFF, 0x0008, "clrt",    "",                K_NONE, 0, 0},
    {0xFFFF, 0x0028, "clrmac",  "",                K_NONE, 0, 0},
    {0xFFFF, 0x0019, "div0u",   "",                K_NONE, 0, 0},
    {0xFFFF, 0x0009, "nop",     "",                K_NONE, 0, 0},
    {0xFFFF, 0x002B, "rte",     "",                K_NONE, 0, BR | DS | END | SH2F_RETURN},
    {0xFFFF, 0x000B, "rts",     "",                K_NONE, 0, BR | DS | END | SH2F_RETURN},
    {0xFFFF, 0x0018, "sett",    "",                K_NONE, 0, 0},
    {0xFFFF, 0x001B, "sleep",   "",                K_NONE, 0, 0},

    /* ---- one register (bits 8-11) -------------------------------------------- */
    {0xF0FF, 0x0029, "movt",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x0002, "stc",     "sr,r%n",          K_NONE, 0, 0},
    {0xF0FF, 0x0012, "stc",     "gbr,r%n",         K_NONE, 0, 0},
    {0xF0FF, 0x0022, "stc",     "vbr,r%n",         K_NONE, 0, 0},
    {0xF0FF, 0x000A, "sts",     "mach,r%n",        K_NONE, 0, 0},
    {0xF0FF, 0x001A, "sts",     "macl,r%n",        K_NONE, 0, 0},
    {0xF0FF, 0x002A, "sts",     "pr,r%n",          K_NONE, 0, 0},
    {0xF0FF, 0x0023, "braf",    "r%n",             K_NONE, 0, BR | DS | END | SH2F_INDIRECT},
    {0xF0FF, 0x0003, "bsrf",    "r%n",             K_NONE, 0, BR | DS | SH2F_CALL | SH2F_INDIRECT},
    {0xF0FF, 0x4010, "dt",      "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4011, "cmp/pz",  "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4015, "cmp/pl",  "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x401B, "tas.b",   "@r%n",            K_NONE, 0, 0},
    {0xF0FF, 0x4004, "rotl",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4005, "rotr",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4024, "rotcl",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4025, "rotcr",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4020, "shal",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4021, "shar",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4000, "shll",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4001, "shlr",    "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4008, "shll2",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4009, "shlr2",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4018, "shll8",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4019, "shlr8",   "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4028, "shll16",  "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x4029, "shlr16",  "r%n",             K_NONE, 0, 0},
    {0xF0FF, 0x402B, "jmp",     "@r%n",            K_NONE, 0, BR | DS | END | SH2F_INDIRECT},
    {0xF0FF, 0x400B, "jsr",     "@r%n",            K_NONE, 0, BR | DS | SH2F_CALL | SH2F_INDIRECT},
    {0xF0FF, 0x400E, "ldc",     "r%n,sr",          K_NONE, 0, 0},
    {0xF0FF, 0x401E, "ldc",     "r%n,gbr",         K_NONE, 0, 0},
    {0xF0FF, 0x402E, "ldc",     "r%n,vbr",         K_NONE, 0, 0},
    {0xF0FF, 0x4007, "ldc.l",   "@r%n+,sr",        K_NONE, 0, 0},
    {0xF0FF, 0x4017, "ldc.l",   "@r%n+,gbr",       K_NONE, 0, 0},
    {0xF0FF, 0x4027, "ldc.l",   "@r%n+,vbr",       K_NONE, 0, 0},
    {0xF0FF, 0x400A, "lds",     "r%n,mach",        K_NONE, 0, 0},
    {0xF0FF, 0x401A, "lds",     "r%n,macl",        K_NONE, 0, 0},
    {0xF0FF, 0x402A, "lds",     "r%n,pr",          K_NONE, 0, 0},
    {0xF0FF, 0x4006, "lds.l",   "@r%n+,mach",      K_NONE, 0, 0},
    {0xF0FF, 0x4016, "lds.l",   "@r%n+,macl",      K_NONE, 0, 0},
    {0xF0FF, 0x4026, "lds.l",   "@r%n+,pr",        K_NONE, 0, 0},
    {0xF0FF, 0x4003, "stc.l",   "sr,@-r%n",        K_NONE, 0, 0},
    {0xF0FF, 0x4013, "stc.l",   "gbr,@-r%n",       K_NONE, 0, 0},
    {0xF0FF, 0x4023, "stc.l",   "vbr,@-r%n",       K_NONE, 0, 0},
    {0xF0FF, 0x4002, "sts.l",   "mach,@-r%n",      K_NONE, 0, 0},
    {0xF0FF, 0x4012, "sts.l",   "macl,@-r%n",      K_NONE, 0, 0},
    {0xF0FF, 0x4022, "sts.l",   "pr,@-r%n",        K_NONE, 0, 0},

    /* ---- two registers --------------------------------------------------------- */
    {0xF00F, 0x0004, "mov.b",   "r%m,@(r0,r%n)",   K_NONE, 0, 0},
    {0xF00F, 0x0005, "mov.w",   "r%m,@(r0,r%n)",   K_NONE, 0, 0},
    {0xF00F, 0x0006, "mov.l",   "r%m,@(r0,r%n)",   K_NONE, 0, 0},
    {0xF00F, 0x000C, "mov.b",   "@(r0,r%m),r%n",   K_NONE, 0, 0},
    {0xF00F, 0x000D, "mov.w",   "@(r0,r%m),r%n",   K_NONE, 0, 0},
    {0xF00F, 0x000E, "mov.l",   "@(r0,r%m),r%n",   K_NONE, 0, 0},
    {0xF00F, 0x0007, "mul.l",   "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x000F, "mac.l",   "@r%m+,@r%n+",     K_NONE, 0, 0},
    {0xF00F, 0x2000, "mov.b",   "r%m,@r%n",        K_NONE, 0, 0},
    {0xF00F, 0x2001, "mov.w",   "r%m,@r%n",        K_NONE, 0, 0},
    {0xF00F, 0x2002, "mov.l",   "r%m,@r%n",        K_NONE, 0, 0},
    {0xF00F, 0x2004, "mov.b",   "r%m,@-r%n",       K_NONE, 0, 0},
    {0xF00F, 0x2005, "mov.w",   "r%m,@-r%n",       K_NONE, 0, 0},
    {0xF00F, 0x2006, "mov.l",   "r%m,@-r%n",       K_NONE, 0, 0},
    {0xF00F, 0x2007, "div0s",   "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x2008, "tst",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x2009, "and",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200A, "xor",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200B, "or",      "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200C, "cmp/str", "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200D, "xtrct",   "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200E, "mulu.w",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x200F, "muls.w",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3000, "cmp/eq",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3002, "cmp/hs",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3003, "cmp/ge",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3004, "div1",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3005, "dmulu.l", "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3006, "cmp/hi",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3007, "cmp/gt",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x3008, "sub",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300A, "subc",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300B, "subv",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300C, "add",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300D, "dmuls.l", "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300E, "addc",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x300F, "addv",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x400F, "mac.w",   "@r%m+,@r%n+",     K_NONE, 0, 0},
    {0xF00F, 0x6000, "mov.b",   "@r%m,r%n",        K_NONE, 0, 0},
    {0xF00F, 0x6001, "mov.w",   "@r%m,r%n",        K_NONE, 0, 0},
    {0xF00F, 0x6002, "mov.l",   "@r%m,r%n",        K_NONE, 0, 0},
    {0xF00F, 0x6003, "mov",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x6004, "mov.b",   "@r%m+,r%n",       K_NONE, 0, 0},
    {0xF00F, 0x6005, "mov.w",   "@r%m+,r%n",       K_NONE, 0, 0},
    {0xF00F, 0x6006, "mov.l",   "@r%m+,r%n",       K_NONE, 0, 0},
    {0xF00F, 0x6007, "not",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x6008, "swap.b",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x6009, "swap.w",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600A, "negc",    "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600B, "neg",     "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600C, "extu.b",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600D, "extu.w",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600E, "exts.b",  "r%m,r%n",         K_NONE, 0, 0},
    {0xF00F, 0x600F, "exts.w",  "r%m,r%n",         K_NONE, 0, 0},

    /* ---- 4-bit opcode, rest operands ------------------------------------------- */
    {0xF000, 0x1000, "mov.l",   "r%m,@(%d,r%n)",   K_D4,   4, 0},
    {0xF000, 0x5000, "mov.l",   "@(%d,r%m),r%n",   K_D4,   4, 0},
    {0xF000, 0x7000, "add",     "#%s,r%n",         K_IS,   0, 0},
    {0xF000, 0x9000, "mov.w",   "@(%t),r%n",       K_PCW,  2, SH2F_PCLOAD},
    {0xF000, 0xA000, "bra",     "%t",              K_BR12, 0, BR | DS | END},
    {0xF000, 0xB000, "bsr",     "%t",              K_BR12, 0, BR | DS | SH2F_CALL},
    {0xF000, 0xD000, "mov.l",   "@(%t),r%n",       K_PCL,  4, SH2F_PCLOAD},
    {0xF000, 0xE000, "mov",     "#%s,r%n",         K_IS,   0, 0},

    /* ---- 8-bit opcode ---------------------------------------------------------- */
    {0xFF00, 0x8000, "mov.b",   "r0,@(%d,r%m)",    K_D4,   1, 0},
    {0xFF00, 0x8100, "mov.w",   "r0,@(%d,r%m)",    K_D4,   2, 0},
    {0xFF00, 0x8400, "mov.b",   "@(%d,r%m),r0",    K_D4,   1, 0},
    {0xFF00, 0x8500, "mov.w",   "@(%d,r%m),r0",    K_D4,   2, 0},
    {0xFF00, 0x8800, "cmp/eq",  "#%s,r0",          K_IS,   0, 0},
    {0xFF00, 0x8900, "bt",      "%t",              K_BR8,  0, BR | SH2F_COND},
    {0xFF00, 0x8B00, "bf",      "%t",              K_BR8,  0, BR | SH2F_COND},
    {0xFF00, 0x8D00, "bt/s",    "%t",              K_BR8,  0, BR | DS | SH2F_COND},
    {0xFF00, 0x8F00, "bf/s",    "%t",              K_BR8,  0, BR | DS | SH2F_COND},
    {0xFF00, 0xC000, "mov.b",   "r0,@(%d,gbr)",    K_D8,   1, 0},
    {0xFF00, 0xC100, "mov.w",   "r0,@(%d,gbr)",    K_D8,   2, 0},
    {0xFF00, 0xC200, "mov.l",   "r0,@(%d,gbr)",    K_D8,   4, 0},
    {0xFF00, 0xC300, "trapa",   "#%u",             K_IU,   0, BR | SH2F_TRAP},
    {0xFF00, 0xC400, "mov.b",   "@(%d,gbr),r0",    K_D8,   1, 0},
    {0xFF00, 0xC500, "mov.w",   "@(%d,gbr),r0",    K_D8,   2, 0},
    {0xFF00, 0xC600, "mov.l",   "@(%d,gbr),r0",    K_D8,   4, 0},
    {0xFF00, 0xC700, "mova",    "@(%t),r0",        K_PCL,  4, SH2F_PCADDR},
    {0xFF00, 0xC800, "tst",     "#%u,r0",          K_IU,   0, 0},
    {0xFF00, 0xC900, "and",     "#%u,r0",          K_IU,   0, 0},
    {0xFF00, 0xCA00, "xor",     "#%u,r0",          K_IU,   0, 0},
    {0xFF00, 0xCB00, "or",      "#%u,r0",          K_IU,   0, 0},
    {0xFF00, 0xCC00, "tst.b",   "#%u,@(r0,gbr)",   K_IU,   0, 0},
    {0xFF00, 0xCD00, "and.b",   "#%u,@(r0,gbr)",   K_IU,   0, 0},
    {0xFF00, 0xCE00, "xor.b",   "#%u,@(r0,gbr)",   K_IU,   0, 0},
    {0xFF00, 0xCF00, "or.b",    "#%u,@(r0,gbr)",   K_IU,   0, 0},
};

#define NUM_OPS ((int)(sizeof OPS / sizeof OPS[0]))

static uint8_t g_table[65536]; /* opcode -> index+1 (0 = invalid) */
static bool g_ready;

static void build_table(void)
{
    for (uint32_t raw = 0; raw < 65536; raw++) {
        g_table[raw] = 0;
        for (int i = 0; i < NUM_OPS; i++) {
            if ((raw & OPS[i].mask) == OPS[i].match) {
                g_table[raw] = (uint8_t)(i + 1);
                break;
            }
        }
    }
    g_ready = true;
}

int sh2_op_count(void) { return NUM_OPS; }
const char *sh2_op_name(int op) { return OPS[op].name; }
uint16_t sh2_op_mask(int op) { return OPS[op].mask; }
uint16_t sh2_op_match(int op) { return OPS[op].match; }
const char *sh2_op_operands(int op) { return OPS[op].fmt; }

bool sh2_decode(uint16_t raw, uint32_t pc, sh2_insn *in)
{
    const op_def *d;
    int idx;
    if (!g_ready)
        build_table();
    memset(in, 0, sizeof *in);
    in->raw = raw;
    in->n = (raw >> 8) & 15;
    in->m = (raw >> 4) & 15;
    idx = g_table[raw];
    if (idx == 0) {
        in->op = SH2_OP_INVALID;
        in->mnemonic = ".word";
        return false;
    }
    d = &OPS[idx - 1];
    in->op = (uint16_t)(idx - 1);
    in->mnemonic = d->name;
    in->flags = d->flags;
    in->size = d->scale;
    switch ((field_kind)d->kind) {
    case K_NONE: break;
    case K_D4: in->disp = (raw & 15) * d->scale; break;
    case K_D8: in->disp = (raw & 255) * d->scale; break;
    case K_IS: in->imm = (int8_t)(raw & 255); break;
    case K_IU: in->imm = raw & 255; break;
    case K_PCW: in->target = pc + 4 + (raw & 255) * 2u; break;
    case K_PCL: in->target = (pc & ~3u) + 4 + (raw & 255) * 4u; break;
    case K_BR8: in->target = pc + 4 + (uint32_t)((int32_t)(int8_t)(raw & 255) * 2); break;
    case K_BR12: {
        int32_t d12 = raw & 0xFFF;
        if (d12 & 0x800)
            d12 -= 0x1000;
        in->target = pc + 4 + (uint32_t)(d12 * 2);
        break;
    }
    }
    return true;
}

void sh2_disasm(const sh2_insn *in, char *buf, size_t len)
{
    const char *f;
    size_t o;
    if (in->op == SH2_OP_INVALID) {
        snprintf(buf, len, ".word 0x%04X", in->raw);
        return;
    }
    f = OPS[in->op].fmt;
    o = (size_t)snprintf(buf, len, "%s", in->mnemonic);
    if (*f && o < len)
        o += (size_t)snprintf(buf + o, len - o, " ");
    for (; *f && o < len; f++) {
        int w;
        if (*f != '%') {
            buf[o++] = *f;
            continue;
        }
        switch (*++f) {
        case 'n': w = snprintf(buf + o, len - o, "%u", in->n); break;
        case 'm': w = snprintf(buf + o, len - o, "%u", in->m); break;
        case 's': w = snprintf(buf + o, len - o, "%d", (int)in->imm); break;
        case 'u': w = snprintf(buf + o, len - o, "0x%02X", (unsigned)in->imm); break;
        case 'd': w = snprintf(buf + o, len - o, "%d", (int)in->disp); break;
        case 't': w = snprintf(buf + o, len - o, "0x%08X", (unsigned)in->target); break;
        default: w = 0; break;
        }
        if (w > 0)
            o += (size_t)w;
    }
    if (o >= len)
        o = len - 1;
    buf[o] = 0;
}
