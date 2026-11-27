/* Loads recompiled modules (.dll / .so built by the bundled TinyCC) into a dispatcher. */
#ifndef SBPC_RUNTIME_LOADER_H
#define SBPC_RUNTIME_LOADER_H

#include "runtime/dispatch.h"

#include <stddef.h>

#ifdef _WIN32
#define SBPC_MODULE_EXT ".dll"
#else
#define SBPC_MODULE_EXT ".so"
#endif

/* Loads <dir>/<name>SBPC_MODULE_EXT, checks its ABI and that it was generated from the disc
 * whose data hash is source_hash, and adds its entry points to d. The library stays loaded
 * for the rest of the process. err receives the reason on failure. */
bool rt_load_module(rt_dispatch *d, const char *dir, const char *name, const char *source_hash,
                    char *err, size_t errlen);

#endif
