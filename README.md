# zwm — the window server for sic

One process that owns the framebuffer, the keyboard and the mouse, and puts
other programs' pixels on the screen with a title bar around them. Optional:
sic and ZAE don't know it exists; `make install` drops it into the sysroot's
`rootfs/` overlay and ZAE packs that into the initrd.

```
server/zwm.c    the server and window manager
server/hwcomp*  composing on the GPU: hwcomp_gl.c (zgl on virgl),
                hwcomp_adreno.c (a phone's Adreno), hwcomp.c picks one
lib/            libzwm: the client side, and pixel-buffer drawing
include/zwm.h   the protocol and the API
```

Clients connect to the `AF_UNIX` socket `/run/zwm`, create windows and
give them pixels: normally a buffer both sides map (`zwm_window_surface`,
a `/dev/shmem` segment) so a flush just names the rectangle that changed,
or a copy over the socket (`zwm_blit`). The server composes the stack into a
back buffer, copies the damaged part to
`/dev/fb0` and asks the kernel to present it (`FBIOPRESENT`, a no-op on
scanout hardware, a push on virtio-gpu). Input comes back as events: keys (already translated, with
modifiers), mouse position and buttons in window coordinates, resize,
focus, and a close request when the box in the title bar is clicked (the
client decides what to do). Windows can be plain, undecorated, or docks
pinned to the top or bottom edge that keep other windows out of their way.

With a GPU (zgl on virgl) the server composes there instead: the screen
is a GL buffer the display scans out, each window a texture (its pixels,
uploaded where they changed) under its decoration (drawn in software
only when it changes). A GL client's window can be its GPU buffer itself
(`zwm_attach_gpu`, when the greeting says `ZWM_HELLO_GPU`): no readback,
no copy; `ZWM_S_FRAME` after each frame paces it (`zwm_wait_frame`), and
the server composes at most 60 times a second. `ZWM_SOFTWARE=1` keeps the
software path. `Ctrl+Alt+P` saves the screen to `/tmp/zwm-shot.ppm`.
On a phone the Power key turns the panel off and on (`/dev/panel`); while
it is off touches are ignored and nothing is composed.

On a phone with an Adreno 6xx (`/dev/adrenogpu`, through zgl's libadreno)
the server composes on its 2D engine instead, when there is no virgl: a
back buffer in GPU memory gets the background, every window's content
and the opaque part of its decoration as GPU copies; the engine cannot
blend, so the CPU blends the shadow, antialiased corners and cursor once
the GPU has drawn under them, and the GPU copies the frame to the screen.
Clients stay on shared memory there (no GL). It checks its copies on a
scratch buffer first and leaves the screen to software if they come out
wrong.

The screen follows the display: when `/dev/fb0` changes mode (a resized
QEMU window) docks re-span the edges and windows are kept on screen.

Managing is what you'd expect: click to focus and raise, drag the title
bar to move, drag the bottom-right corner to resize, and the three
controls on the right of the title bar minimize, maximize (to the work
area, toggling) and close. Minimized windows come back through a taskbar:
a dock created with `ZWM_TASKBAR` receives `ZWM_S_WINDOWS` (every window,
its title and state) whenever that changes and can `zwm_activate`,
`zwm_minimize` or `zwm_maximize` any of them. The look is a dark neutral
palette (`ZWM_COL_*` in `zwm.h`, shared by the programs): a 1 px border,
antialiased rounded corners, a soft shadow, white-on-dark text.
HiDPI: on a screen 2000 pixels or more across the server draws frames,
controls and the cursor twice as big (mouse deltas count double), and
tells every client in its greeting (`ZWM_S_HELLO`); programs lay
themselves out in multiples of `zwm_scale()` and `ZWM_UI_PX` follows.
`ZWM_SCALE=1|2` in the server's environment overrides the choice.
`Alt+Tab` cycles through the windows (Shift+Tab backwards; the round ends
when Alt goes up). `Ctrl+Alt+Q` ends the server and gives the text console back.

Text comes from TrueType fonts through stb_truetype (`lib/font.c`):
`zwm_ttext` draws Inter (Regular or Medium) at a pixel size, `ZWM_FONT_MONO`
is JetBrains Mono for terminals; `fonts/` has them and their OFL licences,
`make install` puts them under `/usr/share/fonts`. Without the files the
calls fall back to the 8x8 bitmap font (`zwm_text`), which stays for grids.
Antialiased shapes go with it: `zwm_round_rect`, `zwm_round_rect_border`,
`zwm_disc`, `zwm_blend`. `zwm_icon_load` pulls a program's icon out of its
`.zicon` ELF section ("ZICN", width, height, RGBA; zde's `mkicon.py` puts
it there), `zwm_icon_file` reads one from a file, `zwm_icon_draw` scales
and blends it.

A client in twenty lines:

```c
#include <zwm.h>
int main(void) {
    zwm *c = zwm_connect();
    struct zwm_m_geom g;
    int win = zwm_create(c, 300, 200, "hello", 0, &g);
    zwm_surface *s = zwm_surface_new(g.w, g.h);
    zwm_fill(s, 0, 0, s->w, s->h, ZWM_COL_WINDOW);
    zwm_ttext(s, 20, 20, "hello, sic", ZWM_COL_TEXT, 16);
    zwm_flush(c, win, s);
    for (zwm_event ev; zwm_next_event(c, &ev, 1) > 0; )
        if (ev.type == ZWM_S_CLOSE) break;
    return 0;
}
```

Link with `libzwm.a` and the sic musl (see the zde Makefile).

## Building

```bash
make            # build/<arch>/zwm and libzwm.a
make install    # -> $SYSROOT/rootfs/bin/zwm, $SYSROOT/usr/{include/zwm.h,lib/libzwm.a}
```

Same toolchain and sysroot as ZAE (`$SIC_SYSROOT`). Needs a kernel with
`CONFIG_FB_CONSOLE` (32 bpp), `CONFIG_KEYBOARD` and `CONFIG_MOUSE`; without a
mouse it runs keyboard-only, which isn't much use.

## Not there yet

Keyboard shortcuts for switching windows,
anything on PowerPC (the port has no mouse).

## License

Copyright (C) 2026 Rigby Foundation. The server is GPL-2.0-only (`LICENSE`).
libzwm (`lib/`, `include/zwm.h`) is LGPL-2.1-or-later (`LICENSE.LGPL`) so
any program can link it.
