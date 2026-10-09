/*
 * Small protocols that desktop programs expect: viewporter and fractional-scale (HiDPI),
 * cursor-shape (named cursors without a cursor theme of their own) and xdg-activation (a program
 * asks for another window to be focused, e.g. a link opened in the browser).
 */
#include "server.h"

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/util/region.h>
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

/* ---------------------------------------------- pointer constraints + relative pointer */

static void constraint_deactivate(struct server *server)
{
    struct wlr_pointer_constraint_v1 *c = server->active_constraint;
    if (!c) {
        return;
    }
    wl_list_remove(&server->active_constraint_destroy.link);
    server->active_constraint = NULL;
    wlr_pointer_constraint_v1_send_deactivated(c);
}

static void handle_active_constraint_destroy(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, active_constraint_destroy);
    wl_list_remove(&server->active_constraint_destroy.link);
    server->active_constraint = NULL; /* the client is told by the protocol object's destruction */
}

void constraints_focus(struct server *server, struct wlr_surface *surface, double origin_x,
                       double origin_y)
{
    struct wlr_pointer_constraint_v1 *want = NULL;
    if (surface && server->cursor_mode == CURSOR_PASSTHROUGH) {
        want = wlr_pointer_constraints_v1_constraint_for_surface(server->pointer_constraints, surface,
                                                                 server->seat);
    }
    if (want == server->active_constraint) {
        if (want) {
            server->constraint_ox = origin_x;
            server->constraint_oy = origin_y;
        }
        return;
    }
    constraint_deactivate(server);
    if (!want) {
        return;
    }
    server->constraint_ox = origin_x;
    server->constraint_oy = origin_y;
    server->active_constraint = want;
    server->active_constraint_destroy.notify = handle_active_constraint_destroy;
    wl_signal_add(&want->events.destroy, &server->active_constraint_destroy);
    wlr_log(WLR_DEBUG, "pointer %s", want->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "locked" : "confined");
    wlr_pointer_constraint_v1_send_activated(want);
}

bool constraints_limit(struct server *server, double *dx, double *dy)
{
    struct wlr_pointer_constraint_v1 *c = server->active_constraint;
    if (!c) {
        return true;
    }
    if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return false;
    }
    double sx = server->cursor->x - server->constraint_ox;
    double sy = server->cursor->y - server->constraint_oy;
    double cx, cy;
    if (!wlr_region_confine(&c->region, sx, sy, sx + *dx, sy + *dy, &cx, &cy)) {
        return false;
    }
    *dx = cx - sx;
    *dy = cy - sy;
    return true;
}

static void handle_new_constraint(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_constraint);
    struct wlr_pointer_constraint_v1 *c = data;
    /* it starts to work as soon as the pointer is over its surface */
    if (c->surface == server->seat->pointer_state.focused_surface) {
        process_cursor_motion(server, now_msec());
    }
}

void constraints_init(struct server *server)
{
    server->relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(server->display);
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
    server->new_constraint.notify = handle_new_constraint;
    wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_constraint);
}

void constraints_finish(struct server *server)
{
    if (server->active_constraint) {
        wl_list_remove(&server->active_constraint_destroy.link);
        server->active_constraint = NULL;
    }
    wl_list_remove(&server->new_constraint.link);
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

    constraints_init(server);
}

void protocols_finish(struct server *server)
{
    wl_list_remove(&server->request_cursor_shape.link);
    wl_list_remove(&server->request_activate.link);
    constraints_finish(server);
}
