#include "launcher/ui_font.h"

#include "half_eighties.h"
#include "icon_rgba.h"

const ui_rgb UI_GREEN = {57, 248, 85};
const ui_rgb UI_GREEN_DIM = {16, 71, 24};
const ui_rgb UI_WHITE = {255, 255, 255};
const ui_rgb UI_GREY = {150, 150, 150};
const ui_rgb UI_BLACK = {0, 0, 0};

SDL_Texture *ui_font_create(SDL_Renderer *ren)
{
    const int n = HALF_EIGHTIES_LAST - HALF_EIGHTIES_FIRST + 1;
    SDL_Surface *s = SDL_CreateSurface(n * UI_CH, UI_CH, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture *t = NULL;
    if (!s)
        return NULL;
    for (int c = 0; c < n; c++)
        for (int y = 0; y < UI_CH; y++)
            for (int x = 0; x < UI_CH; x++) {
                Uint8 *p = (Uint8 *)s->pixels + y * s->pitch + (c * UI_CH + x) * 4;
                bool on = (half_eighties[c][y] & (0x80 >> x)) != 0;
                p[0] = p[1] = p[2] = 255;
                p[3] = on ? 255 : 0;
            }
    t = SDL_CreateTextureFromSurface(ren, s);
    SDL_DestroySurface(s);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);   /* keep the pixels square when scaled up */
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

float ui_text_n(SDL_Renderer *ren, SDL_Texture *font, ui_rgb c, float x, float y, const char *s, int max_chars)
{
    float x0 = x;
    if (font)
        SDL_SetTextureColorMod(font, c.r, c.g, c.b);
    else
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    for (int i = 0; *s && i < max_chars; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 0x80 && ch < 0xC0)
            continue;                         /* UTF-8 continuation byte: one '?' per character */
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        if (ch < HALF_EIGHTIES_FIRST || ch > HALF_EIGHTIES_LAST) ch = '?';
        if (font) {
            SDL_FRect src = {(float)((ch - HALF_EIGHTIES_FIRST) * UI_CH), 0, UI_CH, UI_CH};
            SDL_FRect dst = {x, y, UI_CH, UI_CH};
            SDL_RenderTexture(ren, font, &src, &dst);
        } else {
            char one[2] = {(char)ch, 0};
            SDL_RenderDebugText(ren, x, y, one);
        }
        x += UI_CH;
        i++;
    }
    return x - x0;
}

float ui_text(SDL_Renderer *ren, SDL_Texture *font, ui_rgb c, float x, float y, const char *s)
{
    return ui_text_n(ren, font, c, x, y, s, 1 << 20);
}

int ui_text_len(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

void ui_fill(SDL_Renderer *ren, ui_rgb c, float x, float y, float w, float h)
{
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(ren, &r);
}

float ui_label_box(SDL_Renderer *ren, SDL_Texture *font, float x, float y, const char *s)
{
    float w = (float)(ui_text_len(s) * UI_CH + 8);
    ui_fill(ren, UI_GREEN, x, y, w, UI_BOX_H);
    ui_text(ren, font, UI_BLACK, x + 4, y + UI_BOX_TEXT, s);
    return w;
}

float ui_key(SDL_Renderer *ren, SDL_Texture *font, float x, float y, const char *key, const char *label, bool hot)
{
    float w = ui_label_box(ren, font, x, y, key) + 6;
    return w + ui_text(ren, font, hot ? UI_WHITE : UI_GREEN, x + w, y + UI_BOX_TEXT, label);
}

void ui_set_window_icon(SDL_Window *win)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(SBPC_ICON_SIZE, SBPC_ICON_SIZE, SDL_PIXELFORMAT_RGBA32,
                                           (void *)sbpc_icon_rgba, SBPC_ICON_SIZE * 4);
    if (!s)
        return;
    SDL_SetWindowIcon(win, s);
    SDL_DestroySurface(s);
}
