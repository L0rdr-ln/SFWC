#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <ctype.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>

#include "inifile.h"

/* ----------------------------------------------------------- helpers */

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        abort();
    }
    return d;
}

static void *xrealloc(void *p, size_t n)
{
    void *r = realloc(p, n);
    if (!r) {
        abort();
    }
    return r;
}

static const struct {
    const char *name;
    enum action action;
} action_names[] = {
    {"close", ACTION_CLOSE},
    {"toggle-maximize", ACTION_TOGGLE_MAXIMIZE},
    {"toggle-fullscreen", ACTION_TOGGLE_FULLSCREEN},
    {"minimize", ACTION_MINIMIZE},
    {"restore-minimized", ACTION_RESTORE},
    {"cycle-windows", ACTION_CYCLE},
    {"reload-config", ACTION_RELOAD},
    {"quit", ACTION_QUIT},
    {"move-to-next-output", ACTION_MOVE_OUTPUT},
    {"focus-next-output", ACTION_FOCUS_OUTPUT},
    {"move", ACTION_MOVE},
    {"resize", ACTION_RESIZE},
};

const char *action_name(enum action a)
{
    if (a == ACTION_SPAWN) {
        return "spawn";
    }
    for (size_t i = 0; i < sizeof action_names / sizeof *action_names; i++) {
        if (action_names[i].action == a) {
            return action_names[i].name;
        }
    }
    return "?";
}

/* ---------------------------------------------------------- defaults */

static void add_keybind(struct config *c, uint32_t mods, const char *key, enum action action,
                        const char *arg)
{
    xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
    c->binds = xrealloc(c->binds, (c->n_binds + 1) * sizeof *c->binds);
    c->binds[c->n_binds++] = (struct keybind){
        .mods = mods,
        .sym = xkb_keysym_to_lower(sym),
        .action = action,
        .arg = arg ? xstrdup(arg) : NULL,
    };
}

static void add_mousebind(struct config *c, uint32_t mods, uint32_t button, enum action action)
{
    c->mbinds = xrealloc(c->mbinds, (c->n_mbinds + 1) * sizeof *c->mbinds);
    c->mbinds[c->n_mbinds++] = (struct mousebind){mods, button, action};
}

static void clear_binds(struct config *c)
{
    for (size_t i = 0; i < c->n_binds; i++) {
        free(c->binds[i].arg);
    }
    free(c->binds);
    c->binds = NULL;
    c->n_binds = 0;
}

static void clear_mbinds(struct config *c)
{
    free(c->mbinds);
    c->mbinds = NULL;
    c->n_mbinds = 0;
}

static void clear_autostart(struct config *c)
{
    for (size_t i = 0; i < c->n_autostart; i++) {
        free(c->autostart[i]);
    }
    free(c->autostart);
    c->autostart = NULL;
    c->n_autostart = 0;
}

static void install_default_keybinds(struct config *c);
static void install_default_mousebinds(struct config *c);

void config_init_defaults(struct config *c)
{
    memset(c, 0, sizeof *c);
    const char *term = getenv("SFWC_TERMINAL");
    c->theme = xstrdup("default");
    c->terminal = xstrdup(term && *term ? term : "foot");
    c->mod = CFG_MOD_ALT;
    c->focus = FOCUS_CLICK;
    c->snap_to_edges = true;
    c->snap_to_windows = true;
    c->decorations = true;
    c->repeat_rate = 25;
    c->repeat_delay = 600;
    c->snap_distance = 12;
    c->gap = 8;
    c->anim_enabled = true;
    strcpy(c->anim_open, "fade-scale");
    strcpy(c->anim_close, "fade");
    strcpy(c->anim_easing, "ease-out");
    c->anim_move = c->anim_resize = true;
    c->anim_duration_ms = 180;

    install_default_keybinds(c);
    install_default_mousebinds(c);
}

static void install_default_keybinds(struct config *c)
{
    clear_binds(c);
    uint32_t m = c->mod;
    add_keybind(c, m, "Return", ACTION_SPAWN, "$terminal");
    add_keybind(c, m, "q", ACTION_CLOSE, NULL);
    add_keybind(c, m, "Tab", ACTION_CYCLE, NULL);
    add_keybind(c, m, "f", ACTION_TOGGLE_MAXIMIZE, NULL);
    add_keybind(c, m, "F11", ACTION_TOGGLE_FULLSCREEN, NULL);
    add_keybind(c, m, "m", ACTION_MINIMIZE, NULL);
    add_keybind(c, m | CFG_MOD_SHIFT, "m", ACTION_RESTORE, NULL);
    add_keybind(c, m | CFG_MOD_SHIFT, "r", ACTION_RELOAD, NULL);
    add_keybind(c, m, "o", ACTION_MOVE_OUTPUT, NULL);
    add_keybind(c, m | CFG_MOD_SHIFT, "o", ACTION_FOCUS_OUTPUT, NULL);
    add_keybind(c, m, "Escape", ACTION_QUIT, NULL);
}

static void install_default_mousebinds(struct config *c)
{
    clear_mbinds(c);
    uint32_t m = c->mod;
    add_mousebind(c, m, BTN_LEFT, ACTION_MOVE);
    add_mousebind(c, m, BTN_RIGHT, ACTION_RESIZE);
}

void config_finish(struct config *c)
{
    free(c->theme);
    free(c->terminal);
    free(c->kb_rules);
    free(c->kb_model);
    free(c->kb_layout);
    free(c->kb_variant);
    free(c->kb_options);
    for (size_t i = 0; i < c->n_outputs; i++) {
        free(c->outputs[i].name);
    }
    free(c->outputs);
    clear_binds(c);
    clear_mbinds(c);
    clear_autostart(c);
    memset(c, 0, sizeof *c);
}

/* ----------------------------------------------------------- parsing */

struct loader {
    struct config *c;
    config_log_fn log;
    void *log_data;
    const int *line; /* current line, kept up to date by the reader */
    bool binds_replaced, mbinds_replaced, autostart_replaced;
    char unknown_section[64]; /* last unknown section we warned about */
};

static void report(struct loader *l, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void report(struct loader *l, int level, const char *fmt, ...)
{
    if (!l->log) {
        return;
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    l->log(level, *l->line, msg, l->log_data);
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, int min, int max, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < min || n > max) {
        return false;
    }
    *out = (int)n;
    return true;
}

static bool in_list(const char *v, const char *const *list)
{
    for (; *list; list++) {
        if (!strcmp(v, *list)) {
            return true;
        }
    }
    return false;
}

static void set_bool(struct loader *l, const char *key, const char *v, bool *out)
{
    if (!parse_bool(v, out)) {
        report(l, CONFIG_ERROR, "%s: '%s' is not a boolean (use true/false)", key, v);
    }
}

static void set_int(struct loader *l, const char *key, const char *v, int min, int max, int *out)
{
    if (!parse_int(v, min, max, out)) {
        report(l, CONFIG_ERROR, "%s: '%s' is not a number between %d and %d", key, v, min, max);
    }
}

static void set_choice(struct loader *l, const char *key, const char *v, const char *const *list,
                       char *out, size_t out_size)
{
    if (!in_list(v, list)) {
        char opts[128] = "";
        for (const char *const *o = list; *o; o++) {
            strncat(opts, *o, sizeof opts - strlen(opts) - 1);
            if (o[1]) {
                strncat(opts, " | ", sizeof opts - strlen(opts) - 1);
            }
        }
        report(l, CONFIG_ERROR, "%s: '%s' is not valid (%s)", key, v, opts);
        return;
    }
    snprintf(out, out_size, "%s", v);
}

/* "Alt+Shift+m" -> mods + key name. `key` receives the last token. */
static bool parse_combo(struct loader *l, const char *spec, uint32_t *mods, char *key,
                        size_t key_size)
{
    char buf[128];
    if (strlen(spec) >= sizeof buf) {
        report(l, CONFIG_ERROR, "binding '%.40s...' is too long", spec);
        return false;
    }
    strcpy(buf, spec);
    *mods = 0;
    key[0] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        while (isspace((unsigned char)*tok)) {
            tok++;
        }
        size_t n = strlen(tok);
        while (n && isspace((unsigned char)tok[n - 1])) {
            tok[--n] = '\0';
        }
        if (!n) {
            continue;
        }
        if (key[0]) { /* the previous token was a modifier after all */
            char *prev = key;
            uint32_t m = 0;
            if (!strcasecmp(prev, "shift")) {
                m = CFG_MOD_SHIFT;
            } else if (!strcasecmp(prev, "ctrl") || !strcasecmp(prev, "control")) {
                m = CFG_MOD_CTRL;
            } else if (!strcasecmp(prev, "alt") || !strcasecmp(prev, "mod1")) {
                m = CFG_MOD_ALT;
            } else if (!strcasecmp(prev, "super") || !strcasecmp(prev, "logo") ||
                       !strcasecmp(prev, "mod4") || !strcasecmp(prev, "win")) {
                m = CFG_MOD_LOGO;
            } else if (!strcasecmp(prev, "mod") || !strcasecmp(prev, "$mod")) {
                m = l->c->mod;
            } else {
                report(l, CONFIG_ERROR, "unknown modifier '%s' in '%s'", prev, spec);
                return false;
            }
            *mods |= m;
        }
        snprintf(key, key_size, "%s", tok);
    }
    if (!key[0]) {
        report(l, CONFIG_ERROR, "binding '%s' has no key", spec);
        return false;
    }
    return true;
}

static bool parse_action(struct loader *l, const char *value, bool mouse, enum action *action,
                         char **arg)
{
    *arg = NULL;
    if (!strncmp(value, "spawn:", 6)) {
        const char *cmd = value + 6;
        while (isspace((unsigned char)*cmd)) {
            cmd++;
        }
        if (!*cmd) {
            report(l, CONFIG_ERROR, "'spawn:' needs a command");
            return false;
        }
        if (mouse) {
            report(l, CONFIG_ERROR, "mouse bindings only support move and resize");
            return false;
        }
        *action = ACTION_SPAWN;
        *arg = xstrdup(cmd);
        return true;
    }
    for (size_t i = 0; i < sizeof action_names / sizeof *action_names; i++) {
        if (!strcmp(value, action_names[i].name)) {
            bool is_mouse_action =
                action_names[i].action == ACTION_MOVE || action_names[i].action == ACTION_RESIZE;
            if (is_mouse_action != mouse) {
                report(l, CONFIG_ERROR, "action '%s' cannot be used in this section", value);
                return false;
            }
            *action = action_names[i].action;
            return true;
        }
    }
    report(l, CONFIG_ERROR, "unknown action '%s'", value);
    return false;
}

static void handle_keybind(struct loader *l, const char *name, const char *value)
{
    uint32_t mods;
    char key[64];
    enum action action;
    char *arg;
    if (!parse_combo(l, name, &mods, key, sizeof key) || !parse_action(l, value, false, &action, &arg)) {
        return;
    }
    xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) {
        report(l, CONFIG_ERROR, "unknown key name '%s' (see xkbcommon-keysyms.h)", key);
        free(arg);
        return;
    }
    if (!l->binds_replaced) {
        clear_binds(l->c);
        l->binds_replaced = true;
    }
    struct config *c = l->c;
    c->binds = xrealloc(c->binds, (c->n_binds + 1) * sizeof *c->binds);
    c->binds[c->n_binds++] =
        (struct keybind){.mods = mods, .sym = xkb_keysym_to_lower(sym), .action = action, .arg = arg};
}

static void handle_mousebind(struct loader *l, const char *name, const char *value)
{
    uint32_t mods;
    char key[64];
    enum action action;
    char *arg;
    if (!parse_combo(l, name, &mods, key, sizeof key) || !parse_action(l, value, true, &action, &arg)) {
        return;
    }
    uint32_t button;
    if (!strcasecmp(key, "left")) {
        button = BTN_LEFT;
    } else if (!strcasecmp(key, "right")) {
        button = BTN_RIGHT;
    } else if (!strcasecmp(key, "middle")) {
        button = BTN_MIDDLE;
    } else {
        report(l, CONFIG_ERROR, "unknown mouse button '%s' (Left, Right or Middle)", key);
        return;
    }
    if (!l->mbinds_replaced) {
        clear_mbinds(l->c);
        l->mbinds_replaced = true;
    }
    add_mousebind(l->c, mods, button, action);
}

static const char *const focus_values[] = {"click", "follow-mouse", NULL};
static const char *const open_values[] = {"none", "fade", "fade-scale", "slide", NULL};
static const char *const easing_values[] = {"linear", "ease-in", "ease-out", "ease-in-out", NULL};
static const char *const layout_values[] = {"floating", NULL};

static void handle_general(struct loader *l, const char *key, const char *v)
{
    struct config *c = l->c;
    if (!strcmp(key, "theme")) {
        free(c->theme);
        c->theme = xstrdup(v);
    } else if (!strcmp(key, "terminal")) {
        free(c->terminal);
        c->terminal = xstrdup(v);
    } else if (!strcmp(key, "focus")) {
        char tmp[24];
        set_choice(l, key, v, focus_values, tmp, sizeof tmp);
        if (in_list(v, focus_values)) {
            c->focus = !strcmp(v, "follow-mouse") ? FOCUS_FOLLOW_MOUSE : FOCUS_CLICK;
        }
    } else if (!strcmp(key, "mod")) {
        /* validate the modifier name(s) by parsing "<mod>+x" */
        uint32_t mods;
        char k[64], spec[80];
        snprintf(spec, sizeof spec, "%.60s+x", v);
        if (parse_combo(l, spec, &mods, k, sizeof k)) {
            if (mods) {
                c->mod = mods;
            } else {
                report(l, CONFIG_ERROR, "mod: '%s' is not a modifier (Alt, Super, Ctrl, Shift)", v);
            }
        }
    } else {
        report(l, CONFIG_WARNING, "unknown key '%s' in [general]", key);
    }
}

static void handle_windows(struct loader *l, const char *key, const char *v)
{
    struct config *c = l->c;
    char tmp[24];
    if (!strcmp(key, "snap_to_edges")) {
        set_bool(l, key, v, &c->snap_to_edges);
    } else if (!strcmp(key, "decorations")) {
        set_bool(l, key, v, &c->decorations);
    } else if (!strcmp(key, "snap_to_windows")) {
        set_bool(l, key, v, &c->snap_to_windows);
    } else if (!strcmp(key, "snap_distance")) {
        set_int(l, key, v, 0, 200, &c->snap_distance);
    } else if (!strcmp(key, "gap")) {
        set_int(l, key, v, 0, 200, &c->gap);
    } else if (!strcmp(key, "default_layout")) {
        set_choice(l, key, v, layout_values, tmp, sizeof tmp);
    } else {
        report(l, CONFIG_WARNING, "unknown key '%s' in [windows]", key);
    }
}

static void set_string(char **dst, const char *v)
{
    free(*dst);
    *dst = *v ? xstrdup(v) : NULL; /* empty value = xkb default */
}

static void handle_keyboard(struct loader *l, const char *key, const char *v)
{
    struct config *c = l->c;
    if (!strcmp(key, "rules")) {
        set_string(&c->kb_rules, v);
    } else if (!strcmp(key, "model")) {
        set_string(&c->kb_model, v);
    } else if (!strcmp(key, "layout")) {
        set_string(&c->kb_layout, v);
    } else if (!strcmp(key, "variant")) {
        set_string(&c->kb_variant, v);
    } else if (!strcmp(key, "options")) {
        set_string(&c->kb_options, v);
    } else if (!strcmp(key, "repeat_rate")) {
        set_int(l, key, v, 0, 1000, &c->repeat_rate);
    } else if (!strcmp(key, "repeat_delay")) {
        set_int(l, key, v, 0, 10000, &c->repeat_delay);
    } else {
        report(l, CONFIG_WARNING, "unknown key '%s' in [keyboard]", key);
    }
}

static void handle_output(struct loader *l, const char *name, const char *key, const char *v)
{
    if (!*name) {
        report(l, CONFIG_ERROR, "section [output:] needs an output name, e.g. [output:HDMI-A-1]");
        return;
    }
    struct config *c = l->c;
    struct output_cfg *oc = NULL;
    for (size_t i = 0; i < c->n_outputs; i++) {
        if (!strcmp(c->outputs[i].name, name)) {
            oc = &c->outputs[i];
        }
    }
    if (!oc) {
        c->outputs = xrealloc(c->outputs, (c->n_outputs + 1) * sizeof *c->outputs);
        oc = &c->outputs[c->n_outputs++];
        *oc = (struct output_cfg){.name = xstrdup(name), .enabled = true};
    }
    if (!strcmp(key, "scale")) {
        char *end;
        double sc = strtod(v, &end);
        if (end == v || *end != '\0' || sc < 0.25 || sc > 10) {
            report(l, CONFIG_ERROR, "scale: '%s' is not a number between 0.25 and 10", v);
        } else {
            oc->scale = sc;
        }
    } else if (!strcmp(key, "position")) {
        int x, y;
        char extra;
        if (sscanf(v, " %d , %d %c", &x, &y, &extra) == 2) {
            oc->has_pos = true;
            oc->x = x;
            oc->y = y;
        } else {
            report(l, CONFIG_ERROR, "position: '%s' is not 'x,y' (e.g. 1920,0)", v);
        }
    } else if (!strcmp(key, "enabled")) {
        set_bool(l, key, v, &oc->enabled);
    } else {
        report(l, CONFIG_WARNING, "unknown key '%s' in [output:%s]", key, name);
    }
}

static void handle_animations(struct loader *l, const char *key, const char *v)
{
    struct config *c = l->c;
    if (!strcmp(key, "enabled")) {
        set_bool(l, key, v, &c->anim_enabled);
    } else if (!strcmp(key, "open")) {
        set_choice(l, key, v, open_values, c->anim_open, sizeof c->anim_open);
    } else if (!strcmp(key, "close")) {
        set_choice(l, key, v, open_values, c->anim_close, sizeof c->anim_close);
    } else if (!strcmp(key, "move")) {
        set_bool(l, key, v, &c->anim_move);
    } else if (!strcmp(key, "resize")) {
        set_bool(l, key, v, &c->anim_resize);
    } else if (!strcmp(key, "duration_ms")) {
        set_int(l, key, v, 0, 5000, &c->anim_duration_ms);
    } else if (!strcmp(key, "easing")) {
        set_choice(l, key, v, easing_values, c->anim_easing, sizeof c->anim_easing);
    } else {
        report(l, CONFIG_WARNING, "unknown key '%s' in [animations]", key);
    }
}

static int config_ini_cb(void *user, const char *section, const char *name, const char *value)
{
    struct loader *l = user;
    if (!strcmp(section, "general")) {
        handle_general(l, name, value);
    } else if (!strcmp(section, "windows")) {
        handle_windows(l, name, value);
    } else if (!strcmp(section, "animations")) {
        handle_animations(l, name, value);
    } else if (!strcmp(section, "keyboard")) {
        handle_keyboard(l, name, value);
    } else if (!strncmp(section, "output:", 7)) {
        handle_output(l, section + 7, name, value);
    } else if (!strcmp(section, "keybinds")) {
        handle_keybind(l, name, value);
    } else if (!strcmp(section, "mouse")) {
        handle_mousebind(l, name, value);
    } else if (!strcmp(section, "autostart")) {
        if (strcmp(name, "exec") != 0) {
            report(l, CONFIG_WARNING, "unknown key '%s' in [autostart] (use exec = <command>)", name);
        } else {
            if (!l->autostart_replaced) {
                clear_autostart(l->c);
                l->autostart_replaced = true;
            }
            l->c->autostart = xrealloc(l->c->autostart, (l->c->n_autostart + 1) * sizeof(char *));
            l->c->autostart[l->c->n_autostart++] = xstrdup(value);
        }
    } else if (!strcmp(section, "templates")) {
        /* theme templating arrives with the theme package; accept silently */
    } else if (strcmp(l->unknown_section, section) != 0) {
        snprintf(l->unknown_section, sizeof l->unknown_section, "%s", section);
        report(l, CONFIG_WARNING, "unknown section [%s] ignored", section);
    }
    return 1;
}

/* ------------------------------------------------------------ readers */

static bool run(struct config *c, struct inifile_reader *r, ini_reader fn, config_log_fn log, void *data)
{
    struct loader l = {.c = c, .log = log, .log_data = data, .line = &r->line};
    uint32_t mod_before = c->mod;
    int err = ini_parse_stream(fn, r, config_ini_cb, &l);
    if (c->mod != mod_before) { /* built-in binds follow the modifier */
        if (!l.binds_replaced) {
            install_default_keybinds(c);
        }
        if (!l.mbinds_replaced) {
            install_default_mousebinds(c);
        }
    }
    if (err > 0 && log) {
        log(CONFIG_ERROR, err, "syntax error (expected 'key = value' or '[section]')", data);
    }
    return true;
}

bool config_load_file(struct config *c, const char *path, config_log_fn log, void *data)
{
    struct inifile_reader r = {.fp = fopen(path, "r")};
    if (!r.fp) {
        return false;
    }
    run(c, &r, inifile_file_reader, log, data);
    fclose(r.fp);
    return true;
}

bool config_load_string(struct config *c, const char *text, config_log_fn log, void *data)
{
    struct inifile_reader r = {.text = text};
    return run(c, &r, inifile_string_reader, log, data);
}

/* ------------------------------------------------------------- lookup */

const struct keybind *config_find_keybind(const struct config *c, uint32_t mods, uint32_t sym)
{
    sym = xkb_keysym_to_lower(sym);
    mods &= CFG_MOD_MASK;
    for (size_t i = c->n_binds; i-- > 0;) { /* last definition wins */
        if (c->binds[i].sym == sym && c->binds[i].mods == mods) {
            return &c->binds[i];
        }
    }
    return NULL;
}

const struct output_cfg *config_find_output(const struct config *c, const char *name)
{
    for (size_t i = 0; i < c->n_outputs; i++) {
        if (!strcmp(c->outputs[i].name, name)) {
            return &c->outputs[i];
        }
    }
    return NULL;
}

const struct mousebind *config_find_mousebind(const struct config *c, uint32_t mods, uint32_t button)
{
    mods &= CFG_MOD_MASK;
    for (size_t i = c->n_mbinds; i-- > 0;) {
        if (c->mbinds[i].button == button && c->mbinds[i].mods == mods) {
            return &c->mbinds[i];
        }
    }
    return NULL;
}

char *config_expand(const struct config *c, const char *in, const char *runtime_dir)
{
    size_t cap = strlen(in) + 1;
    char *out = xrealloc(NULL, cap);
    size_t len = 0;
    for (const char *p = in; *p;) {
        const char *rep = NULL;
        size_t skip = 1;
        if (*p == '$') {
            if (!strncmp(p, "$terminal", 9)) {
                rep = c->terminal, skip = 9;
            } else if (!strncmp(p, "$theme", 6)) {
                rep = c->theme, skip = 6;
            } else if (!strncmp(p, "$runtime", 8)) {
                rep = runtime_dir ? runtime_dir : "", skip = 8;
            } else if (p[1] == '$') {
                rep = "$", skip = 2;
            }
        }
        const char *src = rep ? rep : p;
        size_t n = rep ? strlen(rep) : 1;
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            out = xrealloc(out, cap);
        }
        memcpy(out + len, src, n);
        len += n;
        p += skip;
    }
    out[len] = '\0';
    return out;
}

char *config_default_path(void)
{
    const char *env = getenv("SFWC_CONFIG");
    if (env && *env) {
        return xstrdup(env);
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char buf[1024];
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/sfwc/sfwc.conf", xdg);
    } else if (home && *home) {
        snprintf(buf, sizeof buf, "%s/.config/sfwc/sfwc.conf", home);
    } else {
        return NULL;
    }
    return xstrdup(buf);
}
