/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Text from a TrueType font (Inter, in /usr/share/fonts), antialiased,
 * through stb_truetype. Glyphs are rasterised once per size and kept.
 * Without the font files the calls fall back to the 8x8 bitmap font. */
#include "zwm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#include "stb_truetype.h"
#pragma clang diagnostic pop

#define NSIZES 6
#define FIRST  0x20
#define LAST   0x7E
#define NGLYPH (LAST - FIRST + 1)

struct glyph { unsigned char *bm; int w, h, xoff, yoff; int adv; int made; };
struct size { int px; float scale; int ascent, descent, gap; struct glyph g[NGLYPH]; };
struct face { const char *path; unsigned char *data; stbtt_fontinfo info; int state; struct size sizes[NSIZES]; int nsizes; };   /* state: 0 untried, 1 loaded, -1 missing */

static struct face faces[3] = {
    { .path = "/usr/share/fonts/Inter-Regular.ttf" },
    { .path = "/usr/share/fonts/Inter-Medium.ttf" },
    { .path = "/usr/share/fonts/JetBrainsMono-Regular.ttf" },
};

static struct face *load(int weight)
{
    struct face *f = &faces[weight >= 0 && weight < 3 ? weight : 0];
    if (f->state) return f->state > 0 ? f : NULL;
    f->state = -1;
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = n > 0 ? malloc((size_t)n) : NULL;
    if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(f->data); f->data = NULL; return NULL; }
    fclose(fp);
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) { free(f->data); f->data = NULL; return NULL; }
    f->state = 1;
    return f;
}

static struct size *size_for(struct face *f, int px)
{
    if (px < 6) px = 6;
    for (int i = 0; i < f->nsizes; i++) if (f->sizes[i].px == px) return &f->sizes[i];
    struct size *s = &f->sizes[f->nsizes < NSIZES ? f->nsizes++ : NSIZES - 1];   /* one too many: the last slot is recycled */
    for (int i = 0; i < NGLYPH; i++) { free(s->g[i].bm); s->g[i].bm = NULL; s->g[i].made = 0; }
    s->px = px;
    s->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    s->ascent = (int)(a * s->scale + 0.5f); s->descent = (int)(-d * s->scale + 0.5f); s->gap = (int)(g * s->scale + 0.5f);
    return s;
}

static struct glyph *glyph(struct face *f, struct size *s, int cp)
{
    if (cp < FIRST || cp > LAST) cp = '?';
    struct glyph *g = &s->g[cp - FIRST];
    if (!g->made) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
        g->adv = (int)(adv * s->scale + 0.5f);
        g->bm = stbtt_GetCodepointBitmap(&f->info, s->scale, s->scale, cp, &g->w, &g->h, &g->xoff, &g->yoff);
        g->made = 1;
    }
    return g;
}

/* One UTF-8 sequence -> code point; anything outside ASCII draws as '?'. */
static int next_cp(const char **p)
{
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    int cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    (*p)++;
    for (int i = 1; i < n && **p; i++, (*p)++) cp = cp << 6 | ((unsigned char)**p & 0x3F);
    return cp;
}

static int fallback_scale(int px) { int s = (px + 4) / 8; return s < 1 ? 1 : s; }

/* Coverage -> alpha. Blending linearly in sRGB makes thin light-on-dark
 * stems look grey and soft (a 1.3 px stem lands half on two pixels), so
 * coverage is pushed up along a gamma curve, the usual stem darkening. */
static unsigned char curve[256];
static void make_curve(void)
{
    if (curve[255]) return;
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        double a = c * 0.35 + (1 - (1 - c) * (1 - c)) * 0.65;      /* between linear and 1-(1-c)^2 */
        curve[i] = (unsigned char)(a * 255 + 0.5);
    }
    curve[0] = 0; curve[255] = 255;
}

int zwm_ttext_w(zwm_surface *s, int x, int y, const char *str, uint32_t color, int px, int weight)
{
    struct face *f = load(weight);
    if (!f) return zwm_text(s, x, y + (px - 8) / 2, str, color, fallback_scale(px));
    struct size *sz = size_for(f, px);
    make_curve();
    int base = y + sz->ascent, x0 = x, prev = 0;
    for (const char *p = str; *p;) {
        int cp = next_cp(&p);
        if (cp < FIRST || cp > LAST) cp = '?';
        if (prev) x += (int)(stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * sz->scale + 0.5f);
        struct glyph *g = glyph(f, sz, cp);
        for (int j = 0; j < g->h; j++) {
            int py = base + g->yoff + j;
            if (py < 0 || py >= s->h) continue;
            for (int i = 0; i < g->w; i++) {
                int a = g->bm[j * g->w + i];
                if (a) zwm_blend(s, x + g->xoff + i, py, color, curve[a]);
            }
        }
        x += g->adv;
        prev = cp;
    }
    return x - x0;
}

int zwm_ttext(zwm_surface *s, int x, int y, const char *str, uint32_t color, int px)
{
    return zwm_ttext_w(s, x, y, str, color, px, ZWM_FONT_REGULAR);
}

int zwm_ttext_width_w(const char *str, int px, int weight)
{
    struct face *f = load(weight);
    if (!f) return zwm_text_width(str, fallback_scale(px));
    struct size *sz = size_for(f, px);
    int x = 0, prev = 0;
    for (const char *p = str; *p;) {
        int cp = next_cp(&p);
        if (cp < FIRST || cp > LAST) cp = '?';
        if (prev) x += (int)(stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * sz->scale + 0.5f);
        x += glyph(f, sz, cp)->adv;
        prev = cp;
    }
    return x;
}

int zwm_ttext_width(const char *str, int px) { return zwm_ttext_width_w(str, px, ZWM_FONT_REGULAR); }

int zwm_ttext_height_w(int px, int weight)
{
    struct face *f = load(weight);
    if (!f) return 8 * fallback_scale(px);
    struct size *sz = size_for(f, px);
    return sz->ascent + sz->descent;
}

int zwm_ttext_height(int px) { return zwm_ttext_height_w(px, ZWM_FONT_REGULAR); }

int zwm_font_ok(void) { return load(ZWM_FONT_REGULAR) != NULL; }
