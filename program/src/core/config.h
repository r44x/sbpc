/* Tiny key=value config (sbpc.ini). Lines starting with # or ; are comments. */
#ifndef SBPC_CORE_CONFIG_H
#define SBPC_CORE_CONFIG_H

#include <stdbool.h>

#define CONFIG_MAX_ENTRIES 128
#define CONFIG_KEY_MAX 64
#define CONFIG_VAL_MAX 1024

typedef struct {
    int count;
    char keys[CONFIG_MAX_ENTRIES][CONFIG_KEY_MAX];
    char vals[CONFIG_MAX_ENTRIES][CONFIG_VAL_MAX];
} config;

void config_init(config *c);
bool config_load(config *c, const char *path);   /* missing file -> true, empty */
bool config_save(const config *c, const char *path);
const char *config_get(const config *c, const char *key, const char *fallback);
bool config_set(config *c, const char *key, const char *val);

#endif
