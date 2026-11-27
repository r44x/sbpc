#include "sh2/sh2_decode.h"
#include "test.h"
#include "test_sh2_vectors.h"

int main(void)
{
    sh2_insn in;
    char text[64];
    int nvec = (int)(sizeof VECTORS / sizeof VECTORS[0]);
    int hits[256] = {0};

    CHECK(sh2_op_count() == 142);
    _Static_assert(SH2_OP_COUNT == 142, "SH-2 instruction table size");
    /* The id enum must follow the table order. */
    CHECK_STR(sh2_op_name(SH2_CLRT), "clrt");
    CHECK_STR(sh2_op_name(SH2_STSL_PR), "sts.l");
    CHECK(sh2_op_match(SH2_STSL_PR) == 0x4022);
    CHECK(sh2_op_match(SH2_ADDC) == 0x300E);
    CHECK(sh2_op_match(SH2_EXTSW) == 0x600F);
    CHECK(sh2_op_match(SH2_MOVI) == 0xE000);
    CHECK(sh2_op_match(SH2_BFS) == 0x8F00);
    CHECK(sh2_op_match(SH2_MOVA) == 0xC700);
    CHECK(sh2_op_match(SH2_ORB) == 0xCF00);
    CHECK(nvec == 142);

    /* No opcode may match two table entries. */
    for (uint32_t raw = 0; raw < 65536; raw++) {
        int matches = 0;
        for (int i = 0; i < sh2_op_count(); i++)
            matches += (raw & sh2_op_mask(i)) == sh2_op_match(i);
        if (matches > 1) {
            fprintf(stderr, "opcode %04X matches %d entries\n", (unsigned)raw, matches);
            g_failures++;
            break;
        }
    }

    /* Every instruction decodes and disassembles as expected; each table entry hit once. */
    for (int i = 0; i < nvec; i++) {
        CHECK(sh2_decode(VECTORS[i].raw, VEC_PC, &in));
        sh2_disasm(&in, text, sizeof text);
        if (strcmp(text, VECTORS[i].text) != 0) {
            fprintf(stderr, "%04X: got \"%s\", want \"%s\"\n", VECTORS[i].raw, text, VECTORS[i].text);
            g_failures++;
        }
        if (in.op < 256)
            hits[in.op]++;
    }
    for (int i = 0; i < sh2_op_count(); i++)
        if (hits[i] != 1) {
            fprintf(stderr, "table entry %d (%s) hit %d times\n", i, sh2_op_name(i), hits[i]);
            g_failures++;
        }

    /* Invalid opcodes */
    CHECK(!sh2_decode(0x0000, 0, &in));
    CHECK(!sh2_decode(0xFFFF, 0, &in));
    sh2_disasm(&in, text, sizeof text);
    CHECK_STR(text, ".word 0xFFFF");

    /* Flags */
    sh2_decode(0x000B, 0, &in); /* rts */
    CHECK((in.flags & (SH2F_DELAY | SH2F_END | SH2F_RETURN)) == (SH2F_DELAY | SH2F_END | SH2F_RETURN));
    sh2_decode(0x430B, 0, &in); /* jsr @r3 */
    CHECK((in.flags & SH2F_CALL) && (in.flags & SH2F_INDIRECT) && !(in.flags & SH2F_END));
    sh2_decode(0x8900, 0, &in); /* bt */
    CHECK((in.flags & SH2F_COND) && !(in.flags & SH2F_DELAY));
    sh2_decode(0xD301, 0x06000002, &in); /* mov.l @(1*4 + aligned pc + 4) */
    CHECK((in.flags & SH2F_PCLOAD) && in.size == 4 && in.target == 0x06000008);
    sh2_decode(0x9301, 0x06000002, &in); /* mov.w: not aligned */
    CHECK((in.flags & SH2F_PCLOAD) && in.size == 2 && in.target == 0x06000008);
    sh2_decode(0xA800, 0x06001000, &in); /* bra, most negative displacement */
    CHECK(in.target == 0x06001004u - 0x1000u);
    sh2_decode(0x0009, 0, &in); /* nop is fine in a delay slot */
    CHECK(!(in.flags & SH2F_NO_SLOT));

    TEST_DONE();
}
