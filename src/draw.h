/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXT_TUBE_DRAW_H
#define NXT_TUBE_DRAW_H
#include "app.h"
#include <SDL_ttf.h>

/* palette: runtime so Cài đặt → Giao diện can switch presets (D_setTheme) */
extern SDL_Color g_pal[10];
#define THEMES 6
void D_setTheme(int t);
const char *D_themeName(int t);
#define C_BG        (g_pal[0])
#define C_SURFACE   (g_pal[1])
#define C_RAISED    (g_pal[2])
#define C_BAR       (g_pal[3])
#define C_LINE      (g_pal[4])
#define C_INK       (g_pal[5])
#define C_MUTED     (g_pal[6])
#define C_DIM       (g_pal[7])
#define C_TOMATO    (g_pal[8])
#define C_MUSTARD   (g_pal[9])
#define C_BLACK    ((SDL_Color){0, 0, 0, 255})
#define C_DARK     (g_pal[0])
#define RGBA(r, g, b, a) ((SDL_Color){r, g, b, a})

typedef enum { F_REG, F_MED, F_SEMI, F_BOLD, F_XBOLD, F_PIXEL, F_SUB } FontFace;
enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

bool D_init(SDL_Renderer *r, const char *res_dir);
void D_setRenderer(SDL_Renderer *r);   /* after the display was re-created */
void D_dropTextures(void);             /* before the renderer is destroyed */
void D_quit(void);
void D_frame(void);
int D_takePackNote(void);           /* a font pack for Korean/Japanese/Chinese/Thai titles just started downloading */                   /* advance cache clock */
SDL_Renderer *D_renderer(void);

void D_rect(int x, int y, int w, int h, SDL_Color c);
void D_round(int x, int y, int w, int h, int r, SDL_Color c);
void D_roundRing(int x, int y, int w, int h, int r, int t, SDL_Color c);
void D_circle(int cx, int cy, int r, SDL_Color c);
void D_ring(int cx, int cy, int r, int t, SDL_Color c);
void D_line(float x1, float y1, float x2, float y2, float w, SDL_Color c);
void D_tri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c);
void D_corners(int x, int y, int w, int h, int r, SDL_Color bg); /* mask square corners of images */
void D_scanlines(int x, int y, int w, int h);
void D_focus(int x, int y, int w, int h, int r);    /* mustard ring + hard shadow */
void D_shadow(int x, int y, int w, int h, int r, int off);

int D_textW(FontFace f, int size, const char *s);
/* Draw one line, ellipsized to max_w (0 = no limit). Returns drawn width. */
int D_text(FontFace f, int size, const char *s, int x, int y, SDL_Color c, int max_w, int align);
/* Wrap into up to `lines` lines (last one ellipsized). Returns lines used. */
int D_textBox(FontFace f, int size, const char *s, int x, int y, int w, int lines, int line_h, SDL_Color c);
int D_fontHeight(FontFace f, int size);
/* subtitles: F_SUB is Be Vietnam Pro Medium unless a font pack is chosen */
void D_setSubFont(const char *path, long index);
void D_videoNative(SDL_Texture *t, int tw, int th, bool fill);   /* the whole panel; fill = zoom to cover it (2.8) */
void D_videoFill(SDL_Texture *t, int tw, int th, SDL_Rect area, bool fill);
void D_subtitle(const char *s, int size, int cx, int bottom, int max_w, int max_lines);

void D_icon(const char *name, int x, int y, int size, SDL_Color c);
void D_badge(int x, int y, const char *label, int h);   /* button glyph like the demo */
int D_hint(int x, int y, const char *key, const char *text); /* returns width */
void D_spinner(int cx, int cy, int r, SDL_Color c);
void D_video(SDL_Texture *t, int tw, int th, SDL_Rect area); /* aspect-fit, black bars */
void D_thumb(const char *id, SDL_Rect r, SDL_Color bg);       /* thumbnail or placeholder */
#endif
