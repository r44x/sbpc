/* Backup RAM: the on-device format, the BUP_* library as the game calls it, and persistence.
 * No game data. */
#include "saturn/saturn.h"
#include "saturn/saturn_bup.h"
#include "sh2/sh2_interp.h"
#include "core/fs.h"
#include "test.h"

#define LIB 0x060F0000u      /* where the "game" asks BUP_Init to build the function table */
#define BUF 0x06080000u      /* guest scratch: names, dir entries, data */

static uint8_t ram[BUP_SIZE];

static bup_dir make_dir(const char *name, uint32_t size)
{
    bup_dir d;
    memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "%s", name);
    snprintf(d.comment, sizeof d.comment, "TESTDATA");
    d.size = size;
    d.date = bup_date_encode(1996, 7, 19, 12, 30);
    return d;
}

/* Calls backup library function fn through the table, the way game code does (jsr @rN). */
static int32_t lib_call(saturn *m, int fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    sh2_state *s = &m->cpu[0];
    s->r[4] = a; s->r[5] = b; s->r[6] = c; s->r[7] = d;
    s->pr = 0x06004000u;
    s->pc = sh2_rd32(s, LIB + (uint32_t)fn * 4);
    CHECK(bios_hle(m, s));
    CHECK(s->pc == 0x06004000u);
    return (int32_t)s->r[0];
}

static void put_str(sh2_state *s, uint32_t p, const char *str)
{
    do sh2_wr8(s, p++, (uint8_t)*str); while (*str++);
}

static void test_format(void)
{
    static uint8_t data[2000], back[2000];
    bup_dir d = make_dir("SAVE_A", 100), list[8];
    uint32_t size = 0;

    bup_format(ram);
    CHECK(bup_is_formatted(ram));
    CHECK(memcmp(ram, "BackUpRam Format", 16) == 0 && memcmp(ram + 48, "BackUpRam Format", 16) == 0);
    CHECK(bup_free_blocks(ram) == BUP_BLOCKS - 2);
    CHECK(bup_list(ram, "", list, 8) == 0);

    /* Block counts: ceil((size + 30) / 58) */
    CHECK(bup_blocks_for(0) == 1 && bup_blocks_for(28) == 1 && bup_blocks_for(29) == 2 && bup_blocks_for(100) == 3);

    for (int i = 0; i < 100; i++) data[i] = (uint8_t)(i * 7);
    CHECK(bup_write(ram, &d, data, false) == BUP_OK);
    CHECK(bup_free_blocks(ram) == BUP_BLOCKS - 2 - 3);
    /* first block: tag, name, language, comment, date, size, block list (2 more blocks), 0 */
    CHECK(ram[2 * 64] == 0x80 && memcmp(ram + 2 * 64 + 4, "SAVE_A", 6) == 0);
    CHECK(memcmp(ram + 2 * 64 + 0x10, "TESTDATA", 8) == 0);
    CHECK(ram[2 * 64 + 0x21] == 100);                                   /* size, low byte */
    CHECK(ram[2 * 64 + 0x22] == 0 && ram[2 * 64 + 0x23] == 3);         /* next block: 3 */
    CHECK(ram[2 * 64 + 0x24] == 0 && ram[2 * 64 + 0x25] == 4);         /* then 4 */
    CHECK(ram[2 * 64 + 0x26] == 0 && ram[2 * 64 + 0x27] == 0);         /* end of list */
    CHECK(ram[2 * 64 + 0x28] == data[0]);
    CHECK(ram[3 * 64] == 0 && ram[3 * 64 + 4] == data[24]);            /* continuation: tag 0, then payload */

    CHECK(bup_read(ram, "SAVE_A", back, sizeof back, &size) == BUP_OK && size == 100);
    CHECK(memcmp(back, data, 100) == 0);
    CHECK(bup_read(ram, "SAVE_B", back, sizeof back, &size) == BUP_NOT_FOUND);

    /* overwrite rules */
    CHECK(bup_write(ram, &d, data, true) == BUP_FOUND);
    data[0] = 0xAA;
    CHECK(bup_write(ram, &d, data, false) == BUP_OK);
    CHECK(bup_read(ram, "SAVE_A", back, sizeof back, &size) == BUP_OK && back[0] == 0xAA);
    CHECK(bup_free_blocks(ram) == BUP_BLOCKS - 2 - 3);

    /* a bigger file whose block list runs past its first block */
    {
        bup_dir big = make_dir("SAVE_BIG", 2000);
        for (int i = 0; i < 2000; i++) data[i] = (uint8_t)(i ^ (i >> 8));
        CHECK(bup_write(ram, &big, data, false) == BUP_OK);
        CHECK(bup_read(ram, "SAVE_BIG", back, sizeof back, &size) == BUP_OK && size == 2000);
        CHECK(memcmp(back, data, 2000) == 0);
        CHECK(bup_list(ram, "SAVE", list, 8) == 2);
        CHECK(bup_list(ram, "SAVE_B", list, 8) == 1 && list[0].size == 2000 && list[0].blocks == bup_blocks_for(2000));
        CHECK_STR(list[0].comment, "TESTDATA");
        CHECK(bup_delete(ram, "SAVE_BIG") == BUP_OK);
        CHECK(bup_delete(ram, "SAVE_BIG") == BUP_NOT_FOUND);
        CHECK(bup_free_blocks(ram) == BUP_BLOCKS - 2 - 3);
    }

    /* full memory */
    {
        bup_dir huge = make_dir("HUGE", BUP_SIZE);
        CHECK(bup_write(ram, &huge, data, false) == BUP_NOT_ENOUGH_MEMORY);
    }
}

static void test_dates(void)
{
    int y, mo, d, h, mi, wd;
    CHECK(bup_date_encode(1980, 1, 1, 0, 0) == 0);
    CHECK(bup_date_encode(1980, 1, 2, 1, 1) == 1440 + 61);
    bup_date_decode(bup_date_encode(2000, 2, 29, 23, 59), &y, &mo, &d, &h, &mi, &wd);
    CHECK(y == 2000 && mo == 2 && d == 29 && h == 23 && mi == 59);
    CHECK(wd == 2);   /* a Tuesday */
}

/* The library through the BIOS, and the save file surviving a new machine. */
static void test_machine(void)
{
    const char *path = "t_bup.bkr";
    saturn *m = saturn_create();
    sh2_state *s;
    char err[256];
    CHECK(m != NULL);
    s = &m->cpu[0];
    remove(path);
    CHECK(saturn_backup_attach(m, path, err, sizeof err));
    /* A new, blank image isn't a save: nothing is written until the game writes something */
    CHECK(!m->backup_dirty);
    saturn_backup_flush(m);
    CHECK(fs_size(path) < 0);
    bios_install(m);
    s->r[4] = LIB; s->r[5] = 0x06090000u; s->r[6] = BUF + 0x400;
    s->pr = 0x06004000u;
    s->pc = sh2_rd32(s, 0x06000358u);
    CHECK(bios_hle(m, s));
    CHECK(sh2_rd16(s, BUF + 0x400) == 1 && sh2_rd16(s, BUF + 0x404) == 0);   /* internal yes, cartridge no */

    /* Stat for a 100-byte file: 510 free blocks, 3 per file */
    CHECK(lib_call(m, 3, 0, 100, BUF, 0) == BUP_OK);
    CHECK(sh2_rd32(s, BUF + 16) == BUP_BLOCKS - 2 && sh2_rd32(s, BUF + 20) == (BUP_BLOCKS - 2) / 3);
    CHECK(lib_call(m, 3, 1, 100, BUF, 0) == BUP_NON);                         /* no cartridge */

    /* Write: dir entry at BUF, data at BUF+0x100 */
    for (uint32_t i = 0; i < 0x24; i++) sh2_wr8(s, BUF + i, 0);
    put_str(s, BUF, "BOMBER_01");
    put_str(s, BUF + 12, "OPTIONS");
    sh2_wr32(s, BUF + 28, 64);
    for (uint32_t i = 0; i < 64; i++) sh2_wr8(s, BUF + 0x100 + i, (uint8_t)(0xC0 + i));
    CHECK(lib_call(m, 4, 0, BUF, BUF + 0x100, 0) == BUP_OK);
    CHECK(lib_call(m, 4, 0, BUF, BUF + 0x100, 1) == BUP_FOUND);
    CHECK(m->backup_dirty);

    /* Dir: count-only query (table size 0), then a real listing */
    put_str(s, BUF + 0x200, "BOMBER");
    CHECK(lib_call(m, 7, 0, BUF + 0x200, 0, 0) == 1);
    CHECK(lib_call(m, 7, 0, BUF + 0x200, 4, BUF + 0x300) == 1);
    CHECK(sh2_rd8(s, BUF + 0x300) == 'B' && sh2_rd32(s, BUF + 0x300 + 28) == 64 && sh2_rd16(s, BUF + 0x300 + 32) == 2);

    /* Read back into a cleared buffer; Verify */
    for (uint32_t i = 0; i < 64; i++) sh2_wr8(s, BUF + 0x500 + i, 0);
    CHECK(lib_call(m, 5, 0, BUF, BUF + 0x500, 0) == BUP_OK);
    CHECK(sh2_rd8(s, BUF + 0x500) == 0xC0 && sh2_rd8(s, BUF + 0x53F) == 0xFF);
    CHECK(lib_call(m, 8, 0, BUF, BUF + 0x500, 0) == BUP_OK);
    sh2_wr8(s, BUF + 0x510, 0);
    CHECK(lib_call(m, 8, 0, BUF, BUF + 0x500, 0) == BUP_NO_MATCH);

    /* SetDate / GetDate */
    sh2_wr8(s, BUF + 0x600, 16); sh2_wr8(s, BUF + 0x601, 7); sh2_wr8(s, BUF + 0x602, 19);
    sh2_wr8(s, BUF + 0x603, 12); sh2_wr8(s, BUF + 0x604, 30);
    {
        uint32_t date = (uint32_t)lib_call(m, 10, BUF + 0x600, 0, 0, 0);
        CHECK(date == bup_date_encode(1996, 7, 19, 12, 30));
        lib_call(m, 9, date, BUF + 0x610, 0, 0);
        CHECK(sh2_rd8(s, BUF + 0x610) == 16 && sh2_rd8(s, BUF + 0x614) == 30);
    }

    /* The game also sees the bytes on the bus at 0x00180001 (odd addresses) */
    CHECK(sh2_rd8(s, 0x00180001u) == 'B' && sh2_rd8(s, 0x00180000u) == 0xFF);

    saturn_destroy(m);   /* flushes */
    CHECK(fs_size(path) == BUP_SIZE);

    m = saturn_create();
    CHECK(m != NULL);
    CHECK(saturn_backup_attach(m, path, err, sizeof err));
    {
        uint32_t size = 0;
        static uint8_t back[64];
        CHECK(bup_read(m->backup, "BOMBER_01", back, sizeof back, &size) == BUP_OK && size == 64 && back[63] == 0xFF);
    }
    CHECK(!m->backup_dirty);
    saturn_destroy(m);
    remove(path);
}

int main(void)
{
    test_format();
    test_dates();
    test_machine();
    TEST_DONE();
}
