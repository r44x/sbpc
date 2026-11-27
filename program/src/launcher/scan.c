#include "launcher/scan.h"
#include "core/log.h"
#include "core/paths.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    scan_result *r;
    const char *dir;
    int depth;
} walk_ctx;

static bool push(scan_result *r, const char *path)
{
    if (r->count == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        scan_entry *n = realloc(r->items, (size_t)cap * sizeof *n);
        if (!n)
            return false;
        r->items = n;
        r->cap = cap;
    }
    memset(&r->items[r->count], 0, sizeof r->items[0]);
    strcpy(r->items[r->count].cue_path, path);
    r->count++;
    return true;
}

static bool walk(const char *name, bool is_dir, void *user);

static void walk_dir(scan_result *r, const char *dir, int depth)
{
    walk_ctx ctx = {r, dir, depth};
    fs_list_dir(dir, walk, &ctx);
}

static bool walk(const char *name, bool is_dir, void *user)
{
    walk_ctx *ctx = user;
    char full[SBPC_PATH_MAX];
    if (name[0] == '.' || !fs_join(full, ctx->dir, name))
        return true;
    if (is_dir) {
        if (ctx->depth < 2)
            walk_dir(ctx->r, full, ctx->depth + 1);
    } else if (disc_is_image_name(name)) {
        push(ctx->r, full);
    }
    return true;
}

static int rank(id_status s) { return s == ID_SUPPORTED ? 0 : s == ID_UNKNOWN_VERSION ? 1 : 2; }

static int cmp_entries(const void *a, const void *b)
{
    const scan_entry *x = a, *y = b;
    int d = rank(x->id.status) - rank(y->id.status);
    return d ? d : strcmp(x->cue_path, y->cue_path);
}

bool scan_folder(const char *dir, scan_result *out, scan_progress_cb progress, void *user)
{
    memset(out, 0, sizeof *out);
    if (!fs_is_dir(dir)) {
        LOGW(SUB_LAUNCHER, "games folder not found: %s", dir);
        return false;
    }
    walk_dir(out, dir, 0);
    LOGI(SUB_LAUNCHER, "found %d cue sheet(s) in %s", out->count, dir);
    for (int i = 0; i < out->count; i++) {
        scan_entry *e = &out->items[i];
        if (progress)
            progress(i, out->count, fs_basename(e->cue_path), user);
        identify_disc(e->cue_path, &e->id, NULL, NULL);
        LOGI(SUB_LAUNCHER, "%s -> %s%s%s", fs_basename(e->cue_path), id_status_name(e->id.status),
             e->id.reason[0] ? ": " : "", e->id.reason);
    }
    qsort(out->items, (size_t)out->count, sizeof out->items[0], cmp_entries);
    return true;
}

void scan_add_installed(scan_result *r)
{
    const char *gen = paths_built_dir();
    scan_result inst;
    int added = 0;
    if (!fs_is_dir(gen) || !scan_folder(gen, &inst, NULL, NULL))
        return;
    for (int i = 0; i < inst.count; i++) {
        const scan_entry *e = &inst.items[i];
        bool have = false;
        if (e->id.status != ID_SUPPORTED || !e->id.version) continue;
        for (int k = 0; k < r->count && !have; k++)
            have = r->items[k].id.status == ID_SUPPORTED && r->items[k].id.version == e->id.version;
        if (have || !push(r, e->cue_path)) continue;   /* the original dump is listed: it plays this copy anyway */
        r->items[r->count - 1].id = e->id;
        r->items[r->count - 1].installed = true;
        added++;
    }
    scan_free(&inst);
    if (added) {
        LOGI(SUB_LAUNCHER, "%d built game(s) without their original dump", added);
        qsort(r->items, (size_t)r->count, sizeof r->items[0], cmp_entries);
    }
}

void scan_free(scan_result *r)
{
    free(r->items);
    memset(r, 0, sizeof *r);
}
