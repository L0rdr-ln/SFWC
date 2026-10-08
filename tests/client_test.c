/*
 * Test client used by tests/run_client_test.sh.
 *
 *   client_test [single]   one output: windows, popup, maximize/fullscreen/minimize,
 *                          input (virtual keyboard + pointer), snapping, live config reload
 *   client_test multi      two outputs with different size/scale/position: per-output
 *                          placement and maximize, follow-mouse focus, move/focus to the
 *                          next output, reload-config key (no file watching)
 *
 * It connects to the compositor named by $WAYLAND_DISPLAY and exits 0 when every check
 * passed. The compositor is started by the script with matching config and environment.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#ifdef HAVE_VIRTUAL_INPUT
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#endif

#define W 200
#define H 100
#define GAP 8   /* must match the default gap in src/config.c */
#define SNAP 12 /* must match the default snap_distance in src/config.c */
#define MAX_OUTS 4

struct app;

struct out_info {
    struct app *app;
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    char name[32];
    int mw, mh, scale;     /* wl_output mode (physical px) and scale */
    int lx, ly, lw, lh;    /* xdg_output logical position/size */
};

/* A plain toplevel window with its last configure. */
struct win {
    struct wl_surface *surface;
    struct xdg_surface *xs;
    struct xdg_toplevel *tl;
    struct wl_buffer *buf;
    int configured, closed;
    int cfg_arrived, cfg_w, cfg_h, cfg_max, cfg_fs;
};

struct app {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_output_manager_v1 *xdg_out_mgr;
    struct out_info outs[MAX_OUTS];
    int n_outs;
    struct wl_output *output; /* first output */
    int out_w, out_h;         /* its mode */
    int ext_w, ext_h;         /* extent of the whole output layout, for absolute pointer motion */

    /* main window of the single scenario, with its last configure */
    int cfg_arrived;
    int cfg_w, cfg_h, cfg_max, cfg_fs;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int configured;
    int closed;

    struct wl_surface *popup_surface;
    struct xdg_surface *popup_xdg_surface;
    struct xdg_popup *popup;
    int popup_configured;

    int frame_done;

    /* seat + input observed by this client */
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    int kb_enter, kb_leave;       /* counters */
    struct wl_surface *kb_surface; /* surface that currently has keyboard focus */
    int kb_rate, kb_delay;        /* last repeat_info */
    int key_presses[256];         /* key press events received, by key code */
    int ptr_enter, ptr_motion, ptr_button_press, ptr_button_release;
    double ptr_sx, ptr_sy;        /* last pointer position, surface-local */
#ifdef HAVE_VIRTUAL_INPUT
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
    struct zwlr_virtual_pointer_v1 *vptr;
    struct zwp_virtual_keyboard_v1 *vkbd;
    uint32_t t; /* fake timestamp, ms */
#endif
};

static void fail(const char *msg)
{
    fprintf(stderr, "client_test: FAIL: %s\n", msg);
    exit(1);
}

static struct wl_buffer *make_buffer(struct wl_shm *shm, int w, int h, uint32_t color)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("sfwc-test-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        fail("cannot create shm file");
    }
    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fail("mmap failed");
    }
    for (int i = 0; i < w * h; i++) {
        data[i] = color;
    }
    munmap(data, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ------------------------------------------------------------ main window */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(s, serial);
    if (s == app->xdg_surface) {
        app->configured = 1;
    } else {
        app->popup_configured = 1;
    }
}
static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states)
{
    struct app *app = data;
    app->cfg_w = w;
    app->cfg_h = h;
    app->cfg_max = app->cfg_fs = 0;
    uint32_t *st;
    wl_array_for_each(st, states)
    {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            app->cfg_max = 1;
        } else if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            app->cfg_fs = 1;
        }
    }
    app->cfg_arrived = 1;
}
static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    ((struct app *)data)->closed = 1;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
}
static void popup_done(void *d, struct xdg_popup *p) {}
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t token) {}
static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    ((struct app *)data)->frame_done = 1;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

/* ------------------------------------------------------------ seat input */

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    close(fd);
}
static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                     struct wl_array *keys)
{
    struct app *app = data;
    app->kb_surface = s;
    app->kb_enter++;
}
static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    struct app *app = data;
    app->kb_surface = NULL;
    app->kb_leave++;
}
static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
                   uint32_t state)
{
    struct app *app = data;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && key < 256) {
        app->key_presses[key]++;
    }
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b,
                         uint32_t c, uint32_t g)
{
}
static void kb_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    struct app *app = data;
    app->kb_rate = rate;
    app->kb_delay = delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    app->ptr_enter++;
    app->ptr_sx = wl_fixed_to_double(x);
    app->ptr_sy = wl_fixed_to_double(y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    app->ptr_motion++;
    app->ptr_sx = wl_fixed_to_double(x);
    app->ptr_sy = wl_fixed_to_double(y);
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
    struct app *app = data;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        app->ptr_button_press++;
    } else {
        app->ptr_button_release++;
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void ptr_frame(void *d, struct wl_pointer *p) {}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t x) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct app *app = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---------------------------------------------------------------- outputs */

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t sp, const char *make, const char *model, int32_t tr)
{
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh)
{
    struct out_info *info = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        info->mw = w;
        info->mh = h;
        if (info == &info->app->outs[0]) {
            info->app->out_w = w;
            info->app->out_h = h;
        }
    }
}
static void output_done(void *d, struct wl_output *o) {}
static void output_scale(void *data, struct wl_output *o, int32_t f)
{
    ((struct out_info *)data)->scale = f;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void xo_position(void *data, struct zxdg_output_v1 *x, int32_t px, int32_t py)
{
    struct out_info *info = data;
    info->lx = px;
    info->ly = py;
}
static void xo_size(void *data, struct zxdg_output_v1 *x, int32_t w, int32_t h)
{
    struct out_info *info = data;
    info->lw = w;
    info->lh = h;
}
static void xo_done(void *d, struct zxdg_output_v1 *x) {}
static void xo_name(void *data, struct zxdg_output_v1 *x, const char *name)
{
    snprintf(((struct out_info *)data)->name, sizeof(((struct out_info *)data)->name), "%s", name);
}
static void xo_description(void *d, struct zxdg_output_v1 *x, const char *desc) {}
static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xo_position,
    .logical_size = xo_size,
    .done = xo_done,
    .name = xo_name,
    .description = xo_description,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        app->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        app->xdg_out_mgr = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 2);
#ifdef HAVE_VIRTUAL_INPUT
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        app->vptr_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        app->vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
#endif
    } else if (strcmp(interface, wl_output_interface.name) == 0 && app->n_outs < MAX_OUTS) {
        struct out_info *info = &app->outs[app->n_outs++];
        info->app = app;
        info->wl = wl_registry_bind(reg, name, &wl_output_interface, 2);
        wl_output_add_listener(info->wl, &output_listener, info);
        if (!app->output) {
            app->output = info->wl;
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    }
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* Dispatch events until *flag becomes non-zero. Returns 0 on timeout. */
static int wait_flag(struct wl_display *display, const int *flag, int timeout_ms)
{
    while (!*flag) {
        while (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
        }
        wl_display_flush(display);
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) {
            wl_display_cancel_read(display);
            return 0;
        }
        if (wl_display_read_events(display) < 0) {
            fail("connection lost (compositor crashed?)");
        }
        wl_display_dispatch_pending(display);
    }
    return 1;
}

/* Like wait_flag, but a timeout is a test failure. */
static void wait_for(struct wl_display *display, const int *flag, int timeout_ms, const char *what)
{
    if (!wait_flag(display, flag, timeout_ms)) {
        char msg[128];
        snprintf(msg, sizeof msg, "timeout waiting for %s", what);
        fail(msg);
    }
}

/* Wait until a configure of the main window with the given state/size has arrived. */
static void expect_configure(struct app *app, struct wl_display *display, int max, int fs, int w,
                             int h, const char *what)
{
    for (int i = 0; i < 20; i++) {
        if (app->cfg_arrived && app->cfg_max == max && app->cfg_fs == fs && app->cfg_w == w &&
            app->cfg_h == h) {
            return;
        }
        app->cfg_arrived = 0;
        if (!wait_flag(display, &app->cfg_arrived, 3000)) {
            break; /* report what we last saw below */
        }
    }
    char msg[256];
    snprintf(msg, sizeof msg, "%s: last configure max=%d fs=%d size=%dx%d, wanted max=%d fs=%d %dx%d",
             what, app->cfg_max, app->cfg_fs, app->cfg_w, app->cfg_h, max, fs, w, h);
    fail(msg);
}

/* ------------------------------------------------------- additional windows */

static void win_xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    ((struct win *)data)->configured = 1;
}
static const struct xdg_surface_listener win_xs_listener = {.configure = win_xs_configure};

static void win_tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                             struct wl_array *states)
{
    struct win *win = data;
    win->cfg_w = w;
    win->cfg_h = h;
    win->cfg_max = win->cfg_fs = 0;
    uint32_t *st;
    wl_array_for_each(st, states)
    {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            win->cfg_max = 1;
        } else if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            win->cfg_fs = 1;
        }
    }
    win->cfg_arrived = 1;
}
static void win_tl_close(void *data, struct xdg_toplevel *t)
{
    ((struct win *)data)->closed = 1;
}
static const struct xdg_toplevel_listener win_tl_listener = {
    .configure = win_tl_configure,
    .close = win_tl_close,
};

static void win_open(struct app *app, struct wl_display *d, struct win *w, const char *title,
                     uint32_t color)
{
    memset(w, 0, sizeof *w);
    w->surface = wl_compositor_create_surface(app->compositor);
    w->xs = xdg_wm_base_get_xdg_surface(app->wm_base, w->surface);
    xdg_surface_add_listener(w->xs, &win_xs_listener, w);
    w->tl = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->tl, &win_tl_listener, w);
    xdg_toplevel_set_title(w->tl, title);
    wl_surface_commit(w->surface);
    wait_for(d, &w->configured, 3000, "window configure");
    w->buf = make_buffer(app->shm, W, H, color);
    wl_surface_attach(w->surface, w->buf, 0, 0);
    wl_surface_commit(w->surface);
    wl_display_roundtrip(d);
}

static void win_destroy(struct wl_display *d, struct win *w)
{
    xdg_toplevel_destroy(w->tl);
    xdg_surface_destroy(w->xs);
    wl_surface_destroy(w->surface);
    wl_buffer_destroy(w->buf);
    wl_display_roundtrip(d);
}

static void win_expect(struct win *w, struct wl_display *d, int max, int fs, int ew, int eh,
                       const char *what)
{
    for (int i = 0; i < 20; i++) {
        if (w->cfg_arrived && w->cfg_max == max && w->cfg_fs == fs && w->cfg_w == ew &&
            w->cfg_h == eh) {
            return;
        }
        w->cfg_arrived = 0;
        if (!wait_flag(d, &w->cfg_arrived, 3000)) {
            break; /* report what we last saw below */
        }
    }
    char msg[256];
    snprintf(msg, sizeof msg, "%s: last configure max=%d fs=%d size=%dx%d, wanted max=%d fs=%d %dx%d",
             what, w->cfg_max, w->cfg_fs, w->cfg_w, w->cfg_h, max, fs, ew, eh);
    fail(msg);
}

/* The window must NOT receive a configure within `ms` milliseconds. */
static void win_expect_quiet(struct win *w, struct wl_display *d, int ms, const char *what)
{
    w->cfg_arrived = 0;
    for (int waited = 0; waited < ms; waited += 50) {
        wl_display_roundtrip(d);
        usleep(50 * 1000);
    }
    wl_display_roundtrip(d);
    if (w->cfg_arrived) {
        char msg[200];
        snprintf(msg, sizeof msg, "unexpected configure: %s", what);
        fail(msg);
    }
}

#ifdef HAVE_VIRTUAL_INPUT
/* ------------------------------------------------------------ virtual input */

/* Modifier masks of the default xkb keymap. */
#define MOD_SHIFT 0x1
#define MOD_CTRL 0x4
#define MOD_ALT 0x8

static void send_keymap(struct app *app)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        fail("cannot compile default xkb keymap");
    }
    char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(str) + 1;
    int fd = memfd_create("sfwc-test-keymap", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        fail("cannot create keymap file");
    }
    char *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(map, str, size);
    munmap(map, size);
    zwp_virtual_keyboard_v1_keymap(app->vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    close(fd);
    free(str);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
}

static void vkey(struct app *app, struct wl_display *d, uint32_t mods, uint32_t key, int press)
{
    zwp_virtual_keyboard_v1_modifiers(app->vkbd, mods, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(app->vkbd, app->t += 10, key,
                                press ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_roundtrip(d);
}

/* Press and release `key` while `mods` are held (modifiers are set explicitly because
 * virtual keyboards do not update xkb state from key events). */
static void vtap(struct app *app, struct wl_display *d, uint32_t mods, uint32_t key)
{
    vkey(app, d, mods, key, 1);
    vkey(app, d, mods, key, 0);
    zwp_virtual_keyboard_v1_modifiers(app->vkbd, 0, 0, 0, 0);
    wl_display_roundtrip(d);
}

static void vptr_move(struct app *app, struct wl_display *d, double x, double y)
{
    zwlr_virtual_pointer_v1_motion_absolute(app->vptr, app->t += 10, (uint32_t)x, (uint32_t)y,
                                            app->ext_w, app->ext_h);
    zwlr_virtual_pointer_v1_frame(app->vptr);
    wl_display_roundtrip(d);
}

static void vptr_button(struct app *app, struct wl_display *d, uint32_t button, int press)
{
    zwlr_virtual_pointer_v1_button(app->vptr, app->t += 10, button,
                                   press ? WL_POINTER_BUTTON_STATE_PRESSED
                                         : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(app->vptr);
    wl_display_roundtrip(d);
}

static void check_near(double got, double want, double tol, const char *what)
{
    if (got < want - tol || got > want + tol) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: got %.1f, wanted %.1f (+-%.0f)", what, got, want, tol);
        fail(msg);
    }
}

/* Alt+drag with the pointer already at (gx, gy) inside the window; ends at (ex, ey). */
static void alt_drag(struct app *app, struct wl_display *d, uint32_t button, double gx, double gy,
                     double ex, double ey)
{
    zwp_virtual_keyboard_v1_modifiers(app->vkbd, MOD_ALT, 0, 0, 0);
    wl_display_roundtrip(d);
    vptr_button(app, d, button, 1);
    vptr_move(app, d, ex, ey);
    vptr_button(app, d, button, 0);
    zwp_virtual_keyboard_v1_modifiers(app->vkbd, 0, 0, 0, 0);
    wl_display_roundtrip(d);
}

static void setup_virtual_devices(struct app *app, struct wl_display *d, int wait_for_focus)
{
    if (!app->vptr_mgr || !app->vkbd_mgr) {
        fail("virtual input protocols missing (run with SFWC_ENABLE_VIRTUAL_INPUT=1)");
    }
    app->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(app->vkbd_mgr, app->seat);
    send_keymap(app);
    app->vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(app->vptr_mgr, app->seat);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    if (!app->keyboard || !app->pointer) {
        fail("seat did not announce keyboard + pointer after virtual devices appeared");
    }
    if (wait_for_focus) {
        wait_for(d, &app->kb_enter, 3000, "keyboard focus (wl_keyboard.enter) for a window");
    }
}

/* Returns the current config file contents (small) in a malloc'ed string. */
static char *read_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fail("cannot read the config file");
    }
    char *buf = calloc(1, 8192);
    size_t n = fread(buf, 1, 8191, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Atomically replace the config file (write a temp file, rename), as editors do. */
static void write_config(const char *path, const char *text)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fail("cannot write the new config");
    }
    fputs(text, f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        fail("cannot replace the config file");
    }
}

/* ------------------------------------------------------- scenario: single */

static void run_input_tests(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 1);

    /* [keyboard] repeat_rate/repeat_delay from the config reach the client */
    if (app->kb_rate != 33 || app->kb_delay != 250) {
        char msg[100];
        snprintf(msg, sizeof msg, "repeat_info is %d/%d, config says 33/250", app->kb_rate,
                 app->kb_delay);
        fail(msg);
    }

    /* 1. keyboard: a plain key reaches the focused window */
    vtap(app, d, 0, KEY_A);
    if (!app->key_presses[KEY_A]) {
        fail("plain key press was not delivered to the focused window");
    }

    /* 1b. Alt+Return spawns the configured terminal (the script checks the side effect) */
    vtap(app, d, MOD_ALT, KEY_ENTER);

    /* 2. Alt+f / Alt+F11 are handled by the compositor and not forwarded */
    vtap(app, d, MOD_ALT, KEY_F);
    expect_configure(app, d, 1, 0, app->out_w - 2 * GAP, app->out_h - 2 * GAP, "Alt+f maximizes");
    vtap(app, d, MOD_ALT, KEY_F);
    expect_configure(app, d, 0, 0, W, H, "Alt+f again restores");
    vtap(app, d, MOD_ALT, KEY_F11);
    expect_configure(app, d, 0, 1, app->out_w, app->out_h, "Alt+F11 fullscreen");
    vtap(app, d, MOD_ALT, KEY_F11);
    expect_configure(app, d, 0, 0, W, H, "Alt+F11 again restores");
    if (app->key_presses[KEY_F] || app->key_presses[KEY_F11]) {
        fail("compositor keybind leaked to the client");
    }

    /* 3. pointer: enter, motion and buttons reach the window */
    const double gx = 120, gy = 90;
    app->ptr_enter = 0;
    vptr_move(app, d, gx, gy);
    if (!app->ptr_enter) {
        fail("pointer did not enter the window (is it under (120,90)?)");
    }
    double lx = app->ptr_sx, ly = app->ptr_sy; /* pointer position inside the window */
    double ox = gx - lx, oy = gy - ly;         /* window origin on the output */
    vptr_button(app, d, BTN_LEFT, 1);
    vptr_button(app, d, BTN_LEFT, 0);
    if (app->ptr_button_press != 1 || app->ptr_button_release != 1) {
        fail("plain click was not delivered to the window");
    }

    /* 4. Alt+left-drag moves the window and is not forwarded to it */
    alt_drag(app, d, BTN_LEFT, gx, gy, gx + 200, gy + 120);
    if (app->ptr_button_press != 1) {
        fail("Alt+drag click leaked to the client");
    }
    vptr_move(app, d, gx + 201, gy + 121);
    check_near(app->ptr_sx, lx + 1, 3, "pointer x after moving window by +200");
    check_near(app->ptr_sy, ly + 1, 3, "pointer y after moving window by +120");
    ox += 200;
    oy += 120;

    /* 5. dragging near the left edge snaps the window to the gap */
    double px = gx + 201, py = gy + 121;
    double target_x = GAP + SNAP / 2; /* within snap distance of the edge */
    double target_y = 100;
    alt_drag(app, d, BTN_LEFT, px, py, px + (target_x - ox), py + (target_y - oy));
    vptr_move(app, d, px + (target_x - ox) + 1, py + (target_y - oy) + 1);
    check_near(app->ptr_sx, (px + (target_x - ox) + 1) - GAP, 3, "x after snapping to the left edge");
    check_near(app->ptr_sy, (py + (target_y - oy) + 1) - target_y, 3, "y after move without snap");
    ox = GAP;
    oy = target_y;

    /* 6. Alt+right-drag resizes (pointer in the bottom-right quadrant) */
    double rx = ox + 150, ry = oy + 70;
    vptr_move(app, d, rx, ry);
    alt_drag(app, d, BTN_RIGHT, rx, ry, rx + 50, ry + 30);
    expect_configure(app, d, 0, 0, W + 50, H + 30, "Alt+right-drag resizes by the drag distance");

    /* 6b. snapping to another window: the first window is now at (8,100) with a 200x100
     * geometry, so its right edge plus the gap is x=216. Open a second window, drag it to
     * x=222 (within the snap distance) next to the first one and it must snap to 216. */
    {
        struct win b;
        win_open(app, d, &b, "sfwc-test-window-2", 0xffc09030);
        double bx = 250, by = 90; /* inside the new window (cascaded to ~80,80), outside the first */
        app->ptr_enter = 0;
        vptr_move(app, d, bx, by);
        if (!app->ptr_enter) {
            fail("pointer did not enter the second window");
        }
        double blx = app->ptr_sx, bly = app->ptr_sy;
        double box = bx - blx, boy = by - bly; /* origin of the second window */
        double want_x = GAP + W + GAP + 6;     /* 6px away from the snap position */
        double want_y = 130;                   /* overlaps the first window vertically */
        double ex = bx + (want_x - box), ey = by + (want_y - boy);
        alt_drag(app, d, BTN_LEFT, bx, by, ex, ey);
        vptr_move(app, d, ex + 1, ey + 1);
        double snapped_x = GAP + W + GAP;
        check_near(app->ptr_sx, (ex + 1) - snapped_x, 3, "x after snapping next to another window");
        check_near(app->ptr_sy, (ey + 1) - want_y, 3, "y after snapping next to another window");
        win_destroy(d, &b);
    }

    /* 7. minimize and restore with the keyboard; keyboard focus follows */
    int leaves = app->kb_leave, enters = app->kb_enter;
    vtap(app, d, MOD_ALT, KEY_M);
    if (app->kb_leave == leaves) {
        fail("window kept keyboard focus after Alt+m");
    }
    vtap(app, d, MOD_ALT | MOD_SHIFT, KEY_M);
    if (app->kb_enter == enters) {
        fail("window did not get keyboard focus back after Alt+Shift+m");
    }

    /* 7b. live reload: replace the config file atomically (as editors do); the new
     * modifier, keybinds and gap must take effect without restarting the compositor */
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }
    write_config(cfg_path, "[general]\nmod = Ctrl\n[windows]\ngap = 20\n[keybinds]\n"
                           "$mod+x = toggle-maximize\n$mod+q = close\n");
    /* the compositor notices asynchronously: retry until the new binding works */
    app->cfg_max = 0;
    for (int i = 0; i < 40 && !app->cfg_max; i++) {
        vtap(app, d, MOD_CTRL, KEY_X);
        if (!app->cfg_max) {
            usleep(100 * 1000);
        }
    }
    expect_configure(app, d, 1, 0, app->out_w - 40, app->out_h - 40,
                     "Ctrl+x maximizes with the reloaded gap of 20");
    if (app->kb_rate != 25 || app->kb_delay != 600) {
        fail("reload did not re-apply the keyboard repeat settings (expected defaults 25/600)");
    }
    vtap(app, d, MOD_CTRL, KEY_X);
    expect_configure(app, d, 0, 0, W, H, "Ctrl+x again restores");
    int f_before = app->key_presses[KEY_F];
    vtap(app, d, MOD_ALT, KEY_F); /* no longer bound: must reach the client */
    if (app->key_presses[KEY_F] != f_before + 1) {
        fail("old Alt+f binding still active after reload (key should reach the client)");
    }

    /* 8. the reloaded close binding (Ctrl+q) asks the window to close */
    vtap(app, d, MOD_CTRL, KEY_Q);
    wait_for(d, &app->closed, 3000, "xdg_toplevel.close after Ctrl+q");
}

/* -------------------------------------------------------- scenario: multi */

static struct out_info *find_out(struct app *app, const char *name)
{
    for (int i = 0; i < app->n_outs; i++) {
        if (!strcmp(app->outs[i].name, name)) {
            return &app->outs[i];
        }
    }
    char msg[100];
    snprintf(msg, sizeof msg, "output %s not announced by the compositor", name);
    fail(msg);
    return NULL;
}

static void check_out(const struct out_info *o, int mw, int mh, int scale, int lx, int ly, int lw,
                      int lh)
{
    if (o->mw != mw || o->mh != mh || o->scale != scale || o->lx != lx || o->ly != ly ||
        o->lw != lw || o->lh != lh) {
        char msg[300];
        snprintf(msg, sizeof msg,
                 "%s: mode %dx%d scale %d logical %d,%d %dx%d; expected mode %dx%d scale %d logical "
                 "%d,%d %dx%d",
                 o->name, o->mw, o->mh, o->scale, o->lx, o->ly, o->lw, o->lh, mw, mh, scale, lx, ly,
                 lw, lh);
        fail(msg);
    }
}

static void run_multi(struct app *app, struct wl_display *d)
{
    /* outputs: HEADLESS-1 1280x720 at 0,0; HEADLESS-2 1024x600 with scale 2 (logical 512x300)
     * moved to 0,720 by the config */
    if (app->n_outs != 2) {
        fail("expected two outputs");
    }
    check_out(find_out(app, "HEADLESS-1"), 1280, 720, 1, 0, 0, 1280, 720);
    check_out(find_out(app, "HEADLESS-2"), 1024, 600, 2, 0, 720, 512, 300);
    app->ext_w = 1280;
    app->ext_h = 720 + 300;

    /* Windows open on the output under the pointer. Where the pointer starts depends on
     * which output appeared first, so put it on the first output explicitly. */
    struct win a, b;
    setup_virtual_devices(app, d, 0);
    vptr_move(app, d, 100, 100);
    win_open(app, d, &a, "sfwc-multi-A", 0xff3050c0);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for window A");
    if (app->kb_rate != 40 || app->kb_delay != 300) {
        fail("repeat_info does not match the [keyboard] config (40/300)");
    }
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1280 - 2 * GAP, 720 - 2 * GAP, "A maximizes on the first output");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 0, 0, W, H, "A restores");

    /* a window opened while the pointer is on the second output lands there; maximizing it
     * uses that output's logical (scaled) size */
    vptr_move(app, d, 100, 800);
    win_open(app, d, &b, "sfwc-multi-B", 0xffc03050);
    if (app->kb_surface != b.surface) {
        fail("new window B did not get keyboard focus");
    }
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 1, 0, 512 - 2 * GAP, 300 - 2 * GAP, "B maximizes to the second output");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 0, 0, W, H, "B restores");

    /* focus = follow-mouse: keyboard focus follows the pointer between the two outputs */
    vptr_move(app, d, 60, 60); /* over A (48,48) */
    if (app->kb_surface != a.surface) {
        fail("follow-mouse: pointer over A did not focus A");
    }
    vptr_move(app, d, 100, 820); /* over B (80,800) */
    if (app->kb_surface != b.surface) {
        fail("follow-mouse: pointer over B did not focus B");
    }

    /* move-to-next-output (Alt+o): B goes to the first output, maximizes there, and a second
     * Alt+o refits it on the second output */
    vtap(app, d, MOD_ALT, KEY_O);
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 1, 0, 1280 - 2 * GAP, 720 - 2 * GAP, "B moved to the first output");
    vtap(app, d, MOD_ALT, KEY_O);
    win_expect(&b, d, 1, 0, 512 - 2 * GAP, 300 - 2 * GAP, "maximized B refits on the second output");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 0, 0, W, H, "B restores on the second output");

    /* focus-next-output (Alt+Shift+o): the pointer is on the second output, so focus goes
     * to the top window of the first output (A), and then back to B */
    vtap(app, d, MOD_ALT | MOD_SHIFT, KEY_O);
    if (app->kb_surface != a.surface) {
        fail("focus-next-output did not focus the window on the first output (A)");
    }
    vtap(app, d, MOD_ALT | MOD_SHIFT, KEY_O);
    if (app->kb_surface != b.surface) {
        fail("focus-next-output did not focus the window on the second output (B)");
    }

    /* reload-config key: there is no file watching in this run, so a changed file is only
     * picked up when asked for. B is maximized to show the new gap. */
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 1, 0, 512 - 2 * GAP, 300 - 2 * GAP, "B maximized before the config change");
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }
    char *cur = read_config(cfg_path);
    char *next = calloc(1, strlen(cur) + 64);
    sprintf(next, "%s\n[windows]\ngap = 20\n", cur);
    write_config(cfg_path, next);
    free(cur);
    free(next);
    win_expect_quiet(&b, d, 400, "config changed on disk but reload-config was not pressed");
    vtap(app, d, MOD_ALT | MOD_SHIFT, KEY_R);
    win_expect(&b, d, 1, 0, 512 - 40, 300 - 40, "reload-config key applies the new gap");

    win_destroy(d, &b);
    win_destroy(d, &a);
}
#endif

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "single";
    int multi = !strcmp(mode, "multi");
    if (!multi && strcmp(mode, "single") != 0) {
        fail("unknown mode (use single or multi)");
    }

    struct app app = {0};
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fail("cannot connect to compositor");
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &app);
    wl_display_roundtrip(display);
    if (!app.compositor || !app.shm || !app.wm_base || !app.output || !app.xdg_out_mgr) {
        fail("compositor is missing wl_compositor / wl_shm / xdg_wm_base / wl_output / "
             "zxdg_output_manager_v1");
    }
    for (int i = 0; i < app.n_outs; i++) {
        app.outs[i].xdg = zxdg_output_manager_v1_get_xdg_output(app.xdg_out_mgr, app.outs[i].wl);
        zxdg_output_v1_add_listener(app.outs[i].xdg, &xdg_output_listener, &app.outs[i]);
    }
    wl_display_roundtrip(display); /* wl_output mode/scale + xdg_output events */
    wl_display_roundtrip(display);
    if (app.out_w <= 0 || app.out_h <= 0) {
        fail("no output mode received");
    }

    if (multi) {
#ifdef HAVE_VIRTUAL_INPUT
        run_multi(&app, display);
        wl_display_disconnect(display);
        printf("client_test multi: OK\n");
        return 0;
#else
        fail("the multi scenario needs the virtual input protocols");
#endif
    }

    /* xdg_output must agree with wl_output for the single-output setup */
    {
        const struct out_info *o = &app.outs[0];
        if (o->lw != app.out_w || o->lh != app.out_h || o->lx != 0 || o->ly != 0 || !o->name[0]) {
            fail("xdg_output logical geometry/name does not match the output");
        }
    }

    /* Toplevel window */
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "sfwc-test-window");
    xdg_toplevel_set_app_id(app.toplevel, "sfwc.test");
    wl_surface_commit(app.surface);
    wait_for(display, &app.configured, 3000, "toplevel configure");

    struct wl_buffer *buf = make_buffer(app.shm, W, H, 0xff3050c0);
    wl_surface_attach(app.surface, buf, 0, 0);
    struct wl_callback *cb = wl_surface_frame(app.surface);
    wl_callback_add_listener(cb, &frame_listener, &app);
    wl_surface_commit(app.surface);

    /* Popup on the toplevel */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(app.wm_base);
    xdg_positioner_set_size(pos, 50, 30);
    xdg_positioner_set_anchor_rect(pos, 0, 0, 1, 1);
    app.popup_surface = wl_compositor_create_surface(app.compositor);
    app.popup_xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.popup_surface);
    xdg_surface_add_listener(app.popup_xdg_surface, &xdg_surface_listener, &app);
    app.popup = xdg_surface_get_popup(app.popup_xdg_surface, app.xdg_surface, pos);
    xdg_popup_add_listener(app.popup, &popup_listener, &app);
    xdg_positioner_destroy(pos);
    wl_surface_commit(app.popup_surface);
    wait_for(display, &app.popup_configured, 3000, "popup configure");
    struct wl_buffer *pbuf = make_buffer(app.shm, 50, 30, 0xffc03050);
    wl_surface_attach(app.popup_surface, pbuf, 0, 0);
    wl_surface_commit(app.popup_surface);

    /* A frame callback only fires once the compositor drew the window. */
    wait_for(display, &app.frame_done, 3000, "frame callback (no output or window not rendered)");

    /* Close the popup before changing window state. */
    xdg_popup_destroy(app.popup);
    xdg_surface_destroy(app.popup_xdg_surface);
    wl_surface_destroy(app.popup_surface);
    app.popup = NULL;

    /* Maximize: fills the output minus a gap on each side. */
    xdg_toplevel_set_maximized(app.toplevel);
    expect_configure(&app, display, 1, 0, app.out_w - 2 * GAP, app.out_h - 2 * GAP, "maximize");
    xdg_toplevel_unset_maximized(app.toplevel);
    expect_configure(&app, display, 0, 0, W, H, "unmaximize restores the old size");

    /* Fullscreen: covers the whole output. */
    xdg_toplevel_set_fullscreen(app.toplevel, NULL);
    expect_configure(&app, display, 0, 1, app.out_w, app.out_h, "fullscreen");
    xdg_toplevel_unset_fullscreen(app.toplevel);
    expect_configure(&app, display, 0, 0, W, H, "leave fullscreen restores the old size");

#ifdef HAVE_VIRTUAL_INPUT
    run_input_tests(&app, display);
#else
    printf("client_test: virtual input tests skipped (protocol files unavailable)\n");
#endif

    /* Minimize has no reply for the client; the compositor logs it. */
    xdg_toplevel_set_minimized(app.toplevel);
    wl_display_roundtrip(display);

    /* Tear down. */
    xdg_toplevel_destroy(app.toplevel);
    xdg_surface_destroy(app.xdg_surface);
    wl_surface_destroy(app.surface);
    wl_display_roundtrip(display);

    wl_buffer_destroy(pbuf);
    wl_buffer_destroy(buf);
    wl_display_disconnect(display);
    printf("client_test: OK\n");
    return 0;
}
