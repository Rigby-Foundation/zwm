/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The wire side of libzwm: an AF_UNIX connection to the server,
 * framed messages out, a small parser for the ones coming back. */
#include "zwm.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <abi/shm.h>

struct zwm {
    int fd;
    uint8_t *in;                /* unread bytes from the socket */
    size_t in_len, in_cap;
    /* replies to zwm_create are pulled out of the stream; everything else
     * queues here in order for zwm_next_event */
    zwm_event *q;
    size_t q_len, q_cap, q_head;
    struct zwm_m_window wins[32];   /* the last ZWM_S_WINDOWS list */
    uint32_t frames;            /* ZWM_S_FRAME messages seen: counted, never queued */
    uint32_t taken;             /* ZWM_S_TAKEN likewise */
};

static int send_all(zwm *c, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd pf = { c->fd, POLLOUT, 0 }; poll(&pf, 1, -1); continue; }
            return -1;
        }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int send_msg(zwm *c, uint32_t type, uint32_t win, const void *payload, size_t len)
{
    struct zwm_hdr h = { type, (uint32_t)len, win };
    if (send_all(c, &h, sizeof h) != 0) return -1;
    return len ? send_all(c, payload, len) : 0;
}

static int ui_scale;
static uint32_t hello_flags;

static int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

zwm *zwm_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = ZWM_SOCKET };
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return NULL; }
    zwm *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    /* the server's greeting: the screen, and how big things are drawn */
    struct zwm_hdr h;
    struct zwm_m_hello hello;
    if (recv_all(fd, &h, sizeof h) == 0 && h.type == ZWM_S_HELLO && h.len == sizeof hello && recv_all(fd, &hello, sizeof hello) == 0)
    { ui_scale = hello.scale > 0 ? hello.scale : 1; hello_flags = hello.flags; }
    return c;
}

int zwm_scale(void)
{
    if (ui_scale <= 0) {
        const char *e = getenv("ZWM_SCALE");
        ui_scale = e && atoi(e) > 0 ? atoi(e) : 1;
    }
    return ui_scale;
}

void zwm_damage(zwm *c, int win, int x, int y, int w, int h)
{
    struct zwm_m_rect r = { x, y, w, h };
    send_msg(c, ZWM_C_DAMAGE, (uint32_t)win, &r, sizeof r);
}

int zwm_gpu_composited(void) { return (hello_flags & ZWM_HELLO_GPU) != 0; }

int zwm_attach_gpu(zwm *c, int win, uint32_t res, int w, int h)
{
    struct zwm_m_attach_gpu a = { res, w, h };
    return send_msg(c, ZWM_C_ATTACH_GPU, (uint32_t)win, &a, sizeof a);
}

void zwm_disconnect(zwm *c)
{
    if (!c) return;
    close(c->fd);
    free(c->in);
    free(c->q);
    free(c);
}

int zwm_fd(zwm *c) { return c->fd; }

/* Read what the socket has (blocking for at least one byte when asked).
 * Returns 0, or -1 when the server is gone. */
static int fill(zwm *c, int block)
{
    if (c->in_len + 65536 > c->in_cap) {
        size_t cap = c->in_cap ? c->in_cap * 2 : 65536;
        while (cap < c->in_len + 65536) cap *= 2;
        uint8_t *p = realloc(c->in, cap);
        if (!p) return -1;
        c->in = p; c->in_cap = cap;
    }
    for (;;) {
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, block ? 0 : MSG_DONTWAIT);
        if (n > 0) { c->in_len += (size_t)n; return 0; }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN && !block) return 0;
        return -1;
    }
}

/* Pull one complete message off the input buffer into *ev; 1 if there was one. */
static int parse_one(zwm *c, zwm_event *ev)
{
again:
    if (c->in_len < sizeof(struct zwm_hdr)) return 0;
    struct zwm_hdr h;
    memcpy(&h, c->in, sizeof h);
    if (c->in_len < sizeof h + h.len) return 0;
    if (h.type == ZWM_S_FRAME || h.type == ZWM_S_TAKEN) {
        if (h.type == ZWM_S_FRAME) c->frames++; else c->taken++;
        memmove(c->in, c->in + sizeof h + h.len, c->in_len - sizeof h - h.len);
        c->in_len -= sizeof h + h.len;
        goto again;
    }
    memset(ev, 0, sizeof *ev);
    ev->type = h.type;
    ev->win = h.win;
    const uint8_t *p = c->in + sizeof h;
    switch (h.type) {
    case ZWM_S_CREATED: case ZWM_S_RESIZE: if (h.len >= sizeof ev->geom) memcpy(&ev->geom, p, sizeof ev->geom); break;
    case ZWM_S_KEY:   if (h.len >= sizeof ev->key) memcpy(&ev->key, p, sizeof ev->key); break;
    case ZWM_S_MOUSE: if (h.len >= sizeof ev->mouse) memcpy(&ev->mouse, p, sizeof ev->mouse); break;
    case ZWM_S_FOCUS: if (h.len >= sizeof ev->focus) memcpy(&ev->focus, p, sizeof ev->focus); break;
    case ZWM_S_WINDOWS: {
        int n = (int)(h.len / sizeof(struct zwm_m_window));
        if (n > 32) n = 32;
        ev->windows.count = n;
        memcpy(c->wins, p, (size_t)n * sizeof(struct zwm_m_window));
        ev->windows.w = c->wins;
        break;
    }
    default: break;
    }
    size_t used = sizeof h + h.len;
    memmove(c->in, c->in + used, c->in_len - used);
    c->in_len -= used;
    return 1;
}

static int enqueue(zwm *c, const zwm_event *ev)
{
    if (c->q_len == c->q_cap) {
        size_t cap = c->q_cap ? c->q_cap * 2 : 64;
        zwm_event *q = realloc(c->q, cap * sizeof *q);
        if (!q) return -1;
        c->q = q; c->q_cap = cap;
    }
    if (c->q_head && c->q_head == c->q_len) c->q_head = c->q_len = 0;
    c->q[c->q_len++] = *ev;
    return 0;
}

int zwm_next_event(zwm *c, zwm_event *ev, int block)
{
    for (;;) {
        if (c->q_head < c->q_len) { *ev = c->q[c->q_head++]; return 1; }
        c->q_head = c->q_len = 0;
        if (parse_one(c, ev)) return 1;
        if (fill(c, block) != 0) return -1;
        if (!block && !parse_one(c, ev)) return 0;
        if (!block) return 1;
    }
}

uint32_t zwm_frames(zwm *c) { return c->frames; }

/* Until *count moves on from `since`, reading (and queueing) everything else. */
static int wait_count(zwm *c, const uint32_t *count, uint32_t since, int timeout_ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        zwm_event ev;
        while (parse_one(c, &ev)) enqueue(c, &ev);     /* everything else stays in order for zwm_next_event */
        if (*count != since) return 0;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return 1;
        struct pollfd pf = { c->fd, POLLIN, 0 };
        if (poll(&pf, 1, left) > 0 && fill(c, 0) != 0) return -1;
    }
}

int zwm_wait_frame(zwm *c, uint32_t since, int timeout_ms) { return wait_count(c, &c->frames, since, timeout_ms); }

int zwm_create(zwm *c, int w, int h, const char *title, uint32_t flags, struct zwm_m_geom *geom)
{
    struct zwm_m_create m = { w, h, flags, { 0 }, { 0 } };
    if (title) strncpy(m.title, title, sizeof m.title - 1);
    /* who we are, so a dock can show our icon */
    ssize_t n = readlink("/proc/self/exe", m.exe, sizeof m.exe - 1);
    if (n < 0) n = 0;
    m.exe[n] = 0;
    if (send_msg(c, ZWM_C_CREATE, 0, &m, sizeof m) != 0) return -1;
    /* the answer may be behind other events: queue those */
    for (;;) {
        zwm_event ev;
        while (!parse_one(c, &ev))
            if (fill(c, 1) != 0) return -1;
        if (ev.type == ZWM_S_CREATED) {
            if (geom) *geom = ev.geom;
            return (int)ev.win;
        }
        enqueue(c, &ev);
    }
}

void zwm_destroy(zwm *c, int win) { send_msg(c, ZWM_C_DESTROY, (uint32_t)win, NULL, 0); }
void zwm_raise(zwm *c, int win)   { send_msg(c, ZWM_C_RAISE, (uint32_t)win, NULL, 0); }
void zwm_activate(zwm *c, int win) { send_msg(c, ZWM_C_ACTIVATE, (uint32_t)win, NULL, 0); }
void zwm_minimize(zwm *c, int win) { send_msg(c, ZWM_C_MINIMIZE, (uint32_t)win, NULL, 0); }
void zwm_maximize(zwm *c, int win) { send_msg(c, ZWM_C_MAXIMIZE, (uint32_t)win, NULL, 0); }
void zwm_close(zwm *c, int win) { send_msg(c, ZWM_C_CLOSE, (uint32_t)win, NULL, 0); }
void zwm_set_title(zwm *c, int win, const char *title) { send_msg(c, ZWM_C_TITLE, (uint32_t)win, title, strlen(title) + 1); }
void zwm_move(zwm *c, int win, int x, int y) { struct zwm_m_point p = { x, y }; send_msg(c, ZWM_C_MOVE, (uint32_t)win, &p, sizeof p); }

void zwm_blit(zwm *c, int win, int x, int y, int w, int h, const uint32_t *pix, int stride)
{
    if (w <= 0 || h <= 0) return;
    struct zwm_m_rect r = { x, y, w, h };
    struct zwm_hdr hd = { ZWM_C_BLIT, (uint32_t)(sizeof r + (size_t)w * h * 4), (uint32_t)win };
    if (send_all(c, &hd, sizeof hd) != 0 || send_all(c, &r, sizeof r) != 0) return;
    if (stride == w) {
        send_all(c, pix, (size_t)w * h * 4);
        return;
    }
    /* Rows that aren't contiguous: pack them, one send instead of one per
     * row (each send is a round trip through the kernel's loopback). */
    size_t rows = 65536 / ((size_t)w * 4);
    if (rows < 1) rows = 1;
    uint32_t *tmp = malloc(rows * (size_t)w * 4);
    if (!tmp) {
        for (int j = 0; j < h; j++)
            if (send_all(c, pix + (size_t)j * stride, (size_t)w * 4) != 0) return;
        return;
    }
    for (int j = 0; j < h; j += (int)rows) {
        int n = (int)rows < h - j ? (int)rows : h - j;
        for (int k = 0; k < n; k++)
            memcpy(tmp + (size_t)k * w, pix + (size_t)(j + k) * stride, (size_t)w * 4);
        if (send_all(c, tmp, (size_t)n * w * 4) != 0) break;
    }
    free(tmp);
}

/* ---- surfaces the server maps ------------------------------------------- */
/* A segment of w*h pixels from /dev/shmem, mapped here and attached to the
 * window on the server. Returns the fd (kept open while mapped) or -1. */
static int shm_new(zwm *c, int win, int w, int h, uint32_t **pix, size_t *len)
{
    int fd = open("/dev/shmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct shm_segment seg = { .size = (uint64_t)w * h * 4 };
    if (ioctl(fd, SHM_IOC_CREATE, &seg) != 0) { close(fd); return -1; }
    void *m = mmap(NULL, seg.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    struct zwm_m_attach a = { seg.key, w, h, w };
    if (send_msg(c, ZWM_C_ATTACH, (uint32_t)win, &a, sizeof a) != 0) { munmap(m, seg.size); close(fd); return -1; }
    *pix = m; *len = seg.size;
    return fd;
}

zwm_surface *zwm_window_surface(zwm *c, int win, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    zwm_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fd = shm_new(c, win, w, h, &s->pix, &s->map_len);
    if (s->fd < 0) {                        /* no shared memory: an ordinary surface */
        free(s);
        return zwm_surface_new(w, h);
    }
    s->conn = c; s->win = win; s->w = w; s->h = h;
    return s;
}

/* draw.c calls these for surfaces with a connection */
int zwm_shared_resize(zwm_surface *s, int w, int h)
{
    uint32_t *pix; size_t len;
    int fd = shm_new(s->conn, s->win, w, h, &pix, &len);
    if (fd < 0) return -1;
    munmap(s->pix, s->map_len);
    close(s->fd);
    s->fd = fd; s->pix = pix; s->map_len = len; s->w = w; s->h = h;
    return 0;
}

void zwm_shared_free(zwm_surface *s)
{
    munmap(s->pix, s->map_len);
    close(s->fd);
}

void zwm_flush_rect(zwm *c, int win, const zwm_surface *s, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    if (s->conn) {                          /* shared: the server copies them out */
        struct zwm_m_rect r = { x, y, w, h };
        uint32_t since = c->taken;
        send_msg(c, ZWM_C_DAMAGE, (uint32_t)win, &r, sizeof r);
        /* and until it has, drawing the next frame would show half of it */
        if (hello_flags & ZWM_HELLO_TAKEN) wait_count(c, &c->taken, since, 250);
        return;
    }
    zwm_blit(c, win, x, y, w, h, s->pix + (size_t)y * s->w + x, s->w);
}

void zwm_flush(zwm *c, int win, const zwm_surface *s) { zwm_flush_rect(c, win, s, 0, 0, s->w, s->h); }
