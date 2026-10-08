/*
 * Minimal Wayland client used by tests/run_client_test.sh.
 *
 * Connects to the compositor named by $WAYLAND_DISPLAY, opens an xdg toplevel
 * with a shm buffer, opens an xdg popup on it, waits for a frame callback
 * (proves the compositor mapped and rendered the window) and closes both.
 * Exit code 0 = everything worked.
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
#include "xdg-shell-client-protocol.h"
#ifdef HAVE_VIRTUAL_INPUT
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#endif

#define W 200
#define H 100
#define GAP 8 /* must match GAP in src/main.c */
#define SNAP 12 /* must match SNAP_DISTANCE in src/main.c */

struct app {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_output *output;
    int out_w, out_h;

    /* last toplevel configure */
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
    int key_presses[256];         /* key press events received, by key code */
    int ptr_enter, ptr_motion, ptr_button_press, ptr_button_release;
    double ptr_sx, ptr_sy;        /* last pointer position, surface-local */
    int closed_seen;
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

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    close(fd);
}
static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                     struct wl_array *keys)
{
    ((struct app *)data)->kb_enter++;
}
static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    ((struct app *)data)->kb_leave++;
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
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
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

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t sp, const char *make, const char *model, int32_t tr)
{
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh)
{
    struct app *app = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        app->out_w = w;
        app->out_h = h;
    }
}
static void output_done(void *d, struct wl_output *o) {}
static void output_scale(void *d, struct wl_output *o, int32_t f) {}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
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
#ifdef HAVE_VIRTUAL_INPUT
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        app->vptr_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        app->vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
#endif
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !app->output) {
        app->output = wl_registry_bind(reg, name, &wl_output_interface, 2);
        wl_output_add_listener(app->output, &output_listener, app);
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

/* Dispatch events until *flag becomes non-zero, or fail after timeout_ms. */
static void wait_for(struct wl_display *display, const int *flag, int timeout_ms, const char *what)
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
            char msg[128];
            snprintf(msg, sizeof msg, "timeout waiting for %s", what);
            fail(msg);
        }
        if (wl_display_read_events(display) < 0) {
            fail("connection lost (compositor crashed?)");
        }
        wl_display_dispatch_pending(display);
    }
}

/* Wait until a toplevel configure with the given state/size has been received. */
static void expect_configure(struct app *app, struct wl_display *display, int max, int fs, int w,
                             int h, const char *what)
{
    for (int i = 0; i < 20; i++) {
        if (app->cfg_arrived && app->cfg_max == max && app->cfg_fs == fs && app->cfg_w == w &&
            app->cfg_h == h) {
            return;
        }
        app->cfg_arrived = 0;
        wait_for(display, &app->cfg_arrived, 3000, what);
    }
    char msg[256];
    snprintf(msg, sizeof msg, "%s: last configure max=%d fs=%d size=%dx%d, wanted max=%d fs=%d %dx%d",
             what, app->cfg_max, app->cfg_fs, app->cfg_w, app->cfg_h, max, fs, w, h);
    fail(msg);
}

#ifdef HAVE_VIRTUAL_INPUT
/* Modifier masks of the default xkb keymap. */
#define MOD_SHIFT 0x1
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
                                            app->out_w, app->out_h);
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

static void run_input_tests(struct app *app, struct wl_display *d)
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
    wait_for(d, &app->kb_enter, 3000, "keyboard focus (wl_keyboard.enter) for the window");

    /* 1. keyboard: a plain key reaches the focused window */
    vtap(app, d, 0, KEY_A);
    if (!app->key_presses[KEY_A]) {
        fail("plain key press was not delivered to the focused window");
    }

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
    expect_configure(app, d, 0, 0, W + 51, H + 30, "Alt+right-drag resizes by the drag distance");

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

    /* 8. Alt+q asks the window to close */
    vtap(app, d, MOD_ALT, KEY_Q);
    wait_for(d, &app->closed, 3000, "xdg_toplevel.close after Alt+q");
}
#endif

int main(void)
{
    struct app app = {0};
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fail("cannot connect to compositor");
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &app);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); /* wl_output mode events */
    if (!app.compositor || !app.shm || !app.wm_base || !app.output) {
        fail("compositor is missing wl_compositor / wl_shm / xdg_wm_base / wl_output");
    }
    if (app.out_w <= 0 || app.out_h <= 0) {
        fail("no output mode received");
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
