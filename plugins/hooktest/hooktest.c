/*
 * Test plugin: logs every call the compositor makes into it, so the end-to-end test can check
 * the plugin API (loading, settings, frame and window callbacks, reload, unloading).
 */
#include <stdlib.h>
#include <string.h>

#include "../../include/sfwc-plugin.h"

static int frames, commits;
static bool saw_window;

static bool on_init(struct sfwc_host *host)
{
    const char *greeting = host->config_get(host, "greeting");
    const char *missing = host->config_get(host, "missing");
    host->log(host, 2, "init api=%u greeting=%s missing=%s", host->api_version, greeting ? greeting : "(none)",
              missing ? missing : "(none)");
    if (host->struct_size < sizeof(struct sfwc_host) || !host->windows_tree) {
        return false;
    }
    if (greeting && !strcmp(greeting, "refuse")) {
        return false; /* the compositor must unload us and carry on */
    }
    /* the whole [plugin:hooktest] section, repeated keys included */
    for (size_t i = 0, n = host->config_count(host); i < n; i++) {
        const char *k, *v;
        int line;
        if (host->config_entry(host, i, &k, &v, &line)) {
            host->log(host, 2, "setting %s=%s (line %d)", k, v, line);
        }
    }
    struct wlr_box out;
    host->output_box_at(host, 0, 0, &out);
    host->log(host, 2, "workspace %d, clock %s", host->current_workspace(host), host->now_ms(host) ? "ticks" : "stuck");
    frames = commits = 0;
    saw_window = false;
    return true;
}

static void on_fini(struct sfwc_host *host)
{
    host->log(host, 2, "fini after %d frames", frames);
}

static void on_reconfigure(struct sfwc_host *host)
{
    const char *greeting = host->config_get(host, "greeting");
    host->log(host, 2, "reconfigure greeting=%s", greeting ? greeting : "(none)");
}

static void on_frame(struct sfwc_host *host, uint32_t now)
{
    if (frames++ == 0) {
        host->log(host, 2, "first frame");
    }
    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        struct sfwc_toplevel_info info;
        if (host->toplevel_info(host, t, &info) && !saw_window) {
            saw_window = true;
            host->log(host, 2, "window outer=%d,%d %dx%d visible=%d tree=%d", info.outer.x, info.outer.y,
                      info.outer.width, info.outer.height, info.visible, info.tree != NULL);
        }
    }
}

static void on_unmap(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    host->log(host, 2, "window unmapped");
}

static void on_map(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    struct sfwc_toplevel_info info;
    if (host->toplevel_info(host, t, &info)) {
        host->log(host, 2, "window mapped at %d,%d workspace=%d shown=%d", info.outer.x, info.outer.y,
                  info.workspace, info.shown);
    }
}

static void on_commit(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    if (commits++ == 0) {
        host->log(host, 2, "first commit");
    }
}

static bool on_move(struct sfwc_host *host, struct sfwc_toplevel *t, double fx, double fy, double tx, double ty)
{
    host->log(host, 2, "window move %.0f,%.0f -> %.0f,%.0f", fx, fy, tx, ty);
    return false; /* let it jump */
}

static void on_cancel(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    static bool said;
    if (!said) {
        said = true;
        host->log(host, 2, "window cancel");
    }
}

static bool on_focus(struct sfwc_host *host, struct sfwc_toplevel *t, double from, double to)
{
    host->log(host, 2, "window focus %.0f -> %.0f", from, to);
    return false;
}

static void on_ws_leaving(struct sfwc_host *host, int old_ws, int new_ws)
{
    host->log(host, 2, "workspace leaving %d -> %d (current %d)", old_ws, new_ws, host->current_workspace(host));
}

static void on_ws_entered(struct sfwc_host *host, int old_ws, int new_ws)
{
    host->log(host, 2, "workspace entered %d -> %d (current %d)", old_ws, new_ws, host->current_workspace(host));
}

static void on_layer_map(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    host->log(host, 2, "layer mapped tree=%d", tree != NULL);
}

static void on_layer_unmap(struct sfwc_host *host, struct wlr_scene_tree *tree)
{
    host->log(host, 2, "layer unmapped");
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = sizeof(struct sfwc_plugin),
    .name = "hooktest",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .fini = on_fini,
    .reconfigure = on_reconfigure,
    .frame = on_frame,
    .toplevel_unmap = on_unmap,
    .toplevel_map = on_map,
    .toplevel_commit = on_commit,
    .toplevel_move = on_move,
    .toplevel_cancel = on_cancel,
    .toplevel_focus = on_focus,
    .workspace_leaving = on_ws_leaving,
    .workspace_entered = on_ws_entered,
    .layer_map = on_layer_map,
    .layer_unmap = on_layer_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
