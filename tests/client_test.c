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
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#ifdef HAVE_VIRTUAL_INPUT
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
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
    struct app *app;
    struct wl_surface *surface;
    struct xdg_surface *xs;
    struct xdg_toplevel *tl;
    struct zxdg_toplevel_decoration_v1 *deco;
    struct wl_buffer *buf;
    int configured, closed;
    int cfg_arrived, cfg_w, cfg_h, cfg_max, cfg_fs;
    /* optional: follow the compositor's size by re-attaching a buffer of that size */
    int auto_buffer, buf_w, buf_h, pend_w, pend_h;
    uint32_t color;
    int deco_mode;
    struct wp_fractional_scale_v1 *fscale;
    int pref_scale; /* wp_fractional_scale_v1.preferred_scale, 0 = none yet */
};

#define MAX_GLOBALS 64

struct app {
    char *globals[MAX_GLOBALS];
    int n_globals;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_output_manager_v1 *xdg_out_mgr;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wp_fractional_scale_manager_v1 *fscale_mgr;
    struct wp_cursor_shape_manager_v1 *shape_mgr;
    struct xdg_activation_v1 *activation;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *rel_mgr;
    int rel_events;
    double rel_dx, rel_dy; /* sum of the relative motion received */
    struct zwlr_output_manager_v1 *out_mgr;
    uint32_t out_serial;
    int out_done;           /* manager.done events seen */
    int cfg_result;         /* 1 succeeded, -1 failed/cancelled, 0 pending */
    struct { struct zwlr_output_head_v1 *head; char name[32]; int enabled, x, y, alive; double scale; } heads[8];
    int constraint_on;     /* locked / confined events minus unlocked / unconfined */
    uint32_t kb_serial, ptr_serial; /* of the last enter */
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
    struct wl_surface *ptr_surface; /* surface under the pointer */
    int ptr_enter, ptr_motion, ptr_button_press, ptr_button_release;
    double ptr_sx, ptr_sy;        /* last pointer position, surface-local */
#ifdef HAVE_VIRTUAL_INPUT
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct ext_session_lock_manager_v1 *lock_mgr;
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
    app->kb_serial = serial;
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
    app->ptr_serial = serial;
    app->ptr_surface = s;
    app->ptr_sx = wl_fixed_to_double(x);
    app->ptr_sy = wl_fixed_to_double(y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    ((struct app *)d)->ptr_surface = NULL;
}
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

#ifdef HAVE_VIRTUAL_INPUT
/* ---------------------------------------------- wlr-output-management (client side) */

static int head_slot(struct app *app, struct zwlr_output_head_v1 *h)
{
    for (int i = 0; i < 8; i++) {
        if (app->heads[i].head == h) {
            return i;
        }
    }
    return -1;
}
#define HEAD_CB(name, ...) static void name(void *data, struct zwlr_output_head_v1 *h, ##__VA_ARGS__)
HEAD_CB(hd_name, const char *n) { struct app *a = data; int i = head_slot(a, h); if (i >= 0) snprintf(a->heads[i].name, 32, "%s", n); }
HEAD_CB(hd_desc, const char *n) {}
HEAD_CB(hd_size, int32_t w, int32_t hh) {}
HEAD_CB(hd_mode, struct zwlr_output_mode_v1 *m) {}
HEAD_CB(hd_enabled, int32_t e) { struct app *a = data; int i = head_slot(a, h); if (i >= 0) a->heads[i].enabled = e; }
HEAD_CB(hd_current, struct zwlr_output_mode_v1 *m) {}
HEAD_CB(hd_position, int32_t x, int32_t y) { struct app *a = data; int i = head_slot(a, h); if (i >= 0) { a->heads[i].x = x; a->heads[i].y = y; } }
HEAD_CB(hd_transform, int32_t t) {}
HEAD_CB(hd_scale, wl_fixed_t s) { struct app *a = data; int i = head_slot(a, h); if (i >= 0) a->heads[i].scale = wl_fixed_to_double(s); }
HEAD_CB(hd_finished) { struct app *a = data; int i = head_slot(a, h); if (i >= 0) a->heads[i].alive = 0; zwlr_output_head_v1_destroy(h); if (i >= 0) a->heads[i].head = NULL; }
static const struct zwlr_output_head_v1_listener head_listener = {
    .name = hd_name, .description = hd_desc, .physical_size = hd_size, .mode = hd_mode,
    .enabled = hd_enabled, .current_mode = hd_current, .position = hd_position,
    .transform = hd_transform, .scale = hd_scale, .finished = hd_finished,
};
static void om_head(void *data, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *h)
{
    struct app *app = data;
    for (int i = 0; i < 8; i++) {
        if (!app->heads[i].head) {
            memset(&app->heads[i], 0, sizeof app->heads[i]);
            app->heads[i].head = h;
            app->heads[i].alive = 1;
            zwlr_output_head_v1_add_listener(h, &head_listener, app);
            return;
        }
    }
    fail("too many output heads");
}
static void om_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t serial)
{
    struct app *app = data;
    app->out_serial = serial;
    app->out_done++;
}
static void om_finished(void *data, struct zwlr_output_manager_v1 *m) {}
static const struct zwlr_output_manager_v1_listener out_mgr_listener = {
    .head = om_head, .done = om_done, .finished = om_finished,
};
static void oc_ok(void *data, struct zwlr_output_configuration_v1 *c) { ((struct app *)data)->cfg_result = 1; }
static void oc_fail(void *data, struct zwlr_output_configuration_v1 *c) { ((struct app *)data)->cfg_result = -1; }
static const struct zwlr_output_configuration_v1_listener out_cfg_listener = {
    .succeeded = oc_ok, .failed = oc_fail, .cancelled = oc_fail,
};
#endif

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct app *app = data;
    if (app->n_globals < MAX_GLOBALS) {
        app->globals[app->n_globals++] = strdup(interface);
    }
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        app->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        app->deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        app->fscale_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        app->shape_mgr = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
        app->activation = wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
#ifdef HAVE_VIRTUAL_INPUT
    } else if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        app->out_mgr = wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface, 1);
        zwlr_output_manager_v1_add_listener(app->out_mgr, &out_mgr_listener, app);
#endif
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        app->constraints = wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        app->rel_mgr = wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        app->xdg_out_mgr = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 2);
#ifdef HAVE_VIRTUAL_INPUT
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        app->lock_mgr = wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 4);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        app->screencopy_mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 1);
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

static struct wl_buffer *make_buffer(struct wl_shm *shm, int w, int h, uint32_t color);

static void win_xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    struct win *w = data;
    xdg_surface_ack_configure(s, serial);
    if (w->auto_buffer && w->configured && w->pend_w > 0 && w->pend_h > 0 &&
        (w->pend_w != w->buf_w || w->pend_h != w->buf_h)) {
        /* answer the new size with a buffer of that size */
        struct wl_buffer *nb = make_buffer(w->app->shm, w->pend_w, w->pend_h, w->color);
        wl_surface_attach(w->surface, nb, 0, 0);
        wl_surface_commit(w->surface);
        if (w->buf) {
            wl_buffer_destroy(w->buf);
        }
        w->buf = nb;
        w->buf_w = w->pend_w;
        w->buf_h = w->pend_h;
    }
    w->configured = 1;
}
static const struct xdg_surface_listener win_xs_listener = {.configure = win_xs_configure};

static void win_tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                             struct wl_array *states)
{
    struct win *win = data;
    win->cfg_w = w;
    win->cfg_h = h;
    win->pend_w = w;
    win->pend_h = h;
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

static void win_deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
    ((struct win *)data)->deco_mode = mode;
}
static const struct zxdg_toplevel_decoration_v1_listener win_deco_listener = {
    .configure = win_deco_configure,
};

static void fscale_preferred(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
    ((struct win *)data)->pref_scale = scale;
}
static const struct wp_fractional_scale_v1_listener fscale_listener = {
    .preferred_scale = fscale_preferred,
};

/* want_deco: negotiate server-side decorations; auto_buffer: follow configured sizes */
static void win_open_ex(struct app *app, struct wl_display *d, struct win *w, const char *title,
                        uint32_t color, int want_deco, int auto_buffer)
{
    memset(w, 0, sizeof *w);
    w->app = app;
    w->color = color;
    w->auto_buffer = auto_buffer;
    w->surface = wl_compositor_create_surface(app->compositor);
    if (app->fscale_mgr) {
        w->fscale = wp_fractional_scale_manager_v1_get_fractional_scale(app->fscale_mgr, w->surface);
        wp_fractional_scale_v1_add_listener(w->fscale, &fscale_listener, w);
    }
    w->xs = xdg_wm_base_get_xdg_surface(app->wm_base, w->surface);
    xdg_surface_add_listener(w->xs, &win_xs_listener, w);
    w->tl = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->tl, &win_tl_listener, w);
    xdg_toplevel_set_title(w->tl, title);
    if (want_deco) {
        if (!app->deco_mgr) {
            fail("compositor does not offer xdg-decoration");
        }
        w->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(app->deco_mgr, w->tl);
        zxdg_toplevel_decoration_v1_add_listener(w->deco, &win_deco_listener, w);
        zxdg_toplevel_decoration_v1_set_mode(w->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(w->surface);
    wait_for(d, &w->configured, 3000, "window configure");
    if (want_deco && w->deco_mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        fail("compositor did not choose server-side decorations");
    }
    w->buf = make_buffer(app->shm, W, H, color);
    w->buf_w = W;
    w->buf_h = H;
    wl_surface_attach(w->surface, w->buf, 0, 0);
    wl_surface_commit(w->surface);
    wl_display_roundtrip(d);
}

static void win_open(struct app *app, struct wl_display *d, struct win *w, const char *title,
                     uint32_t color)
{
    win_open_ex(app, d, w, title, color, 0, 0);
}

static void win_destroy(struct wl_display *d, struct win *w)
{
    if (w->deco) {
        zxdg_toplevel_decoration_v1_destroy(w->deco);
    }
    if (w->fscale) {
        wp_fractional_scale_v1_destroy(w->fscale);
    }
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
    if (strstr(text, "[animations]") && !strstr(text, "[plugins]")) {
        fputs("[plugins]\nload = animations\n", f); /* the animation scenarios need the animations plugin */
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

/* --------------------------------------------------- screen capture + pixels */

struct capture {
    uint32_t format;
    int w, h, stride;
    int have_buffer, ready, failed, y_invert;
};

static void cap_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                       uint32_t w, uint32_t h, uint32_t stride)
{
    struct capture *c = data;
    c->format = format;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->have_buffer = 1;
}
static void cap_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    ((struct capture *)data)->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}
static void cap_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo,
                      uint32_t ns)
{
    ((struct capture *)data)->ready = 1;
}
static void cap_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    ((struct capture *)data)->failed = 1;
}
static void cap_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h)
{
}
static void cap_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w,
                             uint32_t h)
{
}
static void cap_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {}
static const struct zwlr_screencopy_frame_v1_listener capture_listener = {
    .buffer = cap_buffer,
    .flags = cap_flags,
    .ready = cap_ready,
    .failed = cap_failed,
    .damage = cap_damage,
    .linux_dmabuf = cap_linux_dmabuf,
    .buffer_done = cap_buffer_done,
};

struct image {
    uint32_t *px; /* 0x00RRGGBB, top row first */
    int w, h;
};

/* Capture the first output as it is displayed (without the cursor). */
static struct image capture_screen(struct app *app, struct wl_display *d)
{
    if (!app->screencopy_mgr) {
        fail("compositor does not offer wlr-screencopy");
    }
    struct capture c = {0};
    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(app->screencopy_mgr, 0, app->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &capture_listener, &c);
    wait_for(d, &c.have_buffer, 3000, "screencopy buffer description");

    size_t size = (size_t)c.stride * c.h;
    int fd = memfd_create("sfwc-test-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        fail("cannot create capture file");
    }
    uint8_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, c.w, c.h, c.stride, c.format);
    wl_shm_pool_destroy(pool);
    close(fd);
    zwlr_screencopy_frame_v1_copy(frame, buf);
    for (int i = 0; i < 200 && !c.ready && !c.failed; i++) {
        wait_flag(d, &c.ready, 50);
    }
    if (c.failed) {
        fail("screencopy failed");
    }
    if (!c.ready) {
        fail("timeout waiting for the screen capture (no frame rendered?)");
    }

    struct image img = {.w = c.w, .h = c.h, .px = malloc((size_t)c.w * c.h * 4)};
    for (int y = 0; y < c.h; y++) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)(c.y_invert ? c.h - 1 - y : y) * c.stride);
        for (int x = 0; x < c.w; x++) {
            img.px[y * c.w + x] = row[x] & 0x00ffffff;
        }
    }
    munmap(data, size);
    wl_buffer_destroy(buf);
    zwlr_screencopy_frame_v1_destroy(frame);
    return img;
}

static uint32_t img_px(const struct image *img, int x, int y)
{
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) {
        fail("pixel check outside the screen");
    }
    return img->px[y * img->w + x];
}

static int color_near(uint32_t got, uint32_t want, int tol)
{
    return abs((int)((got >> 16) & 0xff) - (int)((want >> 16) & 0xff)) <= tol &&
           abs((int)((got >> 8) & 0xff) - (int)((want >> 8) & 0xff)) <= tol &&
           abs((int)(got & 0xff) - (int)(want & 0xff)) <= tol;
}

static void expect_px(const struct image *img, int x, int y, uint32_t want, const char *what)
{
    uint32_t got = img_px(img, x, y);
    if (!color_near(got, want, 3)) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: pixel (%d,%d) is #%06x, expected #%06x", what, x, y, got, want);
        fail(msg);
    }
}

static void expect_not_px(const struct image *img, int x, int y, uint32_t unwanted, const char *what)
{
    if (color_near(img_px(img, x, y), unwanted, 3)) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: pixel (%d,%d) is #%06x, which it must not be", what, x, y,
                 img_px(img, x, y));
        fail(msg);
    }
}

/* Click (press + release) at a global position. */
static void vclick(struct app *app, struct wl_display *d, double x, double y)
{
    vptr_move(app, d, x, y);
    vptr_button(app, d, BTN_LEFT, 1);
    vptr_button(app, d, BTN_LEFT, 0);
}

/* Press, move and release the left button. */
static void vdrag(struct app *app, struct wl_display *d, double x, double y, double dx, double dy)
{
    vptr_move(app, d, x, y);
    vptr_button(app, d, BTN_LEFT, 1);
    vptr_move(app, d, x + dx, y + dy);
    vptr_button(app, d, BTN_LEFT, 0);
}

/* --------------------------------------------------------- scenario: deco */

/* Must match the theme the script writes to themes/test.theme (shadow white 50%, radius 20). */
#define BW 4
#define TH 24
#define C_BORDER_F 0xff0000
#define C_BORDER_U 0x0000ff
#define C_TITLE_F 0x00ff00
#define C_TITLE_U 0xffff00
#define C_CLOSE 0xff00ff
#define C_MAX 0x00ffff
#define C_MIN 0xff8000
#define C_CLIENT 0x3050c0

static void run_deco(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    const char *fonts = getenv("SFWC_TEST_FONTS");

    struct win a, b;
    win_open_ex(app, d, &a, "decorated window with a rather long title", 0xff000000u | C_CLIENT, 1, 1);
    setup_virtual_devices(app, d, 1);
    wl_display_roundtrip(d);

    /* A opens cascaded at outer (48,48); content (200x100) therefore starts at (52,76) */
    struct image img = capture_screen(app, d);
    expect_px(&img, 148, 49, C_BORDER_F, "top border (focused)");
    expect_px(&img, 49, 108, C_BORDER_F, "left border");
    expect_px(&img, 254, 108, C_BORDER_F, "right border");
    expect_px(&img, 148, 178, C_BORDER_F, "bottom border");
    expect_px(&img, 148, 53, C_TITLE_F, "titlebar (focused)");
    expect_px(&img, 152, 126, C_CLIENT, "client content");
    expect_px(&img, 240, 64, C_CLOSE, "close button");
    expect_px(&img, 222, 64, C_MAX, "maximize button");
    expect_px(&img, 204, 64, C_MIN, "minimize button");
    expect_not_px(&img, 48, 48, C_BORDER_F, "rounded outer corner");
    uint32_t outside = img_px(&img, 43, 114);          /* just left of the window: shadow */
    if (((outside >> 16) & 0xff) < 15 || ((outside >> 16) & 0xff) > 140) {
        char msg[100];
        snprintf(msg, sizeof msg, "shadow next to the window: pixel is #%06x", outside);
        fail(msg);
    }
    expect_px(&img, 148, 240, 0x000000, "no shadow far below the window");
    if (fonts && !strcmp(fonts, "1")) {
        int differing = 0;
        for (int y = 58; y < 70; y++) {
            for (int x = 66; x < 120; x++) {
                if (!color_near(img_px(&img, x, y), C_TITLE_F, 3)) {
                    differing++;
                }
            }
        }
        if (differing < 20) {
            fail("the window title was not drawn into the titlebar");
        }
    }
    free(img.px);

    /* drag the window by its titlebar: +100,+60 */
    vdrag(app, d, 112, 64, 100, 60);
    img = capture_screen(app, d);
    expect_px(&img, 248, 109, C_BORDER_F, "top border after dragging the titlebar");
    expect_px(&img, 248, 113, C_TITLE_F, "titlebar after dragging");
    expect_px(&img, 252, 186, C_CLIENT, "content after dragging");
    expect_not_px(&img, 148, 49, C_BORDER_F, "old position is empty");
    free(img.px);

    /* resize by dragging the bottom-right corner of the frame: content becomes 240x120 */
    vdrag(app, d, 355, 239, 40, 20);
    win_expect(&a, d, 0, 0, 240, 120, "dragging the frame corner resizes the window");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 394, 200, C_BORDER_F, "right border after resizing");
    expect_px(&img, 250, 258, C_BORDER_F, "bottom border after resizing");
    expect_px(&img, 300, 200, C_CLIENT, "content fills the new size");
    free(img.px);

    /* maximize button: the whole frame fills the output minus the gap */
    vclick(app, d, 362, 124);
    win_expect(&a, d, 1, 0, 1264 - 2 * BW, 704 - TH - 2 * BW, "maximize button");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 600, 9, C_BORDER_F, "top border of the maximized window");
    expect_px(&img, 600, 13, C_TITLE_F, "titlebar of the maximized window");
    expect_px(&img, 600, 300, C_CLIENT, "content of the maximized window");
    free(img.px);

    /* double-click on the titlebar restores it */
    vclick(app, d, 400, 24);
    vclick(app, d, 400, 24);
    win_expect(&a, d, 0, 0, 240, 120, "double click on the titlebar restores");
    wl_display_roundtrip(d);

    /* fullscreen removes the frame */
    xdg_toplevel_set_fullscreen(a.tl, NULL);
    win_expect(&a, d, 0, 1, 1280, 720, "fullscreen");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 3, 3, C_CLIENT, "fullscreen content reaches the corner");
    expect_px(&img, 640, 10, C_CLIENT, "no titlebar in fullscreen");
    free(img.px);
    xdg_toplevel_unset_fullscreen(a.tl);
    win_expect(&a, d, 0, 0, 240, 120, "leaving fullscreen");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 248, 109, C_BORDER_F, "frame is back after fullscreen");
    free(img.px);

    /* a second decorated window takes the focus: the first one is drawn unfocused */
    win_open_ex(app, d, &b, "second", 0xff000000u | C_CLIENT, 1, 1); /* opaque: alpha byte set */
    img = capture_screen(app, d);
    expect_px(&img, 340, 109, C_BORDER_U, "unfocused border");
    expect_px(&img, 340, 113, C_TITLE_U, "unfocused titlebar");
    expect_px(&img, 180, 81, C_BORDER_F, "focused border of the second window");
    free(img.px);

    /* its close button asks it to close; afterwards the first window is focused again */
    vclick(app, d, 272, 96);
    wait_for(d, &b.closed, 3000, "xdg_toplevel.close from the close button");
    win_destroy(d, &b);
    img = capture_screen(app, d);
    expect_px(&img, 340, 109, C_BORDER_F, "border is focused again after the other window closed");
    free(img.px);

    /* live theme change: other colors, same geometry */
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }
    write_config(cfg_path, "[general]\ntheme = test2\n");
    int changed = 0;
    for (int i = 0; i < 40 && !changed; i++) {
        usleep(100 * 1000);
        wl_display_roundtrip(d);
        img = capture_screen(app, d);
        changed = color_near(img_px(&img, 340, 109), 0x123456, 3);
        free(img.px);
    }
    if (!changed) {
        fail("the theme was not reloaded (border color did not change)");
    }
    img = capture_screen(app, d);
    expect_px(&img, 340, 113, 0xabcdef, "titlebar color of the reloaded theme");
    free(img.px);

    win_destroy(d, &a);
}

/* --------------------------------------------------------- scenario: anim */

static void sleep_ms(int ms)
{
    usleep((useconds_t)ms * 1000);
}

static int blue_of(const struct image *img, int x, int y)
{
    return (int)(img_px(img, x, y) & 0xff);
}

/* Animations are 2 s long, linear (see the script); the client color has blue = 0xc0. */
static void run_anim(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);

    /* opening fades the window in */
    struct win a;
    win_open_ex(app, d, &a, "animated", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(150);
    struct image img = capture_screen(app, d);
    int b = blue_of(&img, 148, 98); /* window at (48,48), 200x100 */
    free(img.px);
    if (b < 1 || b > 0xb0) {
        char msg[100];
        snprintf(msg, sizeof msg, "open animation: blue is 0x%02x shortly after opening, expected a partly faded window", b);
        fail(msg);
    }
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 148, 98, C_CLIENT, "window is fully visible after the open animation");
    free(img.px);

    /* maximize slides the window from (48,48) to (8,8) instead of jumping */
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1264, 704, "Alt+f maximizes");
    img = capture_screen(app, d);
    expect_px(&img, 20, 20, 0x000000, "maximized window is still sliding into place");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 20, 20, C_CLIENT, "maximized window arrived");
    expect_px(&img, 4, 4, 0x000000, "gap around the maximized window");
    free(img.px);

    /* closing leaves a fading picture of the window behind */
    win_destroy(d, &a);
    sleep_ms(150);
    img = capture_screen(app, d);
    b = blue_of(&img, 600, 300);
    free(img.px);
    if (b < 1 || b > 0xb4) {
        char msg[100];
        snprintf(msg, sizeof msg, "close animation: blue is 0x%02x shortly after closing, expected a fading window", b);
        fail(msg);
    }
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, 0x000000, "the closed window is gone after the animation");
    free(img.px);

    /* with animations switched off (live reload) everything is immediate */
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }
    write_config(cfg_path, "[animations]\nenabled = false\n");
    sleep_ms(500);
    struct win c;
    win_open_ex(app, d, &c, "plain", 0xff000000u | C_CLIENT, 0, 1);
    img = capture_screen(app, d);
    expect_px(&img, 150, 100, C_CLIENT, "no fade when animations are disabled");
    free(img.px);
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&c, d, 1, 0, 1264, 704, "Alt+f maximizes");
    img = capture_screen(app, d);
    expect_px(&img, 20, 20, C_CLIENT, "no slide when animations are disabled");
    free(img.px);
    win_destroy(d, &c);
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, 0x000000, "no fade-out when animations are disabled");
    free(img.px);
}

#define C_WALLPAPER 0x102030
#define C_BAR 0xaa5500
#define C_OVERLAY 0x00aa55

/* ------------------------------------------- scenario: Hyprland style animations */

static int red_of(const struct image *img, int x, int y)
{
    return (int)((img_px(img, x, y) >> 16) & 0xff);
}

/* New windows cascade: the n-th one (from 0) opens with its top-left at 48 + 32*n, wrapping
 * after six. */
static int cascade_at(int n)
{
    return 48 + (n * 32) % 192;
}

static void run_hypr(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }

    /* popin: the window grows from 50% of its size around its center (no fade, so the colors
     * stay exact). 2 s linear: half a second in it is about 62% of the size. */
    struct win a;
    win_open_ex(app, d, &a, "popin", 0xff000000u | C_CLIENT, 0, 1); /* 200x100 at (48,48) */
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(500);
    struct image img = capture_screen(app, d);
    expect_px(&img, 148, 98, C_CLIENT, "popin: the center of the window is there");
    expect_px(&img, 105, 98, C_CLIENT, "popin: at least half of the window is there");
    expect_px(&img, 190, 98, C_CLIENT, "popin: at least half of the window is there (right)");
    expect_px(&img, 52, 98, 0x000000, "popin: the window is still smaller than its full size (left)");
    expect_px(&img, 244, 98, 0x000000, "popin: the window is still smaller than its full size (right)");
    expect_px(&img, 148, 50, 0x000000, "popin: the window is still smaller than its full size (top)");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 52, 98, C_CLIENT, "popin: the window has its full size afterwards (left)");
    expect_px(&img, 244, 98, C_CLIENT, "popin: the window has its full size afterwards (right)");
    expect_px(&img, 148, 52, C_CLIENT, "popin: the window has its full size afterwards (top)");
    free(img.px);

    /* closing shrinks a picture of the window */
    win_destroy(d, &a);
    sleep_ms(500);
    img = capture_screen(app, d);
    expect_px(&img, 148, 98, C_CLIENT, "popin out: the picture of the window is still there");
    expect_px(&img, 52, 98, 0x000000, "popin out: it is shrinking");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 148, 98, 0x000000, "popin out: gone afterwards");
    free(img.px);

    /* workspaces: going up, the old window slides out to the left and the new one in from the right;
     * going back down it is the other way round */
    win_open_ex(app, d, &a, "slider", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(2300);
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1264, 704, "Alt+f maximizes");
    sleep_ms(600); /* the move animation of the maximize (legacy keys: 180 ms) */
    vtap(app, d, MOD_ALT, KEY_2);
    sleep_ms(500);
    img = capture_screen(app, d);
    expect_px(&img, 300, 300, C_CLIENT, "workspace slide: the old workspace is still on its way out");
    expect_px(&img, 1200, 300, 0x000000, "workspace slide: it has moved to the left");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 300, 300, 0x000000, "workspace slide: the old workspace is gone");
    free(img.px);
    vtap(app, d, MOD_ALT, KEY_1);
    sleep_ms(500);
    img = capture_screen(app, d);
    /* going back to a lower workspace the content moves the other way: in from the left */
    expect_px(&img, 30, 300, C_CLIENT, "workspace slide: the window comes in from the left");
    expect_px(&img, 1000, 300, 0x000000, "workspace slide: it has not arrived yet");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, 300, 300, C_CLIENT, "workspace slide: the window arrived");
    free(img.px);
    win_destroy(d, &a);
    sleep_ms(2300);

    /* slide: a new window (the third one, so at cascade_at(2)) comes in from the left edge (rule
     * changed by a live reload) */
    int sp = cascade_at(2);
    write_config(cfg_path,
                 "[general]\ntheme = test\n[animations]\nbezier = lin, 0, 0, 1, 1\n"
                 "animation = windowsIn, 1, 20, lin, slide left\nanimation = windowsOut, 0\n"
                 "animation = fade, 0\nanimation = border, 0\n");
    sleep_ms(600);
    struct win c;
    win_open_ex(app, d, &c, "slide", 0xff000000u | C_CLIENT, 0, 1);
    sleep_ms(500);
    img = capture_screen(app, d);
    expect_px(&img, 30, sp + 50, C_CLIENT, "slide left: the window is entering from the left edge");
    expect_px(&img, sp + 36, sp + 50, 0x000000, "slide left: it has not reached its place yet");
    free(img.px);
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, sp + 100, sp + 50, C_CLIENT, "slide left: the window is in its place");
    free(img.px);
    win_destroy(d, &c);
    img = capture_screen(app, d);
    expect_px(&img, sp + 100, sp + 50, 0x000000, "windowsOut is off: the window vanishes at once");
    free(img.px);

    /* border: when another window takes the focus the border color moves from the focused to the
     * unfocused color instead of jumping */
    write_config(cfg_path,
                 "[general]\ntheme = test\n[animations]\nbezier = lin, 0, 0, 1, 1\n"
                 "animation = windows, 0\nanimation = fade, 0\nanimation = border, 1, 20, lin\n");
    sleep_ms(600);
    struct win w1, w2;
    int bp = cascade_at(3); /* the fourth window: its outer top-left corner */
    win_open_ex(app, d, &w1, "first", 0xff000000u | C_CLIENT, 1, 1);
    sleep_ms(300);
    img = capture_screen(app, d);
    expect_px(&img, bp + 100, bp + 1, C_BORDER_F, "border: a new window starts with the focused color");
    free(img.px);
    win_open_ex(app, d, &w2, "second", 0xff000000u | C_CLIENT, 1, 1); /* takes the focus */
    sleep_ms(500);
    img = capture_screen(app, d);
    int r = red_of(&img, bp + 100, bp + 1), b = blue_of(&img, bp + 100, bp + 1);
    free(img.px);
    if (r < 0x30 || r > 0xe0 || b < 0x20 || b > 0xd0) {
        char msg[120];
        snprintf(msg, sizeof msg,
                 "border: the unfocused window's border is r=0x%02x b=0x%02x half a second in, expected a color in between", r, b);
        fail(msg);
    }
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, bp + 100, bp + 1, C_BORDER_U, "border: the color ended at the unfocused one");
    free(img.px);

    win_destroy(d, &w2);
    win_destroy(d, &w1);
}

/* ------------------------------------------- scenario: Wayfire style effects */

/* number of flame colored pixels (orange / yellow: red clearly above blue) in a rectangle */
static int count_fiery(const struct image *img, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint32_t c = img_px(img, x, y);
            int r = (c >> 16) & 0xff, b = c & 0xff;
            n += r >= 150 && r > b + 50;
        }
    }
    return n;
}

static void fx_config(const char *path, const char *in_rule, const char *out_rule)
{
    char text[400];
    snprintf(text, sizeof text,
             "[animations]\nbezier = lin, 0, 0, 1, 1\nanimation = windowsIn, %s\n"
             "animation = windowsOut, %s\nanimation = fade, 0\nanimation = border, 0\n",
             in_rule, out_rule);
    write_config(path, text);
    sleep_ms(600);
}

static void run_fx(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);
    const char *cfg_path = getenv("SFWC_CONFIG");
    if (!cfg_path) {
        fail("SFWC_CONFIG is not set (run through tests/run_client_test.sh)");
    }

    /* squeeze: the window opens as a line and unfolds: first the width, then the height. 1.2 s
     * into 2 s it is full width and about half the height. */
    int p = cascade_at(0);
    struct win a;
    win_open_ex(app, d, &a, "squeeze", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(1200);
    struct image img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 50, C_CLIENT, "squeeze in: the middle of the window is there");
    expect_px(&img, p + 4, p + 50, C_CLIENT, "squeeze in: it already has its full width");
    expect_px(&img, p + 100, p + 8, 0x000000, "squeeze in: but not its full height yet");
    free(img.px);
    sleep_ms(1500);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 8, C_CLIENT, "squeeze in: the window is complete afterwards");
    expect_px(&img, p + 4, p + 4, C_CLIENT, "squeeze in: ... in the corner too");
    free(img.px);
    win_destroy(d, &a);
    sleep_ms(1200);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 50, C_CLIENT, "squeeze out: the middle of the picture is still there");
    expect_px(&img, p + 100, p + 8, 0x000000, "squeeze out: the height is collapsing");
    free(img.px);
    sleep_ms(1500);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 50, 0x000000, "squeeze out: gone afterwards");
    free(img.px);

    /* fire, closing: the window burns away from the bottom; flames sit along the burn line */
    fx_config(cfg_path, "0", "1, 20, lin, fire");
    p = cascade_at(1);
    struct win b;
    win_open_ex(app, d, &b, "burn", 0xff000000u | C_CLIENT, 0, 1);
    sleep_ms(400);
    win_destroy(d, &b);
    sleep_ms(500); /* a quarter in: the fire has climbed about a third of the window */
    img = capture_screen(app, d);
    if (blue_of(&img, p + 100, p + 6) < 0x50) { /* the flames are translucent: the blue of the window shows */
        fail("fire out: the top of the window is not there (not burnt yet)");
    }
    expect_not_px(&img, p + 100, p + 90, C_CLIENT, "fire out: the bottom of the window has burnt away");
    int flames = count_fiery(&img, p - 30, p + 10, p + 230, p + 110);
    if (flames < 15) {
        char msg[100];
        snprintf(msg, sizeof msg, "fire out: only %d flame colored pixels around the burn line", flames);
        fail(msg);
    }
    free(img.px);
    sleep_ms(2000);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 20, 0x000000, "fire out: the window is gone afterwards");
    if (count_fiery(&img, 0, 0, 1280, 720) != 0) {
        fail("fire out: flames are still there after the animation");
    }
    free(img.px);

    /* fire, opening: the window is revealed from the top, the flames on its edge */
    fx_config(cfg_path, "1, 20, lin, fire", "0");
    p = cascade_at(2);
    struct win c;
    win_open_ex(app, d, &c, "unburn", 0xff000000u | C_CLIENT, 0, 1);
    sleep_ms(1000); /* half way: the edge has reached the middle of the window */
    img = capture_screen(app, d);
    if (blue_of(&img, p + 100, p + 6) < 0x50) {
        fail("fire in: the top of the window is not there yet");
    }
    expect_not_px(&img, p + 100, p + 90, C_CLIENT, "fire in: the bottom is not there yet");
    flames = count_fiery(&img, p - 30, p + 10, p + 230, p + 110);
    if (flames < 15) {
        char msg[100];
        snprintf(msg, sizeof msg, "fire in: only %d flame colored pixels around the edge", flames);
        fail(msg);
    }
    free(img.px);
    sleep_ms(2000);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 85, C_CLIENT, "fire in: the whole window is there afterwards");
    expect_px(&img, p + 4, p + 96, C_CLIENT, "fire in: ... down to the corner");
    if (count_fiery(&img, 0, 0, 1280, 720) != 0) {
        fail("fire in: flames are still there after the animation");
    }
    free(img.px);
    win_destroy(d, &c);

    /* zoom: a smaller, translucent window that grows */
    fx_config(cfg_path, "1, 20, lin, zoom 50%", "0");
    p = cascade_at(3);
    struct win dd;
    win_open_ex(app, d, &dd, "zoom", 0xff000000u | C_CLIENT, 0, 1);
    sleep_ms(500);
    img = capture_screen(app, d);
    int b_mid = blue_of(&img, p + 100, p + 50);
    expect_px(&img, p + 3, p + 50, 0x000000, "zoom: the window is still smaller than its full size");
    free(img.px);
    if (b_mid < 1 || b_mid > 0xb0) {
        char msg[100];
        snprintf(msg, sizeof msg, "zoom: blue is 0x%02x half a second in, expected a partly faded window", b_mid);
        fail(msg);
    }
    sleep_ms(2300);
    img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 50, C_CLIENT, "zoom: fully visible afterwards");
    expect_px(&img, p + 3, p + 50, C_CLIENT, "zoom: with its full size");
    free(img.px);
    win_destroy(d, &dd);
}

/* ---------------------------------------------- scenario: wobbly windows */
static int count_client(const struct image *img, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            n += (img->px[(size_t)y * img->w + x] & 0xffffff) == C_CLIENT;
        }
    }
    return n;
}

static void run_wobbly(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);
    int p = 48; /* the first window opens at the cascade start */
    struct win a;
    win_open_ex(app, d, &a, "wobble", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(300);
    struct image img = capture_screen(app, d);
    expect_px(&img, p + 100, p + 50, C_CLIENT, "wobbly: the window is where it opened");
    free(img.px);

    /* grab the window in the middle and throw it 300 px to the right. A rigid window would jump;
     * the mesh lags: half a second later pixels of the window are still between the old right
     * edge and the new left edge. */
    double gx = p + 100, gy = p + 50;
    vptr_move(app, d, gx, gy);
    alt_drag(app, d, BTN_LEFT, gx, gy, gx + 300, gy);
    sleep_ms(500);
    img = capture_screen(app, d);
    int lag = count_client(&img, p + 205, p + 4, p + 290, p + 96);
    int at_old = count_client(&img, p, p, p + 200, p + 100);
    int at_new = count_client(&img, p + 300, p, p + 500, p + 100);
    char profile[600] = "";
    for (int x = 0; x < 640; x += 20) { /* window pixels per 20 px wide column, whole screen height */
        size_t n = strlen(profile);
        snprintf(profile + n, sizeof profile - n, " %d", count_client(&img, x, 0, x + 20, img.h));
    }
    free(img.px);
    if (lag < 100) {
        fprintf(stderr, "wobbly: window pixels per 20 px column from x=0:%s\n", profile);
        char msg[200];
        snprintf(msg, sizeof msg,
                 "wobbly: only %d window pixels trail between the old and the new place (old place %d, new place %d)",
                 lag, at_old, at_new);
        fail(msg);
    }

    /* it settles: the old place is empty, the window rigid at the new one */
    sleep_ms(6000);
    img = capture_screen(app, d);
    expect_px(&img, p + 300 + 100, p + 50, C_CLIENT, "wobbly: the window arrived");
    expect_px(&img, p + 300 + 4, p + 4, C_CLIENT, "wobbly: ... with its corner");
    expect_px(&img, p + 100, p + 50, 0x000000, "wobbly: the old place is empty");
    free(img.px);
    win_destroy(d, &a);
}

/* ---------------------------------------------- scenario: plugins */
static void run_plugins(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);
    struct win a;
    win_open_ex(app, d, &a, "plugin-window", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    sleep_ms(300); /* a few frames, so that the plugin sees the window */
    struct image img = capture_screen(app, d);
    expect_px(&img, 48 + 100, 48 + 50, C_CLIENT, "plugins: the window is drawn");
    free(img.px);
    win_destroy(d, &a);
    sleep_ms(200);
}

/* -------------------------------------------------- scenario: workspaces */

#define C_OTHER 0xc03050

static void expect_focus(struct app *app, struct wl_display *d, struct wl_surface *want, const char *what)
{
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    if (app->kb_surface != want) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s (keyboard focus is %s)", what,
                 app->kb_surface ? "another window" : "nobody");
        fail(msg);
    }
}

static void run_workspaces(struct app *app, struct wl_display *d)
{
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);

    /* workspace 1: window A, maximized so that one pixel tells whether it is visible */
    struct win a, b;
    win_open_ex(app, d, &a, "A", 0xff000000u | C_CLIENT, 0, 1);
    expect_focus(app, d, a.surface, "window A did not get focus");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1264, 704, "A maximized");
    wl_display_roundtrip(d);
    struct image img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "A is visible on workspace 1");
    free(img.px);

    /* workspace 2 is empty: A disappears and loses the keyboard focus */
    vtap(app, d, MOD_ALT, KEY_2);
    expect_focus(app, d, NULL, "switching to an empty workspace left the keyboard on A");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, 0x000000, "A is hidden on workspace 2");
    free(img.px);

    /* a new window opens on the current workspace */
    win_open_ex(app, d, &b, "B", 0xff000000u | C_OTHER, 0, 1);
    expect_focus(app, d, b.surface, "window B did not get focus on workspace 2");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&b, d, 1, 0, 1264, 704, "B maximized");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_OTHER, "B is visible on workspace 2");
    free(img.px);

    /* back to workspace 1: A is back with the focus, B is gone */
    vtap(app, d, MOD_ALT, KEY_1);
    expect_focus(app, d, a.surface, "A did not get the focus back on workspace 1");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "A is visible again");
    free(img.px);

    /* send the focused window (A) to workspace 2: workspace 1 is empty */
    vtap(app, d, MOD_ALT | MOD_SHIFT, KEY_2);
    expect_focus(app, d, NULL, "moving the only window away left the keyboard on it");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, 0x000000, "workspace 1 is empty after moving A away");
    free(img.px);
    vtap(app, d, MOD_ALT, KEY_2);
    expect_focus(app, d, a.surface, "the front window of workspace 2 has the focus");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "A is in front of B on workspace 2");
    free(img.px);

    /* numbers beyond the configured workspaces are ignored */
    vtap(app, d, MOD_ALT, KEY_9);
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "workspace 9 does not exist");
    free(img.px);

    win_destroy(d, &b);
    win_destroy(d, &a);
}

/* ------------------------------------------------- scenario: protocols */

static void token_done(void *data, struct xdg_activation_token_v1 *t, const char *token)
{
    *(char **)data = strdup(token);
}
static const struct xdg_activation_token_v1_listener token_listener = {.done = token_done};

static void wait_pref(struct wl_display *d, struct win *w, int want, const char *what)
{
    for (int i = 0; i < 40 && w->pref_scale != want; i++) {
        wl_display_roundtrip(d);
        usleep(50 * 1000);
    }
    if (w->pref_scale != want) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s: preferred_scale %d, wanted %d", what, w->pref_scale, want);
        fail(msg);
    }
}


static void rel_motion(void *data, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo,
                       wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy)
{
    struct app *app = data;
    app->rel_events++;
    app->rel_dx += wl_fixed_to_double(dx);
    app->rel_dy += wl_fixed_to_double(dy);
}
static const struct zwp_relative_pointer_v1_listener rel_listener = {.relative_motion = rel_motion};
static void lp_locked(void *data, struct zwp_locked_pointer_v1 *l) { ((struct app *)data)->constraint_on++; }
static void lp_unlocked(void *data, struct zwp_locked_pointer_v1 *l) { ((struct app *)data)->constraint_on--; }
static const struct zwp_locked_pointer_v1_listener locked_listener = {.locked = lp_locked, .unlocked = lp_unlocked};
static void cp_confined(void *data, struct zwp_confined_pointer_v1 *l) { ((struct app *)data)->constraint_on++; }
static void cp_unconfined(void *data, struct zwp_confined_pointer_v1 *l) { ((struct app *)data)->constraint_on--; }
static const struct zwp_confined_pointer_v1_listener confined_listener = {.confined = cp_confined, .unconfined = cp_unconfined};

/* A relative move of the virtual pointer (what a mouse does), unlike vptr_move's absolute one. */
static void vptr_rel(struct app *app, struct wl_display *d, double dx, double dy)
{
    zwlr_virtual_pointer_v1_motion(app->vptr, app->t += 10, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    zwlr_virtual_pointer_v1_frame(app->vptr);
    wl_display_roundtrip(d);
}

static void run_constraints(struct app *app, struct wl_display *d, struct win *a)
{
    if (!app->constraints || !app->rel_mgr) {
        fail("pointer-constraints / relative-pointer could not be bound");
    }
    struct zwp_relative_pointer_v1 *rel = zwp_relative_pointer_manager_v1_get_relative_pointer(app->rel_mgr, app->pointer);
    zwp_relative_pointer_v1_add_listener(rel, &rel_listener, app);

    /* pointer in the window, free: both motion and relative motion arrive */
    vptr_move(app, d, 48 + 100, 48 + 50); /* the window is 200x100 */
    vptr_rel(app, d, 10, 5);
    check_near(app->rel_dx, 10, 0.5, "relative motion x (free pointer)");
    check_near(app->ptr_sx, 110, 3, "surface x after a relative move");

    /* locked: the pointer stays, the client still gets the relative motion */
    struct zwp_locked_pointer_v1 *lock = zwp_pointer_constraints_v1_lock_pointer(
        app->constraints, a->surface, app->pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(lock, &locked_listener, app);
    vptr_rel(app, d, 1, 1); /* the pointer has to move over the surface for the lock to start */
    wl_display_roundtrip(d);
    if (app->constraint_on != 1) {
        fail("the pointer lock was not activated");
    }
    double before_x = app->ptr_sx, before_y = app->ptr_sy;
    int ev = app->rel_events;
    app->rel_dx = app->rel_dy = 0;
    vptr_rel(app, d, 30, 40);
    vptr_rel(app, d, 30, 40);
    if (app->rel_events - ev != 2) {
        fail("locked pointer: the relative motion events did not arrive");
    }
    check_near(app->rel_dx, 60, 0.5, "locked pointer: relative x");
    check_near(app->rel_dy, 80, 0.5, "locked pointer: relative y");
    check_near(app->ptr_sx, before_x, 0.5, "locked pointer moved in x");
    check_near(app->ptr_sy, before_y, 0.5, "locked pointer moved in y");
    zwp_locked_pointer_v1_destroy(lock);
    wl_display_roundtrip(d);
    app->constraint_on = 0; /* destroying the object sends no "unlocked" */
    vptr_rel(app, d, 20, 0);
    check_near(app->ptr_sx, before_x + 20, 3, "pointer does not move again after the lock is gone");

    /* confined to a 60x60 square at the top left of the window */
    struct wl_region *region = wl_compositor_create_region(app->compositor);
    wl_region_add(region, 0, 0, 60, 60);
    struct zwp_confined_pointer_v1 *conf = zwp_pointer_constraints_v1_confine_pointer(
        app->constraints, a->surface, app->pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_confined_pointer_v1_add_listener(conf, &confined_listener, app);
    wl_region_destroy(region);
    vptr_move(app, d, 48 + 30, 48 + 30);
    wl_display_roundtrip(d);
    if (app->constraint_on != 1) {
        fail("the pointer confinement was not activated");
    }
    vptr_rel(app, d, 500, 500);
    if (app->ptr_sx > 61 || app->ptr_sy > 61) {
        char msg[120];
        snprintf(msg, sizeof msg, "confined pointer left its region: %.1f,%.1f", app->ptr_sx, app->ptr_sy);
        fail(msg);
    }
    vptr_rel(app, d, -500, -500);
    if (app->ptr_sx < -1 || app->ptr_sy < -1) {
        fail("confined pointer left its region at the top left");
    }
    zwp_confined_pointer_v1_destroy(conf);
    zwp_relative_pointer_v1_destroy(rel);
    wl_display_roundtrip(d);
}

static int find_head(struct app *app, const char *name)
{
    for (int i = 0; i < 8; i++) {
        if (app->heads[i].head && app->heads[i].alive && !strcmp(app->heads[i].name, name)) {
            return i;
        }
    }
    fail("output head not announced");
    return -1;
}

/* Waits until the manager announced a state with `done` after the one we have. */
static void wait_out_done(struct app *app, struct wl_display *d, int prev, const char *what)
{
    for (int i = 0; i < 40 && app->out_done == prev; i++) {
        wl_display_roundtrip(d);
        usleep(25 * 1000);
    }
    if (app->out_done == prev) {
        fail(what);
    }
}

static void run_output_mgmt(struct app *app, struct wl_display *d)
{
    if (!app->out_mgr) {
        fail("wlr-output-management could not be bound");
    }
    wl_display_roundtrip(d);
    int h1 = find_head(app, "HEADLESS-1"), h2 = find_head(app, "HEADLESS-2");
    if (!app->heads[h1].enabled || !app->heads[h2].enabled || app->heads[h2].y != 720 ||
        app->heads[h2].scale != 2.0) {
        fail("the announced outputs do not match the config");
    }

    /* scale of the first output 1 -> 1.5 */
    int seen = app->out_done;
    struct zwlr_output_configuration_v1 *cfg = zwlr_output_manager_v1_create_configuration(app->out_mgr, app->out_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &out_cfg_listener, app);
    struct zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(cfg, app->heads[h1].head);
    zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(1.5));
    ch = zwlr_output_configuration_v1_enable_head(cfg, app->heads[h2].head);
    zwlr_output_configuration_head_v1_set_position(ch, 0, 720);
    zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(2.0));
    app->cfg_result = 0;
    zwlr_output_configuration_v1_test(cfg);
    wait_flag(d, &app->cfg_result, 2000);
    if (app->cfg_result != 1) {
        fail("test of a valid output configuration did not succeed");
    }
    app->cfg_result = 0;
    zwlr_output_configuration_v1_apply(cfg);
    wait_flag(d, &app->cfg_result, 2000);
    if (app->cfg_result != 1) {
        fail("a valid output configuration was not applied");
    }
    zwlr_output_configuration_v1_destroy(cfg);
    wait_out_done(app, d, seen, "no new output state after the change");
    h1 = find_head(app, "HEADLESS-1");
    if (app->heads[h1].scale != 1.5) {
        fail("scale of HEADLESS-1 was not changed to 1.5");
    }

    /* the second output off, then on again at its place */
    seen = app->out_done;
    cfg = zwlr_output_manager_v1_create_configuration(app->out_mgr, app->out_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &out_cfg_listener, app);
    h1 = find_head(app, "HEADLESS-1");
    h2 = find_head(app, "HEADLESS-2");
    zwlr_output_configuration_v1_enable_head(cfg, app->heads[h1].head);
    zwlr_output_configuration_v1_disable_head(cfg, app->heads[h2].head);
    app->cfg_result = 0;
    zwlr_output_configuration_v1_apply(cfg);
    wait_flag(d, &app->cfg_result, 2000);
    if (app->cfg_result != 1) {
        fail("switching an output off failed");
    }
    zwlr_output_configuration_v1_destroy(cfg);
    wait_out_done(app, d, seen, "no new output state after switching an output off");
    if (app->heads[find_head(app, "HEADLESS-2")].enabled) {
        fail("HEADLESS-2 is still announced as on");
    }

    seen = app->out_done;
    cfg = zwlr_output_manager_v1_create_configuration(app->out_mgr, app->out_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &out_cfg_listener, app);
    h1 = find_head(app, "HEADLESS-1");
    h2 = find_head(app, "HEADLESS-2");
    zwlr_output_configuration_v1_enable_head(cfg, app->heads[h1].head);
    ch = zwlr_output_configuration_v1_enable_head(cfg, app->heads[h2].head);
    zwlr_output_configuration_head_v1_set_position(ch, 1280, 0);
    app->cfg_result = 0;
    zwlr_output_configuration_v1_apply(cfg);
    wait_flag(d, &app->cfg_result, 2000);
    if (app->cfg_result != 1) {
        fail("switching an output back on failed");
    }
    zwlr_output_configuration_v1_destroy(cfg);
    wait_out_done(app, d, seen, "no new output state after switching an output on");
    h2 = find_head(app, "HEADLESS-2");
    if (!app->heads[h2].enabled || app->heads[h2].x != 1280 || app->heads[h2].y != 0) {
        fail("HEADLESS-2 did not come back at 1280,0");
    }

    /* switching every output off is refused */
    cfg = zwlr_output_manager_v1_create_configuration(app->out_mgr, app->out_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &out_cfg_listener, app);
    zwlr_output_configuration_v1_disable_head(cfg, app->heads[find_head(app, "HEADLESS-1")].head);
    zwlr_output_configuration_v1_disable_head(cfg, app->heads[h2].head);
    app->cfg_result = 0;
    zwlr_output_configuration_v1_apply(cfg);
    wait_flag(d, &app->cfg_result, 2000);
    if (app->cfg_result != -1) {
        fail("a configuration that switches every output off was accepted");
    }
    zwlr_output_configuration_v1_destroy(cfg);
    wl_display_roundtrip(d);
}

static void run_protocols(struct app *app, struct wl_display *d)
{
    if (!app->fscale_mgr || !app->shape_mgr || !app->activation) {
        fail("fractional-scale / cursor-shape / xdg-activation could not be bound");
    }
    app->ext_w = 1280;
    app->ext_h = 720 + 300;
    setup_virtual_devices(app, d, 0);

    /* fractional scale: 1.0 on the first output (120), 2.0 on the second (240), follows the window */
    struct win a, b;
    vptr_move(app, d, 100, 100);
    win_open(app, d, &a, "proto-A", 0xff3050c0);
    wait_pref(d, &a, 120, "window on the 1x output");
    vptr_move(app, d, 100, 800);
    win_open(app, d, &b, "proto-B", 0xffc03050);
    wait_pref(d, &b, 240, "window on the 2x output");
    vtap(app, d, MOD_ALT, KEY_O);
    wait_pref(d, &b, 120, "window moved to the 1x output");

    /* cursor shape: a request with the pointer's enter serial is honoured (checked in the log),
     * one for another client's serial is not an error */
    vptr_move(app, d, 60, 60);
    vptr_move(app, d, 70, 70);
    if (app->ptr_surface != a.surface) {
        fail("pointer is not over window A");
    }
    struct wp_cursor_shape_device_v1 *dev = wp_cursor_shape_manager_v1_get_pointer(app->shape_mgr, app->pointer);
    wp_cursor_shape_device_v1_set_shape(dev, app->ptr_serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    wl_display_roundtrip(d);
    wp_cursor_shape_device_v1_destroy(dev);

    /* activation: a valid token focuses the window, a made up one does not */
    expect_focus(app, d, b.surface, "window B should have the keyboard focus before the activation");
    struct xdg_activation_token_v1 *tok = xdg_activation_v1_get_activation_token(app->activation);
    char *str = NULL;
    xdg_activation_token_v1_add_listener(tok, &token_listener, &str);
    xdg_activation_token_v1_set_serial(tok, app->kb_serial, app->seat);
    xdg_activation_token_v1_commit(tok);
    wl_display_roundtrip(d);
    if (!str) {
        fail("no activation token received");
    }
    xdg_activation_v1_activate(app->activation, "not-a-token", a.surface);
    wl_display_roundtrip(d);
    if (app->kb_surface == a.surface) {
        fail("activation with an unknown token focused the window");
    }
    xdg_activation_v1_activate(app->activation, str, a.surface);
    expect_focus(app, d, a.surface, "xdg-activation did not focus the window");
    free(str);
    xdg_activation_token_v1_destroy(tok);

    win_destroy(d, &b);
    run_constraints(app, d, &a);
    win_destroy(d, &a);
    run_output_mgmt(app, d);
}

/* ----------------------------------------------------- scenario: session lock */

struct lock_state {
    int locked, finished;
};
static void lk_locked(void *data, struct ext_session_lock_v1 *l)
{
    ((struct lock_state *)data)->locked = 1;
}
static void lk_finished(void *data, struct ext_session_lock_v1 *l)
{
    ((struct lock_state *)data)->finished = 1;
}
static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lk_locked,
    .finished = lk_finished,
};

struct lock_surf {
    struct app *app;
    struct wl_surface *surface;
    struct wl_buffer *buf;
    int configured;
};
static void lks_configure(void *data, struct ext_session_lock_surface_v1 *s, uint32_t serial,
                          uint32_t w, uint32_t h)
{
    struct lock_surf *ls = data;
    ext_session_lock_surface_v1_ack_configure(s, serial);
    ls->buf = make_buffer(ls->app->shm, w, h, 0xff000000u | C_OVERLAY);
    wl_surface_attach(ls->surface, ls->buf, 0, 0);
    wl_surface_commit(ls->surface);
    ls->configured = 1;
}
static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lks_configure,
};

static void run_lock(struct app *app, struct wl_display *d)
{
    if (!app->lock_mgr) {
        fail("compositor does not offer ext-session-lock");
    }
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);

    struct win a;
    win_open_ex(app, d, &a, "secret", 0xff000000u | C_CLIENT, 0, 1);
    expect_focus(app, d, a.surface, "window did not get focus");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1264, 704, "window maximized");
    wl_display_roundtrip(d);
    struct image img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "the window is visible before locking");
    free(img.px);

    /* lock: the window disappears at once and loses the keyboard, even without a lock surface */
    struct lock_state st = {0};
    struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(app->lock_mgr);
    ext_session_lock_v1_add_listener(lock, &lock_listener, &st);
    wait_for(d, &st.locked, 3000, "the locked event");
    expect_focus(app, d, NULL, "the window kept the keyboard while locked");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, 0x000000, "the screen is black while locked");
    free(img.px);

    /* keybinds are off while locked */
    vtap(app, d, MOD_ALT, KEY_Q);
    wl_display_roundtrip(d);
    if (a.closed) {
        fail("a keybind (close) worked while the session was locked");
    }

    /* a second locker is refused */
    struct lock_state st2 = {0};
    struct ext_session_lock_v1 *lock2 = ext_session_lock_manager_v1_lock(app->lock_mgr);
    ext_session_lock_v1_add_listener(lock2, &lock_listener, &st2);
    wait_for(d, &st2.finished, 3000, "the second lock to be refused");
    if (st2.locked) {
        fail("a second lock client was accepted");
    }
    ext_session_lock_v1_destroy(lock2);

    /* the lock surface covers the output and takes keyboard and pointer */
    struct lock_surf ls = {.app = app};
    ls.surface = wl_compositor_create_surface(app->compositor);
    struct ext_session_lock_surface_v1 *lsurf =
        ext_session_lock_v1_get_lock_surface(lock, ls.surface, app->output);
    ext_session_lock_surface_v1_add_listener(lsurf, &lock_surface_listener, &ls);
    wait_for(d, &ls.configured, 3000, "lock surface configure");
    expect_focus(app, d, ls.surface, "the lock surface did not get the keyboard");
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_OVERLAY, "the lock surface is shown");
    expect_px(&img, 5, 5, C_OVERLAY, "the lock surface covers the corner");
    free(img.px);
    app->ptr_enter = 0;
    vptr_move(app, d, 640, 360);
    wl_display_roundtrip(d);
    if (app->ptr_surface != ls.surface) {
        fail("the pointer is not on the lock surface");
    }

    /* unlock: everything comes back */
    ext_session_lock_v1_unlock_and_destroy(lock);
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 600, 300, C_CLIENT, "the window is back after unlocking");
    free(img.px);
    expect_focus(app, d, a.surface, "the window did not get the keyboard back after unlocking");
    vtap(app, d, MOD_ALT, KEY_Q);
    wl_display_roundtrip(d);
    wait_for(d, &a.closed, 3000, "keybinds work again after unlocking");

    ext_session_lock_surface_v1_destroy(lsurf);
    wl_surface_destroy(ls.surface);
    win_destroy(d, &a);
}

/* ------------------------------------------------------- scenario: layers */

struct lay {
    struct app *app;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *ls;
    struct wl_buffer *buf;
    uint32_t color;
    int configured, closed, w, h;
};

static void lay_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                          uint32_t w, uint32_t h)
{
    struct lay *l = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0 && h > 0 && (l->w != (int)w || l->h != (int)h)) {
        struct wl_buffer *nb = make_buffer(l->app->shm, w, h, l->color);
        wl_surface_attach(l->surface, nb, 0, 0);
        wl_surface_commit(l->surface);
        if (l->buf) {
            wl_buffer_destroy(l->buf);
        }
        l->buf = nb;
        l->w = w;
        l->h = h;
    }
    l->configured = 1;
}
static void lay_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    ((struct lay *)data)->closed = 1;
}
static const struct zwlr_layer_surface_v1_listener lay_listener = {
    .configure = lay_configure,
    .closed = lay_closed,
};

static void lay_open(struct app *app, struct wl_display *d, struct lay *l, uint32_t layer,
                     uint32_t anchor, int w, int h, int zone, uint32_t kbd, uint32_t color,
                     const char *ns)
{
    memset(l, 0, sizeof *l);
    l->app = app;
    l->color = color;
    l->surface = wl_compositor_create_surface(app->compositor);
    l->ls = zwlr_layer_shell_v1_get_layer_surface(app->layer_shell, l->surface, NULL, layer, ns);
    zwlr_layer_surface_v1_add_listener(l->ls, &lay_listener, l);
    zwlr_layer_surface_v1_set_size(l->ls, w, h);
    zwlr_layer_surface_v1_set_anchor(l->ls, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(l->ls, zone);
    zwlr_layer_surface_v1_set_keyboard_interactivity(l->ls, kbd);
    wl_surface_commit(l->surface);
    wait_for(d, &l->configured, 3000, "layer surface configure");
    wl_display_roundtrip(d);
}

static void lay_close(struct wl_display *d, struct lay *l)
{
    zwlr_layer_surface_v1_destroy(l->ls);
    wl_surface_destroy(l->surface);
    if (l->buf) {
        wl_buffer_destroy(l->buf);
    }
    wl_display_roundtrip(d);
}


static void run_layers(struct app *app, struct wl_display *d)
{
    if (!app->layer_shell) {
        fail("compositor does not offer wlr-layer-shell");
    }
    app->ext_w = app->out_w;
    app->ext_h = app->out_h;
    setup_virtual_devices(app, d, 0);

    /* wallpaper (background, fills the output) and a 30px panel on top that reserves space */
    struct lay wall, bar;
    lay_open(app, d, &wall, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
             ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
             0, 0, -1, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, 0xff000000u | C_WALLPAPER,
             "wallpaper");
    lay_open(app, d, &bar, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
             ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
             0, 30, 30, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, 0xff000000u | C_BAR,
             "bar");
    if (wall.w != 1280 || wall.h != 720 || bar.w != 1280 || bar.h != 30) {
        char msg[120];
        snprintf(msg, sizeof msg, "layer surfaces were sized %dx%d (wallpaper) and %dx%d (bar)", wall.w,
                 wall.h, bar.w, bar.h);
        fail(msg);
    }
    struct image img = capture_screen(app, d);
    expect_px(&img, 600, 10, C_BAR, "panel is drawn at the top");
    expect_px(&img, 600, 400, C_WALLPAPER, "wallpaper fills the rest of the output");
    free(img.px);

    /* the panel reserves its 30px: a maximized window starts below it */
    struct win a;
    win_open_ex(app, d, &a, "below the panel", 0xff000000u | C_CLIENT, 0, 1);
    wait_for(d, &app->kb_enter, 3000, "keyboard focus for the window");
    vtap(app, d, MOD_ALT, KEY_F);
    win_expect(&a, d, 1, 0, 1264, 720 - 30 - 16, "maximized window avoids the panel");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 600, 20, C_BAR, "the window does not cover the panel");
    expect_px(&img, 600, 34, C_WALLPAPER, "gap between the panel and the window");
    expect_px(&img, 600, 100, C_CLIENT, "window content below the panel");
    free(img.px);

    /* the pointer reaches the panel */
    app->ptr_enter = 0;
    vptr_move(app, d, 600, 10);
    if (!app->ptr_enter) {
        fail("pointer did not enter the panel");
    }

    /* a launcher on the overlay layer with exclusive keyboard focus takes the keyboard and
     * gives it back when it goes away */
    struct lay launcher;
    lay_open(app, d, &launcher, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, 0, 200, 100, 0,
             ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, 0xff000000u | C_OVERLAY, "launcher");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 640, 360, C_OVERLAY, "overlay launcher is centred on top of everything");
    free(img.px);
    if (app->kb_surface != launcher.surface) {
        fail("exclusive launcher did not get the keyboard focus");
    }
    lay_close(d, &launcher);
    if (app->kb_surface != a.surface) {
        fail("keyboard focus did not return to the window after the launcher closed");
    }

    /* closing the panel gives the space back */
    lay_close(d, &bar);
    win_expect(&a, d, 1, 0, 1264, 704, "maximized window grows when the panel goes away");
    wl_display_roundtrip(d);
    img = capture_screen(app, d);
    expect_px(&img, 600, 100, C_CLIENT, "window content is still shown");
    expect_not_px(&img, 600, 20, C_BAR, "the panel is gone");
    free(img.px);

    win_destroy(d, &a);
    lay_close(d, &wall);
}
#endif

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "single";
    int multi = !strcmp(mode, "multi");
    int deco = !strcmp(mode, "deco");
    int anim = !strcmp(mode, "anim");
    int layers = !strcmp(mode, "layers");
    int workspaces = !strcmp(mode, "workspaces");
    int lock = !strcmp(mode, "lock");
    int hypr = !strcmp(mode, "hypr");
    int fx = !strcmp(mode, "fx");
    int plugins = !strcmp(mode, "plugins");
    int wobbly = !strcmp(mode, "wobbly");
    int protocols = !strcmp(mode, "protocols");
    if (!multi && !deco && !anim && !layers && !workspaces && !lock && !hypr && !fx && !plugins && !wobbly && !protocols &&
        strcmp(mode, "single") != 0) {
        fail("unknown mode (use single, multi, deco, anim, layers, workspaces, lock, hypr, fx, plugins, wobbly or protocols)");
    }

    struct app app = {0};
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fail("cannot connect to compositor");
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &app);
    wl_display_roundtrip(display);
    static const char *const required[] = {
        "wl_data_device_manager",
        "zwp_primary_selection_device_manager_v1",
        "zwlr_data_control_manager_v1",
        "ext_idle_notifier_v1",
        "zwp_idle_inhibit_manager_v1",
        "zwlr_foreign_toplevel_manager_v1",
        "zwlr_layer_shell_v1",
        "zxdg_decoration_manager_v1",
        "wp_viewporter",
        "wp_fractional_scale_manager_v1",
        "wp_cursor_shape_manager_v1",
        "xdg_activation_v1",
        "zwp_pointer_constraints_v1",
        "zwp_relative_pointer_manager_v1",
        "zwlr_output_manager_v1",
    };
    for (size_t i = 0; i < sizeof required / sizeof *required; i++) {
        int found = 0;
        for (int g = 0; g < app.n_globals; g++) {
            found |= strcmp(app.globals[g], required[i]) == 0;
        }
        if (!found) {
            char msg[160];
            snprintf(msg, sizeof msg, "compositor does not advertise %s", required[i]);
            fail(msg);
        }
    }
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

    if (protocols) {
#ifdef HAVE_VIRTUAL_INPUT
        run_protocols(&app, display);
        wl_display_disconnect(display);
        printf("client_test protocols: OK\n");
        return 0;
#else
        fail("the protocols scenario needs the wlroots protocol files");
#endif
    }
    if (fx) {
#ifdef HAVE_VIRTUAL_INPUT
        run_fx(&app, display);
        wl_display_disconnect(display);
        printf("client_test fx: OK\n");
        return 0;
#else
        fail("the fx scenario needs the wlroots protocol files");
#endif
    }
    if (hypr) {
#ifdef HAVE_VIRTUAL_INPUT
        run_hypr(&app, display);
        wl_display_disconnect(display);
        printf("client_test hypr: OK\n");
        return 0;
#else
        fail("the hypr scenario needs the wlroots protocol files");
#endif
    }
    if (plugins || wobbly) {
#ifdef HAVE_VIRTUAL_INPUT
        if (plugins) {
            run_plugins(&app, display);
        } else {
            run_wobbly(&app, display);
        }
        wl_display_disconnect(display);
        printf("client_test %s: OK\n", mode);
        return 0;
#else
        fail("the plugin scenarios need the wlroots protocol files");
#endif
    }
    if (lock) {
#ifdef HAVE_VIRTUAL_INPUT
        run_lock(&app, display);
        wl_display_disconnect(display);
        printf("client_test lock: OK\n");
        return 0;
#else
        fail("the lock scenario needs the wlroots protocol files");
#endif
    }
    if (workspaces) {
#ifdef HAVE_VIRTUAL_INPUT
        run_workspaces(&app, display);
        wl_display_disconnect(display);
        printf("client_test workspaces: OK\n");
        return 0;
#else
        fail("the workspaces scenario needs the wlroots protocol files");
#endif
    }
    if (layers) {
#ifdef HAVE_VIRTUAL_INPUT
        run_layers(&app, display);
        wl_display_disconnect(display);
        printf("client_test layers: OK\n");
        return 0;
#else
        fail("the layers scenario needs the wlroots protocol files");
#endif
    }
    if (anim) {
#ifdef HAVE_VIRTUAL_INPUT
        run_anim(&app, display);
        wl_display_disconnect(display);
        printf("client_test anim: OK\n");
        return 0;
#else
        fail("the anim scenario needs the wlroots protocol files");
#endif
    }
    if (deco) {
#ifdef HAVE_VIRTUAL_INPUT
        run_deco(&app, display);
        wl_display_disconnect(display);
        printf("client_test deco: OK\n");
        return 0;
#else
        fail("the deco scenario needs the wlroots protocol files");
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
