/* wlr-foreign-toplevel-management: window lists for taskbars. */
#include "server.h"

/* ------------------------------------------- taskbar protocol, idle, clipboard */

/* Push title, app id and state of a window to the taskbars watching it. */
void fth_sync(struct toplevel *t)
{
    if (!t->fth) {
        return;
    }
    struct wlr_xdg_toplevel *x = t->xdg_toplevel;
    wlr_foreign_toplevel_handle_v1_set_title(t->fth, x->title ? x->title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(t->fth, x->app_id ? x->app_id : "");
    wlr_foreign_toplevel_handle_v1_set_maximized(t->fth, t->maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(t->fth, t->minimized);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(t->fth, t->fullscreen);
    wlr_foreign_toplevel_handle_v1_set_activated(
        t->fth, t->mapped && !t->minimized &&
                    t->server->seat->keyboard_state.focused_surface == x->base->surface);
}

static void fth_request_activate(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_activate);
    if (!t->mapped) {
        return;
    }
    if (t->ws != t->server->ws_current) {
        switch_workspace(t->server, t->ws);
    }
    if (t->minimized) {
        toplevel_set_minimized(t, false);
    } else {
        focus_toplevel(t);
    }
}

static void fth_request_maximize(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
    if (t->mapped) {
        toplevel_apply_state(t, ev->maximized, t->fullscreen);
    }
}

static void fth_request_minimize(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
    toplevel_set_minimized(t, ev->minimized);
}

static void fth_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
    if (t->mapped) {
        toplevel_apply_state(t, t->maximized, ev->fullscreen);
    }
}

static void fth_request_close(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_close);
    wlr_xdg_toplevel_send_close(t->xdg_toplevel);
}

void fth_create(struct toplevel *t)
{
    t->fth = wlr_foreign_toplevel_handle_v1_create(t->server->foreign_toplevel_mgr);
    t->fth_activate.notify = fth_request_activate;
    wl_signal_add(&t->fth->events.request_activate, &t->fth_activate);
    t->fth_maximize.notify = fth_request_maximize;
    wl_signal_add(&t->fth->events.request_maximize, &t->fth_maximize);
    t->fth_minimize.notify = fth_request_minimize;
    wl_signal_add(&t->fth->events.request_minimize, &t->fth_minimize);
    t->fth_fullscreen.notify = fth_request_fullscreen;
    wl_signal_add(&t->fth->events.request_fullscreen, &t->fth_fullscreen);
    t->fth_close.notify = fth_request_close;
    wl_signal_add(&t->fth->events.request_close, &t->fth_close);
    fth_sync(t);
}

void fth_destroy(struct toplevel *t)
{
    if (!t->fth) {
        return;
    }
    wl_list_remove(&t->fth_activate.link);
    wl_list_remove(&t->fth_maximize.link);
    wl_list_remove(&t->fth_minimize.link);
    wl_list_remove(&t->fth_fullscreen.link);
    wl_list_remove(&t->fth_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(t->fth);
    t->fth = NULL;
}
