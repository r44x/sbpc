#include "core/paths.h"
#include "core/fs.h"

#include <stdlib.h>
#include <string.h>

static char g_data[SBPC_PATH_MAX];
static char g_logs[SBPC_PATH_MAX];
static char g_built[SBPC_PATH_MAX];
static char g_root[SBPC_PATH_MAX];     /* the repository folder, "" outside the repository layout */
static char g_games[SBPC_PATH_MAX];
static bool g_portable;

static bool user_data_dir(char *out)
{
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    return appdata && *appdata && fs_join(out, appdata, "SBPC");
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        return fs_join(out, xdg, "sbpc");
    if (!home || !*home)
        return false;
    return fs_join(out, home, ".local/share/sbpc");
#endif
}

/* SBPC/builds/sbpc -> SBPC, when SBPC/libs and SBPC/games are there. */
static bool find_root(const char *exe_dir, char *out)
{
    char libs[SBPC_PATH_MAX], games[SBPC_PATH_MAX];
    size_t n = strlen(exe_dir);
    while (n > 0 && (exe_dir[n - 1] == '/' || exe_dir[n - 1] == '\\')) n--;     /* trailing separators */
    while (n > 0 && exe_dir[n - 1] != '/' && exe_dir[n - 1] != '\\') n--;       /* the exe's folder name */
    while (n > 1 && (exe_dir[n - 1] == '/' || exe_dir[n - 1] == '\\')) n--;
    if (n == 0 || n >= SBPC_PATH_MAX)
        return false;
    memcpy(out, exe_dir, n);
    out[n] = 0;
    return fs_join(libs, out, "libs") && fs_join(games, out, "games") && fs_is_dir(libs) && fs_is_dir(games);
}

bool paths_init(bool force_portable)
{
    char exe_dir[SBPC_PATH_MAX], marker[SBPC_PATH_MAX];
    bool have_exe = fs_exe_dir(exe_dir);

    g_root[0] = g_games[0] = 0;
    if (have_exe && find_root(exe_dir, g_root)) {
        if (!fs_join(g_games, g_root, "games"))
            g_games[0] = 0;
    } else {
        g_root[0] = 0;
    }

    g_portable = force_portable;
    if (!g_portable && have_exe && fs_join(marker, exe_dir, "portable.txt"))
        g_portable = fs_is_file(marker);

    if (g_portable) {
        if (!have_exe || !fs_join(g_data, exe_dir, "data") || !fs_join(g_built, g_data, "generated"))
            return false;
    } else if (g_root[0]) {
        if (!fs_join(g_data, g_root, "recompiled"))
            return false;
        strcpy(g_built, g_data);
    } else if (!user_data_dir(g_data) || !fs_join(g_built, g_data, "generated")) {
        return false;
    }
    if (!fs_join(g_logs, g_data, "logs"))
        return false;
    return fs_mkdirs(g_data) && fs_mkdirs(g_logs);
}

bool paths_is_portable(void) { return g_portable; }
const char *paths_data_dir(void) { return g_data; }
const char *paths_log_dir(void) { return g_logs; }
const char *paths_built_dir(void) { return g_built; }
const char *paths_games_dir(void) { return g_games[0] ? g_games : NULL; }

