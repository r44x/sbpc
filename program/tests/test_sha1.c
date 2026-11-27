#include "core/sha1.h"
#include "test.h"

static void hash(const void *d, size_t n, char out[41])
{
    sha1_ctx c;
    uint8_t dig[20];
    sha1_init(&c);
    sha1_update(&c, d, n);
    sha1_final(&c, dig);
    sha1_hex(dig, out);
}

int main(void)
{
    char h[41];
    static char million[1000000];
    sha1_ctx c;
    uint8_t dig[20];

    hash("", 0, h);
    CHECK_STR(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    hash("abc", 3, h);
    CHECK_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    memset(million, 'a', sizeof million);
    hash(million, sizeof million, h);
    CHECK_STR(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    /* Same data fed in odd-sized pieces must give the same digest. */
    sha1_init(&c);
    for (size_t i = 0; i < sizeof million;) {
        size_t n = (i % 7) * 13 + 1;
        if (i + n > sizeof million) n = sizeof million - i;
        sha1_update(&c, million + i, n);
        i += n;
    }
    sha1_final(&c, dig);
    sha1_hex(dig, h);
    CHECK_STR(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    TEST_DONE();
}
