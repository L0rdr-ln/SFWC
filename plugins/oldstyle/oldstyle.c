/*
 * Test plugin written against the first fields of the callback table only (struct_size ends
 * before the callbacks added later): the compositor must load it and treat the missing callbacks
 * as "not interested".
 */
#include <stddef.h>

#include "../../include/sfwc-plugin.h"

static bool on_init(struct sfwc_host *host)
{
    host->log(host, 2, "init of a plugin with a short callback table");
    return true;
}

static void on_unmap(struct sfwc_host *host, struct sfwc_toplevel *t)
{
    host->log(host, 2, "window unmapped (short table)");
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = offsetof(struct sfwc_plugin, toplevel_map), /* up to and including toplevel_unmap */
    .name = "oldstyle",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .toplevel_unmap = on_unmap,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
