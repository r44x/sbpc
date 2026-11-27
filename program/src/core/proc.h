/* Runs another program and waits for it (used to call the bundled TinyCC). */
#ifndef SBPC_CORE_PROC_H
#define SBPC_CORE_PROC_H

#include <stdbool.h>

/* argv[0] is the program path; argv ends with NULL. The program's stdout and stderr go to
 * log_path (created/overwritten; may be NULL to discard). Returns the exit code, or -1 if
 * the program couldn't be started. */
int proc_run(const char *const *argv, const char *log_path);

#endif
