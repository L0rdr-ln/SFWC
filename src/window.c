/* xdg-shell windows: placement, focus, maximize/fullscreen/minimize, popups. */
#include "server.h"

/* ---------------------------------------------------------- geometry */

/* Window geometry (without client-side shadows) in layout coordinates. */
struct wlr_box toplevel_geometry(struct toplevel *t)
{
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    geo.x += t->scene_tree->node.x;
    geo.y += t->scene_tree->node.y;
    return geo;
}

/* Move the window (content top-left to x, y), with a tween when `animate` is set. */
void toplevel_move_to_ex(struct toplevel *t, int x, int y, bool animate)
{
    struct server *server = t->server;
    animations_cancel(t, true);
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    int nx = x - geo.x, ny = y - geo.y;
    if (animate && t->mapped && server->config.anim_move && animations_enabled(server) &&
        server->config.anim_duration_ms > 0 &&
        (abs(nx - t->scene_tree->node.x) > 2 || abs(ny - t->scene_tree->node.y) > 2)) {
        animation_start(server, ANIM_MOVE, t, t->scene_tree, t->scene_tree->node.x,
                        t->scene_tree->node.y, nx, ny, 1, 1);
    } else {
        wlr_scene_node_set_position(&t->scene_tree->node, nx, ny);
    }
}

static void toplevel_move_to(struct toplevel *t, int x, int y)
{
    toplevel_move_to_ex(t, x, y, false);
}

/* On the current workspace and not minimized. */
bool toplevel_shown(const struct toplevel *t)
{
    return !t->minimized && t->ws == t->server->ws_current;
}

struct toplevel *top_visible(struct server *server)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (toplevel_shown(t)) {
            return t;
        }
    }
    return NULL;
}

/* The window with keyboard focus (the front window when nothing is focused). With
 * focus=follow-mouse this is not necessarily the front window. */
struct toplevel *focused_visible(struct server *server)
{
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (toplevel_shown(t) && t->xdg_toplevel->base->surface == focus) {
            return t;
        }
    }
    return top_visible(server);
}

/* Apply maximized/fullscreen state; restores the saved geometry when both are off. */
void toplevel_apply_state(struct toplevel *t, bool max, bool fs)
{
    bool was_normal = !t->maximized && !t->fullscreen;
    struct wlr_box ref = was_normal ? toplevel_geometry(t) : t->saved;
    if (was_normal && (max || fs)) {
        t->saved = ref;
    }
    t->maximized = max;
    t->fullscreen = fs;
    wlr_xdg_toplevel_set_maximized(t->xdg_toplevel, max);
    wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, fs);
    frame_refresh(t); /* fullscreen windows lose their frame */
    struct deco_insets ins = toplevel_insets(t);

    double cx = ref.x + ref.width / 2.0, cy = ref.y + ref.height / 2.0;
    struct wlr_box box = fs ? output_box_at(t->server, cx, cy) : work_area_at(t->server, cx, cy);
    wlr_log(WLR_DEBUG, "apply state max=%d fs=%d, output box %d,%d %dx%d (window centre %.0f,%.0f)",
            max, fs, box.x, box.y, box.width, box.height, ref.x + ref.width / 2.0,
            ref.y + ref.height / 2.0);
    if ((fs || max) && box.width > 0) {
        int inset = fs ? 0 : t->server->config.gap;
        toplevel_move_to_ex(t, box.x + inset + ins.left, box.y + inset + ins.top, true);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, box.width - 2 * inset - ins.left - ins.right,
                                  box.height - 2 * inset - ins.top - ins.bottom);
    } else if (!fs && !max) {
        toplevel_move_to_ex(t, t->saved.x, t->saved.y, true);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, t->saved.width, t->saved.height);
    }
    if (t->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
    }
    fth_sync(t);
}

/* Cascade new windows from the top-left of the output under the cursor. */
static void place_new_toplevel(struct toplevel *t)
{
    struct server *server = t->server;
    struct wlr_box box = work_area_at(server, server->cursor->x, server->cursor->y);
    if (box.width <= 0) {
        return;
    }
    struct wlr_box geo = toplevel_outer(t); /* including the frame */
    struct deco_insets ins = toplevel_insets(t);
    int x, y;
    if (t->xdg_toplevel->parent) { /* dialog: center */
        x = box.x + (box.width - geo.width) / 2;
        y = box.y + (box.height - geo.height) / 2;
    } else {
        x = box.x + 48 + server->cascade;
        y = box.y + 48 + server->cascade;
        server->cascade = (server->cascade + CASCADE_STEP) % (6 * CASCADE_STEP);
    }
    int gap = server->config.gap;
    int max_x = box.x + box.width - gap - geo.width;
    int max_y = box.y + box.height - gap - geo.height;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < box.x + gap) {
        x = box.x + gap;
    }
    if (y < box.y + gap) {
        y = box.y + gap;
    }
    toplevel_move_to(t, x + ins.left, y + ins.top);
}

/* -------------------------------------------------------------- focusing */
void focus_toplevel_ex(struct toplevel *toplevel, bool raise)
{
    if (toplevel == NULL || toplevel->server->locked) {
        return;
    }
    struct server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev != NULL) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);

    if (raise) { /* raise to the top of the stacking order */
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
        wl_list_remove(&toplevel->link);
        wl_list_insert(&server->toplevels, &toplevel->link);
    }
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
}

void focus_toplevel(struct toplevel *toplevel)
{
    focus_toplevel_ex(toplevel, true);
}

void toplevel_set_minimized(struct toplevel *t, bool minimize)
{
    struct server *server = t->server;
    if (!t->mapped || t->minimized == minimize) {
        return;
    }
    t->minimized = minimize;
    wlr_scene_node_set_enabled(&t->scene_tree->node, !minimize && t->ws == server->ws_current);
    if (minimize) {
        wlr_log(WLR_INFO, "window minimized");
        wlr_xdg_toplevel_set_activated(t->xdg_toplevel, false);
        /* park it at the bottom of the stack */
        wl_list_remove(&t->link);
        wl_list_insert(server->toplevels.prev, &t->link);
        if (t == server->grabbed_toplevel) {
            server->cursor_mode = CURSOR_PASSTHROUGH;
            server->grabbed_toplevel = NULL;
        }
        if (server->seat->keyboard_state.focused_surface == t->xdg_toplevel->base->surface) {
            struct toplevel *next = top_visible(server);
            if (next) {
                focus_toplevel(next);
            } else {
                wlr_seat_keyboard_notify_clear_focus(server->seat);
            }
        }
        wlr_seat_pointer_clear_focus(server->seat);
    } else {
        wlr_log(WLR_INFO, "window restored");
        focus_toplevel(t);
    }
    fth_sync(t);
}

/* ------------------------------------------------------ xdg-shell windows */

/* ----------------------------------------------- server-side decorations */
struct toplevel *toplevel_from_xdg(struct wlr_xdg_toplevel *xdg)
{
    struct wlr_scene_tree *tree = xdg ? xdg->base->data : NULL;
    return tree ? tree->node.data : NULL;
}

/* The frame shows whether its window has keyboard focus. */
void handle_keyboard_focus_change(struct wl_listener *listener, void *data)
{
    struct wlr_seat_keyboard_focus_change_event *event = data;
    struct wlr_surface *surfaces[2] = {event->old_surface, event->new_surface};
    for (int i = 0; i < 2; i++) {
        if (!surfaces[i]) {
            continue;
        }
        struct wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_try_from_wlr_surface(surfaces[i]);
        struct toplevel *t = toplevel_from_xdg(xdg);
        if (t) {
            frame_refresh(t);
            fth_sync(t);
        }
    }
}

static void toplevel_set_title(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    frame_refresh(toplevel);
    fth_sync(toplevel);
}

static void toplevel_map(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wlr_log(WLR_INFO, "window mapped: title=%s app_id=%s",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(none)",
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(none)");
    toplevel->mapped = true;
    toplevel->ws = toplevel->server->ws_current;
    if (!toplevel->maximized && !toplevel->fullscreen) {
        place_new_toplevel(toplevel);
    }
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    fth_create(toplevel);
    focus_toplevel(toplevel);
    frame_refresh(toplevel);
    animate_open(toplevel);
    snapshot_refresh(toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    wlr_log(WLR_INFO, "window unmapped");
    struct server *server = toplevel->server;
    plugins_toplevel_unmap(toplevel); /* before the close animation takes its picture */
    animations_cancel(toplevel, false);
    animate_close(toplevel);
    if (toplevel == server->grabbed_toplevel) {
        reset_cursor_mode(server);
    }
    wl_list_remove(&toplevel->link);
    toplevel->mapped = false;
    fth_destroy(toplevel);
    /* Hand keyboard focus to the next window. */
    if (server->seat->keyboard_state.focused_surface == toplevel->xdg_toplevel->base->surface) {
        struct toplevel *next = top_visible(server);
        if (next) {
            focus_toplevel(next);
        } else {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }
}

void toplevel_commit(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit) {
        decoration_apply(toplevel);
        if (toplevel->maximized || toplevel->fullscreen) {
            /* requested before the first commit: size it for the output now */
            toplevel_apply_state(toplevel, toplevel->maximized, toplevel->fullscreen);
        } else {
            /* Let the client pick its own size. */
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
    frame_refresh(toplevel); /* cheap when size, focus and title did not change */
    snapshot_refresh(toplevel);
}

static void toplevel_destroy(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->set_title.link);
    if (toplevel->decoration) {
        wl_list_remove(&toplevel->deco_request_mode.link);
        wl_list_remove(&toplevel->deco_destroy.link);
    }
    animations_cancel(toplevel, false);
    if (toplevel->last_frame) {
        wlr_scene_node_destroy(&toplevel->last_frame->node);
    }
    free(toplevel->frame_title);
    free(toplevel);
}

static void toplevel_request_move(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, CURSOR_MOVE, 0, true);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    begin_interactive(toplevel, CURSOR_RESIZE, event->edges, true);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    toplevel_apply_state(toplevel, toplevel->xdg_toplevel->requested.maximized,
                         toplevel->fullscreen);
}

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    toplevel_apply_state(toplevel, toplevel->maximized,
                         toplevel->xdg_toplevel->requested.fullscreen);
}

static void toplevel_request_minimize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
    toplevel_set_minimized(toplevel, true);
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(server->windows_tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
}

static void popup_commit(struct wl_listener *listener, void *data)
{
    struct popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
    struct popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

/* Show an xdg popup under `parent_tree` (a window's or a layer surface's scene tree). */
void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree)
{
    struct popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_popup *xdg_popup = data;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    popup_create(xdg_popup, parent->data);
}
