/* Test plugin whose callback table is too small to be one: the compositor must refuse it. */
#include <stddef.h>

#include "../../include/sfwc-plugin.h"

static bool on_init(struct sfwc_host *host)
{
    host->log(host, 2, "init was called although the table is too small");
    return true;
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .struct_size = offsetof(struct sfwc_plugin, toplevel_unmap),
    .name = "tinyplugin",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
