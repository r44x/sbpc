/* Where SBPC keeps config, logs, saves and cached modules. */
#ifndef SBPC_CORE_PATHS_H
#define SBPC_CORE_PATHS_H

#include <stdbool.h>

/* Where the data folder is, first match wins:
 * - portable mode (forced, or portable.txt next to the executable): <exe dir>/data;
 * - the repository layout (the executable in SBPC/builds, with SBPC/libs and SBPC/games beside it):
 *   SBPC/recompiled, which then also holds the built games directly (recompiled/usa, ...);
 * - otherwise %APPDATA%\SBPC on Windows, $XDG_DATA_HOME/sbpc or ~/.local/share/sbpc on Linux. */
bool paths_init(bool force_portable);
bool paths_is_portable(void);
const char *paths_data_dir(void);
const char *paths_log_dir(void);
/* Where Build & Play puts each version's recompiled game (<dir>/<version id>/...):
 * the data folder itself in the repository layout, <data>/generated otherwise. */
const char *paths_built_dir(void);
/* The repository's games folder (SBPC/games), or NULL outside the repository layout. */
const char *paths_games_dir(void);

#endif
