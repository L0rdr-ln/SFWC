/* Workspaces: which windows are shown, switching and moving windows. */
#include "server.h"

/* Show the windows of the current workspace, hide the others, and hand the keyboard to the
 * front window that is shown (or to nobody). */
static void workspace_refresh(struct server *server)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        wlr_scene_node_set_enabled(&t->scene_tree->node, toplevel_shown(t));
    }
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct wlr_xdg_toplevel *fx = focus ? wlr_xdg_toplevel_try_from_wlr_surface(focus) : NULL;
    struct toplevel *ft = toplevel_from_xdg(fx);
    if (ft && !toplevel_shown(ft)) {
        wlr_xdg_toplevel_set_activated(ft->xdg_toplevel, false);
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    wlr_seat_pointer_clear_focus(server->seat);
    if (server->grabbed_toplevel && !toplevel_shown(server->grabbed_toplevel)) {
        reset_cursor_mode(server);
    }
    struct toplevel *next = top_visible(server);
    if (next) {
        focus_toplevel(next);
    }
    wl_list_for_each(t, &server->toplevels, link) {
        fth_sync(t);
    }
    process_cursor_motion(server, now_msec());
}

void switch_workspace(struct server *server, int ws)
{
    if (ws < 0 || ws >= server->config.workspaces || ws == server->ws_current) {
        return;
    }
    wlr_log(WLR_INFO, "workspace %d", ws + 1);
    server->ws_current = ws;
    workspace_refresh(server);
}

void move_to_workspace(struct toplevel *t, int ws)
{
    struct server *server = t->server;
    if (ws < 0 || ws >= server->config.workspaces || ws == t->ws) {
        return;
    }
    wlr_log(WLR_INFO, "window moved to workspace %d", ws + 1);
    t->ws = ws;
    workspace_refresh(server);
}

/* After a reload that lowered the number of workspaces. */
void workspace_clamp(struct server *server)
{
    int last = server->config.workspaces - 1;
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t->ws > last) {
            t->ws = last;
        }
    }
    if (server->ws_current > last) {
        server->ws_current = last;
    }
    workspace_refresh(server);
}
