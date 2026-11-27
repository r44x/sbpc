/* Compiling generated C with the bundled TinyCC (sbpc-tcc, a separate LGPL program next to the
 * running executable, never linked in) into a loadable module (.dll / .so). */
#ifndef SBPC_RUNTIME_TCC_BUILD_H
#define SBPC_RUNTIME_TCC_BUILD_H

#include <stdbool.h>
#include <stddef.h>

/* Compiles files[0..nfiles) plus SBPC's prelude (written into work_dir) into out_path. The compiler's
 * output goes to log_path. msg receives a one-line reason on failure. */
bool tcc_build_module(const char *const *files, int nfiles, const char *out_path, const char *work_dir,
                      const char *log_path, char *msg, size_t cap);

#endif
