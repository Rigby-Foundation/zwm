/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* zwm's GPU compositor, on zgl: see hwcomp.h. */
#include "hwcomp.h"
#include <GL/gl.h>
#include <GL/sic_gl.h>
#include <GL/zgl_ext.h>
#include <stdlib.h>
#include <stdio.h>

struct hw_win {
    GLuint deco, content;
    int deco_w, deco_h, deco_mid;
    int tex_w, tex_h;           /* the content texture's size */
    int gpu;                    /* content is a client's resource (rows bottom-up) */
    uint32_t res;
};

static sic_gl_context *ctx;
static int W, H;
static GLuint cursor_tex;
static const uint32_t *cursor_pix;

static void tex_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void setup(void)
{
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    /* pixels, y down, and row 0 of the buffer is the top of the screen:
     * the display scans the buffer out top row first (virgl reports its
     * resources as y-0-top), not GL's bottom-up way */
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   /* premultiplied */
    glColor4f(1, 1, 1, 1);
}

int hw_init(int w, int h)
{
    ctx = sic_gl_create(w, h);
    if (!ctx) return -1;
    sic_gl_make_current(ctx);
    W = w; H = h;
    setup();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (sic_gl_scanout(ctx) != 0) { sic_gl_destroy(ctx); ctx = NULL; return -1; }
    return 0;
}

int hw_resize(int w, int h)
{
    if (!ctx) return -1;
    if (sic_gl_resize(ctx, w, h) != 0) return -1;
    W = w; H = h;
    setup();
    return sic_gl_scanout(ctx);
}

struct hw_win *hw_win_new(void)
{
    struct hw_win *hw = calloc(1, sizeof *hw);
    if (!hw) return NULL;
    glGenTextures(1, &hw->deco);
    glGenTextures(1, &hw->content);
    return hw;
}

void hw_win_free(struct hw_win *hw)
{
    if (!hw) return;
    glDeleteTextures(1, &hw->deco);
    glDeleteTextures(1, &hw->content);
    free(hw);
}

void hw_win_deco(struct hw_win *hw, const uint32_t *argb, int w, int h, int mid)
{
    hw->deco_mid = mid;
    glBindTexture(GL_TEXTURE_2D, hw->deco);
    tex_params();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    hw->deco_w = w; hw->deco_h = h;
}

void hw_win_content(struct hw_win *hw, int w, int h, int x, int y, int cw, int ch, const uint32_t *pix, int stride)
{
    if (hw->gpu || hw->tex_w != w || hw->tex_h != h) {
        if (hw->gpu) { glDeleteTextures(1, &hw->content); glGenTextures(1, &hw->content); }   /* let go of the client's buffer */
        glBindTexture(GL_TEXTURE_2D, hw->content);
        tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        hw->tex_w = w; hw->tex_h = h; hw->gpu = 0; hw->res = 0;
    } else glBindTexture(GL_TEXTURE_2D, hw->content);
    zglTexSubImageXRGB(x, y, cw, ch, pix, stride);
}

int hw_win_content_gpu(struct hw_win *hw, uint32_t res, int w, int h)
{
    if (hw->gpu && hw->res == res && hw->tex_w == w && hw->tex_h == h) return 0;
    glDeleteTextures(1, &hw->content);
    glGenTextures(1, &hw->content);
    hw->gpu = 0; hw->res = 0; hw->tex_w = hw->tex_h = 0;
    if (zglTextureFromResource(hw->content, res, w, h) != 0) return -1;
    glBindTexture(GL_TEXTURE_2D, hw->content);
    tex_params();
    hw->gpu = 1; hw->res = res; hw->tex_w = w; hw->tex_h = h;
    return 0;
}

void hw_begin(uint32_t bg)
{
    sic_gl_make_current(ctx);
    glClearColor((bg >> 16 & 255) / 255.0f, (bg >> 8 & 255) / 255.0f, (bg & 255) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void quad(float x, float y, float w, float h, float s0, float t0, float s1, float t1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(x, y);
    glTexCoord2f(s1, t0); glVertex2f(x + w, y);
    glTexCoord2f(s1, t1); glVertex2f(x + w, y + h);
    glTexCoord2f(s0, t1); glVertex2f(x, y + h);
    glEnd();
}

/* A point of the content outline, with its texture coordinate. */
static void cvert(const struct hw_win *hw, int cx, int cy, float x, float y)
{
    float s = (x - cx) / hw->tex_w, t = (y - cy) / hw->tex_h;
    glTexCoord2f(s, hw->gpu ? 1 - t : t);
    glVertex2f(x, y);
}

/* cos/sin of 0, 1/8 .. 8/8 of a right angle */
static const float arc_c[9] = { 1, 0.98079f, 0.92388f, 0.83147f, 0.70711f, 0.55557f, 0.38268f, 0.19509f, 0 };

void hw_draw_win(struct hw_win *hw, int dx, int dy, int dh, int cx, int cy, int cw, int ch, int radius)
{
    if (cw > 0 && ch > 0 && hw->tex_w > 0) {
        /* opaque: whatever alpha the client left in its buffer */
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, hw->content);
        if (radius <= 0) {
            float s1 = (float)cw / hw->tex_w, t1 = (float)ch / hw->tex_h;
            if (hw->gpu) quad(cx, cy, cw, ch, 0, 1, s1, 1 - t1);
            else quad(cx, cy, cw, ch, 0, 0, s1, t1);
        } else {
            /* the bottom corners follow the frame's rounding */
            float r = radius, bx = cx + cw, by = cy + ch;
            glBegin(GL_TRIANGLE_FAN);
            cvert(hw, cx, cy, cx + cw * 0.5f, cy + ch * 0.5f);
            cvert(hw, cx, cy, cx, cy);
            cvert(hw, cx, cy, bx, cy);
            for (int i = 0; i <= 8; i++)            /* bottom right, from its right edge down */
                cvert(hw, cx, cy, bx - r + r * arc_c[i], by - r + r * arc_c[8 - i]);
            for (int i = 0; i <= 8; i++)            /* bottom left, from its bottom edge up */
                cvert(hw, cx, cy, cx + r - r * arc_c[8 - i], by - r + r * arc_c[i]);
            cvert(hw, cx, cy, cx, cy);
            glEnd();
        }
    }
    if (hw->deco_w > 0) {
        glEnable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, hw->deco);
        float tw = hw->deco_w, th = hw->deco_h;
        int m = hw->deco_mid, below = hw->deco_h - m - 1;   /* rows after the repeated one */
        if (dh <= hw->deco_h) quad(dx, dy, tw, th, 0, 0, 1, 1);
        else {
            quad(dx, dy, tw, m, 0, 0, 1, m / th);
            quad(dx, dy + m, tw, dh - m - below, 0, (m + 0.5f) / th, 1, (m + 0.5f) / th);
            quad(dx, dy + dh - below, tw, below, 0, (m + 1) / th, 1, 1);
        }
    }
}

void hw_draw_cursor(const uint32_t *argb, int w, int h, int x, int y)
{
    if (!cursor_tex) glGenTextures(1, &cursor_tex);
    glBindTexture(GL_TEXTURE_2D, cursor_tex);
    if (cursor_pix != argb) {
        tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        cursor_pix = argb;
    }
    glEnable(GL_BLEND);
    quad(x, y, w, h, 0, 0, 1, 1);
}

void hw_end(void)
{
    sic_gl_present(ctx, 0, 0, W, H);
}

int hw_read(uint32_t *pix)
{
    sic_gl_make_current(ctx);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, pix);      /* row 0 is the top (see setup) */
    for (size_t i = 0; i < (size_t)W * H; i++) pix[i] &= 0xFFFFFF;
    return 0;
}
