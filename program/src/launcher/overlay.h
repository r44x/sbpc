/* In-game overlay (F1 or L3+R3): menu state and live settings. No SDL here, so the logic can be
 * tested; play.c draws it and feeds it keys. The game is paused while it's open. */
#ifndef SBPC_LAUNCHER_OVERLAY_H
#define SBPC_LAUNCHER_OVERLAY_H

#include <stdbool.h>

#include "core/config.h"

typedef enum { OV_UP, OV_DOWN, OV_LEFT, OV_RIGHT, OV_OK, OV_BACK } ov_key;
typedef enum { OV_PAGE_MAIN, OV_PAGE_VIDEO, OV_PAGE_AUDIO, OV_PAGE_INPUT, OV_PAGE_INFO } ov_page;
typedef enum { SCALE_SHARP, SCALE_SMOOTH, SCALE_INTEGER, SCALE_STRETCH, SCALE_SHARP_BILINEAR, SCALE_COUNT } scale_mode;

typedef struct {
    bool fullscreen, vsync;
    bool widescreen;             /* experimental: 16:9 by drawing more of the background and sprites */
    int scale;                   /* scale_mode */
    int master, music, sfx;      /* volume, 0-100 */
    int ports;                   /* controller layout: PORTS_* (saturn.h) */
} play_settings;

/* Saturn buttons in rebinding order. */
enum { SB_UP, SB_DOWN, SB_LEFT, SB_RIGHT, SB_A, SB_B, SB_C, SB_X, SB_Y, SB_Z, SB_L, SB_R, SB_START, SB_COUNT };
extern const char *const SATURN_BUTTON_NAMES[SB_COUNT];   /* "Up", "A", "Start", ... */
extern const char *const SATURN_BUTTON_KEYS[SB_COUNT];    /* ini key suffixes: "up", "a", "start", ... */

#define OV_MAX_ITEMS 16
#define OV_TEXT 80

typedef struct {
    bool open;
    bool quit;                   /* "Quit game" chosen: SBPC ends */
    bool changed;                /* settings changed: apply and save them */
    ov_page page;
    int sel;
    play_settings set;
    char info[10][OV_TEXT];      /* Info page lines, filled by the caller */
    int ninfo;
    char players[12][OV_TEXT];   /* Input page: who plays which player, filled by the caller */
    int nplayers;
    int rebind;                  /* 0, or 1 = keyboard / 2 = gamepads being rebound */
    int rebind_step;             /* SB_* button waiting for an input */
    bool reset_bindings;         /* "Reset bindings" chosen: the caller restores the defaults */
} overlay;

void settings_defaults(play_settings *s);
void settings_load(play_settings *s, const config *c);
void settings_store(const play_settings *s, config *c);
const char *scale_name(int mode);

void overlay_init(overlay *o, const play_settings *s);
void overlay_toggle(overlay *o);
void overlay_key(overlay *o, ov_key k);
/* Labels of the current page; *title gets the page title. Returns the item count. Lines before
 * *first_sel are information, not choices. */
int overlay_items(const overlay *o, char items[OV_MAX_ITEMS][OV_TEXT], const char **title, int *first_sel);
/* While rebinding: the caller captured an input for o->rebind_step (or skipped it); moves on, and ends
 * rebinding (setting changed) after the last button. */
void overlay_rebind_next(overlay *o);

#endif
