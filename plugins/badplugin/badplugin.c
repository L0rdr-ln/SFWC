/* Test plugin that claims another API version: the compositor must refuse it. */
#include "../../include/sfwc-plugin.h"

static bool on_init(struct sfwc_host *host)
{
    host->log(host, 2, "init was called although the API version is wrong");
    return true;
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION + 1,
    .name = "badplugin",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void)
{
    return &plugin;
}
