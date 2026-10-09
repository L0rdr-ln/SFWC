/*
 * Test plugin: logs every call the compositor makes into it, so the end-to-end test can check
 * the plugin API (loading, settings, frame and window callbacks, reload, unloading).
 */
#include <stdlib.h>
#include <string.h>

#include "../../include/sfwc-plugin.h"

static int frames;
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
    frames = 0;
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

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .name = "hooktest",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .fini = on_fini,
    .reconfigure = on_reconfigure,
    .frame = on_frame,
    .toplevel_unmap = on_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
