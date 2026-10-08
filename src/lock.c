/* ext-session-lock: the lock cover and routing of input to the lock client. */
#include "server.h"

/* ----------------------------------------------------------- session lock */

/* Cover the whole layout (all outputs) with black so nothing shows while locked. */
void lock_update_bg(struct server *server)
{
    if (!server->lock_bg) {
        return;
    }
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    wlr_scene_node_set_position(&server->lock_bg->node, box.x, box.y);
    wlr_scene_rect_set_size(server->lock_bg, box.width, box.height);
}

void lock_new_surface(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_new_surface);
    struct wlr_session_lock_surface_v1 *surf = data;
    struct wlr_box box = {0};
    if (surf->output) {
        wlr_output_layout_get_box(server->output_layout, surf->output, &box);
    }
    struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(server->lock_tree, surf->surface);
    wlr_scene_node_set_position(&tree->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(surf, box.width, box.height);
    wlr_log(WLR_INFO, "lock surface for %s", surf->output ? surf->output->name : "(no output)");

    struct wlr_seat *seat = server->seat;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    struct wlr_surface *old = seat->keyboard_state.focused_surface;
    struct wlr_session_lock_surface_v1 *old_lock = old ? wlr_session_lock_surface_v1_try_from_wlr_surface(old) : NULL;
    if (keyboard && !old_lock) {
        wlr_seat_keyboard_notify_enter(seat, surf->surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void lock_release_listeners(struct server *server)
{
    wl_list_remove(&server->lock_new_surface.link);
    wl_list_remove(&server->lock_unlock.link);
    wl_list_remove(&server->lock_destroy.link);
    server->cur_lock = NULL;
}

static void lock_handle_unlock(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_unlock);
    wlr_log(WLR_INFO, "session unlocked");
    server->locked = false;
    wlr_scene_node_set_enabled(&server->lock_tree->node, false);
    struct toplevel *next = top_visible(server);
    if (next) {
        focus_toplevel(next);
    } else {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

/* The lock client went away. Without an unlock request the session stays locked. */
static void lock_handle_destroy(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_destroy);
    if (server->locked) {
        wlr_log(WLR_INFO, "lock client went away without unlocking: the session stays locked");
    }
    lock_release_listeners(server);
}

void server_new_lock(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_lock);
    struct wlr_session_lock_v1 *lock = data;
    if (server->cur_lock) { /* somebody holds the lock already */
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    wlr_log(WLR_INFO, "session locked");
    server->locked = true;
    server->cur_lock = lock;
    reset_cursor_mode(server);
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct wlr_xdg_toplevel *fx = focus ? wlr_xdg_toplevel_try_from_wlr_surface(focus) : NULL;
    if (fx) {
        wlr_xdg_toplevel_set_activated(fx, false);
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_scene_node_set_enabled(&server->lock_tree->node, true);
    lock_update_bg(server);

    server->lock_new_surface.notify = lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
    server->lock_unlock.notify = lock_handle_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock_unlock);
    server->lock_destroy.notify = lock_handle_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock_destroy);
    wlr_session_lock_v1_send_locked(lock);
    process_cursor_motion(server, now_msec());
}
