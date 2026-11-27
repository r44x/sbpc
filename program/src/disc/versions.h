/* Known retail versions of Saturn Bomberman. Identifiers and addresses only - never game bytes. */
#ifndef SBPC_DISC_VERSIONS_H
#define SBPC_DISC_VERSIONS_H

#include <stdint.h>

/* A file on the disc that holds code, and where the game puts it in memory. Modules can
 * overlap in memory (one replaces another at runtime), so each is analysed on its own. */
typedef struct {
    const char *name;       /* short name for reports: "boot", "kernel" */
    const char *path;       /* path on the disc; NULL = the 1st read file from the header */
    uint32_t load_addr;     /* 0 = from the header (1st read file only) */
    uint32_t entry;         /* 0 = load_addr */
    const char *note;
    /* Extra analysis entry points: code nothing points at statically (longjmp targets, computed
     * jumps). Found with SBPC_IHIST ("ihist start" lines); addresses only. */
    const uint32_t *hints;
    int hint_count;
} game_module;

typedef struct {
    const char *id;         /* short id used for module/save names: "usa", "jpn", "eur" */
    const char *name;       /* display name */
    const char *product;    /* header product number */
    const char *version;    /* header version */
    const char *data_sha1;  /* SHA-1 of the data track's user data (see disc_hash_track) */
    const char *redump_sha1;/* Redump SHA-1 of the Track 01 .bin, for reference */
    int track_count;        /* data track + CD audio tracks */
    const game_module *modules;
    int module_count;
    int max_players;        /* most players the game handles (10, on two multitaps); the front end plugs in no more */
    int pal;                /* 1 = a PAL (50 Hz) release: Europe */
    const char *folder;     /* its folder of built files in the data folder: "saturn-bomberman-usa" */
} game_version;

extern const game_version GAME_VERSIONS[];
extern const int GAME_VERSION_COUNT;
/* Used for discs that aren't (yet) in the table: just the 1st read file. */
extern const game_module DEFAULT_MODULES[];
extern const int DEFAULT_MODULE_COUNT;

#endif
