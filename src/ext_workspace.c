/*
 * ext-workspace-v1: bars (waybar's workspaces module) show the workspaces and switch them.
 * One group holds all outputs; the workspaces are the global ones "1" ... "N".
 */
#include "server.h"

#include <wlr/types/wlr_ext_workspace_v1.h>

#define MAX_WS 9

static struct wlr_ext_workspace_handle_v1 *handles[MAX_WS];

/* Make the protocol objects match the config (number of workspaces) and the visible workspace. */
void ext_workspace_sync(struct server *server)
{
    if (!server->ext_ws_mgr) {
        return;
    }
    for (int i = 0; i < MAX_WS; i++) {
        if (i >= server->config.workspaces) {
            if (handles[i]) {
                wlr_ext_workspace_handle_v1_destroy(handles[i]);
                handles[i] = NULL;
            }
            continue;
        }
        if (!handles[i]) {
            char id[8];
            snprintf(id, sizeof id, "%d", i + 1);
            handles[i] = wlr_ext_workspace_handle_v1_create(server->ext_ws_mgr, id,
                                                            EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
            if (!handles[i]) {
                continue;
            }
            wlr_ext_workspace_handle_v1_set_name(handles[i], id);
            uint32_t coord = i;
            wlr_ext_workspace_handle_v1_set_coordinates(handles[i], &coord, 1);
            wlr_ext_workspace_handle_v1_set_group(handles[i], server->ext_ws_group);
        }
        wlr_ext_workspace_handle_v1_set_active(handles[i], i == server->ws_current);
    }
}

/* Outputs come and go: the group lists the ones that are in the layout. */
void ext_workspace_outputs(struct server *server)
{
    if (!server->ext_ws_group) {
        return;
    }
    struct output *out;
    wl_list_for_each(out, &server->outputs, link) {
        bool in_layout = wlr_output_layout_get(server->output_layout, out->wlr_output) != NULL;
        if (in_layout && !out->in_ws_group) {
            wlr_ext_workspace_group_handle_v1_output_enter(server->ext_ws_group, out->wlr_output);
            out->in_ws_group = true;
        } else if (!in_layout && out->in_ws_group) {
            wlr_ext_workspace_group_handle_v1_output_leave(server->ext_ws_group, out->wlr_output);
            out->in_ws_group = false;
        }
    }
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, ext_ws_commit);
    const struct wlr_ext_workspace_v1_commit_event *event = data;
    struct wlr_ext_workspace_v1_request *req;
    wl_list_for_each(req, event->requests, link) {
        if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
            continue; /* workspaces cannot be created, removed or moved between groups */
        }
        for (int i = 0; i < MAX_WS; i++) {
            if (handles[i] && handles[i] == req->activate.workspace) {
                switch_workspace(server, i);
            }
        }
    }
    ext_workspace_sync(server); /* a refused request must not leave the bar out of date */
}

void ext_workspace_init(struct server *server)
{
    server->ext_ws_mgr = wlr_ext_workspace_manager_v1_create(server->display, 1);
    if (!server->ext_ws_mgr) {
        return;
    }
    server->ext_ws_group = wlr_ext_workspace_group_handle_v1_create(server->ext_ws_mgr, 0);
    server->ext_ws_commit.notify = handle_commit;
    wl_signal_add(&server->ext_ws_mgr->events.commit, &server->ext_ws_commit);
    ext_workspace_sync(server);
}

void ext_workspace_finish(struct server *server)
{
    if (server->ext_ws_mgr) {
        wl_list_remove(&server->ext_ws_commit.link);
        server->ext_ws_mgr = NULL;
        server->ext_ws_group = NULL;
        memset(handles, 0, sizeof handles);
    }
}
