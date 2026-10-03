/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * zwm: the window server and manager for sic, in one process.
 *
 * Owns /dev/fb0 (the only thing drawing on it), /dev/console in raw
 * scancode mode and /dev/mouse; listens on the AF_UNIX socket /run/zwm for clients
 * (libzwm). Every window is a pixel buffer: the client's own, shared with
 * us through /dev/shmem, or one here that it fills with blits;
 * the server composes the stack with title bars, borders and a cursor
 * into a back buffer and copies the damaged part to the framebuffer.
 *
 * Management is deliberately plain: click to focus and raise, drag the
 * title bar to move, drag the bottom-right corner to resize, the box at
 * the right of the title bar asks the client to close. Docks
 * (ZWM_DOCK_*) are pinned strips that keep the others out of their way.
 * Ctrl+Alt+Q ends the session and gives the console back.
 */
#include "zwm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <abi/shm.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <abi/mouse.h>
#include "hwcomp.h"

/* Frame metrics, in units of the UI scale (1, or 2 on a HiDPI screen). */
static int S = 1;
#define TITLE_H   (28 * S)
#define BORDER    (1 * S)
#define CTL_W     (26 * S)          /* each title bar control (minimize, maximize, close) */
#define CTL_N     3
#define GRIP      (14 * S)
#define RADIUS    (10 * S)          /* the frame's corners */
#define SHADOW    (18 * S)          /* how far the drop shadow reaches */
#define SHADOW_DY (6 * S)           /* ... and how far down it is shifted */
#define RADIUS_MAX 20
#define SHADOW_A  110               /* how dark it is right at the frame, 0..255 */
#define MAX_CLIENTS 32

struct client;
struct win {
    uint32_t id;
    struct client *owner;
    int x, y, w, h;             /* the client area, in screen coordinates */
    uint32_t flags;
    char title[64];
    char exe[128];              /* the client program, for its icon in a dock */
    uint32_t *pix;              /* w*h, ours: what blits land in */
    uint32_t *shm; int shm_fd, shm_w, shm_h, shm_stride; size_t shm_len;   /* the client's, if attached */
    int minimized, maximized;
    int sx, sy, sw, sh;         /* the geometry to come back to from maximized */
    /* GPU composition: the textures, what of the content changed (window
     * coordinates), the client's GL buffer if the content is one, and
     * what the decoration texture was drawn for */
    struct hw_win *hw;
    int cd_x0, cd_y0, cd_x1, cd_y1;
    uint32_t gpu_res; int gpu_w, gpu_h;
    int frame_wanted;           /* its client waits for ZWM_S_FRAME */
    struct { int fw, ch, active, hot, max; char title[64]; } deco_key;
    struct win *above;          /* stacking: the list runs bottom to top */
    struct win *below;
};

struct client {
    int fd;
    uint8_t *in; size_t in_len, in_cap;
};

static struct { int fd; uint32_t *mem; int w, h, pitch; } fb;
static zwm_surface *back;                   /* the composed screen */
static int con_fd, mouse_fd, listen_fd;
static struct client *clients[MAX_CLIENTS];
static struct win *bottom, *top, *focus;
static uint32_t next_id = 1;
static int mx, my, mbuttons;                /* cursor */
static int damage_x0, damage_y0, damage_x1, damage_y1;
static int workarea_top, workarea_bottom;   /* rows docks have taken */
static int safe_top;                        /* rows a camera cutout hides: docks go below them */
static int quit;
static int gpu;                             /* the GPU composites (hwcomp.c) */
#define FRAME_MS 16                         /* ... at most this often: the display's pace */
static int flush_wait_ms = -1;              /* poll timeout: a frame is due then */
static int panel_fd = -1;                   /* /dev/panel: a phone's panel, which the Power key turns off */
static int screen_off;                      /* then input is ignored and nothing is composed */

/* drag state */
static struct { struct win *w; int mode; int dx, dy; } drag;   /* mode 1 = move, 2 = resize */
static struct win *hover_w; static int hover_ctl;               /* the title bar control under the cursor (2 close, 4 min, 5 max) */
static struct win *mouse_win;                                   /* the window the cursor was last over (its client area) */
static int windows_dirty;                                       /* the taskbar list wants sending */

/* ---- damage & composition ------------------------------------------------ */

static void damage(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb.w) w = fb.w - x;
    if (y + h > fb.h) h = fb.h - y;
    if (w <= 0 || h <= 0) return;
    if (damage_x1 <= damage_x0) { damage_x0 = x; damage_y0 = y; damage_x1 = x + w; damage_y1 = y + h; return; }
    if (x < damage_x0) damage_x0 = x;
    if (y < damage_y0) damage_y0 = y;
    if (x + w > damage_x1) damage_x1 = x + w;
    if (y + h > damage_y1) damage_y1 = y + h;
}

static int decorated(const struct win *w) { return !(w->flags & (ZWM_UNDECORATED | ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP)); }

/* The window's full footprint including decorations. */
static void frame_rect(const struct win *w, int *x, int *y, int *fw, int *fh)
{
    if (decorated(w)) { *x = w->x - BORDER; *y = w->y - TITLE_H - BORDER; *fw = w->w + 2 * BORDER; *fh = w->h + TITLE_H + 2 * BORDER; }
    else { *x = w->x; *y = w->y; *fw = w->w; *fh = w->h; }
}

static void damage_win(const struct win *w) { int x, y, fw, fh; frame_rect(w, &x, &y, &fw, &fh); damage(x - SHADOW, y - SHADOW, fw + 2 * SHADOW, fh + 2 * SHADOW + SHADOW_DY); }

static float fsqrt(float v)         /* libm is not around; Newton is plenty for a shadow */
{
    if (v <= 0) return 0;
    float x = v > 1 ? v : 1;
    for (int i = 0; i < 6; i++) x = 0.5f * (x + v / x);
    return x;
}

/* The drop shadow: darken the back buffer around the frame, falling off
 * with the distance to the rounded rectangle, shifted down a little. */
static void shadow(int fx, int fy, int fw, int fh)
{
    int top = fy + RADIUS, bottom = fy + fh - RADIUS;      /* rows where the frame covers everything in fx..fx+fw */
    fy += SHADOW_DY;
    int x0 = fx - SHADOW, y0 = fy - SHADOW, x1 = fx + fw + SHADOW, y1 = fy + fh + SHADOW;
    if (x0 < damage_x0) x0 = damage_x0; if (y0 < damage_y0) y0 = damage_y0;
    if (x1 > damage_x1) x1 = damage_x1; if (y1 > damage_y1) y1 = damage_y1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > back->w) x1 = back->w; if (y1 > back->h) y1 = back->h;
    float hw = fw * 0.5f - RADIUS, hh = fh * 0.5f - RADIUS, cx = fx + fw * 0.5f, cy = fy + fh * 0.5f;
    for (int y = y0; y < y1; y++) {
        float qy = (y + 0.5f > cy ? y + 0.5f - cy : cy - y - 0.5f) - hh;
        uint32_t *p = back->pix + (size_t)y * back->w;
        for (int x = x0; x < x1; x++) {
            if (y >= top && y < bottom && x >= fx && x < fx + fw) { x = fx + fw - 1; continue; }   /* under the frame */
            float qx = (x + 0.5f > cx ? x + 0.5f - cx : cx - x - 0.5f) - hw, d;
            if (qx > 0 && qy > 0) d = fsqrt(qx * qx + qy * qy) - RADIUS;
            else d = (qx > qy ? qx : qy) - RADIUS;
            if (d >= SHADOW) continue;
            float t = d <= 0 ? 1 : 1 - d / SHADOW;
            int a = (int)(SHADOW_A * t * t);
            uint32_t c = p[x];
            p[x] = ((c >> 16 & 255) * (256 - a) >> 8) << 16 | ((c >> 8 & 255) * (256 - a) >> 8) << 8 | ((c & 255) * (256 - a) >> 8);
        }
    }
}

/* Where the title bar controls sit: index 0..2 = minimize, maximize, close. */
static void ctl_rect(const struct win *w, int i, int *x, int *y, int *cw, int *ch)
{
    *x = w->x + w->w - CTL_W * (CTL_N - i) - 4; *y = w->y - TITLE_H + 2; *cw = CTL_W; *ch = TITLE_H - 4;
}

/* The four corner squares of the frame: what was behind before the frame
 * went down, and afterwards the antialiased rounding, which mixes that,
 * the border and whatever the frame put there by coverage. */
struct corners { uint32_t behind[4][RADIUS_MAX * RADIUS_MAX]; };

static void corner_pos(int fx, int fy, int fw, int fh, int k, int *x, int *y)
{
    *x = (k & 1) ? fx + fw - RADIUS : fx;
    *y = (k & 2) ? fy + fh - RADIUS : fy;
}

static int on_screen(int x, int y) { return x >= damage_x0 && x < damage_x1 && y >= damage_y0 && y < damage_y1 && x >= 0 && y >= 0 && x < back->w && y < back->h; }

static void corners_save(struct corners *c, int fx, int fy, int fw, int fh)
{
    for (int k = 0; k < 4; k++) {
        int cx, cy; corner_pos(fx, fy, fw, fh, k, &cx, &cy);
        for (int j = 0; j < RADIUS; j++) for (int i = 0; i < RADIUS; i++)
            if (on_screen(cx + i, cy + j)) c->behind[k][j * RADIUS_MAX + i] = back->pix[(size_t)(cy + j) * back->w + cx + i];
    }
}

static void corners_round(const struct corners *c, int fx, int fy, int fw, int fh)
{
    for (int k = 0; k < 4; k++) {
        int cx, cy; corner_pos(fx, fy, fw, fh, k, &cx, &cy);
        for (int j = 0; j < RADIUS; j++) for (int i = 0; i < RADIUS; i++) {
            int x = cx + i, y = cy + j;
            if (!on_screen(x, y)) continue;
            int outer = zwm_round_cov(x, y, fx, fy, fw, fh, RADIUS);
            int inner = zwm_round_cov(x, y, fx + BORDER, fy + BORDER, fw - 2 * BORDER, fh - 2 * BORDER, RADIUS - BORDER);
            if (inner == 16) continue;
            uint32_t *p = back->pix + (size_t)y * back->w + x;
            uint32_t edge = zwm_mix(c->behind[k][j * RADIUS_MAX + i], ZWM_COL_BORDER, outer * 255 / 16);
            *p = zwm_mix(edge, *p, inner * 255 / 16);
        }
    }
}

static void thick_rect(int x, int y, int w, int h, uint32_t c)
{
    zwm_fill(back, x, y, w, S, c); zwm_fill(back, x, y + h - S, w, S, c);
    zwm_fill(back, x, y, S, h, c); zwm_fill(back, x + w - S, y, S, h, c);
}

/* The frame of a decorated window: shadow, border, title bar, controls
 * (`hot` is the one under the cursor). The corners wait for corners_round(). */
static void draw_frame(const struct win *w, int active, int hot_ctl, struct corners *corners)
{
    int fx, fy, fw, fh;
    frame_rect(w, &fx, &fy, &fw, &fh);
    shadow(fx, fy, fw, fh);
    corners_save(corners, fx, fy, fw, fh);
    zwm_fill(back, fx, fy, fw, fh, ZWM_COL_BORDER);
    zwm_fill(back, w->x, w->y - TITLE_H, w->w, TITLE_H, active ? ZWM_COL_WINDOW : ZWM_COL_BG);
    zwm_hline(back, w->x, w->y - 1, w->w, ZWM_COL_BORDER);           /* title bar / content divider */
    zwm_ttext_w(back, w->x + 12 * S, w->y - TITLE_H + (TITLE_H - zwm_ttext_height(13 * S)) / 2, w->title, active ? ZWM_COL_TEXT : ZWM_COL_TEXT_DIM, 13 * S, ZWM_FONT_MEDIUM);   /* not ZWM_UI_PX: that asks the client library */
    /* the controls: minimize, maximize, close */
    for (int i = 0; i < CTL_N; i++) {
        int cx, cy, cw, ch;
        ctl_rect(w, i, &cx, &cy, &cw, &ch);
        int hot = hot_ctl == (i == 0 ? 4 : i == 1 ? 5 : 2);
        uint32_t ink = active ? ZWM_COL_TEXT : ZWM_COL_TEXT_DIM;
        if (hot) { zwm_fill(back, cx, cy, cw, ch, i == 2 ? ZWM_COL_DANGER : ZWM_COL_SURFACE); ink = ZWM_COL_TEXT; }
        int gx = cx + cw / 2, gy = cy + ch / 2;                       /* glyph centre */
        /* the glyphs: S pixels thick */
        if (i == 0) zwm_fill(back, gx - 4 * S, gy + 3 * S, 9 * S, S, ink);
        else if (i == 1) {
            if (w->maximized) {
                thick_rect(gx - 2 * S, gy - 4 * S, 7 * S, 7 * S, ink);
                zwm_fill(back, gx - 4 * S, gy - 2 * S, 7 * S, 7 * S, hot ? ZWM_COL_SURFACE : active ? ZWM_COL_WINDOW : ZWM_COL_BG);
                thick_rect(gx - 4 * S, gy - 2 * S, 7 * S, 7 * S, ink);
            } else thick_rect(gx - 4 * S, gy - 4 * S, 9 * S, 9 * S, ink);
        } else for (int k = -4 * S; k <= 4 * S; k++) { zwm_fill(back, gx + k, gy + k, S, S, ink); zwm_fill(back, gx + k, gy - k, S, S, ink); }
    }
}

/* Compose one window (with its frame) into the back buffer, clipped to the damage. */
static void compose_win(const struct win *w)
{
    if (w->minimized) return;
    int fx, fy, fw, fh;
    frame_rect(w, &fx, &fy, &fw, &fh);
    if (fx - SHADOW >= damage_x1 || fy - SHADOW >= damage_y1 || fx + fw + SHADOW <= damage_x0 || fy + fh + SHADOW + SHADOW_DY <= damage_y0) return;
    struct corners corners;
    if (decorated(w)) draw_frame(w, w == focus, hover_w == w ? hover_ctl : 0, &corners);
    /* the client pixels, clipped */
    int x0 = w->x > damage_x0 ? w->x : damage_x0, y0 = w->y > damage_y0 ? w->y : damage_y0;
    int x1 = w->x + w->w < damage_x1 ? w->x + w->w : damage_x1, y1 = w->y + w->h < damage_y1 ? w->y + w->h : damage_y1;
    for (int y = y0; y < y1 && x1 > x0; y++) {
        int wy = y - w->y, wx0 = x0 - w->x, n = x1 - x0;
        memcpy(back->pix + (size_t)y * back->w + x0, w->pix + (size_t)wy * w->w + wx0, (size_t)n * 4);
    }
    if (decorated(w)) corners_round(&corners, fx, fy, fw, fh);
}

/* Part of the client's shared segment, copied into the window's own pixels
 * when the client says it is drawn: frames are composed from those, so a
 * client already drawing its next frame is never seen half done. The
 * segment may be smaller than the window (a resize in flight): the rest is plain. */
static void take_shm(struct win *w, int x, int y, int cw, int ch)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + cw > w->w ? w->w : x + cw, y1 = y + ch > w->h ? w->h : y + ch;
    for (int r = y0; r < y1 && x1 > x0; r++) {
        uint32_t *dst = w->pix + (size_t)r * w->w + x0;
        int n = x1 - x0, avail = r < w->shm_h ? (w->shm_w > x0 ? w->shm_w - x0 : 0) : 0;
        if (avail > n) avail = n;
        if (avail > 0) memcpy(dst, w->shm + (size_t)r * w->shm_stride + x0, (size_t)avail * 4);
        for (int i = avail > 0 ? avail : 0; i < n; i++) dst[i] = ZWM_COL_WINDOW;
    }
}

static void win_detach_shm(struct win *w)
{
    if (!w->shm) return;
    munmap(w->shm, w->shm_len);
    close(w->shm_fd);
    w->shm = NULL;
}

/* A 12x19 arrow, 1 = black, 2 = white. */
static const char cursor_img[19][13] = {
    "1           ", "11          ", "121         ", "1221        ", "12221       ", "122221      ",
    "1222221     ", "12222221    ", "122222221   ", "1222222221  ", "12222222221 ", "122222111111",
    "1222122     ", "12211221    ", "121  1221   ", "11   1221   ", "      1221  ", "      121   ", "       1    ",
};

static void compose_cursor(void)
{
    for (int j = 0; j < 19 * S; j++)
        for (int i = 0; i < 12 * S; i++) {
            char c = cursor_img[j / S][i / S];
            if (c == ' ' || c == 0) continue;
            int x = mx + i, y = my + j;
            if (x < damage_x0 || x >= damage_x1 || y < damage_y0 || y >= damage_y1) continue;
            back->pix[(size_t)y * back->w + x] = c == '1' ? 0x000000 : 0xFFFFFF;
        }
}

/* ---- composing on the GPU (hwcomp.c) --------------------------------------- */

/* Part of a window's pixels changed (window coordinates): upload it before the next frame. */
static void content_damage(struct win *w, int x, int y, int cw, int ch)
{
    if (!gpu) return;
    if (w->cd_x1 <= w->cd_x0) { w->cd_x0 = x; w->cd_y0 = y; w->cd_x1 = x + cw; w->cd_y1 = y + ch; return; }
    if (x < w->cd_x0) w->cd_x0 = x;
    if (y < w->cd_y0) w->cd_y0 = y;
    if (x + cw > w->cd_x1) w->cd_x1 = x + cw;
    if (y + ch > w->cd_y1) w->cd_y1 = y + ch;
}

static void gpu_content(struct win *w)
{
    int x0 = w->cd_x0 < 0 ? 0 : w->cd_x0, y0 = w->cd_y0 < 0 ? 0 : w->cd_y0;
    int x1 = w->cd_x1 > w->w ? w->w : w->cd_x1, y1 = w->cd_y1 > w->h ? w->h : w->cd_y1;
    w->cd_x0 = w->cd_y0 = w->cd_x1 = w->cd_y1 = 0;
    if (w->gpu_res || x1 <= x0 || y1 <= y0) return;
    hw_win_content(w->hw, w->w, w->h, x0, y0, x1 - x0, y1 - y0, w->pix + (size_t)y0 * w->w + x0, w->w);
}

/* The decoration as a texture with alpha, redrawn only when it looks
 * different. It is drawn in software twice, over black and over white,
 * which gives the coverage (and the premultiplied colour) of everything
 * the frame code does, shadow and antialiased corners included. The
 * window is drawn short: its middle rows are all alike, and the one
 * after the title bar is repeated to the real height. */
static void gpu_deco(struct win *w)
{
    int ch = w->h < RADIUS + 4 * S ? w->h : RADIUS + 4 * S;
    int active = w == focus, hot = hover_w == w ? hover_ctl : 0, fx, fy, fw, fh;
    frame_rect(w, &fx, &fy, &fw, &fh);
    if (w->deco_key.fw == fw && w->deco_key.ch == ch && w->deco_key.active == active && w->deco_key.hot == hot &&
        w->deco_key.max == w->maximized && !strcmp(w->deco_key.title, w->title)) return;
    struct win c = *w;
    c.h = ch; c.x = w->x - fx + SHADOW; c.y = w->y - fy + SHADOW;
    int cfx, cfy, cfw, cfh;
    frame_rect(&c, &cfx, &cfy, &cfw, &cfh);
    int dw = cfw + 2 * SHADOW, dh = cfh + 2 * SHADOW + SHADOW_DY;
    zwm_surface *pass[2] = { zwm_surface_new(dw, dh), zwm_surface_new(dw, dh) };
    if (pass[0] && pass[1]) {
        zwm_surface *saved = back;
        int d[4] = { damage_x0, damage_y0, damage_x1, damage_y1 };
        damage_x0 = damage_y0 = 0; damage_x1 = dw; damage_y1 = dh;
        for (int k = 0; k < 2; k++) {
            uint32_t bg = k ? 0xFFFFFF : 0;
            struct corners corners;
            back = pass[k];
            zwm_fill(back, 0, 0, dw, dh, bg);
            draw_frame(&c, active, hot, &corners);
            zwm_fill(back, c.x, c.y, c.w, c.h, bg);     /* the content goes underneath */
            corners_round(&corners, cfx, cfy, cfw, cfh);
        }
        back = saved;
        damage_x0 = d[0]; damage_y0 = d[1]; damage_x1 = d[2]; damage_y1 = d[3];
        uint32_t *b = pass[0]->pix, *wh = pass[1]->pix;
        for (size_t i = 0; i < (size_t)dw * dh; i++) {
            int a = 255 - (int)((wh[i] >> 8 & 255) - (b[i] >> 8 & 255));
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            uint32_t r = b[i] >> 16 & 255, g = b[i] >> 8 & 255, bl = b[i] & 255;
            if (r > (uint32_t)a) r = (uint32_t)a;
            if (g > (uint32_t)a) g = (uint32_t)a;
            if (bl > (uint32_t)a) bl = (uint32_t)a;
            b[i] = (uint32_t)a << 24 | r << 16 | g << 8 | bl;
        }
        hw_win_deco(w->hw, b, dw, dh, c.y);         /* c.y: the first content row */
        w->deco_key.fw = fw; w->deco_key.ch = ch; w->deco_key.active = active; w->deco_key.hot = hot;
        w->deco_key.max = w->maximized; memcpy(w->deco_key.title, w->title, sizeof w->title);
    }
    zwm_surface_free(pass[0]);
    zwm_surface_free(pass[1]);
}

static uint32_t cursor_argb[19 * 2 * 12 * 2];

static void gpu_flush(void)
{
    hw_begin(ZWM_COL_BG);
    for (struct win *w = bottom; w; w = w->above) {
        if (w->minimized || !w->hw) continue;
        gpu_content(w);
        if (decorated(w)) {
            gpu_deco(w);
            int fx, fy, fw, fh;
            frame_rect(w, &fx, &fy, &fw, &fh);
            hw_draw_win(w->hw, fx - SHADOW, fy - SHADOW, fh + 2 * SHADOW + SHADOW_DY, w->x, w->y, w->w, w->h, RADIUS - BORDER);
        } else hw_draw_win(w->hw, 0, 0, 0, w->x, w->y, w->w, w->h, 0);
    }
    if (!cursor_argb[0]) {
        for (int j = 0; j < 19 * S; j++)
            for (int i = 0; i < 12 * S; i++) {
                char c = cursor_img[j / S][i / S];
                cursor_argb[j * 12 * S + i] = c == '1' ? 0xFF000000u : c == '2' ? 0xFFFFFFFFu : 0;
            }
    }
    hw_draw_cursor(cursor_argb, 12 * S, 19 * S, mx, my);
    hw_end();
}

static void send_window_list(void);
static void send_msg(struct client *c, uint32_t type, uint32_t win, const void *payload, size_t len);
static void flush(void)
{
    if (windows_dirty) send_window_list();
    if (screen_off) { flush_wait_ms = -1; return; }     /* the damage waits for the screen */
    if (damage_x1 <= damage_x0 || damage_y1 <= damage_y0) return;
    if (gpu) {                              /* the whole screen: it is cheap there */
        static struct timespec last;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int since = (int)((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000);
        if (since >= 0 && since < FRAME_MS) { flush_wait_ms = FRAME_MS - since; return; }
        flush_wait_ms = -1;
        last = now;
        gpu_flush();
        damage_x0 = damage_y0 = damage_x1 = damage_y1 = 0;
        for (struct win *w = bottom; w; w = w->above)
            if (w->frame_wanted) { w->frame_wanted = 0; send_msg(w->owner, ZWM_S_FRAME, w->id, NULL, 0); }
        return;
    }
    zwm_fill(back, damage_x0, damage_y0, damage_x1 - damage_x0, damage_y1 - damage_y0, ZWM_COL_BG);
    for (struct win *w = bottom; w; w = w->above)
        compose_win(w);
    compose_cursor();
    for (int y = damage_y0; y < damage_y1; y++)
        memcpy((uint8_t *)fb.mem + (size_t)y * fb.pitch + (size_t)damage_x0 * 4,
               back->pix + (size_t)y * back->w + damage_x0, (size_t)(damage_x1 - damage_x0) * 4);
    struct fb_rect r = { (uint32_t)damage_x0, (uint32_t)damage_y0, (uint32_t)(damage_x1 - damage_x0), (uint32_t)(damage_y1 - damage_y0) };
    damage_x0 = damage_y0 = damage_x1 = damage_y1 = 0;
    if (ioctl(fb.fd, FBIOPRESENT_RECT, &r) != 0)     /* displays that need a push (virtio-gpu): just what changed */
        ioctl(fb.fd, FBIOPRESENT, 0);
}

/* ---- windows --------------------------------------------------------------- */

static void unlink_win(struct win *w)
{
    if (w->below) w->below->above = w->above; else bottom = w->above;
    if (w->above) w->above->below = w->below; else top = w->below;
    w->above = w->below = NULL;
}

/* Put w on top, but under any dock. */
static void raise_win(struct win *w)
{
    unlink_win(w);
    struct win *at = top;
    if (!(w->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP)))
        while (at && (at->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP))) at = at->below;
    /* insert above `at` */
    w->below = at;
    w->above = at ? at->above : bottom;
    if (at) at->above = w; else bottom = w;
    if (w->above) w->above->below = w; else top = w;
    damage_win(w);
}

static void send_msg(struct client *c, uint32_t type, uint32_t win, const void *payload, size_t len);

static void set_focus(struct win *w)
{
    if (focus == w) return;
    struct zwm_m_focus f;
    if (focus) { f.focused = 0; send_msg(focus->owner, ZWM_S_FOCUS, focus->id, &f, sizeof f); damage_win(focus); }
    focus = w;
    if (focus) { f.focused = 1; send_msg(focus->owner, ZWM_S_FOCUS, focus->id, &f, sizeof f); damage_win(focus); }
    windows_dirty = 1;
}

static void recompute_workarea(void)
{
    workarea_top = safe_top; workarea_bottom = fb.h;
    for (struct win *w = bottom; w; w = w->above) {
        if (w->flags & ZWM_DOCK_TOP && w->y + w->h > workarea_top) workarea_top = w->y + w->h;
        if (w->flags & ZWM_DOCK_BOTTOM && w->y < workarea_bottom) workarea_bottom = w->y;
    }
}

static struct win *win_create(struct client *c, const struct zwm_m_create *m)
{
    struct win *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->id = next_id++;
    w->owner = c;
    w->flags = m->flags;
    w->w = m->w < 1 ? 1 : m->w;
    w->h = m->h < 1 ? 1 : m->h;
    strncpy(w->title, m->title, sizeof w->title - 1);
    strncpy(w->exe, m->exe, sizeof w->exe - 1);
    if (w->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP)) {
        w->w = fb.w;
        w->x = 0;
        w->y = (w->flags & ZWM_DOCK_TOP) ? workarea_top : workarea_bottom - w->h;
        w->flags |= ZWM_FIXED;
    } else if (w->flags & ZWM_UNDECORATED) {
        /* no frame: it may fill the work area (a launcher sheet asks for more than the screen) */
        if (w->w > fb.w) w->w = fb.w;
        if (w->h > workarea_bottom - workarea_top) w->h = workarea_bottom - workarea_top;
        w->x = (fb.w - w->w) / 2; w->y = workarea_top + (workarea_bottom - workarea_top - w->h) / 2;
    } else {
        /* cascade from the top-left of the work area */
        static int cascade;
        int avail_h = workarea_bottom - workarea_top - TITLE_H - 2 * BORDER;
        if (w->w > fb.w - 2 * BORDER) w->w = fb.w - 2 * BORDER;
        if (w->h > avail_h) w->h = avail_h;
        w->x = BORDER + 24 + cascade * 28;
        w->y = workarea_top + TITLE_H + BORDER + 24 + cascade * 28;
        if (w->x + w->w > fb.w || w->y + w->h > workarea_bottom) { cascade = 0; w->x = BORDER + 24; w->y = workarea_top + TITLE_H + BORDER + 24; }
        cascade = (cascade + 1) % 8;
    }
    w->pix = calloc((size_t)w->w * w->h, 4);
    if (!w->pix) { free(w); return NULL; }
    for (int i = 0; i < w->w * w->h; i++) w->pix[i] = ZWM_COL_WINDOW;
    if (gpu) { w->hw = hw_win_new(); content_damage(w, 0, 0, w->w, w->h); }
    /* link on top */
    w->below = top; w->above = NULL;
    if (top) top->above = w; else bottom = w;
    top = w;
    raise_win(w);
    recompute_workarea();
    set_focus(w);
    return w;
}

static void win_destroy(struct win *w)
{
    damage_win(w);
    if (focus == w) set_focus(NULL);
    if (drag.w == w) drag.w = NULL;
    if (hover_w == w) hover_w = NULL;
    if (mouse_win == w) mouse_win = NULL;
    windows_dirty = 1;
    unlink_win(w);
    win_detach_shm(w);
    hw_win_free(w->hw);
    free(w->pix);
    free(w);
    recompute_workarea();
    /* focus the new top-most ordinary window */
    for (struct win *t = top; t && !focus; t = t->below)
        if (!(t->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP))) set_focus(t);
}

static struct win *win_by_id(struct client *c, uint32_t id)
{
    for (struct win *w = bottom; w; w = w->above)
        if (w->id == id && w->owner == c) return w;
    return NULL;
}

static struct win *win_by_id_any(uint32_t id)
{
    for (struct win *w = bottom; w; w = w->above)
        if (w->id == id) return w;
    return NULL;
}

static void win_resize(struct win *w, int nw, int nh)
{
    if (nw < 40) nw = 40;
    if (nh < 20) nh = 20;
    if (nw == w->w && nh == w->h) return;
    uint32_t *p = calloc((size_t)nw * nh, 4);
    if (!p) return;
    for (int i = 0; i < nw * nh; i++) p[i] = ZWM_COL_WINDOW;
    for (int y = 0; y < nh && y < w->h; y++)
        memcpy(p + (size_t)y * nw, w->pix + (size_t)y * w->w, (size_t)(nw < w->w ? nw : w->w) * 4);
    damage_win(w);
    free(w->pix);
    w->pix = p; w->w = nw; w->h = nh;
    content_damage(w, 0, 0, nw, nh);
    damage_win(w);
    struct zwm_m_geom g = { w->x, w->y, w->w, w->h, fb.w, fb.h };
    send_msg(w->owner, ZWM_S_RESIZE, w->id, &g, sizeof g);
}

/* Topmost window under the point, and where on it: 0 client, 1 title, 2 close box, 3 resize grip. */
/* Topmost window under the point, and where on it: 0 client, 1 title, 2 close, 3 resize grip, 4 minimize, 5 maximize. */
static struct win *win_at(int x, int y, int *where)
{
    for (struct win *w = top; w; w = w->below) {
        if (w->minimized) continue;
        int fx, fy, fw, fh;
        frame_rect(w, &fx, &fy, &fw, &fh);
        if (x < fx || y < fy || x >= fx + fw || y >= fy + fh) continue;
        *where = 0;
        if (decorated(w)) {
            if (y < w->y) {
                *where = 1;
                for (int i = 0; i < CTL_N; i++) {
                    int cx, cy, cw, ch;
                    ctl_rect(w, i, &cx, &cy, &cw, &ch);
                    if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) *where = i == 0 ? 4 : i == 1 ? 5 : 2;
                }
            } else if (!(w->flags & ZWM_FIXED) && !w->maximized && x >= w->x + w->w - GRIP && y >= w->y + w->h - GRIP) {
                *where = 3;
            }
        }
        return w;
    }
    return NULL;
}

static void damage_title(const struct win *w) { damage(w->x - BORDER, w->y - TITLE_H - BORDER, w->w + 2 * BORDER, TITLE_H + BORDER); }

/* Minimize, and the two ways back: activate (from a taskbar) and maximize. */
static void win_minimize(struct win *w)
{
    if (w->minimized || !decorated(w)) return;
    damage_win(w);
    w->minimized = 1;
    windows_dirty = 1;
    if (focus == w) {
        set_focus(NULL);
        for (struct win *t = top; t && !focus; t = t->below)
            if (decorated(t) && !t->minimized) set_focus(t);
    }
}

static void win_activate(struct win *w)
{
    if (w->minimized) { w->minimized = 0; windows_dirty = 1; }
    if (!(w->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP))) raise_win(w);
    set_focus(w);
    damage_win(w);
}

static void win_maximize(struct win *w)
{
    if (!decorated(w) || (w->flags & ZWM_FIXED)) return;
    damage_win(w);
    if (w->maximized) {
        w->maximized = 0;
        w->x = w->sx; w->y = w->sy;
        win_resize(w, w->sw, w->sh);
        if (w->w == w->sw && w->h == w->sh) {           /* win_resize said nothing: say it ourselves */
            struct zwm_m_geom g = { w->x, w->y, w->w, w->h, fb.w, fb.h };
            send_msg(w->owner, ZWM_S_RESIZE, w->id, &g, sizeof g);
        }
    } else {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->maximized = 1;
        w->x = BORDER; w->y = workarea_top + TITLE_H + BORDER;
        win_resize(w, fb.w - 2 * BORDER, workarea_bottom - workarea_top - TITLE_H - 2 * BORDER);
    }
    windows_dirty = 1;
    damage_win(w);
}

/* Tell the taskbars what windows there are. */
static void send_window_list(void)
{
    struct zwm_m_window list[32];
    int n = 0;
    for (struct win *w = bottom; w && n < 32; w = w->above) {
        if (!decorated(w)) continue;
        list[n].id = w->id;
        list[n].state = (w == focus ? ZWM_WIN_FOCUSED : 0) | (w->minimized ? ZWM_WIN_MINIMIZED : 0) | (w->maximized ? ZWM_WIN_MAXIMIZED : 0);
        memcpy(list[n].title, w->title, sizeof list[n].title);
        memcpy(list[n].exe, w->exe, sizeof list[n].exe);
        n++;
    }
    for (struct win *w = bottom; w; w = w->above)
        if (w->flags & ZWM_TASKBAR) send_msg(w->owner, ZWM_S_WINDOWS, w->id, list, (size_t)n * sizeof list[0]);
    windows_dirty = 0;
}

/* ---- clients & protocol ------------------------------------------------ */

static void send_all(struct client *c, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd pf = { c->fd, POLLOUT, 0 }; poll(&pf, 1, 200); continue; }
            return;
        }
        p += n; len -= (size_t)n;
    }
}

static void send_msg(struct client *c, uint32_t type, uint32_t win, const void *payload, size_t len)
{
    if (!c) return;
    struct zwm_hdr h = { type, (uint32_t)len, win };
    send_all(c, &h, sizeof h);
    if (len) send_all(c, payload, len);
}

static void client_close(struct client *c)
{
    for (struct win *w = bottom, *n; w; w = n) {
        n = w->above;
        if (w->owner == c) win_destroy(w);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] == c) clients[i] = NULL;
    close(c->fd);
    free(c->in);
    free(c);
}

static void handle_msg(struct client *c, const struct zwm_hdr *h, const uint8_t *p)
{
    struct win *w = h->type == ZWM_C_CREATE ? NULL : win_by_id(c, h->win);
    switch (h->type) {
    case ZWM_C_CREATE: {
        if (h->len < sizeof(struct zwm_m_create)) return;
        struct zwm_m_create m;
        memcpy(&m, p, sizeof m);
        m.title[sizeof m.title - 1] = 0; m.exe[sizeof m.exe - 1] = 0;
        w = win_create(c, &m);
        struct zwm_m_geom g = { 0 };
        if (w) { g.x = w->x; g.y = w->y; g.w = w->w; g.h = w->h; }
        g.screen_w = fb.w; g.screen_h = fb.h;
        send_msg(c, ZWM_S_CREATED, w ? w->id : 0, &g, sizeof g);
        break;
    }
    case ZWM_C_BLIT: {
        if (!w || h->len < sizeof(struct zwm_m_rect)) return;
        struct zwm_m_rect r;
        memcpy(&r, p, sizeof r);
        if (r.w <= 0 || r.h <= 0 || (uint64_t)r.w * r.h * 4 + sizeof r > h->len) return;
        const uint32_t *src = (const uint32_t *)(p + sizeof r);
        win_detach_shm(w);                  /* back to blits */
        if (w->gpu_res) { w->gpu_res = 0; content_damage(w, 0, 0, w->w, w->h); }
        for (int j = 0; j < r.h; j++) {
            int y = r.y + j;
            if (y < 0 || y >= w->h) continue;
            int x0 = r.x < 0 ? 0 : r.x, x1 = r.x + r.w > w->w ? w->w : r.x + r.w;
            if (x1 <= x0) continue;
            memcpy(w->pix + (size_t)y * w->w + x0, src + (size_t)j * r.w + (x0 - r.x), (size_t)(x1 - x0) * 4);
        }
        content_damage(w, r.x, r.y, r.w, r.h);
        damage(w->x + r.x, w->y + r.y, r.w, r.h);
        break;
    }
    case ZWM_C_ATTACH: {
        if (!w || h->len < sizeof(struct zwm_m_attach)) return;
        struct zwm_m_attach a; memcpy(&a, p, sizeof a);
        if (a.w <= 0 || a.h <= 0 || a.stride < a.w || (uint64_t)a.stride * a.h > (64u << 20)) return;
        int fd = open("/dev/shmem", O_RDWR | O_CLOEXEC);
        if (fd < 0) return;
        struct shm_segment seg = { .key = a.key };
        size_t need = (size_t)a.stride * a.h * 4;
        if (ioctl(fd, SHM_IOC_ATTACH, &seg) != 0 || seg.size < need) { close(fd); return; }
        void *m = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { close(fd); return; }
        win_detach_shm(w);
        w->shm = m; w->shm_fd = fd; w->shm_len = need; w->shm_w = a.w; w->shm_h = a.h; w->shm_stride = a.stride;
        w->gpu_res = 0;
        take_shm(w, 0, 0, w->w, w->h);
        content_damage(w, 0, 0, w->w, w->h);
        damage_win(w);
        break;
    }
    case ZWM_C_ATTACH_GPU: {                /* the client's GL buffer, sampled where it is */
        if (!w || !gpu || h->len < sizeof(struct zwm_m_attach_gpu)) return;
        struct zwm_m_attach_gpu a; memcpy(&a, p, sizeof a);
        if (a.w <= 0 || a.h <= 0 || hw_win_content_gpu(w->hw, a.res, a.w, a.h) != 0) return;
        win_detach_shm(w);
        w->gpu_res = a.res; w->gpu_w = a.w; w->gpu_h = a.h;
        damage_win(w);
        break;
    }
    case ZWM_C_DAMAGE: {
        if (!w || h->len < sizeof(struct zwm_m_rect)) return;
        struct zwm_m_rect r; memcpy(&r, p, sizeof r);
        if (r.w <= 0 || r.h <= 0) return;
        if (w->gpu_res) w->frame_wanted = 1;
        else if (w->shm) { take_shm(w, r.x, r.y, r.w, r.h); send_msg(c, ZWM_S_TAKEN, w->id, NULL, 0); }
        content_damage(w, r.x, r.y, r.w, r.h);
        damage(w->x + r.x, w->y + r.y, r.w, r.h);
        break;
    }
    case ZWM_C_TITLE:
        if (!w) return;
        memset(w->title, 0, sizeof w->title);
        memcpy(w->title, p, h->len < sizeof w->title - 1 ? h->len : sizeof w->title - 1);
        damage_win(w);
        windows_dirty = 1;
        break;
    case ZWM_C_ACTIVATE: {
        struct win *t = win_by_id_any(h->win);
        if (t) win_activate(t);
        break;
    }
    case ZWM_C_MINIMIZE: {                  /* any window: a taskbar puts them away too */
        struct win *t = win_by_id_any(h->win);
        if (t) win_minimize(t);
        break;
    }
    case ZWM_C_MAXIMIZE: {
        struct win *t = win_by_id_any(h->win);
        if (t && decorated(t)) win_maximize(t);
        break;
    }
    case ZWM_C_CLOSE: {
        struct win *t = win_by_id_any(h->win);
        if (t) send_msg(t->owner, ZWM_S_CLOSE, t->id, NULL, 0);
        break;
    }
    case ZWM_C_DESTROY:
        if (w) win_destroy(w);
        break;
    case ZWM_C_MOVE: {
        if (!w || h->len < sizeof(struct zwm_m_point) || (w->flags & ZWM_FIXED)) return;
        struct zwm_m_point pt; memcpy(&pt, p, sizeof pt);
        damage_win(w); w->x = pt.x; w->y = pt.y; damage_win(w);
        break;
    }
    case ZWM_C_RAISE:
        if (w) { raise_win(w); set_focus(w); }
        break;
    default:
        break;
    }
}

static void client_read(struct client *c)
{
    if (c->in_len + 65536 > c->in_cap) {
        size_t cap = c->in_cap ? c->in_cap * 2 : 65536;
        while (cap < c->in_len + 65536) cap *= 2;
        uint8_t *p = realloc(c->in, cap);
        if (!p) { client_close(c); return; }
        c->in = p; c->in_cap = cap;
    }
    ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { client_close(c); return; }
    if (n > 0) c->in_len += (size_t)n;
    size_t off = 0;
    while (c->in_len - off >= sizeof(struct zwm_hdr)) {
        struct zwm_hdr h;
        memcpy(&h, c->in + off, sizeof h);
        if (h.len > 64u << 20) { client_close(c); return; }
        if (c->in_len - off < sizeof h + h.len) break;
        handle_msg(c, &h, c->in + off + sizeof h);
        off += sizeof h + h.len;
    }
    if (off) { memmove(c->in, c->in + off, c->in_len - off); c->in_len -= off; }
}

static void accept_client(void)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) if (!clients[i]) { slot = i; break; }
    if (slot < 0) { close(fd); return; }
    struct client *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    c->fd = fd;
    clients[slot] = c;
    struct zwm_m_hello hello = { fb.w, fb.h, S, (gpu ? ZWM_HELLO_GPU : 0) | ZWM_HELLO_TAKEN };
    send_msg(c, ZWM_S_HELLO, 0, &hello, sizeof hello);
}

/* ---- input ------------------------------------------------------------- */

static const char keymap_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' ',
};
static const char keymap_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' ',
};
static int k_shift, k_ctrl, k_alt, k_caps, k_e0;

/* Alt+Tab: the windows as they were stacked when Alt went down (front
 * first, minimized ones too); each Tab activates the next, Shift+Tab the
 * previous, letting go of Alt ends the round. */
static struct win *sw_list[64];
static int sw_n, sw_at;

static int win_exists(const struct win *w) { for (struct win *x = bottom; x; x = x->above) if (x == w) return 1; return 0; }

static void switch_window(int backwards)
{
    if (!sw_n) {
        for (struct win *w = top; w && sw_n < 64; w = w->below)
            if (decorated(w)) sw_list[sw_n++] = w;
        sw_at = 0;
    }
    if (sw_n < 2) return;
    for (int tries = 0; tries < sw_n; tries++) {
        sw_at = (sw_at + (backwards ? sw_n - 1 : 1)) % sw_n;
        if (win_exists(sw_list[sw_at])) { win_activate(sw_list[sw_at]); return; }
    }
}

/* The Power key: the panel off (and the touchscreen ignored) or back on. */
static void toggle_screen(void)
{
    if (panel_fd < 0) return;
    const char *cmd = screen_off ? "on\n" : "off\n";
    if (write(panel_fd, cmd, strlen(cmd)) < 0) { perror("zwm: /dev/panel"); return; }
    screen_off = !screen_off;
    if (!screen_off) damage(0, 0, fb.w, fb.h);
}

static void screenshot(void);
static void key_event(uint8_t raw)
{
    if (raw == 0xE0) { k_e0 = 1; return; }
    int down = !(raw & 0x80);
    uint8_t sc = raw & 0x7F;
    int e0 = k_e0; k_e0 = 0;
    uint32_t sym = 0;
    if (screen_off && !(e0 && sc == 0x5E)) return;      /* only Power wakes it */
    if (e0) {
        switch (sc) {
        case 0x48: sym = ZWM_KEY_UP; break;    case 0x50: sym = ZWM_KEY_DOWN; break;
        case 0x4B: sym = ZWM_KEY_LEFT; break;  case 0x4D: sym = ZWM_KEY_RIGHT; break;
        case 0x47: sym = ZWM_KEY_HOME; break;  case 0x4F: sym = ZWM_KEY_END; break;
        case 0x49: sym = ZWM_KEY_PGUP; break;  case 0x51: sym = ZWM_KEY_PGDN; break;
        case 0x52: sym = ZWM_KEY_INSERT; break; case 0x53: sym = ZWM_KEY_DELETE; break;
        case 0x1D: sym = ZWM_KEY_CTRL; k_ctrl = down; break;
        case 0x38: sym = ZWM_KEY_ALT; k_alt = down; break;
        case 0x1C: sym = '\n'; break;
        case 0x5E: if (down) toggle_screen(); return;    /* Power (a phone's button) */
        default: return;
        }
    } else {
        switch (sc) {
        case 0x2A: case 0x36: k_shift = down; sym = ZWM_KEY_SHIFT; break;
        case 0x1D: k_ctrl = down; sym = ZWM_KEY_CTRL; break;
        case 0x38: k_alt = down; sym = ZWM_KEY_ALT; break;
        case 0x3A: if (down) k_caps = !k_caps; sym = ZWM_KEY_CAPS; break;
        default:
            if (sc >= 0x3B && sc <= 0x44) sym = ZWM_KEY_F1 + (sc - 0x3B);
            else if (sc == 0x57 || sc == 0x58) sym = ZWM_KEY_F1 + 10 + (sc - 0x57);
            else if (sc < 128) {
                char c = keymap_lower[sc];
                if (!c) return;
                int upper = k_shift;
                if (k_caps && c >= 'a' && c <= 'z') upper = !upper;
                sym = (uint32_t)(unsigned char)(upper ? keymap_upper[sc] : c);
            } else return;
        }
    }
    if (down && k_ctrl && k_alt && (sym == 'q' || sym == 'Q')) { quit = 1; return; }
    if (down && k_ctrl && k_alt && (sym == 'p' || sym == 'P')) { screenshot(); return; }
    if (sym == ZWM_KEY_ALT && !down) sw_n = 0;                    /* the round is over */
    if (k_alt && sym == '\t') { if (down) switch_window(k_shift); return; }
    if (!focus) return;
    struct zwm_m_key k = { sym, (k_shift ? ZWM_MOD_SHIFT : 0) | (k_ctrl ? ZWM_MOD_CTRL : 0) | (k_alt ? ZWM_MOD_ALT : 0), (uint32_t)down, raw };
    send_msg(focus->owner, ZWM_S_KEY, focus->id, &k, sizeof k);
}

/* Ctrl+Alt+P: the screen as it is into /tmp/zwm-shot.ppm. */
static void screenshot(void)
{
    uint32_t *pix = back->pix;
    if (gpu && (!(pix = malloc((size_t)fb.w * fb.h * 4)) || hw_read(pix) != 0)) { if (pix) free(pix); return; }
    FILE *f = fopen("/tmp/zwm-shot.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
        uint8_t *row = malloc((size_t)fb.w * 3);
        for (int y = 0; row && y < fb.h; y++) {
            for (int x = 0; x < fb.w; x++) { uint32_t c = pix[(size_t)y * fb.w + x]; row[x * 3] = c >> 16; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c; }
            fwrite(row, 3, (size_t)fb.w, f);
        }
        free(row);
        fclose(f);
        fprintf(stderr, "zwm: screenshot in /tmp/zwm-shot.ppm\n");
    }
    /* for tests: the framebuffer (not on screen while the GPU composes) gets
     * it too, where the host can read guest memory (QEMU's pmemsave) */
    if (gpu && getenv("ZWM_SHOT_FB"))
        for (int y = 0; y < fb.h; y++) memcpy((uint8_t *)fb.mem + (size_t)y * fb.pitch, pix + (size_t)y * fb.w, (size_t)fb.w * 4);
    if (gpu) free(pix);
}

static void send_mouse(struct win *w, uint32_t kind, uint32_t buttons)
{
    struct zwm_m_mouse m = { mx - w->x, my - w->y, buttons, kind };
    send_msg(w->owner, ZWM_S_MOUSE, w->id, &m, sizeof m);
}

static void mouse_event(const struct mouse_event *e)
{
    if (screen_off) return;                             /* a finger on the dark screen is nothing */
    damage(mx, my, 12 * S, 19 * S);
    if (e->flags & MOUSE_ABSOLUTE) { mx = e->dx; my = e->dy; }      /* a touchscreen: where the finger is */
    else { mx += e->dx * S; my += e->dy * S; }                     /* the pointer moves in UI units */
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    if (mx >= fb.w) mx = fb.w - 1; if (my >= fb.h) my = fb.h - 1;
    damage(mx, my, 12 * S, 19 * S);
    uint32_t pressed = e->buttons & ~mbuttons, released = mbuttons & ~e->buttons;
    mbuttons = e->buttons;

    if (drag.w) {
        if (drag.mode == 1) {
            damage_win(drag.w);
            drag.w->x = mx - drag.dx; drag.w->y = my - drag.dy;
            if (drag.w->y < workarea_top + TITLE_H + BORDER) drag.w->y = workarea_top + TITLE_H + BORDER;
            damage_win(drag.w);
        } else {
            win_resize(drag.w, mx - drag.w->x + drag.dx, my - drag.w->y + drag.dy);
        }
        if (released & MOUSE_BTN_LEFT) drag.w = NULL;
        return;
    }

    int where = 0;
    struct win *w = win_at(mx, my, &where);
    if (pressed) {
        if (w) {
            if (!(w->flags & (ZWM_DOCK_BOTTOM | ZWM_DOCK_TOP))) { raise_win(w); set_focus(w); }   /* docks never take the focus */
            if (pressed & MOUSE_BTN_LEFT) {
                if (where == 2) { send_msg(w->owner, ZWM_S_CLOSE, w->id, NULL, 0); return; }
                if (where == 4) { win_minimize(w); hover_w = NULL; return; }
                if (where == 5) { win_maximize(w); return; }
                if (where == 1 && !(w->flags & ZWM_FIXED) && !w->maximized) { drag.w = w; drag.mode = 1; drag.dx = mx - w->x; drag.dy = my - w->y; return; }
                if (where == 3) { drag.w = w; drag.mode = 2; drag.dx = w->x + w->w - mx; drag.dy = w->y + w->h - my; return; }
            }
            if (where == 0) send_mouse(w, ZWM_MOUSE_PRESS, e->buttons);
        }
        return;
    }
    /* hover feedback on the title bar controls */
    struct win *hw = (w && where >= 2 && where != 3) ? w : NULL;
    int hc = hw ? where : 0;
    if (hw != hover_w || hc != hover_ctl) {
        if (hover_w) damage_title(hover_w);
        hover_w = hw; hover_ctl = hc;
        if (hover_w) damage_title(hover_w);
    }
    struct win *over = w && where == 0 ? w : NULL;
    if (mouse_win && mouse_win != over) {            /* left it: a move to (-1,-1) says so */
        struct zwm_m_mouse m = { -1, -1, e->buttons, ZWM_MOUSE_MOVE };
        send_msg(mouse_win->owner, ZWM_S_MOUSE, mouse_win->id, &m, sizeof m);
    }
    mouse_win = over;
    if (w && where == 0)
        send_mouse(w, released ? ZWM_MOUSE_RELEASE : ZWM_MOUSE_MOVE, e->buttons);
}

/* ---- setup ------------------------------------------------------------- */

static int open_fb(void)
{
    fb.fd = open("/dev/fb0", O_RDWR);
    if (fb.fd < 0) { perror("zwm: /dev/fb0"); return -1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f)) { perror("zwm: fb ioctl"); return -1; }
    if (v.bits_per_pixel != 32) { fprintf(stderr, "zwm: need a 32 bpp framebuffer (have %u)\n", v.bits_per_pixel); return -1; }
    fb.w = (int)v.xres; fb.h = (int)v.yres; fb.pitch = (int)f.line_length;
    safe_top = (int)FB_SAFE_TOP(&v) < fb.h / 4 ? (int)FB_SAFE_TOP(&v) : 0;
    /* HiDPI: a panel at its real size gets everything drawn twice as big */
    const char *e = getenv("ZWM_SCALE");
    S = e && atoi(e) > 0 ? atoi(e) : fb.w >= 2000 ? 2 : 1;
    if (S > 2) S = 2;
    fprintf(stderr, "zwm: %dx%d, ui scale %d, safe top %d\n", fb.w, fb.h, S, safe_top);
    /* the whole buffer, not just the current mode: a display that follows
     * its window (virtio-gpu) changes geometry on the same memory */
    size_t len = f.smem_len > (uint32_t)(fb.pitch * fb.h) ? f.smem_len : (size_t)fb.pitch * fb.h;
    fb.mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.mem == MAP_FAILED) { perror("zwm: mmap fb"); return -1; }
    ioctl(fb.fd, KDSETMODE, KD_GRAPHICS);
    return 0;
}

/* The display changed size (POLLPRI on /dev/fb0): new geometry, same
 * memory. Docks follow the edges, windows stay on the screen, clients of
 * resized windows hear about it. */
static void display_resized(void)
{
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f)) return;
    if ((int)v.xres == fb.w && (int)v.yres == fb.h && (int)f.line_length == fb.pitch) return;
    fb.w = (int)v.xres; fb.h = (int)v.yres; fb.pitch = (int)f.line_length;
    zwm_surface_resize(back, fb.w, fb.h);
    if (gpu && hw_resize(fb.w, fb.h) != 0) fprintf(stderr, "zwm: the GPU screen did not follow the resize\n");
    for (struct win *w = bottom; w; w = w->above) {
        if (w->flags & (ZWM_DOCK_TOP | ZWM_DOCK_BOTTOM)) {
            w->x = 0;
            if (w->flags & ZWM_DOCK_BOTTOM) w->y = fb.h - w->h;
            if (w->w != fb.w) { w->w = fb.w; struct zwm_m_geom g = { w->x, w->y, w->w, w->h, fb.w, fb.h }; send_msg(w->owner, ZWM_S_RESIZE, w->id, &g, sizeof g); }
            continue;
        }
        if (w->maximized) {                 /* keep filling the work area */
            w->x = BORDER; w->y = workarea_top + TITLE_H + BORDER;
            win_resize(w, fb.w - 2 * BORDER, workarea_bottom - workarea_top - TITLE_H - 2 * BORDER);
            continue;
        }
        int nw = w->w > fb.w - 2 * BORDER ? fb.w - 2 * BORDER : w->w, nh = w->h > fb.h - TITLE_H - 2 * BORDER ? fb.h - TITLE_H - 2 * BORDER : w->h;
        if (w->x + nw > fb.w) w->x = fb.w - nw;
        if (w->y + nh > fb.h) w->y = fb.h - nh;
        if (w->x < BORDER) w->x = BORDER;
        if (w->y < TITLE_H + BORDER) w->y = TITLE_H + BORDER;
        if (nw != w->w || nh != w->h) win_resize(w, nw, nh);
    }
    recompute_workarea();
    if (mx >= fb.w) mx = fb.w - 1;
    if (my >= fb.h) my = fb.h - 1;
    damage(0, 0, fb.w, fb.h);
}

static void restore(void)
{
    if (con_fd >= 0) ioctl(con_fd, KDSKBMODE, K_XLATE);
    if (fb.fd >= 0) ioctl(fb.fd, KDSETMODE, KD_TEXT);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);
    if (open_fb() != 0) return 1;
    back = zwm_surface_new(fb.w, fb.h);
    const char *sw = getenv("ZWM_SOFTWARE");
    if (!(sw && *sw == '1') && hw_init(fb.w, fb.h) == 0) { gpu = 1; fprintf(stderr, "zwm: composing on the GPU (%s)\n", hw_name()); }
    con_fd = open("/dev/console", O_RDONLY | O_NONBLOCK);
    if (con_fd < 0 || ioctl(con_fd, KDSKBMODE, K_RAW) != 0) { perror("zwm: /dev/console raw mode"); restore(); return 1; }
    mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    panel_fd = open("/dev/panel", O_WRONLY | O_CLOEXEC);
    if (mouse_fd < 0) fprintf(stderr, "zwm: no /dev/mouse, keyboard only\n");

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = ZWM_SOCKET };
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(listen_fd, 8) != 0) { perror("zwm: listen"); restore(); return 1; }
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    mx = fb.w / 2; my = fb.h / 2;
    workarea_top = safe_top; workarea_bottom = fb.h;
    damage(0, 0, fb.w, fb.h);
    flush();

    while (!quit) {
        struct pollfd pf[4 + MAX_CLIENTS];
        int n = 0;
        pf[n++] = (struct pollfd){ con_fd, POLLIN, 0 };
        pf[n++] = (struct pollfd){ listen_fd, POLLIN, 0 };
        if (mouse_fd >= 0) pf[n++] = (struct pollfd){ mouse_fd, POLLIN, 0 };
        int fbidx = n;
        pf[n++] = (struct pollfd){ fb.fd, POLLPRI, 0 };
        int cbase = n;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i]) pf[n++] = (struct pollfd){ clients[i]->fd, POLLIN, 0 };
        if (poll(pf, (nfds_t)n, flush_wait_ms) < 0) { if (errno == EINTR) continue; break; }

        if (pf[0].revents & POLLIN) {
            uint8_t buf[64];
            ssize_t r = read(con_fd, buf, sizeof buf);
            for (ssize_t i = 0; i < r; i++) key_event(buf[i]);
        }
        if (pf[1].revents & POLLIN) accept_client();
        if (mouse_fd >= 0 && (pf[2].revents & POLLIN)) {
            struct mouse_event ev[16];
            ssize_t r = read(mouse_fd, ev, sizeof ev);
            for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++) mouse_event(&ev[i]);
        }
        if (pf[fbidx].revents & POLLPRI) display_resized();
        for (int i = cbase; i < n; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (int k = 0; k < MAX_CLIENTS; k++)
                if (clients[k] && clients[k]->fd == pf[i].fd) { client_read(clients[k]); break; }
        }
        flush();
    }
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i]) client_close(clients[i]);
    restore();
    return 0;
}
