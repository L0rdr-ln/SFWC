/* Keybind actions (what a bound key does). */
#include "server.h"

void dispatch_action(struct server *server, enum action action, const char *arg)
{
    struct toplevel *top = focused_visible(server);
    wlr_log(WLR_DEBUG, "action %s, focused window: %s", action_name(action),
            top && top->xdg_toplevel->title ? top->xdg_toplevel->title : "(none)");
    switch (action) {
    case ACTION_QUIT:
        wl_display_terminate(server->display);
        break;
    case ACTION_SPAWN: {
        char *cmd = expand_command(server, arg);
        spawn(cmd);
        free(cmd);
        break;
    }
    case ACTION_CLOSE:
        if (top) {
            wlr_xdg_toplevel_send_close(top->xdg_toplevel);
        }
        break;
    case ACTION_CYCLE: {
        /* Focus the bottom-most visible window, which raises it. */
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (toplevel_shown(t)) {
                focus_toplevel(t);
                break;
            }
        }
        break;
    }
    case ACTION_TOGGLE_MAXIMIZE:
        if (top) {
            toplevel_apply_state(top, !top->maximized, false);
        }
        break;
    case ACTION_TOGGLE_FULLSCREEN:
        if (top) {
            toplevel_apply_state(top, top->maximized, !top->fullscreen);
        }
        break;
    case ACTION_MINIMIZE:
        if (top) {
            toplevel_set_minimized(top, true);
        }
        break;
    case ACTION_RESTORE: {
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (t->minimized && t->ws == server->ws_current) {
                toplevel_set_minimized(t, false);
                break;
            }
        }
        break;
    }
    case ACTION_RELOAD:
        reload_config(server);
        break;
    case ACTION_MOVE_OUTPUT:
        if (top) {
            move_to_next_output(top);
        }
        break;
    case ACTION_FOCUS_OUTPUT:
        focus_next_output(server);
        break;
    case ACTION_WORKSPACE:
        switch_workspace(server, atoi(arg) - 1);
        break;
    case ACTION_MOVE_WORKSPACE:
        if (top) {
            move_to_workspace(top, atoi(arg) - 1);
        }
        break;
    case ACTION_MOVE:
    case ACTION_RESIZE:
        break; /* mouse-only */
    }
}
