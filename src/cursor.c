/* Pointer: hit testing, focus under the cursor, move/resize by dragging, buttons. */
#include "server.h"

/* --------------------------------------------------------------- cursor */

/* The window under (lx, ly). When the point is on a client surface, *surface is set and
 * *part is DECO_CLIENT; when it is on a server-side frame, *surface stays NULL and *part
 * (and *edges for resize zones) say which part of the frame it is. */
static struct toplevel *toplevel_at(struct server *server, double lx, double ly,
                                    struct wlr_surface **surface, double *sx, double *sy,
                                    enum deco_part *part, uint32_t *edges)
{
    *part = DECO_OUTSIDE;
    *edges = 0;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);

    /* Walk up to the tree that belongs to a toplevel. */
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    struct toplevel *toplevel = tree ? tree->node.data : NULL;

    if (scene_surface) {
        *surface = scene_surface->surface;
        *part = DECO_CLIENT;
        return toplevel;
    }
    if (toplevel && toplevel->chrome && node == &toplevel->chrome->node) {
        struct wlr_box g = toplevel_geometry(toplevel);
        *part = deco_hit_test(&server->theme, g.width, g.height, lx - g.x, ly - g.y, edges);
        return *part == DECO_OUTSIDE ? NULL : toplevel;
    }
    return NULL;
}

static const char *resize_cursor_name(uint32_t edges)
{
    switch (edges) {
    case WLR_EDGE_TOP:
        return "n-resize";
    case WLR_EDGE_BOTTOM:
        return "s-resize";
    case WLR_EDGE_LEFT:
        return "w-resize";
    case WLR_EDGE_RIGHT:
        return "e-resize";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT:
        return "nw-resize";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT:
        return "ne-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT:
        return "sw-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT:
        return "se-resize";
    default:
        return "default";
    }
}

void reset_cursor_mode(struct server *server)
{
    server->cursor_mode = CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

void begin_interactive(struct toplevel *toplevel, enum cursor_mode mode, uint32_t edges,
                              bool check_focus)
{
    struct server *server = toplevel->server;
    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
    if (toplevel->maximized || toplevel->fullscreen) {
        return; /* un-maximize first */
    }
    if (check_focus && (focused == NULL || toplevel->xdg_toplevel->base->surface !=
                                               wlr_surface_get_root_surface(focused))) {
        return; /* a client may only start a grab while the pointer is over its window */
    }
    plugins_toplevel_cancel(toplevel); /* the window follows the pointer from where it is */
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo;
        geo = toplevel->xdg_toplevel->base->geometry; /* wlroots 0.20: kept up to date on commit */
        double border_x = (toplevel->scene_tree->node.x + geo.x) +
                          ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo.y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;
        server->grab_geobox = geo;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;
        server->resize_edges = edges;
    }
}

/* If `target` is closer to `pos` than *best, remember it. */
static void try_snap(double pos, double target, double *best, double *out)
{
    double d = fabs(pos - target);
    if (d < *best) {
        *best = d;
        *out = target;
    }
}

static void process_cursor_move(struct server *server)
{
    struct toplevel *toplevel = server->grabbed_toplevel;
    double nx = server->cursor->x - server->grab_x;
    double ny = server->cursor->y - server->grab_y;

    /* Snap the window's geometry to the output edges and to other windows (keeping
     * `gap` between them); the nearest candidate on each axis wins. */
    struct wlr_box geo = {0};
    geo = toplevel->xdg_toplevel->base->geometry; /* wlroots 0.20: kept up to date on commit */
    const struct config *cfg = &server->config;
    struct deco_insets ins = toplevel_insets(toplevel);
    /* work on the outer rectangle (content plus frame) */
    double x = nx + geo.x - ins.left, y = ny + geo.y - ins.top;
    int w = geo.width + ins.left + ins.right, h = geo.height + ins.top + ins.bottom;
    double snap = cfg->snap_distance, gap = cfg->gap;
    double best_x = snap, best_y = snap, tx = x, ty = y;
    struct wlr_box box = work_area_at(server, server->cursor->x, server->cursor->y);
    if (box.width > 0 && cfg->snap_to_edges) {
        try_snap(x, box.x + gap, &best_x, &tx);
        try_snap(x, box.x + box.width - gap - w, &best_x, &tx);
        try_snap(y, box.y + gap, &best_y, &ty);
        try_snap(y, box.y + box.height - gap - h, &best_y, &ty);
    }
    if (cfg->snap_to_windows) {
        struct toplevel *o;
        wl_list_for_each(o, &server->toplevels, link) {
            if (o == toplevel || o->minimized) {
                continue;
            }
            struct wlr_box og = toplevel_outer(o);
            bool overlap_y = y < og.y + og.height + snap && y + h > og.y - snap;
            bool overlap_x = x < og.x + og.width + snap && x + w > og.x - snap;
            if (overlap_y) {
                try_snap(x, og.x + og.width + gap, &best_x, &tx);
                try_snap(x, og.x - gap - w, &best_x, &tx);
            }
            if (overlap_x) {
                try_snap(y, og.y + og.height + gap, &best_y, &ty);
                try_snap(y, og.y - gap - h, &best_y, &ty);
            }
        }
    }
    nx = tx + ins.left - geo.x;
    ny = ty + ins.top - geo.y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, nx, ny);
}

static void process_cursor_resize(struct server *server)
{
    struct toplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box geo;
    geo = toplevel->xdg_toplevel->base->geometry; /* wlroots 0.20: kept up to date on commit */
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo.x, new_top - geo.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
}

void process_cursor_motion(struct server *server, uint32_t time)
{
    if (server->cursor_mode == CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    enum deco_part part;
    uint32_t edges;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface,
                                            &sx, &sy, &part, &edges);
    if (!toplevel) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    } else {
        if (!surface) { /* over a server-side frame: resize cursors on the edges */
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                                   part == DECO_BORDER ? resize_cursor_name(edges) : "default");
        }
        if (server->config.focus == FOCUS_FOLLOW_MOUSE) {
            focus_toplevel_ex(toplevel, false); /* focus without raising */
        }
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

void cursor_motion(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    input_activity(server);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    input_activity(server);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    input_activity(server);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
        reset_cursor_mode(server);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    enum deco_part part;
    uint32_t hit_edges;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface,
                                            &sx, &sy, &part, &hit_edges);
    focus_toplevel(toplevel); /* click to focus + raise */

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    const struct mousebind *mbind = config_find_mousebind(&server->config, mods, event->button);
    if (toplevel && mbind) {
        /* modifier+drag: do not forward the click to the client. */
        if (mbind->action == ACTION_MOVE) {
            begin_interactive(toplevel, CURSOR_MOVE, 0, false);
        } else {
            struct wlr_box g = toplevel_geometry(toplevel);
            uint32_t edges = (server->cursor->x - g.x < g.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                             (server->cursor->y - g.y < g.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
            begin_interactive(toplevel, CURSOR_RESIZE, edges, false);
        }
        return;
    }

    if (!toplevel && surface) { /* a panel/launcher that takes keyboard input when clicked */
        struct wlr_layer_surface_v1 *layer =
            wlr_layer_surface_v1_try_from_wlr_surface(wlr_surface_get_root_surface(surface));
        if (layer && layer->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
            focus_layer_surface(server, layer);
        }
    }
    if (toplevel && !surface) { /* the server-side frame */
        if (event->button != BTN_LEFT) {
            return;
        }
        static uint32_t last_click_time;
        static struct toplevel *last_click_toplevel;
        switch (part) {
        case DECO_CLOSE:
            wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
            break;
        case DECO_MAXIMIZE:
            toplevel_apply_state(toplevel, !toplevel->maximized, false);
            break;
        case DECO_MINIMIZE:
            toplevel_set_minimized(toplevel, true);
            break;
        case DECO_TITLE:
            if (last_click_toplevel == toplevel && event->time_msec - last_click_time < 400) {
                toplevel_apply_state(toplevel, !toplevel->maximized, false); /* double click */
                last_click_toplevel = NULL;
            } else {
                last_click_toplevel = toplevel;
                last_click_time = event->time_msec;
                begin_interactive(toplevel, CURSOR_MOVE, 0, false);
            }
            break;
        case DECO_BORDER:
            if (hit_edges) {
                begin_interactive(toplevel, CURSOR_RESIZE, hit_edges, false);
            }
            break;
        default:
            break;
        }
        return;
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

void cursor_axis(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    input_activity(server);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

void cursor_frame(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}
