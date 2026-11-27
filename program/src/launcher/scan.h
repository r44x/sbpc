/* Finds disc images (.cue, .chd) in the games folder (and up to 2 levels of subfolders) and identifies
 * them; scan_add_installed adds the games' own copies made by Build & Play. */
#ifndef SBPC_LAUNCHER_SCAN_H
#define SBPC_LAUNCHER_SCAN_H

#include "disc/identify.h"

typedef struct {
    char cue_path[SBPC_PATH_MAX];
    disc_identity id;
    bool installed;       /* the game's own copy in the data folder (its original dump isn't in the list) */
} scan_entry;

typedef struct {
    int count;
    int cap;
    scan_entry *items;
} scan_result;

/* progress: called before each disc with its file name; may be NULL. */
typedef void (*scan_progress_cb)(int index, int total, const char *name, void *user);

bool scan_folder(const char *dir, scan_result *out, scan_progress_cb progress, void *user);
void scan_free(scan_result *r);
/* Adds built games from <data>/generated/<version>/disc/ whose version isn't already in r. */
void scan_add_installed(scan_result *r);

#endif
