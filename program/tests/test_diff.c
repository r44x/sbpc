/* Differential test: diffgen's random programs, generated code compiled into this test by the host
 * compiler, against the interpreter (see diff_harness.h). */
#include "test.h"
#include "diff_harness.h"

const sbpc_module *sbpc_module_diff(const sbpc_rt *rt);

int main(void)
{
    rt_dispatch d;
    rt_dispatch_init(&d);
    CHECK(rt_dispatch_add(&d, sbpc_module_diff(rt_api())));
    diff_run_all(&d);
    rt_dispatch_free(&d);
    TEST_DONE();
}
