/* Minimal test helpers. No game data is ever used in these tests. */
#ifndef SBPC_TEST_H
#define SBPC_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); if (strcmp(_a, _b) != 0) { \
    fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); g_failures++; } } while (0)

#define TEST_DONE() do { if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; } \
    printf("ok\n"); return 0; } while (0)

/* Writes `n` bytes to path; returns 1 on success. */
static inline int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); return 0; }
    return fclose(f) == 0;
}

#endif
