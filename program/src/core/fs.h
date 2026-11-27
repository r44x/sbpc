/* Small cross-platform file helpers. All paths are UTF-8. */
#ifndef SBPC_CORE_FS_H
#define SBPC_CORE_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SBPC_PATH_MAX 1024

#ifdef _WIN32
#define SBPC_PATH_SEP '\\'
#else
#define SBPC_PATH_SEP '/'
#endif

FILE *fs_open(const char *path, const char *mode);
bool fs_seek(FILE *f, int64_t offset);
int64_t fs_size(const char *path);           /* -1 if missing */
bool fs_is_file(const char *path);
bool fs_is_dir(const char *path);
bool fs_mkdirs(const char *path);             /* like mkdir -p */
bool fs_rename(const char *from, const char *to); /* replaces target */

/* Calls cb for each entry (not "." / ".."). Return false from cb to stop. */
typedef bool (*fs_dir_cb)(const char *name, bool is_dir, void *user);
bool fs_list_dir(const char *dir, fs_dir_cb cb, void *user);

/* Path string helpers; all write into out[SBPC_PATH_MAX] and return false on overflow. */
bool fs_join(char *out, const char *a, const char *b);
bool fs_dirname(char *out, const char *path);
const char *fs_basename(const char *path);
bool fs_has_ext(const char *path, const char *ext); /* case-insensitive, ext like ".cue" */
bool fs_exe_dir(char *out);

#endif
