/* Decide whether a cue sheet is a supported Saturn Bomberman disc. */
#ifndef SBPC_DISC_IDENTIFY_H
#define SBPC_DISC_IDENTIFY_H

#include "disc/disc.h"
#include "disc/saturn_header.h"
#include "disc/versions.h"

typedef enum {
    ID_SUPPORTED,       /* matches a known version exactly */
    ID_UNKNOWN_VERSION, /* Saturn Bomberman, but this version/region isn't recorded yet */
    ID_OTHER_GAME,      /* a Saturn disc, but not Saturn Bomberman */
    ID_NOT_SATURN,      /* first data track isn't a Saturn disc */
    ID_BAD_IMAGE        /* cue/bin problem: missing files, bad sizes, missing tracks, bad hash */
} id_status;

typedef struct {
    id_status status;
    const game_version *version; /* set when status == ID_SUPPORTED */
    bool have_header;
    saturn_header header;
    char data_sha1[41];          /* empty if not computed */
    int track_count;
    char region[16];             /* from header areas: "USA", "Japan", "Europe", ... */
    char reason[256];            /* human-readable explanation for anything but ID_SUPPORTED */
} disc_identity;

void identify_disc(const char *cue_path, disc_identity *out, disc_progress_cb progress, void *user);
/* Quick check of a game's own copy made by a build (<built>/<version>/disc): reads only the header and the
 * track count, and trusts the build for the data hash (the launcher lists built games before its full scan). */
bool identify_installed(const char *cue_path, const game_version *v, disc_identity *out);
const char *id_status_name(id_status s);

#endif
