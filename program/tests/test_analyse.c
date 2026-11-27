#include "analysis/analyse.h"
#include "analysis/boot.h"
#include "fake_iso.h"

/* Hand-written test program at 0x06010000 (big-endian halfwords). */
static const unsigned short PROG[] = {
    /* 00 */ 0x4F22,          /* sts.l pr,@-r15                          */
    /* 02 */ 0xB009,          /* bsr 0x06010018 (funcA)                   */
    /* 04 */ 0x0009,          /* nop                                      */
    /* 06 */ 0xD303,          /* mov.l @(0x06010014),r3  -> funcB         */
    /* 08 */ 0x430B,          /* jsr @r3                                  */
    /* 0A */ 0x0009,          /* nop                                      */
    /* 0C */ 0x4F26,          /* lds.l @r15+,pr                           */
    /* 0E */ 0x000B,          /* rts                                      */
    /* 10 */ 0x0009,          /* nop                                      */
    /* 12 */ 0x0009,          /* (never reached)                          */
    /* 14 */ 0x0601, 0x001C,  /* literal: funcB                           */
    /* 18 */ 0x000B,          /* funcA: rts                               */
    /* 1A */ 0x0009,
    /* 1C */ 0xD104,          /* funcB: mov.l @(0x06010030),r1 (pointer)  */
    /* 1E */ 0xD205,          /* mov.l @(0x06010034),r2 (outside image)   */
    /* 20 */ 0x420B,          /* jsr @r2  -> external call                */
    /* 22 */ 0x0009,
    /* 24 */ 0x440B,          /* jsr @r4  -> unknown target               */
    /* 26 */ 0x0009,
    /* 28 */ 0x000B,          /* rts                                      */
    /* 2A */ 0x0009,
    /* 2C */ 0x0009, 0x0009,  /* (never reached)                          */
    /* 30 */ 0x0601, 0x0038,  /* literal: pointer to funcC                */
    /* 34 */ 0x0600, 0x0800,  /* literal: outside the image               */
    /* 38 */ 0x2FE6,          /* funcC: mov.l r14,@-r15  (prologue)       */
    /* 3A */ 0x000B,          /* rts                                      */
    /* 3C */ 0x6EF6,          /* mov.l @r15+,r14                          */
    /* 3E */ 0x0009,
};

/* A switch through a 16-bit offset table, bounded by "mov #2,r2; cmp/hi r2,r1" (3 cases). */
static const unsigned short SWITCH_PROG[] = {
    /* 00 */ 0xE202,          /* mov #2,r2                     */
    /* 02 */ 0x3126,          /* cmp/hi r2,r1                  */
    /* 04 */ 0x8F02,          /* bf/s 0x0602000C               */
    /* 06 */ 0x311C,          /* add r1,r1                     */
    /* 08 */ 0x000B,          /* rts   (default case)          */
    /* 0A */ 0x0009,
    /* 0C */ 0xC702,          /* mova @(0x06020018),r0         */
    /* 0E */ 0x011D,          /* mov.w @(r0,r1),r1             */
    /* 10 */ 0x301C,          /* add r1,r0                     */
    /* 12 */ 0x402B,          /* jmp @r0                       */
    /* 14 */ 0x0009,          /* nop (slot)                    */
    /* 16 */ 0x0009,          /* padding                       */
    /* 18 */ 0x0008, 0x000C, 0x0010, /* table: +8, +12, +16    */
    /* 1E */ 0x0009,          /* padding                       */
    /* 20 */ 0x000B, 0x0009,  /* case 0: rts                   */
    /* 24 */ 0x000B, 0x0009,  /* case 1: rts                   */
    /* 28 */ 0x000B, 0x0009,  /* case 2: rts                   */
};

/* Kernel-style switch: absolute pointer table, bounded by "mov #8,r3; cmp/hs r0,r3" (3 cases). */
static const unsigned short ABS_PROG[] = {
    /* 00 */ 0xE308,          /* mov #8,r3                    */
    /* 02 */ 0x3302,          /* cmp/hs r0,r3                 */
    /* 04 */ 0x8B06,          /* bf 0x06030014 (default)      */
    /* 06 */ 0xD302,          /* mov.l @(0x06030010),r3       */
    /* 08 */ 0x033E,          /* mov.l @(r0,r3),r3            */
    /* 0A */ 0x432B,          /* jmp @r3                      */
    /* 0C */ 0x0009,          /* nop                          */
    /* 0E */ 0x0009,          /* padding                      */
    /* 10 */ 0x0603, 0x0018,  /* literal: table address       */
    /* 14 */ 0x000B, 0x0009,  /* default: rts                 */
    /* 18 */ 0x0603, 0x0024, 0x0603, 0x0028, 0x0603, 0x002C, /* table */
    /* 24 */ 0x000B, 0x0009,
    /* 28 */ 0x000B, 0x0009,
    /* 2C */ 0x000B, 0x0009,
};

/* Leaf methods without a prologue, reached only through a table (USA kernel stream readers).
 * methodA follows a literal pool, methodB follows methodA's rts; both count as function starts.
 * The third target follows table bytes, so only a per-version hint finds it. */
static const unsigned short METHOD_PROG[] = {
    /* 00 */ 0xD002,          /* mov.l @(0x0604000C),r0        */
    /* 02 */ 0x000B,          /* rts                           */
    /* 04 */ 0x0009,
    /* 06 */ 0x0009, 0x0009, 0x0009, /* padding                */
    /* 0C */ 0x1234, 0x5678,  /* literal pool                  */
    /* 10 */ 0x6042,          /* methodA: mov.l @r4,r0         */
    /* 12 */ 0x000B,          /* rts                           */
    /* 14 */ 0x7001,          /* add #1,r0 (slot)              */
    /* 16 */ 0x6043,          /* methodB: mov r4,r0            */
    /* 18 */ 0x000B,          /* rts                           */
    /* 1A */ 0x0009,
    /* 1C */ 0x0604, 0x0010, 0x0604, 0x0016, 0x0604, 0x0028, /* method table */
    /* 28 */ 0x6003,          /* hinted: mov r0,r0             */
    /* 2A */ 0x000B,          /* rts                           */
    /* 2C */ 0x0009, 0x0009,
};

static int count_sites(const analysis *a, site_kind k)
{
    int n = 0;
    for (int i = 0; i < a->nsites; i++)
        n += a->sites[i].kind == k;
    return n;
}

static int has_func(const analysis *a, uint32_t entry, fn_source src)
{
    for (int i = 0; i < a->nfuncs; i++)
        if (a->funcs[i].entry == entry)
            return a->funcs[i].source == src;
    return 0;
}

int main(void)
{
    unsigned char bytes[sizeof PROG];
    mem_image im;
    analysis a;
    disc d;
    iso_fs fs;
    boot_info bi;
    char err[256];

    for (size_t i = 0; i < sizeof PROG / 2; i++) {
        bytes[i * 2] = (unsigned char)(PROG[i] >> 8);
        bytes[i * 2 + 1] = (unsigned char)PROG[i];
    }

    /* Direct image */
    img_init(&im);
    CHECK(img_add(&im, "prog", 0x26010000, bytes, sizeof bytes)); /* cache-through mirror */
    CHECK(img_find(&im, 0x06010000, 2) != NULL);
    an_init(&a, &im);
    an_add_entry(&a, 0x06010000, FN_ENTRY);
    an_run(&a);

    CHECK(a.nfuncs == 4);
    CHECK(has_func(&a, 0x06010000, FN_ENTRY));
    CHECK(has_func(&a, 0x06010018, FN_BSR));
    CHECK(has_func(&a, 0x0601001C, FN_JSR_CONST));
    CHECK(has_func(&a, 0x06010038, FN_POINTER));
    CHECK(count_sites(&a, SITE_EXTERNAL_CALL) == 1);
    CHECK(count_sites(&a, SITE_INDIRECT_CALL) == 1);
    CHECK(count_sites(&a, SITE_INVALID_OPCODE) == 0);
    CHECK(img_kind(&im, 0x06010014) == BYTE_DATA);
    CHECK(img_kind(&im, 0x06010012) == BYTE_UNKNOWN);
    CHECK(img_kind(&im, 0x0601003C) == BYTE_CODE);
    CHECK(a.pointer_candidates == 0);
    an_free(&a);
    img_free(&im);

    /* Leaf methods found through a table by the function-boundary rule; hints for the rest. */
    for (int pass = 0; pass < 2; pass++) {
        unsigned char mb[sizeof METHOD_PROG];
        for (size_t i = 0; i < sizeof METHOD_PROG / 2; i++) {
            mb[i * 2] = (unsigned char)(METHOD_PROG[i] >> 8);
            mb[i * 2 + 1] = (unsigned char)METHOD_PROG[i];
        }
        img_init(&im);
        CHECK(img_add(&im, "methods", 0x06040000, mb, sizeof mb));
        an_init(&a, &im);
        an_add_entry(&a, 0x06040000, FN_ENTRY);
        if (pass == 1)
            an_add_entry(&a, 0x06040028, FN_HINT);
        an_run(&a);
        CHECK(has_func(&a, 0x06040010, FN_TABLE));
        CHECK(has_func(&a, 0x06040016, FN_TABLE));
        CHECK(pass == 0 ? !has_func(&a, 0x06040028, FN_TABLE) && a.nfuncs == 3 : has_func(&a, 0x06040028, FN_HINT) && a.nfuncs == 4);
        an_free(&a);
        img_free(&im);
    }

    /* Through a disc: boot_load puts the IP and the 1st read file in place. */
    CHECK(make_fake_iso("t_an", bytes, sizeof bytes, 0x06010000));
    CHECK(disc_open(&d, "t_an.cue", err, sizeof err));
    CHECK(iso_open(&fs, &d, 0, err, sizeof err));
    img_init(&im);
    CHECK(boot_load(&fs, &im, &bi, err, sizeof err));
    CHECK(bi.first_addr == 0x06010000 && bi.first_size == sizeof bytes);
    CHECK_STR(bi.first_file, "/0FIRST.BIN");
    CHECK(bi.aip_addr == 0x06002000 + 0xE00 + 0x40); /* two area codes: J, U */
    CHECK(img_find(&im, 0x06002000, 0x1000) != NULL);
    an_init(&a, &im);
    an_add_entry(&a, bi.first_addr, FN_ENTRY);
    an_run(&a);
    CHECK(a.nfuncs == 4);
    an_free(&a);
    img_free(&im);
    iso_close(&fs);
    disc_close(&d);
    remove("t_an.iso"); remove("t_an.cue");

    /* Switch table */
    {
        unsigned char sw[sizeof SWITCH_PROG];
        for (size_t i = 0; i < sizeof SWITCH_PROG / 2; i++) {
            sw[i * 2] = (unsigned char)(SWITCH_PROG[i] >> 8);
            sw[i * 2 + 1] = (unsigned char)SWITCH_PROG[i];
        }
        img_init(&im);
        CHECK(img_add(&im, "sw", 0x06020000, sw, sizeof sw));
        an_init(&a, &im);
        an_add_entry(&a, 0x06020000, FN_ENTRY);
        an_run(&a);
        CHECK(a.njtables == 1);
        if (a.njtables == 1) {
            CHECK(a.jtables[0].kind == JT_OFFSET16);
            CHECK(a.jtables[0].base == 0x06020018);
            CHECK(a.jtables[0].count == 3);
            CHECK(an_jtable_target(&a, &a.jtables[0], 2) == 0x06020028);
        }
        CHECK(count_sites(&a, SITE_INDIRECT_JUMP) == 0);
        CHECK(img_kind(&im, 0x06020018) == BYTE_DATA);
        CHECK(img_kind(&im, 0x06020020) == BYTE_CODE);
        CHECK(img_kind(&im, 0x06020028) == BYTE_CODE);
        CHECK(img_kind(&im, 0x0602001E) == BYTE_UNKNOWN);
        an_free(&a);
        img_free(&im);
    }

    /* Absolute pointer table */
    {
        unsigned char ab[sizeof ABS_PROG];
        for (size_t i = 0; i < sizeof ABS_PROG / 2; i++) {
            ab[i * 2] = (unsigned char)(ABS_PROG[i] >> 8);
            ab[i * 2 + 1] = (unsigned char)ABS_PROG[i];
        }
        img_init(&im);
        CHECK(img_add(&im, "abs", 0x06030000, ab, sizeof ab));
        an_init(&a, &im);
        an_add_entry(&a, 0x06030000, FN_ENTRY);
        an_run(&a);
        CHECK(a.njtables == 1);
        if (a.njtables == 1) {
            CHECK(a.jtables[0].kind == JT_ABS32);
            CHECK(a.jtables[0].base == 0x06030018);
            CHECK(a.jtables[0].count == 3);
            CHECK(an_jtable_target(&a, &a.jtables[0], 1) == 0x06030028);
        }
        CHECK(count_sites(&a, SITE_INDIRECT_JUMP) == 0);
        CHECK(img_kind(&im, 0x0603002C) == BYTE_CODE);
        an_free(&a);
        img_free(&im);
    }

    TEST_DONE();
}
