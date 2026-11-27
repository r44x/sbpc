#include "runtime/tcc_build.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/proc.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define TCC_EXE "sbpc-tcc.exe"
#else
#define TCC_EXE "sbpc-tcc"
#endif

/* Helpers TinyCC may call from generated code (no C library is linked), and on Windows the DLL
 * entry point TinyCC's PE x64 output expects: _dllstart, without __stdcall (-nostdinc drops the
 * header that defines it, and x64 has one calling convention anyway). */
static const char PRELUDE[] =
    "/* Helpers TinyCC may call from generated code: no C library is linked. Written by SBPC. */\n"
    "typedef __SIZE_TYPE__ size_t;\n"
    "void *memcpy(void *d, const void *s, size_t n)\n"
    "{ unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }\n"
    "void *memmove(void *d, const void *s, size_t n)\n"
    "{ unsigned char *a = d; const unsigned char *b = s;\n"
    "  if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } return d; }\n"
    "void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }\n"
    "#ifdef _WIN32\n"
    "int _dllstart(void *h, unsigned long reason, void *reserved)\n"
    "{ (void)h; (void)reason; (void)reserved; return 1; }\n"
    "#endif\n";

bool tcc_build_module(const char *const *files, int nfiles, const char *out_path, const char *work_dir,
                      const char *log_path, char *msg, size_t cap)
{
    char exe_dir[SBPC_PATH_MAX], tcc[SBPC_PATH_MAX], tccdir[SBPC_PATH_MAX], inc1[SBPC_PATH_MAX + 16],
        inc2[SBPC_PATH_MAX + 16], bopt[SBPC_PATH_MAX + 16], prelude[SBPC_PATH_MAX];
    const char *argv[96];
    int argc = 0, rc;
    FILE *f;

    if (nfiles < 0 || nfiles > 80) {
        snprintf(msg, cap, "too many source files");
        return false;
    }
    if (!fs_exe_dir(exe_dir) || !fs_join(tcc, exe_dir, TCC_EXE) || !fs_join(tccdir, exe_dir, "tcc")) {
        snprintf(msg, cap, "can't locate SBPC's folder");
        return false;
    }
    if (!fs_is_file(tcc)) {
        snprintf(msg, cap, "the bundled compiler (%s) is missing next to sbpc", TCC_EXE);
        return false;
    }
    snprintf(bopt, sizeof bopt, "-B%s", tccdir);
    snprintf(inc1, sizeof inc1, "-I%s/include", tccdir);
    snprintf(inc2, sizeof inc2, "-I%s/sbpc", tccdir);
    if (!fs_join(prelude, work_dir, "sbpc_prelude.c") || !(f = fs_open(prelude, "wb"))) {
        snprintf(msg, cap, "can't write the prelude in %.400s", work_dir);
        return false;
    }
    fputs(PRELUDE, f);
    fclose(f);

    argv[argc++] = tcc;
    argv[argc++] = bopt;
    argv[argc++] = "-nostdinc";
    argv[argc++] = "-nostdlib";
    argv[argc++] = "-shared";
    argv[argc++] = inc1;
    argv[argc++] = inc2;
    argv[argc++] = "-o";
    argv[argc++] = out_path;
    for (int i = 0; i < nfiles; i++) argv[argc++] = files[i];
    argv[argc++] = prelude;
    argv[argc] = NULL;
    remove(out_path);
    rc = proc_run(argv, log_path);
    if (rc != 0) {
        snprintf(msg, cap, "compiler exit code %d (see %.400s)", rc, log_path);
        return false;
    }
    return true;
}
