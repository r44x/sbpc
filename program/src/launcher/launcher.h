/* Launcher entry points: command-line and SDL3 GUI. */
#ifndef SBPC_LAUNCHER_LAUNCHER_H
#define SBPC_LAUNCHER_LAUNCHER_H

/* argv here starts after "--cli". Returns process exit code. */
int launcher_cli(int argc, char **argv);

#include <stdbool.h>
#include <stddef.h>

#include "disc/identify.h"

/* Returns process exit code. Only available when built with SDL3. When the user picks Build & Play,
 * the disc's .cue path is written to play_cue (otherwise it is left empty) and the caller runs it. */
int launcher_gui(char *play_cue, size_t cap);

/* Recompiles every module of a disc into <data>/generated/<version>. msg receives a one-line summary. */
bool launcher_build(const char *cue, bool verbose, char *msg, size_t cap);

/* Build progress, for the launcher's recompiling screen. Steps 1-3 repeat for each module (code file) of the
 * game; module is its index, modules the count. The callback runs on the building thread. */
enum { BUILD_STEP_READ, BUILD_STEP_FIND, BUILD_STEP_TRANSLATE, BUILD_STEP_COMPILE, BUILD_STEP_INSTALL,
       BUILD_STEP_ART, BUILD_STEP_DONE, BUILD_STEP_COUNT };
typedef void (*build_progress_cb)(int step, int module, int modules, void *user);
void launcher_set_build_progress(build_progress_cb cb, void *user);

/* True when that disc has already been built by this SBPC version (so "Play" can skip the build). */
bool launcher_build_current(const disc_identity *id);

/* Loads the disc's recompiled modules (built by launcher_build) into the machine's dispatcher.
 * Missing or stale modules are skipped (that code runs on the interpreter). */
struct saturn;
bool launcher_load_modules(struct saturn *m, const disc_identity *id);
/* The game's own copy of the disc, made by launcher_build: <data>/generated/<version>/disc/<name>.cue
 * (ISO+WAV). Once it exists the game plays from it, and the original dump isn't needed any more. */
bool launcher_installed_image(const disc_identity *id, char *out, size_t cap);

/* The launcher wallpaper (the Battle Game menu's bomb pattern, 128x64, repeating): runs the built game headless
   to that screen and saves <data>/generated/<version>/launcher_bg.bmp. Called after a build; a few seconds. */
bool launcher_extract_art(const char *cue, char *msg, size_t cap);
/* That file, when it exists. */
bool launcher_art_path(const disc_identity *id, char *out, size_t cap);
/* True when an attempt found no wallpaper (launcher_bg.none): don't run the game for it again. */
bool launcher_art_tried(const disc_identity *id);

/* The folder holding a version's built files: <data>/generated/saturn-bomberman-<region> (in the repository
   layout, SBPC/recompiled/...). Renames a folder left by an older SBPC under the short version id. */
bool launcher_game_dir(const game_version *v, char *out);

/* Shared: path of sbpc.ini in the data folder. */
const char *launcher_config_path(void);

#endif
