/*
 * Plugin host: loads the shared objects named in the config and gives them the functions of
 * include/sfwc-plugin.h. See docs/PLUGINS.md.
 */
#include "server.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "../include/sfwc-plugin.h"

#ifndef SFWC_PLUGIN_DIR
#define SFWC_PLUGIN_DIR "/usr/local/lib/sfwc/plugins"
#endif

struct plugin_inst {
    struct wl_list link;
    struct server *server;
    char *name;
    void *handle;
    struct sfwc_plugin vt; /* the plugin's table, zero padded to the size this host knows */
    struct sfwc_host host;
};

static struct server *host_server(struct sfwc_host *host)
{
    return ((struct plugin_inst *)host->priv)->server;
}

/* ----------------------------------------------------------- host functions */

static void host_cursor_position(struct sfwc_host *host, double *x, double *y)
{
    struct server *server = host_server(host);
    *x = server->cursor->x;
    *y = server->cursor->y;
}

static const char *host_config_get(struct sfwc_host *host, const char *key)
{
    struct plugin_inst *inst = host->priv;
    return config_plugin_get(&inst->server->config, inst->name, key);
}

static void host_request_frame(struct sfwc_host *host)
{
    struct output *o;
    wl_list_for_each(o, &host_server(host)->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

static struct sfwc_toplevel *host_toplevel_next(struct sfwc_host *host, struct sfwc_toplevel *prev)
{
    struct server *server = host_server(host);
    struct wl_list *pos = prev ? ((struct toplevel *)prev)->link.next : server->toplevels.next;
    /* the list only holds mapped windows (see toplevel_map / toplevel_unmap) */
    if (pos == &server->toplevels) {
        return NULL;
    }
    struct toplevel *next = wl_container_of(pos, next, link);
    return (struct sfwc_toplevel *)next;
}

static bool host_toplevel_info(struct sfwc_host *host, const struct sfwc_toplevel *opaque,
                               struct sfwc_toplevel_info *info)
{
    struct server *server = host_server(host);
    const struct toplevel *t = (const struct toplevel *)opaque;
    if (!t->mapped) {
        return false;
    }
    struct toplevel *mt = (struct toplevel *)t;
    memset(info, 0, sizeof *info);
    info->size = sizeof *info;
    info->outer = toplevel_outer(mt);
    info->tree = t->scene_tree;
    info->shadow = t->shadow;
    info->frame = t->chrome;
    struct deco_insets in = toplevel_insets(mt);
    info->inner = (struct wlr_box){
        info->outer.x + in.left, info->outer.y + in.top,
        info->outer.width - in.left - in.right, info->outer.height - in.top - in.bottom};
    info->mapped = true;
    info->visible = toplevel_shown(t) && t->scene_tree->node.enabled;
    info->maximized = t->maximized;
    info->fullscreen = t->fullscreen;
    info->moving = server->cursor_mode == CURSOR_MOVE && server->grabbed_toplevel == t;
    info->busy = t->plugin_busy > 0;
    info->minimized = t->minimized;
    info->shown = toplevel_shown(t);
    info->workspace = t->ws;
    return true;
}

static size_t host_config_count(struct sfwc_host *host)
{
    struct plugin_inst *inst = host->priv;
    const struct plugin_cfg *pc = config_plugin_section(&inst->server->config, inst->name);
    return pc ? pc->n : 0;
}

static bool host_config_entry(struct sfwc_host *host, size_t i, const char **key, const char **value, int *line)
{
    struct plugin_inst *inst = host->priv;
    const struct plugin_cfg *pc = config_plugin_section(&inst->server->config, inst->name);
    if (!pc || i >= pc->n) {
        return false;
    }
    *key = pc->keys[i];
    *value = pc->values[i];
    *line = pc->lines[i];
    return true;
}

static void host_output_box_at(struct sfwc_host *host, double x, double y, struct wlr_box *out)
{
    *out = output_box_at(host_server(host), x, y);
}

static int host_current_workspace(struct sfwc_host *host)
{
    return host_server(host)->ws_current;
}

static uint32_t host_now_ms(struct sfwc_host *host)
{
    return now_msec();
}

static void host_toplevel_busy(struct sfwc_host *host, struct sfwc_toplevel *opaque, int delta)
{
    struct toplevel *t = (struct toplevel *)opaque;
    t->plugin_busy += delta;
    if (t->plugin_busy < 0) {
        t->plugin_busy = 0;
    }
}

static void host_toplevel_set_focus_mix(struct sfwc_host *host, struct sfwc_toplevel *opaque, double mix)
{
    struct toplevel *t = (struct toplevel *)opaque;
    t->focus_mix = mix < 0 ? 0 : mix > 1 ? 1 : mix;
    frame_refresh(t);
}

static void host_log(struct sfwc_host *host, int level, const char *fmt, ...)
{
    struct plugin_inst *inst = host->priv;
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    enum wlr_log_importance imp = level <= 1 ? WLR_ERROR : level == 2 ? WLR_INFO : WLR_DEBUG;
    wlr_log(imp, "plugin %s: %s", inst->name, text);
}

/* ----------------------------------------------------------------- loading */

/* NAME.so in the first directory of the search path that has it. Caller frees. */
static char *find_plugin(const char *name)
{
    char *dirs[3] = {NULL, NULL, NULL};
    const char *env = getenv("SFWC_PLUGIN_PATH");
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char user[512] = "";
    if (xdg && *xdg) {
        snprintf(user, sizeof user, "%s/sfwc/plugins", xdg);
    } else if (home && *home) {
        snprintf(user, sizeof user, "%s/.local/share/sfwc/plugins", home);
    }
    char *path_copy = env && *env ? strdup(env) : NULL;
    char *found = NULL;
    /* SFWC_PLUGIN_PATH entries, then the user's directory, then the install directory */
    char *save = NULL;
    for (char *d = path_copy ? strtok_r(path_copy, ":", &save) : NULL; d && !found; d = strtok_r(NULL, ":", &save)) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s.so", d, name) < (int)sizeof p && access(p, R_OK) == 0) {
            found = strdup(p);
        }
    }
    free(path_copy);
    dirs[0] = user[0] ? user : NULL;
    dirs[1] = SFWC_PLUGIN_DIR;
    for (int i = 0; i < 2 && !found; i++) {
        char p[1024];
        if (dirs[i] && snprintf(p, sizeof p, "%s/%s.so", dirs[i], name) < (int)sizeof p && access(p, R_OK) == 0) {
            found = strdup(p);
        }
    }
    return found;
}

static void inst_free(struct plugin_inst *inst)
{
    wl_list_remove(&inst->link);
    if (inst->vt.fini) {
        inst->vt.fini(&inst->host);
    }
    dlclose(inst->handle);
    free(inst->name);
    free(inst);
}

static void plugin_load(struct server *server, const char *name)
{
    char *path = find_plugin(name);
    if (!path) {
        wlr_log(WLR_ERROR, "plugin %s: %s.so not found (looked in $SFWC_PLUGIN_PATH, the user's data "
                           "directory and %s)", name, name, SFWC_PLUGIN_DIR);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & S_IWOTH)) {
        wlr_log(WLR_ERROR, "plugin %s: %s is writable by everyone, not loading it", name, path);
        free(path);
        return;
    }
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        wlr_log(WLR_ERROR, "plugin %s: cannot load %s: %s", name, path, dlerror());
        free(path);
        return;
    }
    sfwc_plugin_entry_fn entry = (sfwc_plugin_entry_fn)dlsym(handle, SFWC_PLUGIN_ENTRY_SYMBOL);
    const struct sfwc_plugin *p = entry ? entry() : NULL;
    const char *why = NULL;
    if (!p) {
        why = "it has no sfwc_plugin_entry()";
    } else if (p->api_version != SFWC_PLUGIN_API_VERSION) {
        why = "it was written for another plugin API version";
    } else if (!p->name || strcmp(p->name, name) != 0) {
        why = "the name inside does not match the file name";
    } else if (!p->wlroots_version || strcmp(p->wlroots_version, WLR_VERSION_STR) != 0) {
        why = "it was built against another wlroots version";
    } else if (p->struct_size < offsetof(struct sfwc_plugin, toplevel_unmap) + sizeof p->toplevel_unmap) {
        why = "its callback table is too small (struct_size)";
    } else if (!p->init) {
        why = "it has no init()";
    }
    if (why) {
        wlr_log(WLR_ERROR, "plugin %s: not loading %s: %s", name, path, why);
        dlclose(handle);
        free(path);
        return;
    }
    struct plugin_inst *inst = calloc(1, sizeof *inst);
    if (!inst) {
        dlclose(handle);
        free(path);
        return;
    }
    inst->server = server;
    inst->name = strdup(name);
    inst->handle = handle;
    memcpy(&inst->vt, p, p->struct_size < sizeof inst->vt ? p->struct_size : sizeof inst->vt);
    inst->host = (struct sfwc_host){
        .api_version = SFWC_PLUGIN_API_VERSION,
        .struct_size = sizeof(struct sfwc_host),
        .priv = inst,
        .windows_tree = server->windows_tree,
        .cursor_position = host_cursor_position,
        .config_get = host_config_get,
        .request_frame = host_request_frame,
        .toplevel_next = host_toplevel_next,
        .toplevel_info = host_toplevel_info,
        .log = host_log,
        .config_count = host_config_count,
        .config_entry = host_config_entry,
        .output_box_at = host_output_box_at,
        .current_workspace = host_current_workspace,
        .now_ms = host_now_ms,
        .toplevel_busy = host_toplevel_busy,
        .toplevel_set_focus_mix = host_toplevel_set_focus_mix,
    };
    wl_list_insert(server->plugins.prev, &inst->link);
    if (!inst->vt.init(&inst->host)) {
        wlr_log(WLR_ERROR, "plugin %s: init failed, unloading it", name);
        wl_list_remove(&inst->link);
        dlclose(handle);
        free(inst->name);
        free(inst);
        free(path);
        return;
    }
    wlr_log(WLR_INFO, "plugin %s loaded from %s", name, path);
    free(path);
}

static struct plugin_inst *find_inst(struct server *server, const char *name)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &server->plugins, link) {
        if (!strcmp(inst->name, name)) {
            return inst;
        }
    }
    return NULL;
}

/* Make the loaded plugins match the config: start, stop and tell the others to re-read. */
void plugins_reload(struct server *server)
{
    const struct config *c = &server->config;
    struct plugin_inst *inst, *tmp;
    wl_list_for_each_safe(inst, tmp, &server->plugins, link) {
        bool wanted = false;
        for (size_t i = 0; i < c->n_plugins; i++) {
            wanted = wanted || !strcmp(c->plugins[i], inst->name);
        }
        if (!wanted) {
            wlr_log(WLR_INFO, "plugin %s unloaded", inst->name);
            inst_free(inst);
        }
    }
    for (size_t i = 0; i < c->n_plugins; i++) {
        inst = find_inst(server, c->plugins[i]);
        if (!inst) {
            plugin_load(server, c->plugins[i]);
        } else if (inst->vt.reconfigure) {
            inst->vt.reconfigure(&inst->host);
        }
    }
}

void plugins_frame(struct server *server, uint32_t now)
{
    struct plugin_inst *inst, *tmp;
    wl_list_for_each_safe(inst, tmp, &server->plugins, link) {
        if (inst->vt.frame) {
            inst->vt.frame(&inst->host, now);
        }
    }
}

/* The callbacks that take a window and return nothing. */
#define WINDOW_HOOK(fn)                                                                            \
    void plugins_##fn(struct toplevel *t)                                                          \
    {                                                                                              \
        struct plugin_inst *inst;                                                                  \
        wl_list_for_each(inst, &t->server->plugins, link) {                                        \
            if (inst->vt.fn) {                                                                     \
                inst->vt.fn(&inst->host, (struct sfwc_toplevel *)t);                               \
            }                                                                                      \
        }                                                                                          \
    }

WINDOW_HOOK(toplevel_map)
WINDOW_HOOK(toplevel_commit)
WINDOW_HOOK(toplevel_cancel)

void plugins_toplevel_unmap(struct toplevel *t)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &t->server->plugins, link) {
        if (inst->vt.toplevel_unmap) {
            inst->vt.toplevel_unmap(&inst->host, (struct sfwc_toplevel *)t);
        }
    }
    t->plugin_busy = 0;
}

/* The first plugin that wants to animate the move gets it. */
bool plugins_toplevel_move(struct toplevel *t, double from_x, double from_y, double to_x, double to_y)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &t->server->plugins, link) {
        if (inst->vt.toplevel_move &&
            inst->vt.toplevel_move(&inst->host, (struct sfwc_toplevel *)t, from_x, from_y, to_x, to_y)) {
            return true;
        }
    }
    return false;
}

bool plugins_toplevel_focus(struct toplevel *t, double from, double to)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &t->server->plugins, link) {
        if (inst->vt.toplevel_focus && inst->vt.toplevel_focus(&inst->host, (struct sfwc_toplevel *)t, from, to)) {
            return true;
        }
    }
    return false;
}

void plugins_workspace_leaving(struct server *server, int old_ws, int new_ws)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &server->plugins, link) {
        if (inst->vt.workspace_leaving) {
            inst->vt.workspace_leaving(&inst->host, old_ws, new_ws);
        }
    }
}

void plugins_workspace_entered(struct server *server, int old_ws, int new_ws)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &server->plugins, link) {
        if (inst->vt.workspace_entered) {
            inst->vt.workspace_entered(&inst->host, old_ws, new_ws);
        }
    }
}

void plugins_layer_map(struct server *server, struct wlr_scene_tree *tree)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &server->plugins, link) {
        if (inst->vt.layer_map) {
            inst->vt.layer_map(&inst->host, tree);
        }
    }
}

void plugins_layer_unmap(struct server *server, struct wlr_scene_tree *tree)
{
    struct plugin_inst *inst;
    wl_list_for_each(inst, &server->plugins, link) {
        if (inst->vt.layer_unmap) {
            inst->vt.layer_unmap(&inst->host, tree);
        }
    }
}

void plugins_finish(struct server *server)
{
    struct plugin_inst *inst, *tmp;
    wl_list_for_each_safe(inst, tmp, &server->plugins, link) {
        inst_free(inst);
    }
}
