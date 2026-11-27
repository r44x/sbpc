#include "disc/iso9660.h"
#include "fake_iso.h"

int main(void)
{
    static const unsigned char prog[8] = {0x00, 0x0B, 0x00, 0x09}; /* rts; nop */
    disc d;
    iso_fs fs;
    char err[256];
    const iso_file *f;
    unsigned char buf[3000];

    CHECK(make_fake_iso("t_iso", prog, sizeof prog, 0x06010000));
    CHECK(disc_open(&d, "t_iso.cue", err, sizeof err));
    CHECK(iso_open(&fs, &d, 0, err, sizeof err));
    CHECK_STR(fs.volume_id, "FAKEDISC");
    CHECK(fs.count == 3);

    f = iso_first_root_file(&fs);
    CHECK(f != NULL);
    if (f) {
        CHECK_STR(f->path, "/0FIRST.BIN");
        CHECK(f->lba == 20 && f->size == sizeof prog);
    }
    CHECK(iso_find(&fs, "SUB") && iso_find(&fs, "/sub")->is_dir);
    f = iso_find(&fs, "sub/data.bin");
    CHECK(f != NULL);
    if (f) {
        CHECK(f->size == 3000);
        CHECK(iso_read(&fs, f, 0, buf, 3000));
        CHECK(buf[0] == 0 && buf[2999] == ((2999 * 7) & 0xFF));
        CHECK(iso_read(&fs, f, 2040, buf, 20)); /* crosses a sector boundary */
        CHECK(buf[0] == ((2040 * 7) & 0xFF) && buf[19] == ((2059 * 7) & 0xFF));
        CHECK(!iso_read(&fs, f, 2990, buf, 20)); /* past end */
    }
    CHECK(iso_find(&fs, "nope.bin") == NULL);

    iso_close(&fs);
    disc_close(&d);
    remove("t_iso.iso"); remove("t_iso.cue");
    TEST_DONE();
}
