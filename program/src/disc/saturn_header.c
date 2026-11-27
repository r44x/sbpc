#include "disc/saturn_header.h"

#include <string.h>

static void field(char *out, const uint8_t *src, int len)
{
    int n = len;
    for (int i = 0; i < len; i++)
        out[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool saturn_header_parse(const uint8_t s[2048], saturn_header *h)
{
    memset(h, 0, sizeof *h);
    if (memcmp(s, "SEGA SEGASATURN ", 16) != 0)
        return false;
    field(h->maker, s + 0x10, 16);
    field(h->product, s + 0x20, 10);
    field(h->version, s + 0x2A, 6);
    field(h->date, s + 0x30, 8);
    field(h->device, s + 0x38, 8);
    field(h->areas, s + 0x40, 10);
    field(h->peripherals, s + 0x50, 16);
    field(h->title, s + 0x60, 112);
    h->ip_size = be32(s + 0xE0);
    h->master_stack = be32(s + 0xE8);
    h->slave_stack = be32(s + 0xEC);
    h->first_read_addr = be32(s + 0xF0);
    h->first_read_size = be32(s + 0xF4);
    return true;
}
