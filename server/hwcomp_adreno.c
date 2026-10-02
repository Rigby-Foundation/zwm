/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * zwm's GPU compositor on a phone's Adreno 6xx (zgl's libadreno over sic's
 * /dev/adrenogpu): see hwcomp.h. Only the 2D engine runs, which fills and
 * copies but cannot blend. A frame is drawn into a back buffer in GPU
 * memory: the background, every window's content and the opaque part of
 * its decoration are fills and copies on the GPU; the decoration's
 * translucent pixels (the shadow, antialiased corners) and the cursor are
 * blended by the CPU once the GPU has drawn what lies under them. The GPU
 * then copies the back buffer to the screen. Images live in GPU buffers,
 * which we map uncached: the CPU writes them freely and reads back only
 * the few pixels it blends.
 */
#include "hwcomp.h"
#include <adreno_hw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct run { int x0, x1; };                 /* [x0, x1) of a row */

struct hw_win {
    struct adreno_bo content;
    uint32_t pitch;
    int tex_w, tex_h;
    struct adreno_bo deco;
    uint32_t deco_pitch;
    int deco_w, deco_h, deco_mid;
    uint32_t *deco_pix;                     /* a cached copy, for blending */
    /* per decoration row y: the opaque runs [solid_at[y], solid_at[y + 1]) of
     * solid, which the GPU copies, and the translucent ones of soft, which
     * the CPU blends; same_above[y]: row y's opaque runs are row y - 1's */
    struct run *solid, *soft;
    int *solid_at, *soft_at;
    unsigned char *same_above;
};

static struct adreno_hw gpu;
static struct adreno_bo back;               /* the frame being drawn */
static uint32_t back_pitch;
static int W, H;
static uint32_t *row_tmp;                   /* a row of the back buffer, while blending */

static uint32_t align64(uint32_t v) { return (v + 63) & ~63u; }

/* The CPU's writes to (uncached, write-combined) GPU buffers reach memory
 * before the GPU is told to run. */
static void wmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb st" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static void submit(void) { wmb(); adreno_commit(&gpu); }

/* Everything queued has run and is in memory: the CPU may read or blend. */
static void finish(void)
{
    wmb();
    if (adreno_finish(&gpu) != 0) fprintf(stderr, "zwm: the Adreno did not finish a frame\n");
}

/* Before a GPU operation: room for it, and a submission starts by dropping
 * what the GPU's caches hold of what the CPU wrote since the last one. */
static void gpu_op(void)
{
    if (adreno_space(&gpu) < 256) submit();
    if (!gpu.used) adreno_invalidate(&gpu);
}

/* A copy into the back buffer, clipped to it: (sx, sy, w, sh) of src to
 * (dx, dy, w, dh), either unscaled (sh == dh) or one row stretched. */
static void put(const struct adreno_bo *src, uint32_t spitch, int sx, int sy, int sh, int dx, int dy, int w, int dh)
{
    int stretch = sh != dh;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dx + w > W) w = W - dx;
    if (dy < 0) { if (!stretch) { sy -= dy; sh += dy; } dh += dy; dy = 0; }
    if (dy + dh > H) { dh = H - dy; if (!stretch) sh = dh; }
    if (w <= 0 || dh <= 0 || sh <= 0) return;
    gpu_op();
    adreno_2d_copy(&gpu, src->gpuaddr, spitch, sx, sy, w, sh, back.gpuaddr, back_pitch, dx, dy, w, dh);
}

/* Premultiplied pixels over the back buffer at (x, y), by the CPU (finish() first). */
static void blend_row(const uint32_t *src, int n, int x, int y)
{
    if (y < 0 || y >= H) return;
    if (x < 0) { src -= x; n += x; x = 0; }
    if (x + n > W) n = W - x;
    if (n <= 0) return;
    uint32_t *d = (uint32_t *)(back.map + (size_t)y * back_pitch) + x;
    memcpy(row_tmp, d, (size_t)n * 4);      /* one burst from uncached memory */
    for (int i = 0; i < n; i++) {
        uint32_t s = src[i], a = s >> 24, ia = 255 - a, o = row_tmp[i];
        if (!a) continue;
        uint32_t r = (s >> 16 & 255) + ((o >> 16 & 255) * ia + 127) / 255;
        uint32_t g = (s >> 8 & 255) + ((o >> 8 & 255) * ia + 127) / 255;
        uint32_t b = (s & 255) + ((o & 255) * ia + 127) / 255;
        row_tmp[i] = 0xFF000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
    }
    memcpy(d, row_tmp, (size_t)n * 4);
}

static int back_new(int w, int h)
{
    if (w <= 0 || h <= 0 || (uint32_t)w > gpu.width || (uint32_t)h > gpu.height) return -1;
    uint32_t pitch = align64((uint32_t)w * 4);
    struct adreno_bo bo;
    uint32_t *tmp = malloc((size_t)w * 4);
    if (!tmp || adreno_bo_new(&gpu, pitch * (uint32_t)h, &bo) != 0) { free(tmp); return -1; }
    if (back.handle) { finish(); adreno_bo_free(&gpu, &back); }
    free(row_tmp);
    back = bo; back_pitch = pitch; row_tmp = tmp;
    W = w; H = h;
    return 0;
}

/* Copies the way the compositor uses them, checked on a scratch buffer
 * before the GPU gets the display: unscaled, and one row stretched. */
static int selftest(void)
{
    struct adreno_bo t;
    if (adreno_bo_new(&gpu, 256 * 16, &t) != 0) return -1;
    volatile uint32_t *p = (volatile uint32_t *)t.map;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 16; x++) p[y * 64 + x] = 0xFF000000u | (uint32_t)y << 16 | (uint32_t)x << 8 | 0x5a;
    gpu_op();
    adreno_2d_copy(&gpu, t.gpuaddr, 256, 0, 0, 16, 4, t.gpuaddr, 256, 32, 4, 16, 4);
    adreno_2d_copy(&gpu, t.gpuaddr, 256, 0, 1, 16, 1, t.gpuaddr, 256, 16, 8, 16, 4);
    finish();
    int ok = 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 16; x++) {
            if (p[(4 + y) * 64 + 32 + x] != p[y * 64 + x]) ok = 0;
            if (p[(8 + y) * 64 + 16 + x] != p[64 + x]) ok = 0;
        }
    if (!ok) fprintf(stderr, "zwm: the Adreno's copies came out wrong ((32,4) %08x, want %08x)\n", p[4 * 64 + 32], p[0]);
    adreno_bo_free(&gpu, &t);
    return ok ? 0 : -1;
}

static int a_init(int w, int h)
{
    if (adreno_open(&gpu) != 0) return -1;
    /* the 2D engine wants 64-byte-aligned surfaces: the screen, or its even and odd rows */
    if ((gpu.fb_gpuaddr & 63) || ((gpu.fb_pitch & 63) && ((2 * gpu.fb_pitch) & 63)) || back_new(w, h) != 0 || selftest() != 0) {
        if (back.handle) adreno_bo_free(&gpu, &back);
        free(row_tmp); row_tmp = NULL;
        adreno_close(&gpu);
        return -1;
    }
    gpu_op();
    adreno_2d_fill(&gpu, back.gpuaddr, back_pitch, 0, 0, W, H, 0xFF000000u);
    adreno_2d_copy_to_screen(&gpu, back.gpuaddr, back_pitch, 0, 0, W, H, 0, 0);
    finish();
    return 0;
}

static int a_resize(int w, int h) { return back_new(w, h); }

static struct hw_win *a_win_new(void) { return calloc(1, sizeof(struct hw_win)); }

static void deco_drop(struct hw_win *hw)
{
    free(hw->deco_pix); free(hw->solid); free(hw->soft); free(hw->solid_at); free(hw->soft_at); free(hw->same_above);
    hw->deco_pix = NULL; hw->solid = hw->soft = NULL; hw->solid_at = hw->soft_at = NULL; hw->same_above = NULL;
    hw->deco_w = hw->deco_h = 0;
}

static void a_win_free(struct hw_win *hw)
{
    if (!hw) return;
    adreno_bo_free(&gpu, &hw->content);     /* the kernel waits for the GPU to be done with them */
    adreno_bo_free(&gpu, &hw->deco);
    deco_drop(hw);
    free(hw);
}

/* A buffer for w x h pixels, kept if it is big enough. */
static int bo_fit(struct adreno_bo *bo, uint32_t *pitch, int w, int h)
{
    uint32_t p = align64((uint32_t)w * 4);
    if (bo->handle && p * (uint32_t)h <= bo->size) { *pitch = p; return 0; }
    adreno_bo_free(&gpu, bo);
    if (adreno_bo_new(&gpu, p * (uint32_t)h, bo) != 0) { fprintf(stderr, "zwm: no GPU memory for a %dx%d window\n", w, h); return -1; }
    *pitch = p;
    return 0;
}

/* Runs of a row where keep(alpha) holds. */
static int runs(const uint32_t *row, int w, int opaque, struct run **out, int *n, int *cap)
{
    for (int x = 0; x < w;) {
        uint32_t a = row[x] >> 24;
        if (opaque ? a != 255 : a == 0 || a == 255) { x++; continue; }
        int x0 = x;
        while (x < w && (opaque ? row[x] >> 24 == 255 : (row[x] >> 24) != 0 && row[x] >> 24 != 255)) x++;
        if (*n == *cap) {
            struct run *r = realloc(*out, (size_t)(*cap ? *cap * 2 : 64) * sizeof **out);
            if (!r) return -1;
            *out = r; *cap = *cap ? *cap * 2 : 64;
        }
        (*out)[(*n)++] = (struct run){ x0, x };
    }
    return 0;
}

static void a_win_deco(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid)
{
    deco_drop(hw);
    if (w <= 0 || h <= 0 || bo_fit(&hw->deco, &hw->deco_pitch, w, h) != 0) return;
    hw->deco_pix = malloc((size_t)w * h * 4);
    hw->solid_at = malloc((size_t)(h + 1) * sizeof(int));
    hw->soft_at = malloc((size_t)(h + 1) * sizeof(int));
    hw->same_above = calloc((size_t)h, 1);
    if (!hw->deco_pix || !hw->solid_at || !hw->soft_at || !hw->same_above) { deco_drop(hw); return; }
    memcpy(hw->deco_pix, argb, (size_t)w * h * 4);
    int ns = 0, cs = 0, nt = 0, ct = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = argb + (size_t)y * w;
        memcpy((void *)(hw->deco.map + (size_t)y * hw->deco_pitch), row, (size_t)w * 4);
        hw->solid_at[y] = ns; hw->soft_at[y] = nt;
        if (runs(row, w, 1, &hw->solid, &ns, &cs) != 0 || runs(row, w, 0, &hw->soft, &nt, &ct) != 0) { deco_drop(hw); return; }
        if (y > 0) {
            int a0 = hw->solid_at[y - 1], n0 = hw->solid_at[y] - a0, n1 = ns - hw->solid_at[y];
            hw->same_above[y] = n0 == n1 && (!n1 || !memcmp(hw->solid + a0, hw->solid + hw->solid_at[y], (size_t)n1 * sizeof(struct run)));
        }
    }
    hw->solid_at[h] = ns; hw->soft_at[h] = nt;
    hw->deco_w = w; hw->deco_h = h; hw->deco_mid = mid < 0 ? 0 : mid >= h ? h - 1 : mid;
}

static void a_win_content(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride)
{
    if (w <= 0 || h <= 0) return;
    if (hw->tex_w != w || hw->tex_h != h) {
        hw->tex_w = hw->tex_h = 0;
        if (bo_fit(&hw->content, &hw->pitch, w, h) != 0) return;
        hw->tex_w = w; hw->tex_h = h;
    }
    if (x < 0 || y < 0 || x + cw > w || y + ch > h) return;
    for (int j = 0; j < ch; j++)
        memcpy((void *)(hw->content.map + (size_t)(y + j) * hw->pitch + (size_t)x * 4), pix + (size_t)j * stride, (size_t)cw * 4);
}

/* Clients' GL buffers are virgl resources: there are none on this GPU. */
static int a_win_content_gpu(struct hw_win *hw, uint32_t res, int w, int h) { (void)hw; (void)res; (void)w; (void)h; return -1; }

static void a_begin(uint32_t bg)
{
    gpu_op();
    adreno_2d_fill(&gpu, back.gpuaddr, back_pitch, 0, 0, W, H, 0xFF000000u | bg);
}

/* Rows [s0, s0 + n) of the decoration at row d0 of the window's (dx, dy), or row s0 stretched over n rows. */
struct seg { int s0, d0, n, stretch; };

static void a_draw_win(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius)
{
    if (cw > hw->tex_w) cw = hw->tex_w;
    if (ch > hw->tex_h) ch = hw->tex_h;
    if (cw > 0 && ch > 0) {
        /* opaque: whatever alpha the client left in its buffer */
        int r = radius > 0 && radius <= ch && 2 * radius <= cw ? radius : 0;
        put(&hw->content, hw->pitch, 0, 0, ch - r, cx, cy, cw, ch - r);
        for (int i = ch - r; i < ch; i++) {     /* the bottom corners follow the frame's rounding */
            float d = i + 0.5f - (ch - r);
            int in = (int)(r - sqrtf((float)r * r - d * d) + 0.5f);
            put(&hw->content, hw->pitch, in, i, 1, cx + in, cy + i, cw - 2 * in, 1);
        }
    }
    if (hw->deco_w <= 0) return;
    int m = hw->deco_mid, below = hw->deco_h - m - 1;   /* rows after the repeated one */
    struct seg seg[3];
    int nseg = 0;
    if (dh <= hw->deco_h) seg[nseg++] = (struct seg){ 0, 0, hw->deco_h, 0 };
    else {
        seg[nseg++] = (struct seg){ 0, 0, m, 0 };
        seg[nseg++] = (struct seg){ m, m, dh - m - below, 1 };
        seg[nseg++] = (struct seg){ m + 1, dh - below, below, 0 };
    }
    int soft = 0;
    for (int k = 0; k < nseg; k++) {
        struct seg s = seg[k];
        if (s.n <= 0) continue;
        if (s.stretch) {
            for (int i = hw->solid_at[s.s0]; i < hw->solid_at[s.s0 + 1]; i++)
                put(&hw->deco, hw->deco_pitch, hw->solid[i].x0, s.s0, 1, dx + hw->solid[i].x0, dy + s.d0, hw->solid[i].x1 - hw->solid[i].x0, s.n);
            soft |= hw->soft_at[s.s0 + 1] > hw->soft_at[s.s0];
            continue;
        }
        for (int y = s.s0; y < s.s0 + s.n;) {     /* rows alike in what is opaque go as one copy each run */
            int y1 = y + 1;
            while (y1 < s.s0 + s.n && hw->same_above[y1]) y1++;
            for (int i = hw->solid_at[y]; i < hw->solid_at[y + 1]; i++)
                put(&hw->deco, hw->deco_pitch, hw->solid[i].x0, y, y1 - y, dx + hw->solid[i].x0, dy + s.d0 + y - s.s0, hw->solid[i].x1 - hw->solid[i].x0, y1 - y);
            y = y1;
        }
        soft |= hw->soft_at[s.s0 + s.n] > hw->soft_at[s.s0];
    }
    if (!soft) return;
    finish();                               /* what lies under the shadow is drawn */
    for (int k = 0; k < nseg; k++) {
        struct seg s = seg[k];
        for (int j = 0; j < s.n; j++) {
            int sy = s.stretch ? s.s0 : s.s0 + j, y = dy + s.d0 + j;
            if (y < 0 || y >= H) continue;
            const uint32_t *row = hw->deco_pix + (size_t)sy * hw->deco_w;
            for (int i = hw->soft_at[sy]; i < hw->soft_at[sy + 1]; i++)
                blend_row(row + hw->soft[i].x0, hw->soft[i].x1 - hw->soft[i].x0, dx + hw->soft[i].x0, y);
        }
    }
}

static void a_draw_cursor(const uint32_t *argb, int w, int h, int x, int y)
{
    finish();
    for (int j = 0; j < h; j++) blend_row(argb + (size_t)j * w, w, x, y + j);
}

static void a_end(void)
{
    finish();                               /* (nothing queued after the cursor, usually) */
    gpu_op();
    adreno_2d_copy_to_screen(&gpu, back.gpuaddr, back_pitch, 0, 0, W, H, 0, 0);
    submit();                               /* the next frame's GPU work queues behind it */
}

static int a_read(uint32_t *pix)
{
    finish();
    for (int y = 0; y < H; y++) {
        memcpy(pix + (size_t)y * W, (const void *)(back.map + (size_t)y * back_pitch), (size_t)W * 4);
        for (int x = 0; x < W; x++) pix[(size_t)y * W + x] &= 0xFFFFFF;
    }
    return 0;
}

const struct hw_backend hwcomp_adreno = {
    "adreno", a_init, a_resize, a_win_new, a_win_free, a_win_deco, a_win_content, a_win_content_gpu,
    a_begin, a_draw_win, a_draw_cursor, a_end, a_read,
};
