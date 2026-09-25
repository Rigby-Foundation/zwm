/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Client-side pixel buffers and the handful of primitives a toolkit-less
 * program needs: fills, outlines, lines, copies, bitmap text. */
#include "zwm.h"
#include <stdlib.h>
#include <string.h>

zwm_surface *zwm_surface_new(int w, int h)
{
    zwm_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (zwm_surface_resize(s, w, h) != 0) { free(s); return NULL; }
    return s;
}

int zwm_shared_resize(zwm_surface *s, int w, int h);     /* client.c */
void zwm_shared_free(zwm_surface *s);

int zwm_surface_resize(zwm_surface *s, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (s->conn) return zwm_shared_resize(s, w, h);
    uint32_t *p = calloc((size_t)w * h, sizeof(uint32_t));
    if (!p) return -1;
    free(s->pix);
    s->pix = p; s->w = w; s->h = h;
    return 0;
}

void zwm_surface_free(zwm_surface *s)
{
    if (!s) return;
    if (s->conn) zwm_shared_free(s); else free(s->pix);
    free(s);
}

/* Clip a rectangle to the surface; returns 0 if nothing is left. */
static int clip(const zwm_surface *s, int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}

void zwm_fill(zwm_surface *s, int x, int y, int w, int h, uint32_t color)
{
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->pix + (size_t)(y + j) * s->w + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void zwm_hline(zwm_surface *s, int x, int y, int w, uint32_t color) { zwm_fill(s, x, y, w, 1, color); }
void zwm_vline(zwm_surface *s, int x, int y, int h, uint32_t color) { zwm_fill(s, x, y, 1, h, color); }

void zwm_rect(zwm_surface *s, int x, int y, int w, int h, uint32_t color)
{
    zwm_hline(s, x, y, w, color);
    zwm_hline(s, x, y + h - 1, w, color);
    zwm_vline(s, x, y, h, color);
    zwm_vline(s, x + w - 1, y, h, color);
}

void zwm_copy(zwm_surface *dst, int dx, int dy, const zwm_surface *src, int sx, int sy, int w, int h)
{
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;
    if (dst == src && dy > sy)              /* overlapping downwards: copy bottom-up */
        for (int j = h - 1; j >= 0; j--)
            memmove(dst->pix + (size_t)(dy + j) * dst->w + dx, src->pix + (size_t)(sy + j) * src->w + sx, (size_t)w * 4);
    else
        for (int j = 0; j < h; j++)
            memmove(dst->pix + (size_t)(dy + j) * dst->w + dx, src->pix + (size_t)(sy + j) * src->w + sx, (size_t)w * 4);
}

uint32_t zwm_mix(uint32_t under, uint32_t over, int alpha)   /* alpha 0..255 */
{
    if (alpha >= 255) return over;
    if (alpha <= 0) return under;
    int ia = 255 - alpha;
    uint32_t r = ((under >> 16 & 255) * ia + (over >> 16 & 255) * alpha) / 255;
    uint32_t g = ((under >> 8 & 255) * ia + (over >> 8 & 255) * alpha) / 255;
    uint32_t b = ((under & 255) * ia + (over & 255) * alpha) / 255;
    return r << 16 | g << 8 | b;
}

void zwm_blend(zwm_surface *s, int x, int y, uint32_t color, int alpha)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    uint32_t *p = s->pix + (size_t)y * s->w + x;
    *p = zwm_mix(*p, color, alpha);
}

/* How much of pixel (px,py) a rounded rectangle covers, 4x4 samples, in
 * eighths of a pixel so the maths stays in integers. */
int zwm_round_cov(int px, int py, int x, int y, int w, int h, int r)
{
    if (r < 0) r = 0;
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    int inx = px >= x + r && px < x + w - r, iny = py >= y + r && py < y + h - r;
    if (inx || iny) return 16;
    int x8 = x * 8, y8 = y * 8, w8 = w * 8, h8 = h * 8, r8 = r * 8, n = 0;
    for (int j = 0; j < 4; j++) {
        int sy = py * 8 + 2 * j + 1;
        int dy = sy < y8 + r8 ? y8 + r8 - sy : sy >= y8 + h8 - r8 ? sy - (y8 + h8 - r8) : 0;
        for (int i = 0; i < 4; i++) {
            int sx = px * 8 + 2 * i + 1;
            int dx = sx < x8 + r8 ? x8 + r8 - sx : sx >= x8 + w8 - r8 ? sx - (x8 + w8 - r8) : 0;
            if (dx * dx + dy * dy <= r8 * r8) n++;
        }
    }
    return n;
}

void zwm_round_rect(zwm_surface *s, int x, int y, int w, int h, int r, uint32_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    zwm_fill(s, x + r, y, w - 2 * r, h, color);
    zwm_fill(s, x, y + r, r, h - 2 * r, color);
    zwm_fill(s, x + w - r, y + r, r, h - 2 * r, color);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int c;
            c = zwm_round_cov(x + i, y + j, x, y, w, h, r); zwm_blend(s, x + i, y + j, color, c * 255 / 16);
            c = zwm_round_cov(x + w - 1 - i, y + j, x, y, w, h, r); zwm_blend(s, x + w - 1 - i, y + j, color, c * 255 / 16);
            c = zwm_round_cov(x + i, y + h - 1 - j, x, y, w, h, r); zwm_blend(s, x + i, y + h - 1 - j, color, c * 255 / 16);
            c = zwm_round_cov(x + w - 1 - i, y + h - 1 - j, x, y, w, h, r); zwm_blend(s, x + w - 1 - i, y + h - 1 - j, color, c * 255 / 16);
        }
}

void zwm_round_rect_border(zwm_surface *s, int x, int y, int w, int h, int r, uint32_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    zwm_hline(s, x + r, y, w - 2 * r, color); zwm_hline(s, x + r, y + h - 1, w - 2 * r, color);
    zwm_vline(s, x, y + r, h - 2 * r, color); zwm_vline(s, x + w - 1, y + r, h - 2 * r, color);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int xs[2] = { x + i, x + w - 1 - i }, ys[2] = { y + j, y + h - 1 - j };
            for (int k = 0; k < 4; k++) {
                int px = xs[k & 1], py = ys[k >> 1];
                int c = zwm_round_cov(px, py, x, y, w, h, r) - zwm_round_cov(px, py, x + 1, y + 1, w - 2, h - 2, r - 1);
                if (c > 0) zwm_blend(s, px, py, color, c * 255 / 16);
            }
        }
}

void zwm_disc(zwm_surface *s, int cx, int cy, int r, uint32_t color)
{
    zwm_round_rect(s, cx - r, cy - r, 2 * r + 1, 2 * r + 1, r, color);
}

int zwm_text_width(const char *str, int scale)
{
    return (int)strlen(str) * 8 * scale;
}

int zwm_text(zwm_surface *s, int x, int y, const char *str, uint32_t color, int scale)
{
    if (scale < 1) scale = 1;
    int x0 = x;
    for (; *str; str++) {
        unsigned char ch = (unsigned char)*str;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *g = zwm_font8x8[ch - 0x20];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = g[gy];
            if (!bits) continue;
            for (int gx = 0; gx < 8; gx++)
                if (bits & (1 << gx))
                    zwm_fill(s, x + gx * scale, y + gy * scale, scale, scale, color);
        }
        x += 8 * scale;
    }
    return x - x0;
}
