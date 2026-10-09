/*
 * Small protocols that desktop programs expect: viewporter and fractional-scale (HiDPI),
 * cursor-shape (named cursors without a cursor theme of their own) and xdg-activation (a program
 * asks for another window to be focused, e.g. a link opened in the browser).
 */
#include "server.h"

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

/* A client asks for a named cursor (an arrow, a text beam, a hand ...). It only counts while the
 * pointer is over that client and the compositor is not dragging a window. */
static void handle_cursor_shape(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_cursor_shape);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
        server->cursor_mode != CURSOR_PASSTHROUGH ||
        server->seat->pointer_state.focused_client != event->seat_client) {
        return;
    }
    const char *name = wlr_cursor_shape_v1_name(event->shape);
    wlr_log(WLR_DEBUG, "cursor shape: %s", name);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

/* A program asks for a window to be activated. Only honored for a token that was made in answer
 * to an input event (a click or a key), so that background programs cannot steal the focus; a
 * window on another workspace is left alone. */
static void handle_activate(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_activate);
    const struct wlr_xdg_activation_v1_request_activate_event *event = data;
    struct wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
    struct toplevel *t = toplevel_from_xdg(xdg);
    if (!t || !t->mapped) {
        return;
    }
    if (!event->token->seat) {
        wlr_log(WLR_INFO, "activation of '%s' refused: the token does not come from an input event",
                xdg->title ? xdg->title : "(none)");
        return;
    }
    if (t->ws != server->ws_current) {
        wlr_log(WLR_INFO, "'%s' on workspace %d wants attention", xdg->title ? xdg->title : "(none)",
                t->ws + 1);
        return;
    }
    wlr_log(WLR_INFO, "window activated by request: %s", xdg->title ? xdg->title : "(none)");
    if (t->minimized) {
        toplevel_set_minimized(t, false);
    }
    focus_toplevel(t);
}

void protocols_init(struct server *server)
{
    wlr_viewporter_create(server->display);
    wlr_fractional_scale_manager_v1_create(server->display, 1);

    server->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server->display, 1);
    server->request_cursor_shape.notify = handle_cursor_shape;
    wl_signal_add(&server->cursor_shape_mgr->events.request_set_shape, &server->request_cursor_shape);

    server->xdg_activation = wlr_xdg_activation_v1_create(server->display);
    server->request_activate.notify = handle_activate;
    wl_signal_add(&server->xdg_activation->events.request_activate, &server->request_activate);
}

void protocols_finish(struct server *server)
{
    wl_list_remove(&server->request_cursor_shape.link);
    wl_list_remove(&server->request_activate.link);
}
