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

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define W 200
#define H 100
#define GAP 8 /* must match GAP in src/main.c */

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
