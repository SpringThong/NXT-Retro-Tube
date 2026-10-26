/* Drawing helpers: shapes via SDL_RenderGeometry, cached text and icons.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "draw.h"
#ifdef NXT_TUBE_PW
/* muOS / Knulli: no dependency on the firmware's SDL2_image */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ASSERT(x) ((void)0)   /* no __FILE__ paths: reproducible builds */
#include <string.h>
#include "stb_image.h"
static SDL_Surface *IMG_Load(const char *p) {
    int w, h, n; unsigned char *px = stbi_load(p, &w, &h, &n, 4);
    if (!px) return NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (s) for (int y = 0; y < h; y++) memcpy((Uint8 *)s->pixels + y * s->pitch, px + (size_t)y * w * 4, (size_t)w * 4);
    stbi_image_free(px);
    return s;
}
#else
#include <SDL2/SDL_image.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Renderer *R;
static char res[1024];
static unsigned clock_n;

/* ---------- fonts ---------- */
typedef struct { FontFace f; int size; TTF_Font *font; } FontSlot;
static FontSlot fonts[48]; static int nfonts;
static char sub_path[1300]; static long sub_index;
static char sub_last[1024];   /* the subtitle line last wrapped */
static const char *face_file(FontFace f) {
    switch (f) {
        case F_REG: return "BeVietnamPro-Regular.ttf";
        case F_MED: return "BeVietnamPro-Medium.ttf";
        case F_SEMI: return "BeVietnamPro-SemiBold.ttf";
        case F_BOLD: return "BeVietnamPro-Bold.ttf";
        case F_XBOLD: return "BeVietnamPro-ExtraBold.ttf";
        case F_PIXEL: return "VT323-Regular.ttf";
        case F_SUB: return "BeVietnamPro-Medium.ttf";
    }
    return "BeVietnamPro-Regular.ttf";
}
static TTF_Font *font(FontFace f, int size) {
    for (int i = 0; i < nfonts; i++) if (fonts[i].f == f && fonts[i].size == size) return fonts[i].font;
    char p[1200]; snprintf(p, sizeof(p), "%s/fonts/%s", res, face_file(f));
    TTF_Font *t = f == F_SUB && sub_path[0] ? TTF_OpenFontIndex(sub_path, size, sub_index) : NULL;
    if (f == F_SUB && sub_path[0] && !t) Log_msg("font %s: %s", sub_path, TTF_GetError());
    if (!t) t = TTF_OpenFont(p, size);
    if (!t) { Log_msg("font %s: %s", p, TTF_GetError()); return NULL; }
    TTF_SetFontHinting(t, TTF_HINTING_LIGHT);
    if (nfonts < 48) fonts[nfonts++] = (FontSlot){f, size, t};
    return t;
}
int D_fontHeight(FontFace f, int size) { TTF_Font *t = font(f, size); return t ? TTF_FontHeight(t) : size; }

/* ---------- text cache ---------- */
typedef struct { char *s; FontFace f; int size; Uint32 rgba; SDL_Texture *tex; int w, h; unsigned used; } TextSlot;
#define TEXT_N 384
static TextSlot texts[TEXT_N];

static SDL_Texture *text_tex(FontFace f, int size, const char *s, SDL_Color c, int *w, int *h) {
    if (!s || !*s) return NULL;
    Uint32 rgba = (c.r << 24) | (c.g << 16) | (c.b << 8) | c.a;
    int victim = 0;
    for (int i = 0; i < TEXT_N; i++) {
        TextSlot *t = &texts[i];
        if (t->tex && t->f == f && t->size == size && t->rgba == rgba && !strcmp(t->s, s)) { t->used = clock_n; *w = t->w; *h = t->h; return t->tex; }
        if (!texts[i].tex) victim = i; else if (texts[victim].tex && texts[i].used < texts[victim].used) victim = i;
    }
    TTF_Font *ft = font(f, size); if (!ft) return NULL;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(ft, s, c);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(R, surf);
    *w = surf->w; *h = surf->h; SDL_FreeSurface(surf);
    if (!tex) return NULL;
    TextSlot *t = &texts[victim];
    if (t->tex) SDL_DestroyTexture(t->tex);
    free(t->s);
    *t = (TextSlot){strdup(s), f, size, rgba, tex, *w, *h, clock_n};
    return tex;
}
int D_textW(FontFace f, int size, const char *s) {
    s = T(s);
    TTF_Font *t = font(f, size); int w = 0, h = 0;
    if (t && s && *s) TTF_SizeUTF8(t, s, &w, &h);
    return w;
}
static int utf8_prev(const char *s, int i) { do i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80); return i; }
static void ellipsize(FontFace f, int size, const char *s, int max_w, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (max_w <= 0 || D_textW(f, size, out) <= max_w) return;
    int len = (int)strlen(out);
    int lo = 0, hi = len;
    while (lo < hi) { /* binary search on byte length, then snap to char boundary */
        int mid = (lo + hi + 1) / 2;
        int cut = mid; while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        char tmp[1024]; snprintf(tmp, sizeof(tmp), "%.*s…", cut, s);
        if (D_textW(f, size, tmp) <= max_w) lo = mid; else hi = mid - 1;
    }
    int cut = lo; while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    while (cut > 0 && s[cut - 1] == ' ') cut--;
    snprintf(out, n, "%.*s…", cut, s);
}
int D_text(FontFace f, int size, const char *s, int x, int y, SDL_Color c, int max_w, int align) {
    if (!s || !*s) return 0;
    s = T(s);
    char buf[1024]; ellipsize(f, size, s, max_w, buf, sizeof(buf));
    int w, h; SDL_Texture *t = text_tex(f, size, buf, c, &w, &h);
    if (!t) return 0;
    int dx = align == AL_CENTER ? x - w / 2 : align == AL_RIGHT ? x - w : x;
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_RenderCopy(R, t, NULL, &(SDL_Rect){dx, y, w, h});
    return w;
}
int D_textBox(FontFace f, int size, const char *s, int x, int y, int w, int lines, int line_h, SDL_Color c) {
    if (!s || !*s) return 0;
    s = T(s);
    const char *p = s; int used = 0;
    while (*p && used < lines) {
        while (*p == ' ') p++;
        if (used == lines - 1) { D_text(f, size, p, x, y + used * line_h, c, w, AL_LEFT); return used + 1; }
        /* longest prefix ending at a space that fits */
        int best = 0, i = 0, len = (int)strlen(p);
        char tmp[1024];
        while (i < len) {
            int j = i; while (j < len && p[j] != ' ') j++;
            snprintf(tmp, sizeof(tmp), "%.*s", j, p);
            if (D_textW(f, size, tmp) <= w) { best = j; i = j + 1; } else break;
        }
        if (best == 0) { /* single long word: cut by characters */
            int k = len; snprintf(tmp, sizeof(tmp), "%s", p);
            while (k > 1 && D_textW(f, size, tmp) > w) { k = utf8_prev(p, k); snprintf(tmp, sizeof(tmp), "%.*s", k, p); }
            best = k;
        }
        snprintf(tmp, sizeof(tmp), "%.*s", best, p);
        D_text(f, size, tmp, x, y + used * line_h, c, 0, AL_LEFT);
        used++; p += best;
    }
    return used;
}

void D_setSubFont(const char *path, long index) {
    if (!path) path = "";
    if (!strcmp(path, sub_path) && index == sub_index) return;
    snprintf(sub_path, sizeof(sub_path), "%s", path); sub_index = index; sub_last[0] = 0;
    for (int i = 0; i < nfonts; i++) if (fonts[i].f == F_SUB) { TTF_CloseFont(fonts[i].font); fonts[i--] = fonts[--nfonts]; }
    for (int i = 0; i < TEXT_N; i++) if (texts[i].tex && texts[i].f == F_SUB) { SDL_DestroyTexture(texts[i].tex); texts[i].tex = NULL; free(texts[i].s); texts[i].s = NULL; }
}
/* Subtitle lines centred above `bottom`, each on a dark band. The wrap is cached:
 * it only changes when the line does. Wraps at spaces, or between characters for
 * scripts written without them (Chinese, Japanese, Thai). */
void D_subtitle(const char *s, int size, int cx, int bottom, int max_w, int max_lines) {
    static int last_size, last_w, nl; static char lines[4][512];
    if (!s || !*s) return;
    if (max_lines > 4) max_lines = 4;
    TTF_Font *ft = font(F_SUB, size); if (!ft) return;
    if (strcmp(s, sub_last) || size != last_size || max_w != last_w) {
        snprintf(sub_last, sizeof(sub_last), "%s", s); last_size = size; last_w = max_w; nl = 0;
        const char *p = s;
        while (*p && nl < 4) {
            while (*p == ' ') p++;
            int len = (int)strlen(p), best = 0, sp = 0, i = 0, w = 0, h = 0;
            char tmp[512];
            /* the longest prefix that fits, preferring to break after a space */
            while (i < len) {
                int j = i + 1; while (j < len && ((unsigned char)p[j] & 0xC0) == 0x80) j++;
                if (j >= (int)sizeof(tmp)) break;
                snprintf(tmp, sizeof(tmp), "%.*s", j, p);
                TTF_SizeUTF8(ft, tmp, &w, &h);
                if (w > max_w) break;
                best = j; if (p[j] == ' ' || !p[j]) sp = j;
                i = j;
            }
            if (best == 0) best = i > 0 ? i : len;
            if (best < len && sp > best / 2) best = sp;
            snprintf(lines[nl++], sizeof(lines[0]), "%.*s", best, p);
            p += best;
        }
    }
    int first = nl > max_lines ? nl - max_lines : 0, n = nl - first;   /* too long: keep the newest words */
    int lh = TTF_FontHeight(ft), y = bottom - n * (lh + 4);
    for (int k = first; k < nl; k++, y += lh + 4) {
        int w, h; SDL_Texture *t = text_tex(F_SUB, size, lines[k], (SDL_Color){255, 255, 255, 255}, &w, &h);
        if (!t) continue;
        D_rect(cx - w / 2 - 10, y, w + 20, lh + 4, (SDL_Color){0, 0, 0, 168});
        SDL_RenderCopy(R, t, NULL, &(SDL_Rect){cx - w / 2, y + 2, w, h});
    }
}

/* ---------- shapes ---------- */
static SDL_Vertex vtx(float x, float y, SDL_Color c) { SDL_Vertex v; v.position.x = x; v.position.y = y; v.color = c; v.tex_coord.x = v.tex_coord.y = 0; return v; }
void D_rect(int x, int y, int w, int h, SDL_Color c) {
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(R, &(SDL_Rect){x, y, w, h});
}
/* ---------- anti-aliased convex shapes ----------
 * Every edge gets a 1 px feather (alpha fades to 0 across the pixel), so curves and
 * diagonals stay smooth without MSAA. */
#define MAXP 112
static SDL_Color fade(SDL_Color c) { c.a = 0; return c; }
static int dedupe(float *px, float *py, int n) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && fabsf(px[i] - px[m - 1]) < 0.01f && fabsf(py[i] - py[m - 1]) < 0.01f) continue;
        px[m] = px[i]; py[m] = py[i]; m++;
    }
    while (m > 2 && fabsf(px[m - 1] - px[0]) < 0.01f && fabsf(py[m - 1] - py[0]) < 0.01f) m--;
    return m;
}
/* outward miter offsets (unit edge distance) for a convex polygon */
static void miters(const float *px, const float *py, int n, float *mx, float *my) {
    float cx = 0, cy = 0; for (int i = 0; i < n; i++) { cx += px[i]; cy += py[i]; } cx /= n; cy /= n;
    for (int i = 0; i < n; i++) {
        int a = (i + n - 1) % n, b = (i + 1) % n;
        float n1x = py[i] - py[a], n1y = -(px[i] - px[a]), n2x = py[b] - py[i], n2y = -(px[b] - px[i]);
        float l1 = sqrtf(n1x * n1x + n1y * n1y), l2 = sqrtf(n2x * n2x + n2y * n2y);
        if (l1 > 0) { n1x /= l1; n1y /= l1; } if (l2 > 0) { n2x /= l2; n2y /= l2; }
        if ((px[i] - cx) * n1x + (py[i] - cy) * n1y < 0) { n1x = -n1x; n1y = -n1y; }
        if ((px[i] - cx) * n2x + (py[i] - cy) * n2y < 0) { n2x = -n2x; n2y = -n2y; }
        float sx = n1x + n2x, sy = n1y + n2y, d = 1 + n1x * n2x + n1y * n2y;
        if (d < 0.3f) d = 0.3f;   /* limit spikes on sharp corners */
        mx[i] = sx / d; my[i] = sy / d;
    }
}
static void aa_fill(float *px, float *py, int n, SDL_Color c) {
    n = dedupe(px, py, n);
    if (n < 3) return;
    float mx[MAXP], my[MAXP];
    miters(px, py, n, mx, my);
    SDL_Vertex v[2 * MAXP + 1]; int idx[9 * MAXP]; int k = 0;
    float cx = 0, cy = 0; for (int i = 0; i < n; i++) { cx += px[i]; cy += py[i]; }
    v[0] = vtx(cx / n, cy / n, c);
    for (int i = 0; i < n; i++) {
        v[1 + i] = vtx(px[i] - mx[i] * 0.5f, py[i] - my[i] * 0.5f, c);
        v[1 + n + i] = vtx(px[i] + mx[i] * 0.5f, py[i] + my[i] * 0.5f, fade(c));
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        idx[k++] = 0; idx[k++] = 1 + i; idx[k++] = 1 + j;
        idx[k++] = 1 + i; idx[k++] = 1 + n + i; idx[k++] = 1 + j;
        idx[k++] = 1 + j; idx[k++] = 1 + n + i; idx[k++] = 1 + n + j;
    }
    SDL_RenderGeometry(R, NULL, v, 1 + 2 * n, idx, k);
}
void D_tri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c) {
    float px[3] = {x1, x2, x3}, py[3] = {y1, y2, y3};
    aa_fill(px, py, 3, c);
}
void D_line(float x1, float y1, float x2, float y2, float w, SDL_Color c) {
    float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * w / 2, ny = dx / l * w / 2;
    float px[4] = {x1 + nx, x2 + nx, x2 - nx, x1 - nx}, py[4] = {y1 + ny, y2 + ny, y2 - ny, y1 - ny};
    aa_fill(px, py, 4, c);
}
static int segs(float r) { int s = (int)(r / 2.5f); return s < 3 ? 3 : s > 24 ? 24 : s; }
/* contour of a rounded rect: 4 corners × (seg+1) points */
static int contour(float x, float y, float w, float h, float r, int seg, float *px, float *py) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    float a0[4] = {-90, 0, 90, 180};
    int n = 0;
    for (int k = 0; k < 4; k++) for (int i = 0; i <= seg; i++) {
        float a = (a0[k] + 90.0f * i / seg) * (float)M_PI / 180.0f;
        px[n] = cx[k] + r * cosf(a); py[n] = cy[k] + r * sinf(a); n++;
    }
    return n;
}
void D_round(int x, int y, int w, int h, int r, SDL_Color c) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { D_rect(x, y, w, h, c); return; }
    float px[MAXP], py[MAXP];
    int n = contour(x, y, w, h, r, segs(r), px, py);
    aa_fill(px, py, n, c);
}
void D_roundRing(int x, int y, int w, int h, int r, int t, SDL_Color c) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    int seg = segs(r);
    float ox[MAXP], oy[MAXP], ix[MAXP], iy[MAXP], omx[MAXP], omy[MAXP], imx[MAXP], imy[MAXP];
    /* same point count on both contours keeps the strips aligned */
    int n = contour(x, y, w, h, r > 0 ? r : 0.01f, seg, ox, oy);
    contour(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 0.01f, seg, ix, iy);
    miters(ox, oy, n, omx, omy);
    miters(ix, iy, n, imx, imy);
    for (int i = 0; i < n; i++) {   /* degenerate corner points: reuse a neighbour's normal */
        if (!isfinite(omx[i])) omx[i] = omy[i] = 0;
        if (!isfinite(imx[i])) imx[i] = imy[i] = 0;
    }
    SDL_Vertex v[4 * MAXP]; int idx[18 * MAXP]; int k = 0;
    for (int i = 0; i < n; i++) {
        v[4 * i + 0] = vtx(ox[i] + omx[i] * 0.5f, oy[i] + omy[i] * 0.5f, fade(c));
        v[4 * i + 1] = vtx(ox[i] - omx[i] * 0.5f, oy[i] - omy[i] * 0.5f, c);
        v[4 * i + 2] = vtx(ix[i] + imx[i] * 0.5f, iy[i] + imy[i] * 0.5f, c);
        v[4 * i + 3] = vtx(ix[i] - imx[i] * 0.5f, iy[i] - imy[i] * 0.5f, fade(c));
    }
    for (int i = 0; i < n; i++) {
        int a = 4 * i, b = 4 * ((i + 1) % n);
        for (int s2 = 0; s2 < 3; s2++) {
            idx[k++] = a + s2; idx[k++] = a + s2 + 1; idx[k++] = b + s2;
            idx[k++] = b + s2; idx[k++] = a + s2 + 1; idx[k++] = b + s2 + 1;
        }
    }
    SDL_RenderGeometry(R, NULL, v, 4 * n, idx, k);
}
void D_circle(int cx, int cy, int r, SDL_Color c) { D_round(cx - r, cy - r, 2 * r, 2 * r, r, c); }
void D_ring(int cx, int cy, int r, int t, SDL_Color c) { D_roundRing(cx - r, cy - r, 2 * r, 2 * r, r, t, c); }
void D_corners(int x, int y, int w, int h, int r, SDL_Color bg) {
    /* fill the area between each square corner and its arc with the background,
     * fading over one pixel into the picture so the curve is smooth */
    float cxs[4] = {x + r, x + w - r, x + w - r, x + r}, cys[4] = {y + r, y + r, y + h - r, y + h - r};
    float sx[4] = {x, x + w, x + w, x}, sy[4] = {y, y, y + h, y + h};
    float a0[4] = {180, 270, 0, 90};
    int seg = segs(r);
    for (int k = 0; k < 4; k++) {
        SDL_Vertex v[2 * 26 + 1]; int idx[9 * 26]; int m = 0;
        v[0] = vtx(sx[k], sy[k], bg);
        for (int i = 0; i <= seg; i++) {
            float a = (a0[k] + 90.0f * i / seg) * (float)M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
            v[1 + i] = vtx(cxs[k] + (r + 0.5f) * ca, cys[k] + (r + 0.5f) * sa, bg);
            v[2 + seg + i] = vtx(cxs[k] + (r - 0.5f) * ca, cys[k] + (r - 0.5f) * sa, fade(bg));
        }
        for (int i = 0; i < seg; i++) {
            idx[m++] = 0; idx[m++] = 1 + i; idx[m++] = 2 + i;
            idx[m++] = 1 + i; idx[m++] = 2 + seg + i; idx[m++] = 2 + i;
            idx[m++] = 2 + i; idx[m++] = 2 + seg + i; idx[m++] = 3 + seg + i;
        }
        SDL_RenderGeometry(R, NULL, v, 2 * seg + 3, idx, m);
    }
}
void D_shadow(int x, int y, int w, int h, int r, int off) { D_round(x + off, y + off, w, h, r, RGBA(0, 0, 0, 255)); }
void D_focus(int x, int y, int w, int h, int r) { D_roundRing(x - 8, y - 8, w + 16, h + 16, r + 6, 4, C_MUSTARD); }

static SDL_Texture *scan_tex;
void D_scanlines(int x, int y, int w, int h) {
    if (!scan_tex) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 1024, 768, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) return;
        Uint32 line = SDL_MapRGBA(s->format, 0, 0, 0, 52), none = SDL_MapRGBA(s->format, 0, 0, 0, 0);
        for (int yy = 0; yy < 768; yy++) { Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + yy * s->pitch); for (int xx = 0; xx < 1024; xx++) row[xx] = yy % 3 == 0 ? line : none; }
        scan_tex = SDL_CreateTextureFromSurface(R, s); SDL_FreeSurface(s);
        if (scan_tex) SDL_SetTextureBlendMode(scan_tex, SDL_BLENDMODE_BLEND);
    }
    if (scan_tex && w > 0 && h > 0) SDL_RenderCopy(R, scan_tex, &(SDL_Rect){0, 0, w > 1024 ? 1024 : w, h > 768 ? 768 : h}, &(SDL_Rect){x, y, w, h});
}

/* ---------- icons ----------
 * Lucide PNGs are 192 px; each size used gets its own texture, box-filtered
 * (premultiplied area average) on the CPU so thin strokes stay smooth. */
typedef struct { char name[32]; int size; SDL_Texture *tex; } IconSlot;
#define ICON_N 96
static IconSlot icons[ICON_N]; static int nicons;
static SDL_Surface *icon_src(const char *name) {
    char p[1200]; snprintf(p, sizeof(p), "%s/icons/%s.png", res, name);
    SDL_Surface *s = IMG_Load(p);
    if (!s) return NULL;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    return c;
}
SDL_Surface *D_downscale(SDL_Surface *src, int dw, int dh) {
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!dst) return NULL;
    SDL_LockSurface(src);
    float fx = (float)src->w / dw, fy = (float)src->h / dh;
    for (int y = 0; y < dh; y++) {
        float y0 = y * fy, y1 = y0 + fy;
        Uint32 *out = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch);
        for (int x = 0; x < dw; x++) {
            float x0 = x * fx, x1 = x0 + fx;
            double r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (int sy = (int)y0; sy < (int)ceilf(y1) && sy < src->h; sy++) {
                float wy = fminf(y1, sy + 1) - fmaxf(y0, sy);
                const Uint8 *row = (const Uint8 *)src->pixels + sy * src->pitch;
                for (int sx = (int)x0; sx < (int)ceilf(x1) && sx < src->w; sx++) {
                    float wgt = wy * (fminf(x1, sx + 1) - fmaxf(x0, sx));
                    const Uint8 *px = row + sx * 4;   /* R G B A */
                    double al = px[3] / 255.0 * wgt;
                    r += px[0] * al; g += px[1] * al; b += px[2] * al; a += al; wsum += wgt;
                }
            }
            Uint8 R8 = 0, G8 = 0, B8 = 0, A8 = 0;
            if (a > 0) { R8 = (Uint8)fmin(255, r / a + 0.5); G8 = (Uint8)fmin(255, g / a + 0.5); B8 = (Uint8)fmin(255, b / a + 0.5); }
            if (wsum > 0) A8 = (Uint8)fmin(255, a / wsum * 255 + 0.5);
            out[x] = (Uint32)R8 | ((Uint32)G8 << 8) | ((Uint32)B8 << 16) | ((Uint32)A8 << 24);
        }
    }
    SDL_UnlockSurface(src);
    return dst;
}
void D_icon(const char *name, int x, int y, int size, SDL_Color c) {
    SDL_Texture *t = NULL; bool found = false;
    for (int i = 0; i < nicons; i++) if (icons[i].size == size && !strcmp(icons[i].name, name)) { t = icons[i].tex; found = true; break; }
    if (!found && nicons < ICON_N) {
        SDL_Surface *src = icon_src(name);
        if (src) {
            SDL_Surface *d = D_downscale(src, size, size);
            if (d) { t = SDL_CreateTextureFromSurface(R, d); SDL_FreeSurface(d); }
            SDL_FreeSurface(src);
        }
        if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest); }
        snprintf(icons[nicons].name, sizeof(icons[nicons].name), "%s", name); icons[nicons].size = size; icons[nicons++].tex = t;
    }
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a);
    SDL_RenderCopy(R, t, NULL, &(SDL_Rect){x, y, size, size});
}

void D_badge(int x, int y, const char *label, int h) {
    if (!strcmp(label, "UD") || !strcmp(label, "LR")) {   /* d-pad glyphs drawn, fonts lack arrows */
        int w = h + 14; D_round(x, y, w, h, h / 2, C_INK);
        float cx = x + w / 2.0f, cy = y + h / 2.0f;
        if (label[0] == 'U') { D_tri(cx - 6, cy - 2, cx + 6, cy - 2, cx, cy - 9, C_DARK); D_tri(cx - 6, cy + 2, cx + 6, cy + 2, cx, cy + 9, C_DARK); }
        else { D_tri(cx - 2, cy - 6, cx - 2, cy + 6, cx - 10, cy, C_DARK); D_tri(cx + 2, cy - 6, cx + 2, cy + 6, cx + 10, cy, C_DARK); }
        return;
    }
    int w = D_textW(F_PIXEL, h - 4, label) + (strlen(label) > 1 ? 14 : 0);
    if (w < h) w = h;
    D_round(x, y, w, h, h / 2, C_INK);
    D_text(F_PIXEL, h - 4, label, x + w / 2, y + (h - D_fontHeight(F_PIXEL, h - 4)) / 2, C_DARK, 0, AL_CENTER);
}
int D_hint(int x, int y, const char *key, const char *text) {
    int h = 28;
    int w = D_textW(F_PIXEL, h - 4, key) + (strlen(key) > 1 ? 14 : 0); if (w < h) w = h;
    if (!strcmp(key, "UD") || !strcmp(key, "LR")) w = h + 14;
    D_badge(x, y, key, h);
    int tw = D_text(F_MED, 18, text, x + w + 8, y + 2, C_MUTED, 0, AL_LEFT);
    return w + 8 + tw;
}
static void arc(float cx, float cy, float r, float t, float a0, float a1, SDL_Color c) {
    int n = 24; SDL_Vertex v[4 * 25]; int idx[18 * 24]; int k = 0;
    float rr[4] = {r + 0.5f, r - 0.5f, r - t + 0.5f, r - t - 0.5f};
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * i / n, ca = cosf(a), sa = sinf(a);
        for (int j = 0; j < 4; j++) v[4 * i + j] = vtx(cx + rr[j] * ca, cy + rr[j] * sa, (j == 0 || j == 3) ? fade(c) : c);
    }
    for (int i = 0; i < n; i++) for (int j = 0; j < 3; j++) {
        int p0 = 4 * i + j, p1 = 4 * (i + 1) + j;
        idx[k++] = p0; idx[k++] = p0 + 1; idx[k++] = p1; idx[k++] = p1; idx[k++] = p0 + 1; idx[k++] = p1 + 1;
    }
    SDL_RenderGeometry(R, NULL, v, 4 * (n + 1), idx, k);
}
void D_spinner(int cx, int cy, int r, SDL_Color c) {
    D_ring(cx, cy, r, 6, RGBA(244, 239, 230, 50));
    float a0 = (SDL_GetTicks() % 900) / 900.0f * 2 * (float)M_PI;
    arc(cx, cy, r, 6, a0, a0 + 1.6f, c);
}
void D_video(SDL_Texture *t, int tw, int th, SDL_Rect a) {
    D_rect(a.x, a.y, a.w, a.h, C_BLACK);
    if (!t || tw <= 0 || th <= 0) return;
    int w = a.w, h = a.w * th / tw;
    if (h > a.h) { h = a.h; w = a.h * tw / th; }
    SDL_RenderCopy(R, t, NULL, &(SDL_Rect){a.x + (a.w - w) / 2, a.y + (a.h - h) / 2, w, h});
}
/* Full-screen video on a screen wider than the 4:3 layout (Smart Pro / Smart Pro S, 1280x720):
 * drawn at the panel's own resolution instead of inside the letterboxed 1024x768 canvas. */
void D_videoNative(SDL_Texture *t, int tw, int th) {
    int lw, lh, ow, oh;
    SDL_RenderGetLogicalSize(R, &lw, &lh);
    if (SDL_GetRendererOutputSize(R, &ow, &oh) || lw <= 0 || ow * 3 <= oh * 4 + 8) { D_video(t, tw, th, (SDL_Rect){0, 96, 1024, 576}); return; }
    SDL_RenderSetLogicalSize(R, 0, 0); SDL_RenderSetViewport(R, NULL); SDL_RenderSetScale(R, 1, 1);
    D_video(t, tw, th, (SDL_Rect){0, 0, ow, oh});
    SDL_RenderSetLogicalSize(R, lw, lh);
}
void D_thumb(const char *id, SDL_Rect r, SDL_Color bg) {
    SDL_Texture *t = Thumbs_get(id);
    if (t) SDL_RenderCopy(R, t, NULL, &r);
    else {
        D_rect(r.x, r.y, r.w, r.h, RGBA(0x2A, 0x26, 0x36, 255));
        D_circle(r.x + r.w / 2, r.y + r.h * 45 / 100, r.h / 5, RGBA(0x3A, 0x35, 0x48, 255));
        D_rect(r.x, r.y + r.h * 65 / 100, r.w, r.h - r.h * 65 / 100, RGBA(0x20, 0x1D, 0x29, 255));
    }
    D_scanlines(r.x, r.y, r.w, r.h);
    D_corners(r.x, r.y, r.w, r.h, 10, bg);
    D_roundRing(r.x, r.y, r.w, r.h, 10, 2, C_BLACK);
}

bool D_init(SDL_Renderer *r, const char *res_dir) {
    R = r; snprintf(res, sizeof(res), "%s", res_dir);
    if (TTF_Init()) { Log_msg("TTF_Init: %s", TTF_GetError()); return false; }
#ifndef NXT_TUBE_PW
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
#endif
    return font(F_REG, 20) != NULL;
}
static void release_textures(void) {
    for (int i = 0; i < TEXT_N; i++) { if (texts[i].tex) SDL_DestroyTexture(texts[i].tex); free(texts[i].s); memset(&texts[i], 0, sizeof(texts[i])); }
    for (int i = 0; i < nicons; i++) if (icons[i].tex) SDL_DestroyTexture(icons[i].tex);
    nicons = 0;
    if (scan_tex) SDL_DestroyTexture(scan_tex);
    scan_tex = NULL;
}
void D_setRenderer(SDL_Renderer *r) { if (r != R) { if (r) {} } R = r; }
void D_dropTextures(void) { release_textures(); }
void D_quit(void) {
    release_textures();
    for (int i = 0; i < nfonts; i++) TTF_CloseFont(fonts[i].font);
    nfonts = 0;
    TTF_Quit();
}
void D_frame(void) { clock_n++; }
SDL_Renderer *D_renderer(void) { return R; }
