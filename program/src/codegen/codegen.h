/* Turns analysed SH-2 functions into C that calls the shared semantics in sh2_ops.h.
 *
 * Each guest function becomes  void f_XXXXXXXX(sh2_state *s)  that starts with a switch on
 * s->pc, so it can be entered at its entry point and also resumed at return sites, branch
 * targets and switch-table cases. That lets execution leave and re-enter generated code at
 * any time: at the end of a time slice, at an unknown jump, or after an exception.
 *   - known calls       -> direct C calls (with a guard when the target was only guessed)
 *   - unknown calls     -> rt->call(s, return_address)
 *   - rts               -> set s->pc = PR and return
 *   - jumps out of the function / unknown targets -> set s->pc and return to the dispatcher
 *   - backward branches check s->cycles and yield when the slice is used up
 * Generated code is derived from the user's disc: it is written to the local data folder and
 * compiled there, never distributed. */
#ifndef SBPC_CODEGEN_H
#define SBPC_CODEGEN_H

/* Bump whenever the analysis or the generated code changes: builds made by an older generator are
 * redone by the launcher's Play button. */
#define SBPC_CODEGEN_VERSION 5

#include "analysis/analyse.h"

typedef struct {
    int functions;
    int instructions;
    int files;
    int direct_calls;      /* calls to known functions */
    int guarded_calls;     /* indirect calls with a guessed target */
    int dynamic_calls;     /* calls resolved at runtime */
    int dynamic_jumps;
    int fallbacks;         /* places that hand over to the interpreter */
    int resume_points;
} cg_stats;

typedef struct {
    const char *module;       /* "kernel" -> files gen_kernel_*.c */
    const char *source_hash;  /* disc data hash, stored in the module */
    int funcs_per_file;       /* default 400 */
} cg_options;

/* Writes <out_dir>/gen_<module>.h, gen_<module>_NNN.c and gen_<module>_table.c.
 * The list of .c files is written to <out_dir>/gen_<module>.files (one per line). */
bool cg_generate(analysis *a, const cg_options *opt, const char *out_dir, cg_stats *st);

#endif
