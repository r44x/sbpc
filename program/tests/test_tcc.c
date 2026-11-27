/* TinyCC end to end, without a disc: diffgen's generated C is compiled by the bundled sbpc-tcc (next
 * to this test, as next to sbpc) into a real module (.dll / .so, with SBPC's prelude and, on Windows,
 * the _dllstart entry), loaded with rt_load_module like a game module, and checked against the
 * interpreter on every program (see diff_harness.h). This is the path Build & Play uses. */
#include "test.h"
#include "diff_harness.h"
#include "core/fs.h"
#include "runtime/loader.h"
#include "runtime/tcc_build.h"

#ifndef SBPC_DIFF_DIR
#error "SBPC_DIFF_DIR must point at diffgen's output"
#endif

int main(void)
{
    const char *files[] = {SBPC_DIFF_DIR "/gen_diff_000.c", SBPC_DIFF_DIR "/gen_diff_table.c"};
    char msg[512], err[512];
    rt_dispatch d;
    CHECK(fs_mkdirs("t_tcc"));
    if (!tcc_build_module(files, 2, "t_tcc/diff" SBPC_MODULE_EXT, "t_tcc", "t_tcc/diff.compile.log", msg, sizeof msg)) {
        fprintf(stderr, "TinyCC build failed: %s\n", msg);
        {   /* show the compiler's own output */
            FILE *f = fopen("t_tcc/diff.compile.log", "r");
            char line[512];
            while (f && fgets(line, sizeof line, f)) fputs(line, stderr);
            if (f) fclose(f);
        }
        CHECK(false);
        TEST_DONE();
    }
    rt_dispatch_init(&d);
    if (!rt_load_module(&d, "t_tcc", "diff", "test", err, sizeof err)) {
        fprintf(stderr, "loading the TinyCC module failed: %s\n", err);
        CHECK(false);
        TEST_DONE();
    }
    CHECK(d.count > 0);
    diff_run_all(&d);
    TEST_DONE();
}
