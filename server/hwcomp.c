/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* zwm's GPU compositor: the backend that takes the display (see hwcomp.h).
 * Built without HWCOMP_ZGL (no zgl in the sysroot, e.g. powerpc) there is
 * none and zwm composes in software. */
#include "hwcomp.h"
#include <stddef.h>

static const struct hw_backend *const backends[] = {
#ifdef HWCOMP_ZGL
    &hwcomp_gl,                 /* virgl: OpenGL on the host's GPU */
    &hwcomp_adreno,             /* a phone's Adreno: /dev/adrenogpu */
#endif
    NULL
};
static const struct hw_backend *be;

int hw_init(int w, int h)
{
    for (int i = 0; backends[i]; i++)
        if (backends[i]->init(w, h) == 0) { be = backends[i]; return 0; }
    return -1;
}

const char *hw_name(void) { return be ? be->name : "none"; }
int  hw_resize(int w, int h) { return be ? be->resize(w, h) : -1; }
struct hw_win *hw_win_new(void) { return be ? be->win_new() : NULL; }
void hw_win_free(struct hw_win *hw) { if (be && hw) be->win_free(hw); }
void hw_win_deco(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid) { if (be && hw) be->win_deco(hw, argb, w, h, mid); }
void hw_win_content(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride)
{ if (be && hw) be->win_content(hw, w, h, x, y, cw, ch, pix, stride); }
int  hw_win_content_gpu(struct hw_win *hw, uint32_t res, int w, int h) { return be && hw ? be->win_content_gpu(hw, res, w, h) : -1; }
void hw_begin(uint32_t bg) { if (be) be->begin(bg); }
void hw_draw_win(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius)
{ if (be && hw) be->draw_win(hw, dx, dy, dh, cx, cy, cw, ch, radius); }
void hw_draw_cursor(const uint32_t *argb, int w, int h, int x, int y) { if (be) be->draw_cursor(argb, w, h, x, y); }
void hw_end(void) { if (be) be->end(); }
int  hw_read(uint32_t *pix) { return be ? be->read(pix) : -1; }
