/* The front end's look, shared by the launcher and the in-game overlay: green lines and uppercase text on
 * black, in the Half Eighties 8x8 pixel font (libs/half-eighties, OFL 1.1). */
#ifndef SBPC_LAUNCHER_UI_FONT_H
#define SBPC_LAUNCHER_UI_FONT_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#define UI_CH 8              /* font cell */
#define UI_BOX_H 21          /* key boxes and badges */
#define UI_BOX_TEXT 6        /* text offset inside a box */
#define UI_ROW_H 19          /* list rows */
#define UI_ROW_TEXT 5        /* text offset inside a row */

typedef struct { Uint8 r, g, b; } ui_rgb;
extern const ui_rgb UI_GREEN, UI_GREEN_DIM, UI_WHITE, UI_GREY, UI_BLACK;

/* The glyph strip (white, tinted per draw); NULL on failure (text then uses SDL's debug font). */
SDL_Texture *ui_font_create(SDL_Renderer *ren);

/* Draws at most max_chars characters, uppercased, non-ASCII as '?'. Returns the width drawn. */
float ui_text_n(SDL_Renderer *ren, SDL_Texture *font, ui_rgb c, float x, float y, const char *s, int max_chars);
float ui_text(SDL_Renderer *ren, SDL_Texture *font, ui_rgb c, float x, float y, const char *s);
int ui_text_len(const char *s);   /* characters (UTF-8 aware) */

void ui_fill(SDL_Renderer *ren, ui_rgb c, float x, float y, float w, float h);
/* A filled green box with black text. Returns its width. */
float ui_label_box(SDL_Renderer *ren, SDL_Texture *font, float x, float y, const char *s);
/* Sets the program icon (res/icon.png, the user's art) on a window: Linux and macOS show it in the title bar
   and task bar; on Windows the .exe icon already covers it. */
void ui_set_window_icon(SDL_Window *win);
/* Footer entry [KEY] LABEL; the label is white when hot. Returns its total width. */
float ui_key(SDL_Renderer *ren, SDL_Texture *font, float x, float y, const char *key, const char *label, bool hot);

#endif
