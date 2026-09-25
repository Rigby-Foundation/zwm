/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* No zgl in the sysroot (e.g. powerpc): zwm composes in software. */
#include "hwcomp.h"
#include <stddef.h>

int  hw_init(int w, int h) { (void)w; (void)h; return -1; }
int  hw_resize(int w, int h) { (void)w; (void)h; return -1; }
struct hw_win *hw_win_new(void) { return NULL; }
void hw_win_free(struct hw_win *hw) { (void)hw; }
void hw_win_deco(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid) { (void)hw; (void)argb; (void)w; (void)h; (void)mid; }
void hw_win_content(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride)
{ (void)hw; (void)w; (void)h; (void)x; (void)y; (void)cw; (void)ch; (void)pix; (void)stride; }
int  hw_win_content_gpu(struct hw_win *hw, uint32_t res, int w, int h) { (void)hw; (void)res; (void)w; (void)h; return -1; }
void hw_begin(uint32_t bg) { (void)bg; }
void hw_draw_win(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius)
{ (void)hw; (void)dx; (void)dy; (void)dh; (void)cx; (void)cy; (void)cw; (void)ch; (void)radius; }
void hw_draw_cursor(const uint32_t *argb, int w, int h, int x, int y) { (void)argb; (void)w; (void)h; (void)x; (void)y; }
void hw_end(void) {}
int  hw_read(uint32_t *pix) { (void)pix; return -1; }
