#include "core/config.h"
#include "core/fs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void config_init(config *c) { memset(c, 0, sizeof *c); }

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

bool config_load(config *c, const char *path)
{
    char line[CONFIG_KEY_MAX + CONFIG_VAL_MAX + 8];
    FILE *f = fs_open(path, "r");
    config_init(c);
    if (!f)
        return !fs_is_file(path);
    while (fgets(line, sizeof line, f)) {
        char *eq, *k = trim(line);
        if (*k == 0 || *k == '#' || *k == ';')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        config_set(c, trim(k), trim(eq + 1));
    }
    fclose(f);
    return true;
}

bool config_save(const config *c, const char *path)
{
    char tmp[SBPC_PATH_MAX];
    FILE *f;
    if (strlen(path) + 5 > sizeof tmp)
        return false;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fs_open(tmp, "w");
    if (!f)
        return false;
    fputs("# SBPC settings - written by the launcher/overlay\n", f);
    for (int i = 0; i < c->count; i++)
        fprintf(f, "%s=%s\n", c->keys[i], c->vals[i]);
    if (fclose(f) != 0)
        return false;
    return fs_rename(tmp, path);
}

const char *config_get(const config *c, const char *key, const char *fallback)
{
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->keys[i], key) == 0)
            return c->vals[i];
    return fallback;
}

bool config_set(config *c, const char *key, const char *val)
{
    int i;
    if (strlen(key) >= CONFIG_KEY_MAX || strlen(val) >= CONFIG_VAL_MAX || strchr(val, '\n'))
        return false;
    for (i = 0; i < c->count; i++)
        if (strcmp(c->keys[i], key) == 0)
            break;
    if (i == c->count) {
        if (c->count == CONFIG_MAX_ENTRIES)
            return false;
        c->count++;
        strcpy(c->keys[i], key);
    }
    strcpy(c->vals[i], val);
    return true;
}
