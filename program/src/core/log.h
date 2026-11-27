/* Logging: human-readable sbpc.log + machine-readable sbpc.jsonl (one JSON object per line).
 * The previous run's files are kept as *.1. Not thread-safe yet (single-threaded runtime). */
#ifndef SBPC_CORE_LOG_H
#define SBPC_CORE_LOG_H

#include <stdbool.h>

typedef enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE } log_level;

/* Subsystems for --trace=... ; LOG_TRACE messages print only if their subsystem is enabled. */
typedef enum {
    SUB_CORE, SUB_DISC, SUB_LAUNCHER, SUB_CPU, SUB_VDP1, SUB_VDP2,
    SUB_SCSP, SUB_CD, SUB_INPUT, SUB_HOOKS, SUB_IO, SUB_BIOS, SUB_SCU, SUB_SMPC, SUB_COUNT
} log_subsys;

bool log_open(const char *dir);          /* rotates and opens files in dir */
void log_close(void);
void log_set_level(log_level lvl);
void log_set_echo(bool to_stderr);
bool log_enable_traces(const char *csv); /* "cpu,disc" or "all"; false on unknown name */
bool log_trace_on(log_subsys s);

#if defined(__GNUC__) || defined(__clang__)
#define SBPC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define SBPC_PRINTF(a, b)
#endif

void log_msg(log_level lvl, log_subsys s, const char *fmt, ...) SBPC_PRINTF(3, 4);

#define LOGE(s, ...) log_msg(LOG_ERROR, s, __VA_ARGS__)
#define LOGW(s, ...) log_msg(LOG_WARN, s, __VA_ARGS__)
#define LOGI(s, ...) log_msg(LOG_INFO, s, __VA_ARGS__)
#define LOGD(s, ...) log_msg(LOG_DEBUG, s, __VA_ARGS__)
#define LOGT(s, ...) do { if (log_trace_on(s)) log_msg(LOG_TRACE, s, __VA_ARGS__); } while (0)

#endif
