/* Config and theme loading, live reload, theme templates, spawning commands. */
#include "server.h"

static void load_theme_file(struct server *server);

/* ---------------------------------------------------------------- spawn */

/* Run a shell command detached: double fork so we never leave zombies. */
/* $terminal, $theme, $runtime ... and @colors.background:hex@ style theme placeholders (the
 * latter are single-quoted for the shell, see TEMPLATE_SHELL). */
char *expand_command(struct server *server, const char *in)
{
    char *a = config_expand(&server->config, in, getenv("XDG_RUNTIME_DIR"));
    char *b = template_render(&server->theme, a, TEMPLATE_SHELL, NULL, 0);
    if (!b) {
        return a;
    }
    free(a);
    return b;
}

void spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "fork failed");
        return;
    }
    if (pid == 0) {
        setsid();
        pid_t pid2 = fork();
        if (pid2 == 0) {
            execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(pid2 < 0 ? 1 : 0);
    }
    waitpid(pid, NULL, 0);
}

/* --------------------------------------------------------------- config */
static void config_log_cb(int level, int line, const char *msg, void *data)
{
    struct server *server = data;
    if (level == CONFIG_ERROR) {
        wlr_log(WLR_ERROR, "config %s:%d: %s", server->config_path, line, msg);
    } else {
        wlr_log(WLR_INFO, "config %s:%d: warning: %s", server->config_path, line, msg);
    }
}

struct theme_log_ctx {
    const char *path;
};

static void theme_log_cb(int level, int line, const char *msg, void *data)
{
    struct theme_log_ctx *ctx = data;
    if (level == INI_ERROR) {
        wlr_log(WLR_ERROR, "theme %s:%d: %s", ctx->path, line, msg);
    } else {
        wlr_log(WLR_INFO, "theme %s:%d: warning: %s", ctx->path, line, msg);
    }
}

/* Render the theme's templates for the companion tools (waybar, fuzzel, ...) with the
 * sfwc-theme-apply helper that is installed next to the compositor. Runs to completion
 * (it takes a few milliseconds) so that autostarted tools see current files. */
static void run_theme_apply(struct server *server)
{
    const char *off = getenv("SFWC_NO_THEME_APPLY");
    if (!server->config.templates_enabled || (off && *off == '1') || !getenv("XDG_RUNTIME_DIR")) {
        return;
    }
    char *argv[4 + 2 * 33 + 1];
    int n = 0;
    char self[1024], helper[1100];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    const char *prog = "sfwc-theme-apply";
    if (len > 0) {
        self[len] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(helper, sizeof helper, "%s/sfwc-theme-apply", self);
            if (access(helper, X_OK) == 0) {
                prog = helper;
            }
        }
    }
    char *config_dir = server->config_path ? strdup(server->config_path) : NULL;
    if (config_dir) {
        char *slash = strrchr(config_dir, '/');
        if (slash) {
            *slash = 0;
        }
    }
    argv[n++] = (char *)prog;
    argv[n++] = "--quiet";
    argv[n++] = "--theme";
    argv[n++] = server->config.theme;
    if (config_dir) {
        argv[n++] = "--config-dir";
        argv[n++] = config_dir;
    }
    for (size_t i = 0; i < server->config.n_templates_off && n < 4 + 2 * 32; i++) {
        argv[n++] = "--skip";
        argv[n++] = server->config.templates_off[i];
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            wlr_log(WLR_DEBUG, "sfwc-theme-apply is not installed, themes only style the windows");
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            wlr_log(WLR_ERROR, "sfwc-theme-apply reported a problem (see its messages above)");
        } else {
            wlr_log(WLR_INFO, "theme templates rendered");
        }
    }
    free(config_dir);
}

/* Load the theme named in the config and render the templates for the companion tools. */
static void load_theme(struct server *server)
{
    load_theme_file(server);
    run_theme_apply(server);
}

/* Load the theme named in the config (the built-in default if there is no such file). */
static void load_theme_file(struct server *server)
{
    if (server->theme.name) {
        theme_finish(&server->theme);
    }
    theme_init_default(&server->theme);
    server->theme_gen++;

    char *config_dir = NULL;
    if (server->config_path) {
        config_dir = strdup(server->config_path);
        char *slash = strrchr(config_dir, '/');
        if (slash) {
            *slash = '\0';
        }
    }
    char *path = theme_find(server->config.theme, config_dir);
    free(config_dir);
    if (!path) {
        if (strcmp(server->config.theme, "default") != 0) {
            wlr_log(WLR_ERROR, "theme '%s' not found, using the built-in default theme",
                    server->config.theme);
        }
        return;
    }
    struct theme_log_ctx ctx = {path};
    if (theme_load_file(&server->theme, path, theme_log_cb, &ctx)) {
        wlr_log(WLR_INFO, "theme loaded: %s", path);
    } else {
        wlr_log(WLR_ERROR, "cannot read theme %s, using the built-in default theme", path);
    }
    free(path);
}

/* Re-read the config file. A file that cannot be read keeps the current config; invalid
 * entries inside a readable file are reported and fall back to their defaults. */
void reload_config(struct server *server)
{
    if (!server->config_path) {
        wlr_log(WLR_INFO, "no config file location known, nothing to reload");
        return;
    }
    struct config fresh;
    config_init_defaults(&fresh);
    if (!config_load_file(&fresh, server->config_path, config_log_cb, server)) {
        wlr_log(WLR_ERROR, "cannot read %s, keeping the current configuration",
                server->config_path);
        config_finish(&fresh);
        return;
    }
    config_finish(&server->config);
    server->config = fresh;
    wlr_log(WLR_INFO, "config loaded: %s", server->config_path);

    struct keyboard *kb;
    wl_list_for_each(kb, &server->keyboards, link) {
        apply_keyboard_config(server, kb, !kb->is_virtual);
    }
    /* scale and position of outputs that are already running */
    struct output *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!wlr_output_layout_get(server->output_layout, out->wlr_output)) {
            continue; /* disabled at startup: `enabled` is only read when the output appears */
        }
        const struct output_cfg *oc = config_find_output(&server->config, out->wlr_output->name);
        double want = oc && oc->scale > 0 ? oc->scale : 1.0;
        if (fabs(out->wlr_output->scale - want) > 0.001) {
            struct wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_scale(&state, want);
            if (!wlr_output_commit_state(out->wlr_output, &state)) {
                wlr_log(WLR_ERROR, "cannot set scale %.2f on %s", want, out->wlr_output->name);
            }
            wlr_output_state_finish(&state);
        }
        if (oc && oc->has_pos) {
            wlr_output_layout_add(server->output_layout, out->wlr_output, oc->x, oc->y);
        }
    }

    load_theme(server);
    plugins_reload(server);
    workspace_clamp(server);
    wl_list_for_each(out, &server->outputs, link) {
        arrange_layers(out); /* output positions may have changed */
    }

    /* windows that are fitted to the output depend on the gap and on the frame */
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        decoration_apply(t);
        if (t->maximized || t->fullscreen) {
            toplevel_apply_state(t, t->maximized, t->fullscreen);
        }
        frame_refresh(t);
    }
}

/* Editors save by writing a temp file and renaming it, so watch the directory. */
static int handle_config_event(int fd, uint32_t mask, void *data)
{
    struct server *server = data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && strcmp(ev->name, server->config_name) == 0) {
                changed = true;
            }
            p += sizeof(*ev) + ev->len;
        }
    }
    if (changed) {
        reload_config(server);
    }
    return 0;
}

void init_config(struct server *server, struct wl_event_loop *loop)
{
    config_init_defaults(&server->config);
    server->inotify_fd = -1;
    server->config_path = config_default_path();
    if (!server->config_path) {
        wlr_log(WLR_INFO, "no HOME/XDG_CONFIG_HOME, using built-in defaults");
        load_theme(server);
        return;
    }
    if (config_load_file(&server->config, server->config_path, config_log_cb, server)) {
        wlr_log(WLR_INFO, "config loaded: %s", server->config_path);
    } else {
        wlr_log(WLR_INFO, "no config file at %s, using built-in defaults", server->config_path);
    }
    load_theme(server);

    char *dir = strdup(server->config_path);
    char *slash = strrchr(dir, '/');
    if (slash == dir) {
        dir[1] = '\0'; /* "/sfwc.conf" -> watch "/" */
        server->config_name = strdup(server->config_path + 1);
    } else if (slash) {
        *slash = '\0';
        server->config_name = strdup(slash + 1);
    } else {
        strcpy(dir, ".");
        server->config_name = strdup(server->config_path);
    }
    const char *no_watch = getenv("SFWC_NO_CONFIG_WATCH"); /* testing aid */
    int fd = no_watch && !strcmp(no_watch, "1") ? -1 : inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO) >= 0) {
        server->inotify_fd = fd;
        server->inotify_source =
            wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, handle_config_event, server);
    } else {
        wlr_log(WLR_INFO, "cannot watch %s for changes; use the reload-config action", dir);
        if (fd >= 0) {
            close(fd);
        }
    }
    free(dir);
}
