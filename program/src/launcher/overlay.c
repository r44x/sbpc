#include "launcher/overlay.h"
#include "saturn/saturn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const SATURN_BUTTON_NAMES[SB_COUNT] = {"Up", "Down", "Left", "Right", "A", "B", "C",
                                                    "X", "Y", "Z", "L", "R", "Start"};
const char *const SATURN_BUTTON_KEYS[SB_COUNT] = {"up", "down", "left", "right", "a", "b", "c",
                                                   "x", "y", "z", "l", "r", "start"};

static const char *const SCALE_NAMES[SCALE_COUNT] = {"Sharp", "Smooth", "Integer", "Stretch", "Sharp bilinear"};

const char *scale_name(int mode) { return mode >= 0 && mode < SCALE_COUNT ? SCALE_NAMES[mode] : "?"; }

void settings_defaults(play_settings *s)
{
    s->fullscreen = false;
    s->vsync = true;
    s->widescreen = false;
    s->scale = SCALE_SHARP;
    s->master = s->music = s->sfx = 100;
    s->ports = PORTS_AUTO;
}

static int get_int(const config *c, const char *key, int fallback, int lo, int hi)
{
    const char *v = config_get(c, key, NULL);
    int n;
    if (!v) return fallback;
    n = atoi(v);
    return n < lo ? lo : n > hi ? hi : n;
}

void settings_load(play_settings *s, const config *c)
{
    settings_defaults(s);
    s->fullscreen = get_int(c, "video_fullscreen", 0, 0, 1) != 0;
    s->vsync = get_int(c, "video_vsync", 1, 0, 1) != 0;
    s->widescreen = get_int(c, "video_widescreen", 0, 0, 1) != 0;
    s->scale = get_int(c, "video_scale", SCALE_SHARP, 0, SCALE_COUNT - 1);
    s->master = get_int(c, "audio_master", 100, 0, 100);
    s->music = get_int(c, "audio_music", 100, 0, 100);
    s->sfx = get_int(c, "audio_sfx", 100, 0, 100);
    s->ports = get_int(c, "input_ports", PORTS_AUTO, 0, PORTS_COUNT - 1);
}

void settings_store(const play_settings *s, config *c)
{
    char v[16];
#define PUT(key, val) do { snprintf(v, sizeof v, "%d", (int)(val)); config_set(c, key, v); } while (0)
    PUT("video_fullscreen", s->fullscreen);
    PUT("video_vsync", s->vsync);
    PUT("video_widescreen", s->widescreen);
    PUT("video_scale", s->scale);
    PUT("audio_master", s->master);
    PUT("audio_music", s->music);
    PUT("audio_sfx", s->sfx);
    PUT("input_ports", s->ports);
#undef PUT
}

void overlay_init(overlay *o, const play_settings *s)
{
    memset(o, 0, sizeof *o);
    o->set = *s;
}

void overlay_toggle(overlay *o)
{
    o->open = !o->open;
    o->page = OV_PAGE_MAIN;
    o->sel = 0;
    o->rebind = 0;
}

void overlay_rebind_next(overlay *o)
{
    if (!o->rebind)
        return;
    if (++o->rebind_step >= SB_COUNT) {
        o->rebind = 0;
        o->changed = true;
    }
}

int overlay_items(const overlay *o, char items[OV_MAX_ITEMS][OV_TEXT], const char **title, int *first_sel)
{
    int n = 0;
    *first_sel = 0;
    switch (o->page) {
    case OV_PAGE_MAIN:
        *title = "Paused";
        snprintf(items[n++], OV_TEXT, "Resume");
        snprintf(items[n++], OV_TEXT, "Video");
        snprintf(items[n++], OV_TEXT, "Audio");
        snprintf(items[n++], OV_TEXT, "Input");
        snprintf(items[n++], OV_TEXT, "Info");
        snprintf(items[n++], OV_TEXT, "Quit game");
        break;
    case OV_PAGE_VIDEO:
        *title = "Video";
        snprintf(items[n++], OV_TEXT, "%-14s< %s >", "Fullscreen", o->set.fullscreen ? "On" : "Off");
        snprintf(items[n++], OV_TEXT, "%-14s< %s >", "Scaling", scale_name(o->set.scale));
        snprintf(items[n++], OV_TEXT, "%-14s< %s >", "VSync", o->set.vsync ? "On" : "Off");
        snprintf(items[n++], OV_TEXT, "%-14s< %s >", "Ratio", o->set.widescreen ? "16:9 (experimental)" : "4:3");
        snprintf(items[n++], OV_TEXT, "Back");
        break;
    case OV_PAGE_AUDIO:
        *title = "Audio";
        snprintf(items[n++], OV_TEXT, "%-14s< %3d%% >", "Master", o->set.master);
        snprintf(items[n++], OV_TEXT, "%-14s< %3d%% >", "Music", o->set.music);
        snprintf(items[n++], OV_TEXT, "%-14s< %3d%% >", "Sound effects", o->set.sfx);
        snprintf(items[n++], OV_TEXT, "Back");
        break;
    case OV_PAGE_INPUT:
        if (o->rebind) {
            *title = o->rebind == 1 ? "Rebind keyboard" : "Rebind gamepads";
            snprintf(items[n++], OV_TEXT, "Press the %s for Saturn %s", o->rebind == 1 ? "key" : "button",
                     SATURN_BUTTON_NAMES[o->rebind_step]);
            snprintf(items[n++], OV_TEXT, "(%d of %d)", o->rebind_step + 1, SB_COUNT);
            snprintf(items[n++], OV_TEXT, "%s keeps the current one", o->rebind == 1 ? "Esc" : "Back/Select");
            *first_sel = n;
            snprintf(items[n++], OV_TEXT, "Cancel");
            break;
        }
        *title = "Input";
        for (int i = 0; i < o->nplayers && n < OV_MAX_ITEMS - 5; i++)
            snprintf(items[n++], OV_TEXT, "%s", o->players[i]);
        *first_sel = n;
        snprintf(items[n++], OV_TEXT, "%-14s< %s >", "Ports", smpc_layout_name(o->set.ports));
        snprintf(items[n++], OV_TEXT, "Rebind keyboard");
        snprintf(items[n++], OV_TEXT, "Rebind gamepads");
        snprintf(items[n++], OV_TEXT, "Reset bindings");
        snprintf(items[n++], OV_TEXT, "Back");
        break;
    case OV_PAGE_INFO:
        *title = "Info";
        for (int i = 0; i < o->ninfo && n < OV_MAX_ITEMS - 1; i++)
            snprintf(items[n++], OV_TEXT, "%s", o->info[i]);
        *first_sel = n;
        snprintf(items[n++], OV_TEXT, "Back");
        break;
    }
    return n;
}

static int clamp_vol(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

/* Left/right (or OK) on a setting line. */
static void adjust(overlay *o, int dir)
{
    play_settings *s = &o->set;
    if (o->page == OV_PAGE_VIDEO) {
        switch (o->sel) {
        case 0: s->fullscreen = !s->fullscreen; break;
        case 1: s->scale = (s->scale + dir + SCALE_COUNT) % SCALE_COUNT; break;
        case 2: s->vsync = !s->vsync; break;
        case 3: s->widescreen = !s->widescreen; break;
        default: return;
        }
    } else if (o->page == OV_PAGE_AUDIO) {
        int *v = o->sel == 0 ? &s->master : o->sel == 1 ? &s->music : o->sel == 2 ? &s->sfx : NULL;
        if (!v) return;
        *v = clamp_vol(*v + dir * 10);
    } else if (o->page == OV_PAGE_INPUT && !o->rebind && o->sel == o->nplayers) {
        s->ports = (s->ports + dir + PORTS_COUNT) % PORTS_COUNT;
    } else {
        return;
    }
    o->changed = true;
}

static void back(overlay *o)
{
    if (o->rebind) {
        o->rebind = 0;               /* cancel: bindings captured so far stay until saved by a finish */
        return;
    }
    if (o->page == OV_PAGE_MAIN) {
        o->open = false;
    } else {
        o->sel = (int)o->page;       /* its line in the main page: Video 1, Audio 2, Input 3, Info 4 */
        o->page = OV_PAGE_MAIN;
    }
}

void overlay_key(overlay *o, ov_key k)
{
    char items[OV_MAX_ITEMS][OV_TEXT];
    const char *title;
    int first, n;
    if (!o->open)
        return;
    n = overlay_items(o, items, &title, &first);
    if (o->sel < first) o->sel = first;
    switch (k) {
    case OV_UP: o->sel = o->sel > first ? o->sel - 1 : n - 1; break;
    case OV_DOWN: o->sel = o->sel < n - 1 ? o->sel + 1 : first; break;
    case OV_LEFT: adjust(o, -1); break;
    case OV_RIGHT: adjust(o, 1); break;
    case OV_BACK: back(o); break;
    case OV_OK:
        if (o->sel == n - 1 && o->page != OV_PAGE_MAIN) { back(o); break; }
        if (o->page == OV_PAGE_MAIN) {
            switch (o->sel) {
            case 0: o->open = false; break;
            case 1: o->page = OV_PAGE_VIDEO; o->sel = 0; break;
            case 2: o->page = OV_PAGE_AUDIO; o->sel = 0; break;
            case 3: o->page = OV_PAGE_INPUT; o->sel = o->nplayers; break;   /* on Ports */
            case 4: o->page = OV_PAGE_INFO; o->sel = o->ninfo < OV_MAX_ITEMS - 1 ? o->ninfo : OV_MAX_ITEMS - 1; break;  /* on Back */
            default: o->quit = true; o->open = false; break;
            }
        } else if (o->page == OV_PAGE_INPUT && !o->rebind && o->sel > o->nplayers) {
            int act = o->sel - o->nplayers;   /* 1 rebind keyboard, 2 rebind gamepads, 3 reset */
            if (act == 1 || act == 2) { o->rebind = act; o->rebind_step = 0; o->sel = 3; }
            else if (act == 3) { o->reset_bindings = true; o->changed = true; }
        } else {
            adjust(o, 1);
        }
        break;
    }
}
