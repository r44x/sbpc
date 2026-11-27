#include "core/log.h"
#include "core/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *g_txt, *g_json;
static log_level g_level = LOG_INFO;
static bool g_echo = true;
static unsigned g_traces;

static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug", "trace"};
static const char *const SUB_NAMES[SUB_COUNT] = {
    "core", "disc", "launcher", "cpu", "vdp1", "vdp2", "scsp", "cd", "input", "hooks",
    "io", "bios", "scu", "smpc"};

static FILE *rotate_open(const char *dir, const char *name)
{
    char path[SBPC_PATH_MAX], old[SBPC_PATH_MAX];
    if (!fs_join(path, dir, name))
        return NULL;
    if (fs_is_file(path) && strlen(path) + 3 < sizeof old) {
        size_t n = strlen(path);
        memcpy(old, path, n);
        memcpy(old + n, ".1", 3);
        fs_rename(path, old);
    }
    return fs_open(path, "w");
}

bool log_open(const char *dir)
{
    log_close();
    if (!fs_mkdirs(dir))
        return false;
    g_txt = rotate_open(dir, "sbpc.log");
    g_json = rotate_open(dir, "sbpc.jsonl");
    return g_txt && g_json;
}

void log_close(void)
{
    if (g_txt)
        fclose(g_txt);
    if (g_json)
        fclose(g_json);
    g_txt = g_json = NULL;
}

void log_set_level(log_level lvl) { g_level = lvl; }
void log_set_echo(bool on) { g_echo = on; }
bool log_trace_on(log_subsys s) { return (g_traces >> s) & 1u; }

bool log_enable_traces(const char *csv)
{
    bool ok = true;
    while (*csv) {
        const char *end = strchr(csv, ',');
        size_t n = end ? (size_t)(end - csv) : strlen(csv);
        bool found = false;
        if (n == 3 && strncmp(csv, "all", 3) == 0) {
            g_traces = (1u << SUB_COUNT) - 1;
            found = true;
        }
        for (int i = 0; i < SUB_COUNT && !found; i++) {
            if (strlen(SUB_NAMES[i]) == n && strncmp(csv, SUB_NAMES[i], n) == 0) {
                g_traces |= 1u << i;
                found = true;
            }
        }
        ok = ok && found;
        csv += n;
        if (*csv == ',')
            csv++;
    }
    if (g_traces && g_level < LOG_TRACE)
        g_level = LOG_TRACE;
    return ok;
}

static void json_escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
        }
    }
}

void log_msg(log_level lvl, log_subsys s, const char *fmt, ...)
{
    char msg[2048], ts[32];
    time_t now;
    struct tm tmv;
    va_list ap;

    if (lvl > g_level)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    now = time(NULL);
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tmv);

    if (g_txt) {
        fprintf(g_txt, "%s [%-5s] %-8s %s\n", ts, LEVEL_NAMES[lvl], SUB_NAMES[s], msg);
        fflush(g_txt);
    }
    if (g_json) {
        fprintf(g_json, "{\"t\":\"%s\",\"lvl\":\"%s\",\"sub\":\"%s\",\"msg\":\"", ts,
                LEVEL_NAMES[lvl], SUB_NAMES[s]);
        json_escape(g_json, msg);
        fputs("\"}\n", g_json);
        fflush(g_json);
    }
    if (g_echo && lvl <= LOG_WARN)
        fprintf(stderr, "[%s] %s: %s\n", LEVEL_NAMES[lvl], SUB_NAMES[s], msg);
}
