/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * libzwm: talking to the zwm window server, and drawing into the pixel
 * buffers you send it.
 *
 * A client connects to the server's AF_UNIX socket (ZWM_SOCKET), creates
 * windows, and gives them pixels: either a buffer shared with the server
 * (zwm_window_surface, /dev/shmem) that a flush merely points at, or a
 * copy sent over the socket (zwm_blit). The server composes them onto the
 * framebuffer, draws the decorations and the cursor, and sends back input
 * and window events. Pixels are 0x00RRGGBB.
 *
 * Everything is synchronous and single-threaded: zwm_next_event() reads the
 * socket (blocking or not), zwm_fd() lets you poll() it alongside your own
 * descriptors.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ZWM_SOCKET "/run/zwm"          /* the server's AF_UNIX name */

/* ---- protocol ------------------------------------------------------------ */
/* Every message is a zwm_hdr followed by `len` bytes of payload. Both
 * sides run on the same machine, so fields are in host byte order. */
struct zwm_hdr { uint32_t type, len, win; };

enum {
    /* client -> server */
    ZWM_C_CREATE = 1,       /* zwm_m_create; answered by ZWM_S_CREATED */
    ZWM_C_BLIT,             /* zwm_m_rect + w*h pixels */
    ZWM_C_TITLE,            /* NUL-terminated title */
    ZWM_C_DESTROY,          /* - */
    ZWM_C_MOVE,             /* zwm_m_point */
    ZWM_C_RAISE,            /* - */
    ZWM_C_ATTACH,           /* zwm_m_attach: the window's pixels are this shared segment from now on */
    ZWM_C_DAMAGE,           /* zwm_m_rect: this part of the shared segment changed */
    ZWM_C_ACTIVATE,         /* header win = a window (any client's): raise it, restore it if minimized, focus it */
    ZWM_C_MINIMIZE,         /* - : hide the window until it is activated */
    ZWM_C_MAXIMIZE,         /* - : toggle filling the work area (any window: a panel's menu does it too) */
    ZWM_C_ATTACH_GPU,       /* zwm_m_attach_gpu: the window's pixels are this GPU resource (only if the hello says ZWM_HELLO_GPU); DAMAGE says when a frame is done */
    ZWM_C_CLOSE,            /* header win = a window (any client's): ask its owner to close it (it gets ZWM_S_CLOSE) */
    /* server -> client */
    ZWM_S_CREATED = 0x100,  /* zwm_m_geom: the id is in the header */
    ZWM_S_KEY,              /* zwm_m_key */
    ZWM_S_MOUSE,            /* zwm_m_mouse */
    ZWM_S_RESIZE,           /* zwm_m_geom: the server changed the size, redraw */
    ZWM_S_CLOSE,            /* the close box was clicked; the window still exists */
    ZWM_S_FOCUS,            /* zwm_m_focus */
    ZWM_S_WINDOWS,          /* zwm_m_window[]: every decorated window, to ZWM_TASKBAR clients, whenever it changes */
    ZWM_S_HELLO,            /* zwm_m_hello, first thing after connecting: the screen and the UI scale */
    ZWM_S_FRAME,            /* - : a frame with this window's GPU content went on screen (pacing; libzwm counts these) */
    ZWM_S_TAKEN,            /* - : the server copied what a DAMAGE named out of the shared segment; it may be drawn again (libzwm counts these) */
};

/* window flags */
#define ZWM_UNDECORATED   0x1   /* no title bar or border */
#define ZWM_DOCK_BOTTOM   0x2   /* full width, pinned to the bottom of the screen, always on top */
#define ZWM_DOCK_TOP      0x4   /* likewise, at the top */
#define ZWM_FIXED         0x8   /* not movable or resizable by the user */
#define ZWM_TASKBAR       0x10  /* this client wants ZWM_S_WINDOWS (a panel) */

struct zwm_m_create { int32_t w, h; uint32_t flags; char title[64]; char exe[128]; };   /* exe: the program, for its icon (libzwm fills it in) */
struct zwm_m_rect   { int32_t x, y, w, h; };
struct zwm_m_point  { int32_t x, y; };
struct zwm_m_geom   { int32_t x, y, w, h; int32_t screen_w, screen_h; };
struct zwm_m_key    { uint32_t sym; uint32_t mods; uint32_t down; uint32_t scancode; };
struct zwm_m_mouse  { int32_t x, y; uint32_t buttons; uint32_t kind; };
struct zwm_m_focus  { uint32_t focused; };
struct zwm_m_attach { uint32_t key; int32_t w, h, stride; };   /* /dev/shmem key; stride in pixels */
struct zwm_m_window { uint32_t id; uint32_t state; char title[64]; char exe[128]; };
struct zwm_m_attach_gpu { uint32_t res; int32_t w, h; };     /* a host resource (sic_gl_buffer), B8G8R8A8, rows bottom-up */
struct zwm_m_hello  { int32_t screen_w, screen_h; int32_t scale; uint32_t flags; };
#define ZWM_HELLO_GPU     1     /* the server composites on the GPU: windows may be GPU resources */
#define ZWM_HELLO_TAKEN   2     /* the server answers DAMAGE on a shared segment with ZWM_S_TAKEN */
/* ZWM_S_WINDOWS entries, bottom to top; exe = the program behind it */
#define ZWM_WIN_FOCUSED   1
#define ZWM_WIN_MINIMIZED 2
#define ZWM_WIN_MAXIMIZED 4

/* key syms: printable ASCII as itself (already shifted), the rest above 0xFF */
enum {
    ZWM_KEY_UP = 0x100, ZWM_KEY_DOWN, ZWM_KEY_LEFT, ZWM_KEY_RIGHT,
    ZWM_KEY_HOME, ZWM_KEY_END, ZWM_KEY_PGUP, ZWM_KEY_PGDN,
    ZWM_KEY_INSERT, ZWM_KEY_DELETE, ZWM_KEY_F1,     /* F1..F12 are consecutive */
    ZWM_KEY_SHIFT = 0x200, ZWM_KEY_CTRL, ZWM_KEY_ALT, ZWM_KEY_CAPS,
};
#define ZWM_MOD_SHIFT 1
#define ZWM_MOD_CTRL  2
#define ZWM_MOD_ALT   4

#define ZWM_MOUSE_MOVE    0     /* at (-1, -1): the cursor left the window */
#define ZWM_MOUSE_PRESS   1
#define ZWM_MOUSE_RELEASE 2
#define ZWM_BTN_LEFT   1
#define ZWM_BTN_RIGHT  2
#define ZWM_BTN_MIDDLE 4

/* ---- client API ---------------------------------------------------------- */
typedef struct zwm zwm;

typedef struct {
    uint32_t type;              /* ZWM_S_* */
    uint32_t win;
    union {
        struct zwm_m_geom  geom;
        struct zwm_m_key   key;
        struct zwm_m_mouse mouse;
        struct zwm_m_focus focus;
        /* ZWM_S_WINDOWS: the newest list the server sent, kept by the
         * connection; read it before the next zwm_next_event. */
        struct { int count; const struct zwm_m_window *w; } windows;
    };
} zwm_event;

zwm *zwm_connect(void);                     /* NULL (errno set) if no server */
void zwm_disconnect(zwm *c);
int  zwm_fd(zwm *c);                        /* for poll(); readable = events waiting */

/* Creates a window and waits for the server's answer: the id (> 0) or -1.
 * *geom (optional) receives the placement and the screen size. */
int  zwm_create(zwm *c, int w, int h, const char *title, uint32_t flags, struct zwm_m_geom *geom);
void zwm_destroy(zwm *c, int win);
void zwm_set_title(zwm *c, int win, const char *title);
void zwm_move(zwm *c, int win, int x, int y);
void zwm_raise(zwm *c, int win);
void zwm_activate(zwm *c, int win);         /* any window: raise, unminimize, focus (panels) */
void zwm_minimize(zwm *c, int win);
void zwm_maximize(zwm *c, int win);         /* toggles; any window */
void zwm_close(zwm *c, int win);            /* any window: its owner is asked to close it, as by the close box */

/* Copies a w*h block of pixels (rows `stride` pixels apart) to (x, y) of
 * the window, over the socket. */
void zwm_blit(zwm *c, int win, int x, int y, int w, int h, const uint32_t *pix, int stride);

/* Whether the server composites on the GPU (valid once connected); then a
 * window's pixels may be a GPU resource (sic_gl_buffer), attached once per
 * buffer, with zwm_damage() after each finished frame. */
int  zwm_gpu_composited(void);
int  zwm_attach_gpu(zwm *c, int win, uint32_t res, int w, int h);
void zwm_damage(zwm *c, int win, int x, int y, int w, int h);   /* that part of the window's pixels changed */
/* Pacing for GPU windows: frames shown so far, and waiting (up to
 * timeout_ms) for the count to move past `since`. 0 when it did. */
uint32_t zwm_frames(zwm *c);
int  zwm_wait_frame(zwm *c, uint32_t since, int timeout_ms);

/* Next event: 1 and *ev filled, 0 if none (non-blocking mode), -1 if the
 * server went away. */
int  zwm_next_event(zwm *c, zwm_event *ev, int block);

/* ---- drawing into a client-side buffer ----------------------------------- */
typedef struct {
    int w, h;
    uint32_t *pix;              /* w*h, row-major */
    /* private: a surface shared with the server */
    zwm *conn; int win, fd; size_t map_len;
} zwm_surface;

zwm_surface *zwm_surface_new(int w, int h);                 /* plain memory: flush = blit */
/* Memory the server maps too (/dev/shmem): flushing sends only the
 * rectangle. Falls back to a plain surface when there is no shared memory. */
zwm_surface *zwm_window_surface(zwm *c, int win, int w, int h);
int   zwm_surface_resize(zwm_surface *s, int w, int h);     /* contents are lost */
void  zwm_surface_free(zwm_surface *s);

void  zwm_fill(zwm_surface *s, int x, int y, int w, int h, uint32_t color);
void  zwm_rect(zwm_surface *s, int x, int y, int w, int h, uint32_t color);   /* 1px outline */
void  zwm_hline(zwm_surface *s, int x, int y, int w, uint32_t color);
void  zwm_vline(zwm_surface *s, int x, int y, int h, uint32_t color);
void  zwm_copy(zwm_surface *dst, int dx, int dy, const zwm_surface *src, int sx, int sy, int w, int h);

/* Antialiased shapes. Coverage is 0..16 (4x4 samples per pixel). */
int   zwm_round_cov(int px, int py, int x, int y, int w, int h, int r);   /* of a rounded rectangle at one pixel */
uint32_t zwm_mix(uint32_t under, uint32_t over, int alpha);           /* alpha 0..255 */
void  zwm_blend(zwm_surface *s, int x, int y, uint32_t color, int alpha);  /* alpha 0..255 over one pixel */
void  zwm_round_rect(zwm_surface *s, int x, int y, int w, int h, int r, uint32_t color);           /* filled */
void  zwm_round_rect_border(zwm_surface *s, int x, int y, int w, int h, int r, uint32_t color);    /* 1px outline */
void  zwm_disc(zwm_surface *s, int cx, int cy, int r, uint32_t color);     /* filled circle, centre (cx,cy) is a pixel */

/* 8x8 bitmap font scaled by `scale`; returns the width drawn. For grids
 * (the terminal). */
int   zwm_text(zwm_surface *s, int x, int y, const char *str, uint32_t color, int scale);
int   zwm_text_width(const char *str, int scale);
#define ZWM_FONT_H 8
extern const uint8_t zwm_font8x8[95][8];

/* The UI font: Inter from /usr/share/fonts, antialiased, `px` pixels tall
 * (ascent + descent = zwm_ttext_height). (x, y) is the top left of the
 * line box; the width drawn comes back. Falls back to the bitmap font
 * when the file is not there. ASCII; other code points draw as '?'. */
#define ZWM_FONT_REGULAR 0
#define ZWM_FONT_MEDIUM  1
#define ZWM_FONT_MONO    2          /* JetBrains Mono: terminals, code */
/* HiDPI: on a screen 2000 pixels or more across (a Retina panel at its
 * real size) the server draws its frames twice as big and tells every
 * client, which lays itself out in multiples of zwm_scale(). ZWM_SCALE in
 * the server's environment forces it. Programs multiply their pixel sizes
 * by zwm_scale() (valid once connected). */
int   zwm_scale(void);
#define ZWM_UI_PX        (13 * zwm_scale())   /* what the desktop uses */
int   zwm_ttext(zwm_surface *s, int x, int y, const char *str, uint32_t color, int px);
int   zwm_ttext_w(zwm_surface *s, int x, int y, const char *str, uint32_t color, int px, int weight);
int   zwm_ttext_width(const char *str, int px);
int   zwm_ttext_width_w(const char *str, int px, int weight);
int   zwm_ttext_height(int px);
int   zwm_ttext_height_w(int px, int weight);
int   zwm_font_ok(void);

/* App icons: the `.zicon` section of a program (mkicon.py in zde puts it
 * there: "ZICN", width, height, RGBA), or a file of the same bytes
 * (/usr/share/icons). Pixels come back 0xAARRGGBB, malloc'd. */
uint32_t *zwm_icon_load(const char *elf_path, int *w, int *h);
uint32_t *zwm_icon_file(const char *path, int *w, int *h);
void      zwm_icon_draw(zwm_surface *s, int x, int y, int size, const uint32_t *pix, int w, int h);

/* Send the whole surface, or a part of it, to the window. A shared one
 * returns once the server has copied the part out, so the next frame can
 * be drawn into it right away without the screen showing half of it. */
void  zwm_flush(zwm *c, int win, const zwm_surface *s);
void  zwm_flush_rect(zwm *c, int win, const zwm_surface *s, int x, int y, int w, int h);

/* The palette everyone shares so the desktop looks like one thing: a
 * dark neutral scale (zinc), one border tone, white-on-dark text. */
#define ZWM_RGB(r, g, b)     ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define ZWM_COL_BG           ZWM_RGB(0x09, 0x09, 0x0b)   /* the desktop */
#define ZWM_COL_WINDOW       ZWM_RGB(0x18, 0x18, 0x1b)   /* a window's content area */
#define ZWM_COL_SURFACE      ZWM_RGB(0x27, 0x27, 0x2a)   /* raised: hovered rows, scrollbars, inputs */
#define ZWM_COL_TEXT         ZWM_RGB(0xfa, 0xfa, 0xfa)
#define ZWM_COL_TEXT_DIM     ZWM_RGB(0xa1, 0xa1, 0xaa)
#define ZWM_COL_ACCENT       ZWM_RGB(0xfa, 0xfa, 0xfa)   /* primary: selected things are white ... */
#define ZWM_COL_ACCENT_TEXT  ZWM_RGB(0x18, 0x18, 0x1b)   /* ... with dark text on them */
#define ZWM_COL_ACCENT_DIM   ZWM_RGB(0x3f, 0x3f, 0x46)
#define ZWM_COL_PANEL        ZWM_RGB(0x09, 0x09, 0x0b)
#define ZWM_COL_PANEL_TEXT   ZWM_RGB(0xfa, 0xfa, 0xfa)
#define ZWM_COL_BORDER       ZWM_RGB(0x27, 0x27, 0x2a)
#define ZWM_COL_DANGER       ZWM_RGB(0xef, 0x44, 0x44)
