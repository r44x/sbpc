#include "launcher/input.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Saturn pad bits (active high here; the SMPC wants them inverted), high byte: R L D U Start A C B;
 * low byte: R X Y Z L 1 1 1 */
static const uint16_t SB_BIT[SB_COUNT] = {
    1 << 12, 1 << 13, 1 << 14, 1 << 15,     /* up down left right */
    1 << 10, 1 << 8, 1 << 9,                /* A B C */
    1 << 6, 1 << 5, 1 << 4,                 /* X Y Z */
    1 << 3, 1 << 7,                         /* L R */
    1 << 11                                 /* Start */
};

void input_defaults(input_state *in)
{
    static const SDL_Scancode K[SB_COUNT] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
        SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_RETURN};
    /* Saturn layout on a modern pad: A B C on the bottom and right face buttons and a shoulder, X Y Z
     * on the left and top face buttons and the other shoulder, L R on the triggers. */
    static const int P[SB_COUNT] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_AXIS_POS + SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        PAD_AXIS_POS + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_BUTTON_START};
    memcpy(in->key, K, sizeof K);
    memcpy(in->pad, P, sizeof P);
}

const char *input_pad_code_name(int code, char *buf, size_t cap)
{
    const char *n;
    if (code >= PAD_AXIS_NEG) {
        n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)(code - PAD_AXIS_NEG));
        snprintf(buf, cap, "-%s", n ? n : "?");
    } else if (code >= PAD_AXIS_POS) {
        n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)(code - PAD_AXIS_POS));
        snprintf(buf, cap, "+%s", n ? n : "?");
    } else {
        n = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
        snprintf(buf, cap, "%s", n ? n : "?");
    }
    return buf;
}

static int pad_code_from_name(const char *s)
{
    if (s[0] == '+' || s[0] == '-') {
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s + 1);
        if (a == SDL_GAMEPAD_AXIS_INVALID) return -1;
        return (s[0] == '+' ? PAD_AXIS_POS : PAD_AXIS_NEG) + (int)a;
    }
    {
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s);
        return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
    }
}

void input_init(input_state *in, const config *cfg)
{
    memset(in, 0, sizeof *in);
    input_defaults(in);
    for (int b = 0; b < SB_COUNT; b++) {
        char key[32];
        const char *v;
        snprintf(key, sizeof key, "key_%s", SATURN_BUTTON_KEYS[b]);
        if ((v = config_get(cfg, key, NULL)) != NULL) {
            SDL_Scancode sc = SDL_GetScancodeFromName(v);
            if (sc != SDL_SCANCODE_UNKNOWN) in->key[b] = sc;
        }
        snprintf(key, sizeof key, "pad_%s", SATURN_BUTTON_KEYS[b]);
        if ((v = config_get(cfg, key, NULL)) != NULL) {
            int c = pad_code_from_name(v);
            if (c >= 0) in->pad[b] = c;
        }
    }
}

void input_store(const input_state *in, config *cfg)
{
    for (int b = 0; b < SB_COUNT; b++) {
        char key[32], name[48];
        snprintf(key, sizeof key, "key_%s", SATURN_BUTTON_KEYS[b]);
        config_set(cfg, key, SDL_GetScancodeName(in->key[b]));
        snprintf(key, sizeof key, "pad_%s", SATURN_BUTTON_KEYS[b]);
        config_set(cfg, key, input_pad_code_name(in->pad[b], name, sizeof name));
    }
}

void input_pad_added(input_state *in, SDL_JoystickID id)
{
    SDL_Gamepad *g;
    for (int i = 0; i < in->npads; i++)
        if (SDL_GetGamepadID(in->pads[i]) == id) return;
    if (in->npads >= INPUT_MAX_PADS || !(g = SDL_OpenGamepad(id)))
        return;
    in->pads[in->npads++] = g;
    LOGI(SUB_INPUT, "gamepad connected: %s (player %d)", SDL_GetGamepadName(g), in->npads);
}

void input_pad_removed(input_state *in, SDL_JoystickID id)
{
    for (int i = 0; i < in->npads; i++)
        if (SDL_GetGamepadID(in->pads[i]) == id) {
            LOGI(SUB_INPUT, "gamepad disconnected: player %d", i + 1);
            SDL_CloseGamepad(in->pads[i]);
            memmove(&in->pads[i], &in->pads[i + 1], (size_t)(in->npads - i - 1) * sizeof in->pads[0]);
            in->npads--;
            return;
        }
}

void input_close(input_state *in)
{
    for (int i = 0; i < in->npads; i++) SDL_CloseGamepad(in->pads[i]);
    in->npads = 0;
}

int input_players(const input_state *in)
{
    int n = in->npads > 1 ? in->npads : 1;
    return in->max_players > 0 && n > in->max_players ? in->max_players : n;
}

static bool pad_down(SDL_Gamepad *g, int code)
{
    if (code >= PAD_AXIS_NEG) return SDL_GetGamepadAxis(g, (SDL_GamepadAxis)(code - PAD_AXIS_NEG)) < -16000;
    if (code >= PAD_AXIS_POS) return SDL_GetGamepadAxis(g, (SDL_GamepadAxis)(code - PAD_AXIS_POS)) > 16000;
    return SDL_GetGamepadButton(g, (SDL_GamepadButton)code);
}

uint16_t input_read(const input_state *in, int player)
{
    uint16_t p = 0;
    if (player == 0) {
        const bool *k = SDL_GetKeyboardState(NULL);
        for (int b = 0; b < SB_COUNT; b++)
            if (k[in->key[b]]) p |= SB_BIT[b];
    }
    if (player < in->npads) {
        SDL_Gamepad *g = in->pads[player];
        int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        for (int b = 0; b < SB_COUNT; b++)
            if (pad_down(g, in->pad[b])) p |= SB_BIT[b];
        /* the left stick always works as the d-pad too */
        if (ly < -16000) p |= SB_BIT[SB_UP];
        if (ly > 16000) p |= SB_BIT[SB_DOWN];
        if (lx < -16000) p |= SB_BIT[SB_LEFT];
        if (lx > 16000) p |= SB_BIT[SB_RIGHT];
    }
    return p;
}

int input_pad_code(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return ev->gbutton.button;
    if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int a = ev->gaxis.axis, v = ev->gaxis.value;
        bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        /* sticks need a firm push so a resting stick isn't captured */
        if (v > (trigger ? 16000 : 24000)) return PAD_AXIS_POS + a;
        if (!trigger && v < -24000) return PAD_AXIS_NEG + a;
    }
    return -1;
}

void input_load_mappings(void)
{
    char dir[SBPC_PATH_MAX], path[SBPC_PATH_MAX];
    const char *dirs[2] = {NULL, paths_data_dir()};
    if (fs_exe_dir(dir)) dirs[0] = dir;
    for (int i = 0; i < 2; i++) {
        int n;
        if (!dirs[i] || !fs_join(path, dirs[i], "gamecontrollerdb.txt") || !fs_is_file(path)) continue;
        n = SDL_AddGamepadMappingsFromFile(path);
        if (n < 0) LOGW(SUB_INPUT, "can't read %s: %s", path, SDL_GetError());
        else LOGI(SUB_INPUT, "loaded %d gamepad mappings from %s", n, path);
    }
}
