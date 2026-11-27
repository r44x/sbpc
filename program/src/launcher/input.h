/* Host input for the game window: keyboard and SDL gamepads -> Saturn pads, with rebindable
 * keyboard and gamepad layouts saved in sbpc.ini. Player 1 = keyboard + first gamepad, player N =
 * gamepad N (connection order), up to the game's limit (10); extra gamepads are left unused. */
#ifndef SBPC_LAUNCHER_INPUT_H
#define SBPC_LAUNCHER_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/config.h"
#include "launcher/overlay.h"

#define INPUT_MAX_PADS 12
/* Gamepad binding codes: 0..SDL_GAMEPAD_BUTTON_COUNT-1 = a button; PAD_AXIS_POS + axis / PAD_AXIS_NEG + axis
 * = an axis pushed past half way (triggers, sticks). */
#define PAD_AXIS_POS 100
#define PAD_AXIS_NEG 200

typedef struct {
    SDL_Scancode key[SB_COUNT];
    int pad[SB_COUNT];
    SDL_Gamepad *pads[INPUT_MAX_PADS];
    int npads;
    int max_players;          /* the game's limit; 0 = no limit */
} input_state;

void input_init(input_state *in, const config *cfg);   /* bindings from cfg (defaults where unset) */
void input_defaults(input_state *in);
void input_store(const input_state *in, config *cfg);
void input_close(input_state *in);

/* Gamepad hot-plug. */
void input_pad_added(input_state *in, SDL_JoystickID id);
void input_pad_removed(input_state *in, SDL_JoystickID id);
int input_players(const input_state *in);              /* max(1, gamepads), at most max_players */

/* Active-high Saturn bits (P_* layout, see play.c) for player p (0-based). */
uint16_t input_read(const input_state *in, int player);
/* While rebinding a gamepad: the code of an input on the event, or -1. */
int input_pad_code(const SDL_Event *ev);
const char *input_pad_code_name(int code, char *buf, size_t cap);
/* Loads gamecontrollerdb.txt from the exe folder and the data folder, if present. */
void input_load_mappings(void);

#endif
