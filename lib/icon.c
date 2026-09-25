/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* App icons: a `.zicon` section in the program's ELF ("ZICN", width,
 * height, RGBA), or the same bytes as a file. Drawn scaled with alpha. */
#include "zwm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *parse(const uint8_t *d, size_t n, int *w, int *h)
{
    if (n < 12 || memcmp(d, "ZICN", 4) != 0) return NULL;
    uint32_t iw = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24, ih = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
    if (iw == 0 || ih == 0 || iw > 256 || ih > 256 || n < 12 + (size_t)iw * ih * 4) return NULL;
    uint32_t *pix = malloc((size_t)iw * ih * 4);
    if (!pix) return NULL;
    const uint8_t *p = d + 12;
    for (size_t i = 0; i < (size_t)iw * ih; i++, p += 4)
        pix[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    *w = (int)iw; *h = (int)ih;
    return pix;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return d;
}

uint32_t *zwm_icon_file(const char *path, int *w, int *h)
{
    size_t n;
    uint8_t *d = slurp(path, &n);
    if (!d) return NULL;
    uint32_t *pix = parse(d, n, w, h);
    free(d);
    return pix;
}

static uint64_t rd(const uint8_t *p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i]; return v; }

/* The section named .zicon of a little-endian ELF32/ELF64 file. */
uint32_t *zwm_icon_load(const char *path, int *w, int *h)
{
    size_t n;
    uint8_t *d = slurp(path, &n);
    if (!d) return NULL;
    uint32_t *pix = NULL;
    if (n >= 64 && memcmp(d, "\x7f" "ELF", 4) == 0 && d[5] == 1) {
        int is64 = d[4] == 2;
        uint64_t shoff = is64 ? rd(d + 0x28, 8) : rd(d + 0x20, 4);
        unsigned shentsize = is64 ? rd(d + 0x3a, 2) : rd(d + 0x2e, 2), shnum = is64 ? rd(d + 0x3c, 2) : rd(d + 0x30, 2);
        unsigned shstrndx = is64 ? rd(d + 0x3e, 2) : rd(d + 0x32, 2);
        if (shoff && shentsize && shnum && shstrndx < shnum && shoff + (uint64_t)shentsize * shnum <= n) {
            const uint8_t *strsh = d + shoff + (uint64_t)shentsize * shstrndx;
            uint64_t stroff = is64 ? rd(strsh + 0x18, 8) : rd(strsh + 0x10, 4), strsz = is64 ? rd(strsh + 0x20, 8) : rd(strsh + 0x14, 4);
            for (unsigned i = 0; i < shnum && stroff + strsz <= n; i++) {
                const uint8_t *sh = d + shoff + (uint64_t)shentsize * i;
                uint32_t name = (uint32_t)rd(sh, 4);
                if (name >= strsz || strcmp((const char *)d + stroff + name, ".zicon") != 0) continue;
                uint64_t off = is64 ? rd(sh + 0x18, 8) : rd(sh + 0x10, 4), size = is64 ? rd(sh + 0x20, 8) : rd(sh + 0x14, 4);
                if (off + size <= n) pix = parse(d + off, size, w, h);
                break;
            }
        }
    } else
        pix = parse(d, n, w, h);
    free(d);
    return pix;
}

/* Box-filtered to `size` pixels square, alpha over the surface. */
void zwm_icon_draw(zwm_surface *s, int x, int y, int size, const uint32_t *pix, int w, int h)
{
    if (!pix || size <= 0) return;
    for (int j = 0; j < size; j++) {
        int y0 = j * h / size, y1 = (j + 1) * h / size; if (y1 <= y0) y1 = y0 + 1;
        for (int i = 0; i < size; i++) {
            int x0 = i * w / size, x1 = (i + 1) * w / size; if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    uint32_t p = pix[yy * w + xx], pa = p >> 24;
                    r += (p >> 16 & 255) * pa; g += (p >> 8 & 255) * pa; b += (p & 255) * pa; a += pa; n++;
                }
            if (!a || !n) continue;
            zwm_blend(s, x + i, y + j, (r / a) << 16 | (g / a) << 8 | (b / a), (int)(a / n));
        }
    }
}
