/* --play: boots the game and shows it in a window. Player 1 = keyboard + first gamepad, player N =
 * gamepad N, up to the game's limit (10) through two multitaps (input.c, smpc_layout).
 * F1 or L3+R3 opens the overlay (pause, video/audio settings, info, return to launcher). */
#include "launcher/play.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "disc/disc.h"
#include "disc/identify.h"
#include "disc/iso9660.h"
#include "launcher/launcher.h"
#include "launcher/overlay.h"
#include "launcher/input.h"
#include "launcher/scan.h"
#include "launcher/ui_font.h"
#include "saturn/saturn.h"
#include "core/png.h"
#include "core/fs.h"
#include "saturn/saturn_rec.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- overlay drawing: the launcher's look (ui_font.h) ---- */

/* Footer keys: label, keyboard key, pad button (shown once a pad was used). */
static const struct { const char *label, *key, *pad; } OV_KEYS[3] = {
    {"OK", "Enter", "A"}, {"Back", "Esc", "B"}, {"Close", "F1", "L3+R3"}};

static void draw_overlay(SDL_Renderer *ren, SDL_Texture *font, const overlay *o, bool pad)
{
    char items[OV_MAX_ITEMS][OV_TEXT];
    const char *title;
    int first, n = overlay_items(o, items, &title, &first), ow, oh, sel = o->sel < first ? first : o->sel;
    float sc, w = 0, h, x0, y0, foot_w = 0, x, y;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    sc = (float)(oh / 400 > 1 ? oh / 400 : 1);      /* whole steps, like the launcher, so the font stays crisp */
    for (int i = 0; i < n; i++) {
        float lw = (float)(ui_text_len(items[i]) * UI_CH);
        if (lw > w) w = lw;
    }
    for (int k = 0; k < 3; k++)
        foot_w += (float)((ui_text_len(pad ? OV_KEYS[k].pad : OV_KEYS[k].key) + ui_text_len(OV_KEYS[k].label)) * UI_CH + 14 + 16);
    if (w < foot_w - 16) w = foot_w - 16;
    if (w < (float)(ui_text_len(title) * UI_CH)) w = (float)(ui_text_len(title) * UI_CH);
    w += 2 * 14;
    h = 34 + (float)n * UI_ROW_H + 12 + 1 + 10 + UI_BOX_H + 10;
    x0 = (float)(int)(((float)ow / sc - w) / 2);
    y0 = (float)(int)(((float)oh / sc - h) / 2);
    SDL_SetRenderScale(ren, sc, sc);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);                          /* dim the game */
    SDL_RenderFillRect(ren, NULL);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    ui_fill(ren, UI_BLACK, x0, y0, w, h);
    SDL_SetRenderDrawColor(ren, UI_GREEN.r, UI_GREEN.g, UI_GREEN.b, 255);
    SDL_RenderRect(ren, &(SDL_FRect){x0, y0, w, h});
    ui_text(ren, font, UI_GREEN, x0 + 14, y0 + 10, title);
    ui_fill(ren, UI_GREEN, x0, y0 + 27, w, 1);
    y = y0 + 34;
    for (int i = 0; i < n; i++, y += UI_ROW_H) {
        if (i >= first && i == sel) {
            ui_fill(ren, UI_GREEN, x0 + 6, y, w - 12, UI_ROW_H);
            ui_text(ren, font, UI_BLACK, x0 + 14, y + UI_ROW_TEXT, items[i]);
        } else {
            ui_text(ren, font, i < first ? UI_GREY : UI_WHITE, x0 + 14, y + UI_ROW_TEXT, items[i]);
        }
    }
    y += 12;
    ui_fill(ren, UI_GREEN, x0, y, w, 1);
    y += 10;
    x = x0 + 10;
    for (int k = 0; k < 3; k++)
        x += ui_key(ren, font, x, y, pad ? OV_KEYS[k].pad : OV_KEYS[k].key, OV_KEYS[k].label, false) + 16;
    SDL_SetRenderScale(ren, 1, 1);
}

/* Applies the overlay's settings to the window, renderer, audio and the machine. */
static void apply_settings(const play_settings *s, SDL_Window *win, SDL_Renderer *ren, SDL_AudioStream *audio, saturn *m)
{
    SDL_SetWindowFullscreen(win, s->fullscreen);
    SDL_SetRenderVSync(ren, s->vsync ? 1 : 0);
    if (audio) SDL_SetAudioStreamGain(audio, (float)s->master / 100.0f);
    m->scsp.gain_music = s->music * 256 / 100;
    m->wide_extra = s->widescreen ? 53 : 0;   /* 320 + 2 * 53 = 426 wide: 16:9 */
    m->scsp.gain_sfx = s->sfx * 256 / 100;
}

static bool find_cue(const char *what, char *out, size_t cap)
{
    size_t n = strlen(what);
    config cfg;
    scan_result r;
    const char *dir;
    bool found = false;
    if (n > 4 && disc_is_image_name(what)) {   /* a .cue or .chd path (else a version id) */
        snprintf(out, cap, "%s", what);
        return true;
    }
    config_init(&cfg);
    config_load(&cfg, launcher_config_path());
    dir = config_get(&cfg, "games_dir", paths_games_dir());   /* default: SBPC/games */
    if (!dir) {
        fprintf(stderr, "no games folder set: run 'sbpc --cli set-games-folder <folder>' or use the launcher\n");
        return false;
    }
    if (!scan_folder(dir, &r, NULL, NULL))
        return false;
    for (int i = 0; i < r.count && !found; i++) {
        const scan_entry *e = &r.items[i];
        if (e->id.status == ID_SUPPORTED && e->id.version && strcmp(e->id.version->id, what) == 0) {
            snprintf(out, cap, "%s", e->cue_path);
            found = true;
        }
    }
    scan_free(&r);
    if (!found)
        fprintf(stderr, "no supported '%s' disc found in %s\n", what, dir);
    return found;
}

int play_run(const char *what)
{
    char cue[SBPC_PATH_MAX], err[256];
    disc d;
    iso_fs fs;
    disc_identity id;
    saturn *m;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL, *pre = NULL;   /* pre: integer-scaled copy for "Sharp bilinear" */
    SDL_Texture *font = NULL;               /* the overlay's pixel font */
    bool pad_used = false;                  /* footer prompts follow the last input used */
    int pre_w = 0, pre_h = 0;
    SDL_AudioStream *audio = NULL;
    input_state in;
    int slot[SMPC_SLOTS], layout_players = -1, layout_mode = -1;
    static uint32_t pix[704 * 512];
    int tw = 0, th = 0, track, rc = 1;
    bool running = true;
    config cfg;
    overlay ov;
    uint64_t frame_ns = 0, stat_at = 0, stat_native = 0, stat_interp = 0;
    double native_share = 0;

    if (!find_cue(what, cue, sizeof cue))
        return 1;
    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED) {
        fprintf(stderr, "%s\n", id.reason[0] ? id.reason : "unsupported disc");
        return 1;
    }
    if (!disc_open(&d, cue, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    track = disc_first_data_track(&d);
    if (track < 0 || !iso_open(&fs, &d, track, err, sizeof err)) {
        fprintf(stderr, "%s\n", track < 0 ? "no data track" : err);
        disc_close(&d);
        return 1;
    }
    m = saturn_create();
    if (!m) { fprintf(stderr, "out of memory\n"); return 1; }
    m->version = id.version;
    /* No save file: backup memory stays in memory. The game has no save feature and only reads it. */
    if (!saturn_boot(m, &fs, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        goto done;
    }
    if (!launcher_load_modules(m, &id))
        LOGW(SUB_CORE, "no recompiled code loaded: the game runs on the interpreter");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        goto done;
    }
    if (!SDL_CreateWindowAndRenderer(id.version->name, 960, 720, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "window failed: %s\n", SDL_GetError());
        goto quit;
    }
    ui_set_window_icon(win);
    SDL_SetRenderVSync(ren, 1);
    font = ui_font_create(ren);
    {   /* the SCSP mixes 44.1 kHz stereo into a ring buffer; we feed it to the audio device every frame */
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (audio) SDL_ResumeAudioStreamDevice(audio);
        else LOGW(SUB_CORE, "no audio: %s", SDL_GetError());
        m->scsp.out_r = m->scsp.out_w;
    }
    config_init(&cfg);
    config_load(&cfg, launcher_config_path());
    input_load_mappings();
    input_init(&in, &cfg);
    in.max_players = id.version->max_players;
    {   /* gamepads already plugged in, in SDL's order (players 1, 2, ...) */
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n; i++) input_pad_added(&in, ids[i]);
        SDL_free(ids);
    }
    {
        play_settings s;
        settings_load(&s, &cfg);
        overlay_init(&ov, &s);
        if (s.widescreen && !s.fullscreen)
            SDL_SetWindowSize(win, 1280, 720);   /* a 16:9 window for a 16:9 picture */
        apply_settings(&ov.set, win, ren, audio, m);
    }
    LOGI(SUB_CORE, "playing %s", id.version->name);

    input_rec rec = {0};
    {   /* the session's pad input, frame by frame, for replaying a problem headless (SBPC_REPLAY) */
        char path[SBPC_PATH_MAX], old[SBPC_PATH_MAX];
        if (fs_join(path, paths_log_dir(), "input.rec") && fs_join(old, paths_log_dir(), "input.rec.1")) {
            if (fs_is_file(path))
                fs_rename(path, old);
            if (!rec_create(&rec, path, m->rtc_start))
                LOGW(SUB_CORE, "can't record input to %s", path);
        }
    }
    uint64_t next = SDL_GetTicksNS();
    while (running) {
        SDL_Event ev;
        bool was_open = ov.open;
        while (SDL_PollEvent(&ev)) {
            if (ov.open && ov.rebind == 1 && ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
                ev.key.key != SDLK_F1) {             /* rebinding the keyboard: this key, Esc keeps the old one */
                if (ev.key.key != SDLK_ESCAPE) in.key[ov.rebind_step] = ev.key.scancode;
                overlay_rebind_next(&ov);
                continue;
            }
            if (ov.open && ov.rebind == 2) {         /* rebinding gamepads: this input, Back keeps the old one */
                int code = input_pad_code(&ev);
                if (code == SDL_GAMEPAD_BUTTON_BACK) { overlay_rebind_next(&ov); continue; }
                if (code >= 0) { in.pad[ov.rebind_step] = code; overlay_rebind_next(&ov); continue; }
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) pad_used = false;
            else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pad_used = true;
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                /* Esc alone never opens the overlay or quits: Steam Input maps controller B to Esc */
                SDL_Keycode k = ev.key.key;
                if (k == SDLK_F1) overlay_toggle(&ov);
                else if (k == SDLK_F11) { ov.set.fullscreen = !ov.set.fullscreen; ov.changed = true; }
                else if (ov.open) {
                    if (k == SDLK_UP) overlay_key(&ov, OV_UP);
                    else if (k == SDLK_DOWN) overlay_key(&ov, OV_DOWN);
                    else if (k == SDLK_LEFT) overlay_key(&ov, OV_LEFT);
                    else if (k == SDLK_RIGHT) overlay_key(&ov, OV_RIGHT);
                    else if (k == SDLK_RETURN || k == SDLK_Z) overlay_key(&ov, OV_OK);
                    else if (k == SDLK_ESCAPE || k == SDLK_X || k == SDLK_BACKSPACE) overlay_key(&ov, OV_BACK);
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.repeat && ov.open) {
                if (ev.key.key == SDLK_UP) overlay_key(&ov, OV_UP);
                else if (ev.key.key == SDLK_DOWN) overlay_key(&ov, OV_DOWN);
            } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                SDL_Gamepad *g = SDL_GetGamepadFromID(ev.gbutton.which);
                int b = ev.gbutton.button;
                if ((b == SDL_GAMEPAD_BUTTON_LEFT_STICK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) && g &&
                    SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                    SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                    overlay_toggle(&ov);
                } else if (ov.open) {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) overlay_key(&ov, OV_UP);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) overlay_key(&ov, OV_DOWN);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) overlay_key(&ov, OV_LEFT);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) overlay_key(&ov, OV_RIGHT);
                    else if (b == SDL_GAMEPAD_BUTTON_SOUTH) overlay_key(&ov, OV_OK);
                    else if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START) overlay_key(&ov, OV_BACK);
                }
            } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
                input_pad_added(&in, ev.gdevice.which);
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                input_pad_removed(&in, ev.gdevice.which);
            }
        }
        if (input_players(&in) != layout_players || ov.set.ports != layout_mode) {
            /* plug the right number of pads (and multitaps) into the emulated ports */
            int placed;
            layout_players = input_players(&in);
            layout_mode = ov.set.ports;
            placed = smpc_layout(m, layout_players, layout_mode, slot);
            for (int i = 0; i < SMPC_SLOTS; i++) m->smpc.pad[i] = 0xFFFF;
            LOGI(SUB_INPUT, "%d player(s), ports: %s%s", placed, smpc_layout_name(layout_mode),
                 placed < layout_players ? " (not enough ports for everyone)" : "");
        }
        if (ov.changed) {
            ov.changed = false;
            if (ov.reset_bindings) { ov.reset_bindings = false; input_defaults(&in); }
            apply_settings(&ov.set, win, ren, audio, m);
            settings_store(&ov.set, &cfg);
            input_store(&in, &cfg);
            if (!config_save(&cfg, launcher_config_path()))
                LOGW(SUB_CORE, "could not save settings to %s", launcher_config_path());
        }
        if (ov.quit) {
            running = false;
            break;
        }
        if (ov.open != was_open && audio) {   /* the game is paused while the overlay is open */
            if (ov.open) SDL_PauseAudioStreamDevice(audio);
            else { SDL_ClearAudioStream(audio); SDL_ResumeAudioStreamDevice(audio); }
        }
        if (ov.open) {
            ov.ninfo = 0;
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "SBPC %s", SBPC_VERSION_STRING);
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "%s %s", id.version->name, id.version->version);
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "Recompiled entry points: %d", m->dispatch.count);
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "Recompiled share (last s): %.1f%%", native_share);
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "Frame time: %.2f ms", (double)frame_ns / 1e6);
            snprintf(ov.info[ov.ninfo++], OV_TEXT, "Frame: %llu", (unsigned long long)m->frame);
            ov.nplayers = 0;
            for (int i = 0; i < in.npads + (in.npads ? 0 : 1) && i < 12; i++) {
                const char *pad_name = i < in.npads ? SDL_GetGamepadName(in.pads[i]) : NULL;
                char dev[64];
                if (i == 0) snprintf(dev, sizeof dev, "Keyboard%s%.40s", pad_name ? " + " : "", pad_name ? pad_name : "");
                else snprintf(dev, sizeof dev, "%.50s", pad_name ? pad_name : "?");
                if (i >= input_players(&in))   /* more pads than the game has players */
                    snprintf(ov.players[ov.nplayers++], OV_TEXT, "--  %s (unused: %d players max)", dev, in.max_players);
                else
                    snprintf(ov.players[ov.nplayers++], OV_TEXT, "P%-2d %s%s", i + 1, dev, slot[i] < 0 ? " (no port)" : "");
            }
        }
        if (!ov.open) {
            uint64_t t0 = SDL_GetTicksNS();
            for (int pl = 0; pl < input_players(&in) && pl < SMPC_SLOTS; pl++)
                if (slot[pl] >= 0)   /* active low; the three low bits read 1 */
                    m->smpc.pad[slot[pl]] = (uint16_t)~input_read(&in, pl) | 0x0007;
            if (rec.f) {
                rec_frame fr;
                fr.players = (uint8_t)layout_players;
                fr.ports = (uint8_t)layout_mode;
                memcpy(fr.pad, m->smpc.pad, sizeof fr.pad);
                rec_write(&rec, &fr);
            }
            saturn_run_frame(m);
            frame_ns = (frame_ns * 15 + (SDL_GetTicksNS() - t0)) / 16;
            if (SDL_GetTicks() - stat_at >= 1000) {   /* share of guest work done in recompiled code */
                uint64_t nat = m->dispatch.native_calls - stat_native, itp = m->dispatch.interp_steps - stat_interp;
                /* a native entry runs a whole function: weigh it as ~20 instructions for this rough figure */
                native_share = nat + itp ? 100.0 * (double)(nat * 20) / (double)(nat * 20 + itp) : 0;
                stat_native = m->dispatch.native_calls;
                stat_interp = m->dispatch.interp_steps;
                stat_at = SDL_GetTicks();
            }
        }
        if (audio && !ov.open) {
            scsp_state *sp = &m->scsp;
            /* keep about 100 ms queued at most: drop audio rather than fall behind */
            if (SDL_GetAudioStreamQueued(audio) > 44100 * 4 / 10) sp->out_r = sp->out_w;
            while (sp->out_r != sp->out_w) {
                uint32_t at = sp->out_r % SCSP_OUT_FRAMES;
                uint32_t n = sp->out_w - sp->out_r;
                if (n > SCSP_OUT_FRAMES - at) n = SCSP_OUT_FRAMES - at;
                SDL_PutAudioStreamData(audio, &sp->out[at * 2], (int)(n * 4));
                sp->out_r += n;
            }
        } else {
            m->scsp.out_r = m->scsp.out_w;
        }
        if (ov.open)
            m->scsp.out_r = m->scsp.out_w;
        {
            int w, h;
            saturn_render(m, pix, &w, &h);
            if (!tex || w != tw || h != th) {
                if (pre) SDL_DestroyTexture(pre);
    if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                if (!tex) { fprintf(stderr, "texture failed: %s\n", SDL_GetError()); break; }
                tw = w; th = h;
            }
            SDL_SetTextureScaleMode(tex, ov.set.scale == SCALE_SMOOTH ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            /* the picture's shape: a 320/352-wide frame fills a 4:3 TV; widescreen frames are wider */
            float aspect = 4.0f / 3.0f * (float)w / (float)(w - 2 * m->render_extra);
            SDL_UpdateTexture(tex, NULL, pix, w * 4);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            {   /* 4:3 picture (all Saturn modes fill a 4:3 TV), sized by the scaling setting */
                int ow, oh;
                SDL_FRect dst;
                SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
                if (ov.set.scale == SCALE_STRETCH) {
                    dst = (SDL_FRect){0, 0, (float)ow, (float)oh};
                } else {
                    dst.w = (float)ow;
                    dst.h = (float)ow / aspect;
                    if (dst.h > (float)oh) { dst.h = (float)oh; dst.w = (float)oh * aspect; }
                    if (ov.set.scale == SCALE_INTEGER) {   /* whole multiples of the Saturn's line count */
                        int k = (int)dst.h / h;
                        if (k >= 1) { dst.h = (float)(k * h); dst.w = dst.h * aspect; }
                    }
                    dst.x = ((float)ow - dst.w) / 2;
                    dst.y = ((float)oh - dst.h) / 2;
                }
                if (ov.set.scale == SCALE_SHARP_BILINEAR) {
                    /* GPU "sharp bilinear": scale up by whole numbers with hard pixels, then smooth only
                     * the last fractional step, so pixels stay crisp and even */
                    int kx = (int)(dst.w / (float)w), ky = (int)(dst.h / (float)h);
                    if (kx < 1) kx = 1;
                    if (ky < 1) ky = 1;
                    if (!pre || pre_w != w * kx || pre_h != h * ky) {
                        if (pre) SDL_DestroyTexture(pre);
                        pre = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w * kx, h * ky);
                        if (pre) SDL_SetTextureScaleMode(pre, SDL_SCALEMODE_LINEAR);
                        pre_w = w * kx; pre_h = h * ky;
                    }
                    if (pre && SDL_SetRenderTarget(ren, pre)) {
                        SDL_RenderTexture(ren, tex, NULL, NULL);
                        SDL_SetRenderTarget(ren, NULL);
                        SDL_RenderTexture(ren, pre, NULL, &dst);
                    } else {
                        SDL_RenderTexture(ren, tex, NULL, &dst);
                    }
                } else {
                    SDL_RenderTexture(ren, tex, NULL, &dst);
                }
            }
            if (ov.open)
                draw_overlay(ren, font, &ov, pad_used);
            {   /* developer: SBPC_PLAY_SHOT=<frame> opens the overlay there (page from SBPC_PLAY_SHOT_PAGE:
                 * video, audio, input, info; none = no overlay), saves the window to <logs>/play_shot.png and quits */
                static long shot_at = -2;
                if (shot_at == -2) shot_at = getenv("SBPC_PLAY_SHOT") ? atol(getenv("SBPC_PLAY_SHOT")) : -1;
                const char *pg = getenv("SBPC_PLAY_SHOT_PAGE");
                bool bare = pg && strcmp(pg, "none") == 0;   /* "none": the game picture without the overlay */
                if (shot_at >= 0 && (long)m->frame >= shot_at && !ov.open && !bare) {
                    overlay_toggle(&ov);
                    if (pg) {
                        ov.sel = strcmp(pg, "video") == 0 ? 1 : strcmp(pg, "audio") == 0 ? 2 : strcmp(pg, "input") == 0 ? 3 : 4;
                        overlay_key(&ov, OV_OK);
                    }
                } else if (shot_at >= 0 && (ov.open || (bare && (long)m->frame >= shot_at))) {
                    SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
                    SDL_Surface *cv = sf ? SDL_ConvertSurface(sf, SDL_PIXELFORMAT_ARGB8888) : NULL;
                    char path[SBPC_PATH_MAX];
                    if (cv && fs_join(path, paths_log_dir(), "play_shot.png")) {
                        static uint32_t buf[1920 * 1080];
                        int pw = cv->w < 1920 ? cv->w : 1920, ph = cv->h < 1080 ? cv->h : 1080;
                        for (int yy = 0; yy < ph; yy++)
                            memcpy(buf + (size_t)yy * (size_t)pw, (uint8_t *)cv->pixels + (size_t)yy * (size_t)cv->pitch, (size_t)pw * 4);
                        LOGI(SUB_CORE, "play shot %s: %s", path, png_write(path, buf, pw, ph) ? "saved" : "failed");
                    }
                    SDL_DestroySurface(cv);
                    SDL_DestroySurface(sf);
                    running = false;
                }
            }
            SDL_RenderPresent(ren);
        }
        /* pace to 60 Hz when vsync isn't doing it */
        next += m->pal ? 20000000ull : 16666667ull;   /* 50 Hz on PAL */
        {
            uint64_t now = SDL_GetTicksNS();
            if (next > now) SDL_DelayNS(next - now);
            else next = now;
        }
    }
    rc = 0;
    rec_close(&rec);
    if (font) SDL_DestroyTexture(font);
    if (pre) SDL_DestroyTexture(pre);
    if (tex) SDL_DestroyTexture(tex);
    input_close(&in);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
quit:
    SDL_Quit();
done:
    saturn_destroy(m);
    iso_close(&fs);
    disc_close(&d);
    return rc;
}
