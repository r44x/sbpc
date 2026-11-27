/* SDL3 launcher: one view that lists the discs found in the games folder (SBPC/games, else the launcher's own
 * folder), with Build & Play. Terminal style: green lines and uppercase text on black, in the Half Eighties
 * 8x8 pixel font (libs/half-eighties, OFL), on an 800x533 canvas; the window opens at that size. */
#include "launcher/launcher.h"
#include "launcher/scan.h"
#include "core/config.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "launcher/ui_font.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout, in canvas pixels (the mockups are this at 3/4 size). */
#define VIEW_W 800
#define VIEW_H 533
#define CH UI_CH
#define PAD_X 13             /* left margin of header text */
#define HEADER_Y 17
#define LINE1_Y 43
#define FOLDER_Y 61
#define LINE2_Y 87
#define SPLIT_X 400
#define LINE3_Y 484
#define LIST_X 11
#define LIST_Y 99
#define LIST_W 378
#define ROW_H 19
#define ROW_TEXT 5           /* text cell offset inside a row */
#define LIST_ROWS 19
#define DETAIL_X 415
#define DETAIL_Y 104
#define VALUE_X 487
#define FOOT_Y 499
#define BOX_H 21
#define BOX_TEXT 6           /* text cell offset inside a key box or badge */
#define STATUS_Y 420         /* notices (build failed, unreadable folder) at the bottom of the details panel */

typedef ui_rgb rgb;
#define GREEN UI_GREEN
#define GREEN_DIM UI_GREEN_DIM
#define WHITE UI_WHITE
#define GREY UI_GREY
#define BLACK UI_BLACK

typedef enum { SCREEN_SCANNING, SCREEN_LIST, SCREEN_BUILDING } screen;
enum { JOB_SCAN = 1, JOB_BUILD = 2 };
typedef enum { BTN_NONE, BTN_CHOOSE, BTN_PLAY, BTN_RESCAN, BTN_QUIT } button_id;

typedef struct {
    SDL_FRect r;             /* key box + label: the clickable area */
    button_id id;
} button;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *font;       /* 95 white glyphs in a row, tinted per draw */
    config cfg;
    screen scr;
    scan_result games;
    signed char *ready;      /* per entry: recompiled by this version (cached after each scan) */
    int sel, scroll;
    float mx, my;
    char base_dir[SBPC_PATH_MAX]; /* SBPC/games, or the launcher's own folder: scanned when no games folder is configured */
    bool running;
    char status[256];             /* a notice, shown only when set */
    char play_cue[SBPC_PATH_MAX]; /* set when Build & Play finished: the caller runs it */
    int build_index;              /* entry being built */
    SDL_GamepadType pad_type; /* last used pad, for button prompts */
    bool pad_used;
    button buttons[4];
    int nbuttons;
    /* Scanning and recompiling run on a worker thread so the window keeps responding (moving, resizing,
       quitting). The worker only touches the job_* fields; the main thread reads them once job_done is set. */
    SDL_Thread *job;
    SDL_AtomicInt job_done;
    SDL_AtomicInt job_index, job_total;  /* scan progress */
    bool rescan;                  /* a scan was asked for while a job ran */
    char job_dir[SBPC_PATH_MAX];  /* scan: the folder; build: the cue sheet */
    scan_result job_games;
    signed char *job_ready;
    bool job_ok;
    char job_msg[512];
    int job_kind;                 /* JOB_SCAN or JOB_BUILD while job runs */
    bool scanning;                /* a folder scan runs behind the list (built games are listed already) */
    bool autoplay;                /* developer: press Play once (SBPC_LAUNCHER_AUTOPLAY) */
    SDL_AtomicInt cancel;         /* leaving to play during a scan: skip the rest of its extra work */
    SDL_AtomicInt build_step, build_module, build_modules;   /* launcher_build progress (BUILD_STEP_*) */
    SDL_Texture *art;             /* the selected built game's wallpaper (launcher_bg.bmp), or NULL */
    char art_for[SBPC_PATH_MAX];  /* the file it came from */
} gui;

/* ---- folder dialog (callback may arrive on another thread) ---------------------- */
static SDL_AtomicInt g_dialog; /* 0 idle, 1 open, 2 picked, 3 cancelled */
static char g_dialog_path[SBPC_PATH_MAX];

static void SDLCALL on_folder(void *user, const char *const *list, int filter)
{
    (void)user; (void)filter;
    if (list && list[0] && strlen(list[0]) < sizeof g_dialog_path) {
        strcpy(g_dialog_path, list[0]);
        SDL_SetAtomicInt(&g_dialog, 2);
    } else {
        if (!list)
            LOGW(SUB_LAUNCHER, "folder dialog failed: %s", SDL_GetError());
        SDL_SetAtomicInt(&g_dialog, 3);
    }
}

static void open_folder_dialog(gui *g)
{
    const char *cur = config_get(&g->cfg, "games_dir", NULL);
    if (SDL_GetAtomicInt(&g_dialog) == 1)
        return;
    SDL_SetAtomicInt(&g_dialog, 1);
    SDL_ShowOpenFolderDialog(on_folder, NULL, g->win, cur, false);
}

/* ---- font and drawing ---------------------------------------------------------------------- */
static void fill(gui *g, rgb c, float x, float y, float w, float h) { ui_fill(g->ren, c, x, y, w, h); }

static float text_n(gui *g, rgb c, float x, float y, const char *s, int max_chars)
{
    return ui_text_n(g->ren, g->font, c, x, y, s, max_chars);
}

static float text(gui *g, rgb c, float x, float y, const char *s) { return ui_text(g->ren, g->font, c, x, y, s); }

#define text_len ui_text_len

/* Fits s into max_chars, ending in "..." when cut. */
static void text_clip(gui *g, rgb c, float x, float y, const char *s, int max_chars)
{
    int n = text_len(s);
    if (n <= max_chars || max_chars <= 3) {
        text_n(g, c, x, y, s, max_chars);
        return;
    }
    x += text_n(g, c, x, y, s, max_chars - 3);
    text(g, c, x, y, "...");
}

/* Keeps the end of s (for paths): "...\GAMES". */
static void text_clip_left(gui *g, rgb c, float x, float y, const char *s, int max_chars)
{
    int n = text_len(s);
    if (n <= max_chars || max_chars <= 3) {
        text_n(g, c, x, y, s, max_chars);
        return;
    }
    x += text(g, c, x, y, "...");
    for (int skip = n - (max_chars - 3); skip > 0; s++)
        if (((unsigned char)s[1] & 0xC0) != 0x80)
            skip--;
    text(g, c, x, y, s);
}

/* Wraps s into lines of at most cols characters; returns the y after the last line. */
static float text_wrap(gui *g, rgb c, float x, float y, const char *s, int cols, float line_h)
{
    while (*s) {
        int n = (int)strlen(s), cut = n;
        if (n > cols) {
            cut = cols;
            while (cut > 0 && s[cut] != ' ')
                cut--;
            if (cut == 0)
                cut = cols;
        }
        text_n(g, c, x, y, s, cut);
        y += line_h;
        s += cut;
        while (*s == ' ')
            s++;
    }
    return y;
}

static float label_box(gui *g, float x, float y, const char *s) { return ui_label_box(g->ren, g->font, x, y, s); }

static bool inside(const SDL_FRect *r, float x, float y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* Footer entry: [KEY] LABEL. Returns the x where the next one starts. */
static float footer_button(gui *g, button_id id, float x, const char *key, const char *label)
{
    button *b = &g->buttons[g->nbuttons++];
    float w = (float)((text_len(key) + text_len(label)) * CH + 14);
    b->id = id;
    b->r = (SDL_FRect){x, FOOT_Y, w, BOX_H};
    ui_key(g->ren, g->font, x, FOOT_Y, key, label, inside(&b->r, g->mx, g->my));
    return x + w + 16;
}

/* Button-prompt names follow the last pad used (Xbox letters vs PlayStation symbols). */
static const char *prompt(gui *g, SDL_GamepadButton b, const char *key)
{
    bool ps;
    if (!g->pad_used)
        return key;
    ps = g->pad_type == SDL_GAMEPAD_TYPE_PS3 || g->pad_type == SDL_GAMEPAD_TYPE_PS4 ||
         g->pad_type == SDL_GAMEPAD_TYPE_PS5;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? "Cross" : "A";
    case SDL_GAMEPAD_BUTTON_EAST: return ps ? "Circle" : "B";
    case SDL_GAMEPAD_BUTTON_WEST: return ps ? "Square" : "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? "Triangle" : "Y";
    default: return key;
    }
}

static const char *entry_name(const scan_entry *e)
{
    static char buf[SBPC_PATH_MAX];
    size_t n;
    snprintf(buf, sizeof buf, "%s", fs_basename(e->cue_path));
    n = strlen(buf);
    if (n > 4)
        buf[n - 4] = 0; /* drop .cue / .chd */
    if (e->installed && strlen(buf) + 12 < sizeof buf)
        strcat(buf, " (installed)");   /* a built game whose original dump isn't in the folder */
    return buf;
}

static bool entry_ready(const gui *g, int i)
{
    return g->ready && g->ready[i] > 0;
}

/* ---- actions ------------------------------------------------------------------------------ */
static const char *games_dir(gui *g);
static int SDLCALL scan_job(void *user);
static int SDLCALL build_job(void *user);
static void finish_scan(gui *g);

static bool start_job(gui *g, SDL_ThreadFunction fn, const char *name)
{
    SDL_SetAtomicInt(&g->job_done, 0);
    g->job_kind = fn == scan_job ? JOB_SCAN : JOB_BUILD;
    g->job = SDL_CreateThread(fn, name, g);
    if (!g->job)
        LOGW(SUB_LAUNCHER, "no worker thread (%s), working on the window thread", SDL_GetError());
    return g->job != NULL;
}

/* Built games, listed at once: each version's own disc copy with a current build stamp. Only the copy's
 * header is read (identify_installed), so this takes a moment where the full scan hashes every disc. */
static void quick_list(gui *g)
{
    scan_result *r = &g->games;
    if (r->count)
        return;   /* a rescan keeps the current list on screen until it's done */
    for (int i = 0; i < GAME_VERSION_COUNT; i++) {
        disc_identity id;
        char cue[SBPC_PATH_MAX];
        memset(&id, 0, sizeof id);
        id.version = &GAME_VERSIONS[i];
        if (!launcher_installed_image(&id, cue, sizeof cue) || !identify_installed(cue, &GAME_VERSIONS[i], &id) ||
            !launcher_build_current(&id))
            continue;
        if (r->count == r->cap) {
            int cap = r->cap ? r->cap * 2 : 8;
            scan_entry *n = realloc(r->items, (size_t)cap * sizeof *n);
            signed char *rd = realloc(g->ready, (size_t)cap);
            if (!n || !rd) { if (n) r->items = n; if (rd) g->ready = rd; break; }
            r->items = n; r->cap = cap; g->ready = rd;
        }
        memset(&r->items[r->count], 0, sizeof r->items[0]);
        snprintf(r->items[r->count].cue_path, sizeof r->items[0].cue_path, "%s", cue);
        r->items[r->count].id = id;
        g->ready[r->count] = 1;   /* not marked "installed": the scan may still find its original dump */
        r->count++;
    }
    if (r->count)
        LOGI(SUB_LAUNCHER, "%d built game(s) listed before the folder scan", r->count);
}

static void start_scan(gui *g)
{
    if (g->job) {          /* a scan or build is running: scan again once it ends */
        g->rescan = true;
        return;
    }
    quick_list(g);
    g->scanning = true;
    SDL_SetAtomicInt(&g->cancel, 0);
    g->scr = g->games.count ? SCREEN_LIST : SCREEN_SCANNING;
    snprintf(g->job_dir, sizeof g->job_dir, "%s", games_dir(g));
    SDL_SetAtomicInt(&g->job_index, 0);
    SDL_SetAtomicInt(&g->job_total, 0);
    if (!start_job(g, scan_job, "sbpc-scan")) {
        scan_job(g);
        SDL_SetAtomicInt(&g->job_done, 0);
        finish_scan(g);
    }
}

/* The configured games folder, or the default one until the user picks another. */
static const char *games_dir(gui *g)
{
    return config_get(&g->cfg, "games_dir", g->base_dir);
}

static void scan_progress(int index, int total, const char *name, void *user)
{
    gui *g = user;
    (void)name;
    SDL_SetAtomicInt(&g->job_total, total);
    SDL_SetAtomicInt(&g->job_index, index);
}

/* Worker: identifies the discs and checks which are already recompiled (both read whole files). */
static int SDLCALL scan_job(void *user)
{
    gui *g = user;
    scan_result *r = &g->job_games;
    g->job_ok = scan_folder(g->job_dir, r, scan_progress, g);
    scan_add_installed(r);                  /* built games still play without the folder */
    g->job_ready = r->count ? calloc((size_t)r->count, 1) : NULL;
    for (int i = 0; g->job_ready && i < r->count; i++)
        g->job_ready[i] = (signed char)(r->items[i].id.status == ID_SUPPORTED &&
                                        launcher_build_current(&r->items[i].id));
    for (int i = 0; g->job_ready && i < r->count && !SDL_GetAtomicInt(&g->cancel); i++) {   /* built before the wallpaper existed */
        char art[SBPC_PATH_MAX], cue[SBPC_PATH_MAX], msg[SBPC_PATH_MAX + 32];
        if (g->job_ready[i] && !launcher_art_path(&r->items[i].id, art, sizeof art) &&
            !launcher_art_tried(&r->items[i].id) &&
            launcher_installed_image(&r->items[i].id, cue, sizeof cue))
            launcher_extract_art(cue, msg, sizeof msg);
    }
    SDL_SetAtomicInt(&g->job_done, 1);
    return 0;
}

static void finish_scan(gui *g)
{
    const char *last = config_get(&g->cfg, "last_game", "");
    const game_version *was = g->games.count && g->scr == SCREEN_LIST ? g->games.items[g->sel].id.version : NULL;
    scan_result r = g->job_games;
    signed char *rd = g->job_ready;
    int n = 0;
    scan_free(&g->games);
    free(g->ready);
    g->games = r;
    g->ready = rd;
    memset(&g->job_games, 0, sizeof g->job_games);
    g->job_ready = NULL;
    /* ready games first, each group in the scan's order */
    for (int pass = 0; pass < 2 && rd; pass++)
        for (int i = n; i < r.count; i++)
            if ((rd[i] > 0) == (pass == 0) && i != n) {
                scan_entry e = r.items[i];
                signed char k = rd[i];
                memmove(&r.items[n + 1], &r.items[n], (size_t)(i - n) * sizeof e);
                memmove(&rd[n + 1], &rd[n], (size_t)(i - n));
                r.items[n] = e; rd[n] = k; n++;
            } else if ((rd[i] > 0) == (pass == 0)) {
                n++;
            }
    g->scanning = false;
    g->sel = g->scroll = 0;
    g->status[0] = 0;
    if (!g->job_ok)
        snprintf(g->status, sizeof g->status, "Can't read that folder. Press F2 to pick another.");
    for (int i = 0; i < g->games.count; i++)
        if (strcmp(g->games.items[i].cue_path, last) == 0)
            g->sel = i;
    for (int i = 0; was && i < g->games.count; i++)   /* the game picked during the scan stays picked */
        if (g->games.items[i].id.version == was && g->ready[i] > 0) {
            g->sel = i;
            break;
        }
    {   /* developer: SBPC_LAUNCHER_AUTOPLAY=<version id> presses Play on that game after the scan */
        const char *ap = SDL_getenv("SBPC_LAUNCHER_AUTOPLAY");
        for (int i = 0; ap && i < g->games.count; i++)
            if (g->games.items[i].id.version && strcmp(g->games.items[i].id.version->id, ap) == 0) {
                g->sel = i;
                g->scr = SCREEN_LIST;
                g->autoplay = true;
                break;
            }
    }
    if (g->sel >= LIST_ROWS)
        g->scroll = g->sel - LIST_ROWS + 1;
    g->scr = SCREEN_LIST;
}

static void set_games_dir(gui *g, const char *dir)
{
    config_set(&g->cfg, "games_dir", dir);
    if (!config_save(&g->cfg, launcher_config_path()))
        LOGW(SUB_LAUNCHER, "could not save %s", launcher_config_path());
    LOGI(SUB_LAUNCHER, "games folder: %s", dir);
    start_scan(g);
}

/* The game plays from its own copy of the disc (made by Build) when there is one, else from the dump. */
static void set_play_path(gui *g, const scan_entry *e)
{
    if (!launcher_installed_image(&e->id, g->play_cue, sizeof g->play_cue))
        snprintf(g->play_cue, sizeof g->play_cue, "%s", e->cue_path);
}

static void play_selected(gui *g)
{
    const scan_entry *e;
    if (g->scr != SCREEN_LIST || g->games.count == 0)
        return;
    e = &g->games.items[g->sel];
    if (e->id.status != ID_SUPPORTED) {
        snprintf(g->status, sizeof g->status, "That disc can't be used: %s", e->id.reason);
        return;
    }
    config_set(&g->cfg, "last_game", e->cue_path);
    config_save(&g->cfg, launcher_config_path());
    LOGI(SUB_LAUNCHER, "build & play: %s (%s)", e->id.version->id, e->cue_path);
    g->build_index = g->sel;
    if (launcher_build_current(&e->id)) {   /* already recompiled by this version: just play */
        set_play_path(g, e);
        g->running = false;
        return;
    }
    if (g->job) {   /* the folder scan is still running: one job at a time */
        snprintf(g->status, sizeof g->status, "Wait for the folder scan to finish, then recompile.");
        return;
    }
    g->scr = SCREEN_BUILDING;
    SDL_SetAtomicInt(&g->build_step, BUILD_STEP_READ);
    SDL_SetAtomicInt(&g->build_module, 0);
    SDL_SetAtomicInt(&g->build_modules, e->id.version->module_count);
    snprintf(g->job_dir, sizeof g->job_dir, "%s", e->cue_path);
    if (!start_job(g, build_job, "sbpc-build"))
        build_job(g);            /* finished below, on the next frame */
}

static void build_progress(int step, int module, int modules, void *user)
{
    gui *g = user;
    SDL_SetAtomicInt(&g->build_module, module);
    SDL_SetAtomicInt(&g->build_modules, modules);
    SDL_SetAtomicInt(&g->build_step, step);
}

/* Worker: recompiles the game (minutes on a slow machine). */
static int SDLCALL build_job(void *user)
{
    gui *g = user;
    launcher_set_build_progress(build_progress, g);
    g->job_ok = launcher_build(g->job_dir, false, g->job_msg, sizeof g->job_msg);
    SDL_SetAtomicInt(&g->job_done, 1);
    return 0;
}

static void finish_build(gui *g)
{
    if (!g->job_ok) {
        /* The game still runs without recompiled code (the interpreter covers it), just slower.
           Play anyway; the log says why, and F1 -> Info shows no recompiled entry points. */
        LOGW(SUB_LAUNCHER, "build failed, playing with the interpreter: %s", g->job_msg);
    } else {
        LOGI(SUB_LAUNCHER, "%s", g->job_msg);
    }
    set_play_path(g, &g->games.items[g->build_index]);
    g->running = false;          /* leave the launcher; main() opens the game window */
}

/* Called every frame: picks up a finished job. */
static void poll_job(gui *g)
{
    if (!SDL_GetAtomicInt(&g->job_done))
        return;
    SDL_SetAtomicInt(&g->job_done, 0);
    if (g->job) {
        SDL_WaitThread(g->job, NULL);
        g->job = NULL;
    }
    if (g->job_kind == JOB_SCAN)
        finish_scan(g);
    else
        finish_build(g);
    if (g->rescan && g->running) {
        g->rescan = false;
        start_scan(g);
    }
}

static void move_sel(gui *g, int d)
{
    if (g->scr != SCREEN_LIST || g->games.count == 0)
        return;
    g->sel = (g->sel + d + g->games.count) % g->games.count;
    if (g->sel < g->scroll)
        g->scroll = g->sel;
    if (g->sel >= g->scroll + LIST_ROWS)
        g->scroll = g->sel - LIST_ROWS + 1;
}

static void press(gui *g, button_id id)
{
    switch (id) {
    case BTN_CHOOSE: open_folder_dialog(g); break;
    case BTN_PLAY: play_selected(g); break;
    case BTN_RESCAN: start_scan(g); break;
    case BTN_QUIT: g->running = false; break;
    default: break;
    }
}

/* ---- screens -------------------------------------------------------------------------------- */
/* The Battle Game menu's bomb wallpaper behind the details of a built game: tiled at 2x and scrolling up and
   to the left at the game's own speed (half a game pixel per frame), dimmed so the text stays readable. */
static void draw_art(gui *g, const scan_entry *e, int index)
{
    char path[SBPC_PATH_MAX];
    SDL_Rect clip = {SPLIT_X + 1, LINE2_Y + 1, VIEW_W - SPLIT_X - 1, LINE3_Y - LINE2_Y - 1};
    float tw, th, off_x, off_y;
    Uint64 ms;
    if (!entry_ready(g, index) || !launcher_art_path(&e->id, path, sizeof path))
        return;
    if (!g->art || strcmp(g->art_for, path) != 0) {
        SDL_Surface *s = SDL_LoadBMP(path);
        if (g->art)
            SDL_DestroyTexture(g->art);
        g->art = s ? SDL_CreateTextureFromSurface(g->ren, s) : NULL;
        if (s)
            SDL_DestroySurface(s);
        snprintf(g->art_for, sizeof g->art_for, "%s", path);
        if (!g->art)
            return;
        SDL_SetTextureScaleMode(g->art, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureColorMod(g->art, 90, 90, 90);
    }
    if (!g->art)
        return;
    tw = (float)g->art->w * 2;
    th = (float)g->art->h * 2;
    ms = SDL_GetTicks();
    off_x = (float)(ms * 60 / 1000 % (Uint64)tw);   /* 0.5 game px = 1 canvas px per 60 Hz frame */
    off_y = (float)(ms * 60 / 1000 % (Uint64)th);
    SDL_SetRenderClipRect(g->ren, &clip);
    for (float y = (float)clip.y - off_y; y < (float)(clip.y + clip.h); y += th)
        for (float x = (float)clip.x - off_x; x < (float)(clip.x + clip.w); x += tw) {
            SDL_FRect dst = {x, y, tw, th};
            SDL_RenderTexture(g->ren, g->art, NULL, &dst);
        }
    SDL_SetRenderClipRect(g->ren, NULL);
}

static void draw_details(gui *g, const scan_entry *e, int index)
{
    static const char *labels[5] = {"Format", "Region", "Product", "Version", "Tracks"};
    const disc_identity *id = &e->id;
    char tracks[16];
    const char *values[5];
    float y = DETAIL_Y;
    size_t n = strlen(e->cue_path);

    draw_art(g, e, index);
    bool chd = n > 4 && SDL_strcasecmp(e->cue_path + n - 4, ".chd") == 0;   /* everything else is a cue sheet */

    text_clip(g, GREEN, DETAIL_X, y, id->version ? id->version->name : entry_name(e), 47);
    snprintf(tracks, sizeof tracks, "%d", id->track_count);
    values[0] = chd ? "CHD" : "Disc";
    values[1] = id->region; values[2] = id->header.product; values[3] = id->header.version; values[4] = tracks;
    y += 32;
    for (int i = 0; i < 5; i++) {
        if (i >= 1 && i <= 3 && !id->have_header)
            continue;   /* no readable header: only the format and track count */
        text(g, GREY, DETAIL_X, y, labels[i]);
        text_clip(g, WHITE, VALUE_X, y, values[i], 38);
        y += 16;
    }
    y += 11;
    if (id->status == ID_SUPPORTED) {
        label_box(g, DETAIL_X - 4, y, entry_ready(g, index) ? "Recompiled / ready to play" : "Supported");
    } else {   /* the "Supported" badge on red, without the reason (the user asked for the badge alone) */
        static const rgb red = {0xF8, 0x39, 0x39};
        ui_fill(g->ren, red, DETAIL_X - 4, y, (float)(text_len("Not supported") * CH + 8), BOX_H);
        text(g, BLACK, DETAIL_X, y + BOX_TEXT, "Not supported");
    }
}

/* Loader: three small squares, the lit one moving along (the font has no good spinner glyph). */
static void draw_loader(gui *g, float x, float y, rgb on)
{
    int lit = (int)((SDL_GetTicks() / 160) % 3);
    for (int i = 0; i < 3; i++)
        fill(g, i == lit ? on : GREEN_DIM, x + (float)i * 5, y + 2, 3, 3);
}

static void draw_list(gui *g)
{
    const scan_entry *cur = g->games.count ? &g->games.items[g->sel] : NULL;
    float x;

    if (g->games.count == 0)
        text(g, GREEN, PAD_X, LIST_Y + ROW_TEXT, "Nothing found.");
    if (g->scanning && g->games.count < LIST_ROWS) {   /* the folder scan behind the built games already listed */
        int total = SDL_GetAtomicInt(&g->job_total);
        float y = (float)(LIST_Y + g->games.count * ROW_H + ROW_TEXT);
        char line[64];
        if (total > 0)
            snprintf(line, sizeof line, "Scanning the games folder... %d/%d", SDL_GetAtomicInt(&g->job_index) + 1, total);
        else
            snprintf(line, sizeof line, "Scanning the games folder...");
        draw_loader(g, LIST_X + 2, y, WHITE);
        text(g, GREY, LIST_X + 2 + 3 * CH, y, line);
    }

    for (int row = 0; row < LIST_ROWS && g->scroll + row < g->games.count; row++) {
        int i = g->scroll + row;
        const scan_entry *e = &g->games.items[i];
        float y = (float)(LIST_Y + row * ROW_H);
        SDL_FRect r = {LIST_X, y, LIST_W, ROW_H};
        bool sel = i == g->sel, ready = entry_ready(g, i);
        rgb c = sel ? BLACK : e->id.status != ID_SUPPORTED ? GREY : WHITE;
        if (sel)
            fill(g, GREEN, r.x, r.y, r.w, r.h);
        else if (inside(&r, g->mx, g->my))
            fill(g, GREEN_DIM, r.x, r.y, r.w, r.h);
        text_clip(g, c, LIST_X + 2, y + ROW_TEXT, entry_name(e), ready ? 39 : 46);
        if (ready)
            text(g, sel ? BLACK : GREEN, LIST_X + LIST_W - 3 - 5 * CH + 1, y + ROW_TEXT, "Ready");
    }
    if (cur)
        draw_details(g, cur, g->sel);

    x = PAD_X - 3;
    if (cur && cur->id.status == ID_SUPPORTED)
        x = footer_button(g, BTN_PLAY, x, prompt(g, SDL_GAMEPAD_BUTTON_SOUTH, "Enter"),
                          entry_ready(g, g->sel) ? "Play" : "Recompile & play");
    x = footer_button(g, BTN_RESCAN, x, prompt(g, SDL_GAMEPAD_BUTTON_NORTH, "F5"), "Scan folder");
    if (SDL_GetAtomicInt(&g_dialog) != 1)
        x = footer_button(g, BTN_CHOOSE, x, prompt(g, SDL_GAMEPAD_BUTTON_WEST, "F2"), "Change folder");
    footer_button(g, BTN_QUIT, x, prompt(g, SDL_GAMEPAD_BUTTON_EAST, "Esc"), "Quit");
}

/* SBPC_LAUNCHER_SHOT=<file.bmp>: save the first list frame and quit (headless checks of the layout). */
static bool save_shot(gui *g)
{
    const char *path = SDL_getenv("SBPC_LAUNCHER_SHOT");
    const char *ms = SDL_getenv("SBPC_LAUNCHER_SHOT_MS");   /* developer: any screen, after that many ms */
    SDL_Surface *s;
    if (!path || !*path)
        return false;
    if (ms ? SDL_GetTicks() < (Uint64)SDL_atoi(ms) : g->scr != SCREEN_LIST)
        return false;
    s = SDL_RenderReadPixels(g->ren, NULL);
    if (s) {
        if (!SDL_SaveBMP(s, path))
            LOGW(SUB_LAUNCHER, "can't save %s: %s", path, SDL_GetError());
        SDL_DestroySurface(s);
    }
    return true;
}

/* The recompiling checklist: done rows in green with OK, the current one in white with a loader, the rest grey.
 * Finding and recompiling run once per code file of the game; the current row says which ("main program (2/3)"). */
static const char *module_label(const char *name)
{
    if (strcmp(name, "kernel") == 0) return "main program";
    if (strcmp(name, "boot") == 0) return "start-up program";
    return "extra program";
}

static void draw_build_steps(gui *g)
{
    /* Four rows, each covering a range of launcher_build's steps (BUILD_STEP_*). */
    static const struct { const char *label; int first, last; bool per_module; } ROWS[] = {
        {"Reading your disc", BUILD_STEP_READ, BUILD_STEP_READ, false},
        {"Finding the game's code", BUILD_STEP_FIND, BUILD_STEP_FIND, true},
        {"Recompiling code", BUILD_STEP_TRANSLATE, BUILD_STEP_COMPILE, true},
        {"Copying the recompilation to your PC", BUILD_STEP_INSTALL, BUILD_STEP_ART, false}};
    const game_version *v = g->games.items[g->build_index].id.version;
    int step = SDL_GetAtomicInt(&g->build_step), mod = SDL_GetAtomicInt(&g->build_module);
    int mods = SDL_GetAtomicInt(&g->build_modules);
    float y = DETAIL_Y;
    text(g, GREEN, DETAIL_X, y, "Recompiling");
    y += 32;
    for (int i = 0; i < (int)(sizeof ROWS / sizeof ROWS[0]); i++, y += 20) {
        /* steps 1-3 repeat per module: rows 1 and 2 show which module the current one is on */
        bool done = step > ROWS[i].last;
        bool now = step >= ROWS[i].first && step <= ROWS[i].last;
        char line[96];
        if (now && ROWS[i].per_module && v && mod < v->module_count)
            snprintf(line, sizeof line, "%s: %s (%d/%d)", ROWS[i].label, module_label(v->modules[mod].name), mod + 1, mods);
        else
            snprintf(line, sizeof line, "%s", ROWS[i].label);
        if (done) {
            text(g, GREEN, DETAIL_X, y, "OK");
            text_clip(g, GREEN, DETAIL_X + 4 * CH, y, line, 42);
        } else if (now) {
            draw_loader(g, DETAIL_X, y, WHITE);
            text_clip(g, WHITE, DETAIL_X + 4 * CH, y, line, 42);
        } else {
            text(g, GREY, DETAIL_X, y, "..");
            text_clip(g, GREY, DETAIL_X + 4 * CH, y, line, 42);
        }
    }
    text_wrap(g, GREY, DETAIL_X, y + 16, "The game starts when the recompilation is ready.", 46, 16);
}

static void draw(gui *g)
{
    float x;
    SDL_SetRenderDrawColor(g->ren, 0, 0, 0, 255);
    SDL_RenderClear(g->ren);
    g->nbuttons = 0;

    text(g, GREEN, PAD_X, HEADER_Y, "Saturn Bomberman PC Recomp");
    fill(g, GREEN, 0, LINE1_Y, VIEW_W, 1);
    x = text(g, GREEN, PAD_X, FOLDER_Y, "Games folder: ");
    text_clip_left(g, GREEN, PAD_X + x, FOLDER_Y, games_dir(g), (VIEW_W - 2 * PAD_X - (int)x) / CH);
    fill(g, GREEN, 0, LINE2_Y, VIEW_W, 1);
    fill(g, GREEN, SPLIT_X, LINE2_Y + 1, 1, LINE3_Y - LINE2_Y - 1);
    fill(g, GREEN, 0, LINE3_Y, VIEW_W, 1);

    switch (g->scr) {
    case SCREEN_SCANNING:
    {   int total = SDL_GetAtomicInt(&g->job_total);
        char line[64];
        if (total > 0)
            snprintf(line, sizeof line, "Scanning games... %d/%d", SDL_GetAtomicInt(&g->job_index) + 1, total);
        else
            snprintf(line, sizeof line, "Scanning games...");
        text(g, GREEN, PAD_X, LIST_Y + ROW_TEXT, line);
        break;
    }
    case SCREEN_LIST:
        draw_list(g);
        break;
    case SCREEN_BUILDING:
        text(g, GREEN, PAD_X, LIST_Y + ROW_TEXT, "Recompiling...");
        text_clip(g, WHITE, PAD_X, LIST_Y + ROW_TEXT + ROW_H, entry_name(&g->games.items[g->build_index]), 46);
        draw_build_steps(g);
        break;
    }
    if (g->status[0] && g->scr == SCREEN_LIST)
        text_wrap(g, WHITE, DETAIL_X, STATUS_Y, g->status, 46, 16);
    if (save_shot(g))
        g->running = false;
    SDL_RenderPresent(g->ren);
}

/* ---- input ------------------------------------------------------------------------------------- */
static void on_event(gui *g, SDL_Event *ev)
{
    SDL_ConvertEventToRenderCoordinates(g->ren, ev);
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        g->running = false;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        g->mx = ev->motion.x;
        g->my = ev->motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->button.button != SDL_BUTTON_LEFT)
            break;
        for (int i = 0; i < g->nbuttons; i++)
            if (inside(&g->buttons[i].r, ev->button.x, ev->button.y))
                press(g, g->buttons[i].id);
        if (g->scr == SCREEN_LIST && ev->button.x >= LIST_X && ev->button.x < LIST_X + LIST_W &&
            ev->button.y >= LIST_Y && ev->button.y < LIST_Y + LIST_ROWS * ROW_H) {
            int i = g->scroll + (int)((ev->button.y - LIST_Y) / ROW_H);
            if (i < g->games.count) {
                if (i == g->sel && ev->button.clicks == 2)
                    play_selected(g);
                g->sel = i;
            }
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        move_sel(g, ev->wheel.y > 0 ? -1 : 1);
        break;
    case SDL_EVENT_KEY_DOWN:
        g->pad_used = false;
        switch (ev->key.key) {
        case SDLK_UP: move_sel(g, -1); break;
        case SDLK_DOWN: move_sel(g, 1); break;
        case SDLK_RETURN:
            press(g, BTN_PLAY);
            break;
        case SDLK_F5: if (!ev->key.repeat) press(g, BTN_RESCAN); break;
        case SDLK_F2: press(g, BTN_CHOOSE); break;
        case SDLK_ESCAPE: g->running = false; break;
        default: break;
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *pad = SDL_OpenGamepad(ev->gdevice.which);
        if (pad)
            LOGI(SUB_INPUT, "gamepad connected: %s", SDL_GetGamepadName(pad));
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ev->gbutton.which);
        g->pad_used = true;
        if (pad)
            g->pad_type = SDL_GetGamepadType(pad);
        switch (ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: move_sel(g, -1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move_sel(g, 1); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: press(g, BTN_PLAY); break;
        case SDL_GAMEPAD_BUTTON_NORTH: press(g, BTN_RESCAN); break;
        case SDL_GAMEPAD_BUTTON_WEST: press(g, BTN_CHOOSE); break;
        case SDL_GAMEPAD_BUTTON_EAST: g->running = false; break;
        default: break;
        }
        break;
    }
    default:
        break;
    }
}

int launcher_gui(char *play_cue, size_t cap)
{
    static gui g; /* large (config); keep off the stack */
    memset(&g, 0, sizeof g);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGE(SUB_LAUNCHER, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    /* opens at the canvas size (800x533); the user can resize it, the canvas stays letterboxed */
    if (!SDL_CreateWindowAndRenderer("SBPC", VIEW_W, VIEW_H, SDL_WINDOW_RESIZABLE, &g.win, &g.ren)) {
        LOGE(SUB_LAUNCHER, "could not create window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    ui_set_window_icon(g.win);
    SDL_SetRenderLogicalPresentation(g.ren, VIEW_W, VIEW_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(g.ren, 1);
    g.font = ui_font_create(g.ren);
    if (!g.font)
        LOGW(SUB_LAUNCHER, "font texture failed, using SDL's debug font: %s", SDL_GetError());

    config_load(&g.cfg, launcher_config_path());
    {   const char *base = SDL_GetBasePath(); /* ends with a separator */
        size_t n = base ? strlen(base) : 0;
        const char *appimage = SDL_getenv("APPIMAGE");   /* Linux AppImage: the .AppImage file's own path */
        if (paths_games_dir()) {   /* the repository layout: SBPC/games */
            snprintf(g.base_dir, sizeof g.base_dir, "%s", paths_games_dir());
        } else if (appimage && *appimage && fs_dirname(g.base_dir, appimage)) {
            /* the program itself runs from the AppImage's read-only mount: use the folder the file is in */
        } else if (n > 1 && n < sizeof g.base_dir) {
            memcpy(g.base_dir, base, n + 1);
            if (g.base_dir[n - 1] == '/' || g.base_dir[n - 1] == '\\')
                g.base_dir[n - 1] = 0;
        } else {
            snprintf(g.base_dir, sizeof g.base_dir, ".");
        }
    }
    g.running = true;
    start_scan(&g);

    while (g.running) {
        SDL_Event ev;
        int dlg;
        while (SDL_PollEvent(&ev))
            on_event(&g, &ev);

        dlg = SDL_GetAtomicInt(&g_dialog);
        if (dlg == 2) {
            SDL_SetAtomicInt(&g_dialog, 0);
            set_games_dir(&g, g_dialog_path);
        } else if (dlg == 3) {
            SDL_SetAtomicInt(&g_dialog, 0);
        }

        poll_job(&g);
        if (g.autoplay) { g.autoplay = false; play_selected(&g); }
        draw(&g);
    }

    if (g.job && g.job_kind == JOB_SCAN) {
        /* playing (or quitting) during the folder scan: let it finish on its own; it only reads files and
           writes into this static state, and skips its remaining extra work (wallpapers) */
        SDL_SetAtomicInt(&g.cancel, 1);
        SDL_DetachThread(g.job);
        g.job = NULL;
    } else if (g.job) {   /* quit during a build: it can't be interrupted, so it finishes out of sight */
        SDL_HideWindow(g.win);
        SDL_WaitThread(g.job, NULL);
    }

    if (play_cue && cap) snprintf(play_cue, cap, "%s", g.play_cue);
    scan_free(&g.games);
    free(g.ready);
    if (g.art)
        SDL_DestroyTexture(g.art);
    if (g.font)
        SDL_DestroyTexture(g.font);
    SDL_DestroyRenderer(g.ren);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    return 0;
}
