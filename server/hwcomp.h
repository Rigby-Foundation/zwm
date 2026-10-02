/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * zwm's GPU compositor: every frame is drawn on the GPU from per-window
 * images: its decoration (drawn in software when it changes, premultiplied
 * alpha) and its content, uploaded from the window's pixels where they
 * changed or, with GL, the client's own buffer sampled in place.
 * hwcomp.c hands these calls to the first backend that takes the display:
 * hwcomp_gl.c (zgl on virgl: the screen is zgl's colour target, made the
 * scanout), then hwcomp_adreno.c (a phone's Adreno 6xx, its 2D engine).
 * Without zgl in the sysroot there are none: hw_init() fails and zwm
 * composes in software.
 */
#pragma once
#include <stdint.h>

struct hw_win;

int  hw_init(int w, int h);                 /* 0: the GPU has the display */
const char *hw_name(void);                  /* which backend has it */
int  hw_resize(int w, int h);

struct hw_win *hw_win_new(void);
void hw_win_free(struct hw_win *hw);
/* The decoration, w x h, 0xAARRGGBB premultiplied, drawn for a window
 * shorter than the real one: row `mid` repeats to make up the height. */
void hw_win_deco(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid);
/* Content from memory: the texture is w x h; (x, y, cw, ch) of it changes to `pix` (rows `stride` apart). */
void hw_win_content(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride);
/* Content that is a GPU resource (bottom-up rows); 0 = back to memory. */
int  hw_win_content_gpu(struct hw_win *hw, uint32_t res, int w, int h);

void hw_begin(uint32_t bg);
/* The content in (cx, cy, cw, ch), its bottom corners rounded by
 * `radius`, then the decoration at (dx, dy), dh tall; either may be missing. */
void hw_draw_win(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius);
void hw_draw_cursor(const uint32_t *argb, int w, int h, int x, int y);
void hw_end(void);                          /* put it on screen */
int  hw_read(uint32_t *pix);                /* the screen, top row first, 0x00RRGGBB */

/* A backend: the calls above, from init to read. */
struct hw_backend {
    const char *name;
    int  (*init)(int w, int h);
    int  (*resize)(int w, int h);
    struct hw_win *(*win_new)(void);
    void (*win_free)(struct hw_win *hw);
    void (*win_deco)(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid);
    void (*win_content)(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride);
    int  (*win_content_gpu)(struct hw_win *hw, uint32_t res, int w, int h);
    void (*begin)(uint32_t bg);
    void (*draw_win)(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius);
    void (*draw_cursor)(const uint32_t *argb, int w, int h, int x, int y);
    void (*end)(void);
    int  (*read)(uint32_t *pix);
};
extern const struct hw_backend hwcomp_gl, hwcomp_adreno;
