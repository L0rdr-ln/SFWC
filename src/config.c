#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
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
    if (a == ACTION_WORKSPACE) {
        return "workspace";
    }
    if (a == ACTION_MOVE_WORKSPACE) {
        return "move-to-workspace";
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
    c->workspaces = 4;
    c->templates_enabled = true;
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
    c->fire_particles = 400;
    c->fire_size = 14;
    c->fire_color = 0xff7a18;

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
    for (int i = 1; i <= 4; i++) {
        char key[2] = {(char)('0' + i), 0};
        add_keybind(c, m, key, ACTION_WORKSPACE, key);
        add_keybind(c, m | CFG_MOD_SHIFT, key, ACTION_MOVE_WORKSPACE, key);
    }
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
    for (size_t i = 0; i < c->n_templates_off; i++) {
        free(c->templates_off[i]);
    }
    free(c->templates_off);
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
    bool ws = !strncmp(value, "workspace:", 10);
    bool mv = !strncmp(value, "move-to-workspace:", 18);
    if (ws || mv) {
        const char *n = value + (ws ? 10 : 18);
        while (isspace((unsigned char)*n)) {
            n++;
        }
        if (mouse) {
            report(l, CONFIG_ERROR, "mouse bindings only support move and resize");
            return false;
        }
        if (n[0] < '1' || n[0] > '9' || n[1]) {
            report(l, CONFIG_ERROR, "'%s' needs a workspace number from 1 to 9", ws ? "workspace:" : "move-to-workspace:");
            return false;
        }
        *action = ws ? ACTION_WORKSPACE : ACTION_MOVE_WORKSPACE;
        *arg = xstrdup(n);
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
    } else if (!strcmp(key, "workspaces")) {
        set_int(l, key, v, 1, 9, &c->workspaces);
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

/* ---------------------------------------------------------- animation rules */

static bool parse_double(const char *v, double min, double max, double *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (end == v || *end || errno || !isfinite(d) || d < min || d > max) {
        return false;
    }
    *out = d;
    return true;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

bool config_find_curve(const struct config *c, const char *name, struct anim_curve *out)
{
    for (int i = c->n_curves - 1; i >= 0; i--) {
        if (!strcmp(c->curves[i].name, name)) {
            *out = c->curves[i].curve;
            return true;
        }
    }
    return anim_builtin_curve(name, out);
}

bool config_anim_rule(const struct config *c, enum anim_type type, struct anim_rule *out)
{
    if (type < 0 || type >= ANIMT_COUNT) {
        return false;
    }
    if (c->anim[type].set) {
        *out = c->anim[type];
        return true;
    }
    if (c->anim_global.set) {
        *out = c->anim_global;
        out->style = ANIM_STYLE_DEFAULT; /* the global rule has no style */
        return true;
    }
    return false;
}

static const struct {
    const char *name;
    enum anim_type types[4]; /* ANIMT_COUNT ends the list */
} anim_names[] = {
    {"windows", {ANIMT_WINDOWS_IN, ANIMT_WINDOWS_OUT, ANIMT_WINDOWS_MOVE, ANIMT_COUNT}},
    {"windowsIn", {ANIMT_WINDOWS_IN, ANIMT_COUNT}},
    {"windowsOut", {ANIMT_WINDOWS_OUT, ANIMT_COUNT}},
    {"windowsMove", {ANIMT_WINDOWS_MOVE, ANIMT_COUNT}},
    {"fade", {ANIMT_FADE_IN, ANIMT_FADE_OUT, ANIMT_COUNT}},
    {"fadeIn", {ANIMT_FADE_IN, ANIMT_COUNT}},
    {"fadeOut", {ANIMT_FADE_OUT, ANIMT_COUNT}},
    {"border", {ANIMT_BORDER, ANIMT_COUNT}},
    {"workspaces", {ANIMT_WORKSPACES, ANIMT_COUNT}},
    {"layers", {ANIMT_LAYERS_IN, ANIMT_LAYERS_OUT, ANIMT_COUNT}},
    {"layersIn", {ANIMT_LAYERS_IN, ANIMT_COUNT}},
    {"layersOut", {ANIMT_LAYERS_OUT, ANIMT_COUNT}},
};

/* Hyprland animation names that exist there but not here; accepted so that a pasted
 * Hyprland animation block loads, but they do nothing. */
static const char *const anim_ignored[] = {"borderangle", "fadeSwitch", "fadeShadow", "fadeDim",
                                           "fadeLayers", "specialWorkspace", "specialWorkspaceIn",
                                           "specialWorkspaceOut", "workspacesIn", "workspacesOut",
                                           "monitorAdded"};

static bool style_allowed(enum anim_type t, enum anim_style s)
{
    switch (t) {
    case ANIMT_WINDOWS_IN:
    case ANIMT_WINDOWS_OUT:
        return s == ANIM_STYLE_POPIN || s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_SLIDEFADE ||
               s == ANIM_STYLE_ZOOM || s == ANIM_STYLE_SQUEEZE || s == ANIM_STYLE_FIRE;
    case ANIMT_WORKSPACES:
        return s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_SLIDEVERT || s == ANIM_STYLE_SLIDEFADE ||
               s == ANIM_STYLE_SLIDEFADEVERT || s == ANIM_STYLE_FADE;
    case ANIMT_LAYERS_IN:
    case ANIMT_LAYERS_OUT:
        return s == ANIM_STYLE_POPIN || s == ANIM_STYLE_SLIDE || s == ANIM_STYLE_FADE;
    default:
        return false;
    }
}

/* "popin 80%", "slide left", "slidefade 20%", "slidevert", "fade" */
static bool parse_style(const char *text, enum anim_style *style, int *percent, enum anim_dir *dir)
{
    static const struct {
        const char *name;
        enum anim_style style;
    } styles[] = {{"popin", ANIM_STYLE_POPIN},
                  {"slide", ANIM_STYLE_SLIDE},
                  {"slidevert", ANIM_STYLE_SLIDEVERT},
                  {"slidefade", ANIM_STYLE_SLIDEFADE},
                  {"slidefadevert", ANIM_STYLE_SLIDEFADEVERT},
                  {"fade", ANIM_STYLE_FADE},
                  {"zoom", ANIM_STYLE_ZOOM},
                  {"squeeze", ANIM_STYLE_SQUEEZE},
                  {"fire", ANIM_STYLE_FIRE}};
    char buf[64];
    snprintf(buf, sizeof buf, "%s", text);
    char *save = NULL, *word = strtok_r(buf, " \t", &save), *arg = strtok_r(NULL, " \t", &save);
    if (!word || strtok_r(NULL, " \t", &save)) {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < sizeof styles / sizeof *styles; i++) {
        if (!strcmp(word, styles[i].name)) {
            *style = styles[i].style;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *percent = 0;
    *dir = ANIM_DIR_AUTO;
    if (!arg) {
        return true;
    }
    size_t n = strlen(arg);
    if (n > 1 && arg[n - 1] == '%') {
        char num[16];
        snprintf(num, sizeof num, "%.*s", (int)(n - 1), arg);
        double pct;
        if (!parse_double(num, 1, 300, &pct)) {
            return false;
        }
        *percent = (int)(pct + 0.5);
        /* squeeze and fire take no argument; whether a slide takes a percent or a direction
         * depends on the type and is checked by the caller */
        return *style != ANIM_STYLE_SQUEEZE && *style != ANIM_STYLE_FIRE && *style != ANIM_STYLE_FADE;
    }
    if (*style != ANIM_STYLE_SLIDE) {
        return false;
    }
    if (!strcmp(arg, "left")) {
        *dir = ANIM_DIR_LEFT;
    } else if (!strcmp(arg, "right")) {
        *dir = ANIM_DIR_RIGHT;
    } else if (!strcmp(arg, "top")) {
        *dir = ANIM_DIR_TOP;
    } else if (!strcmp(arg, "bottom")) {
        *dir = ANIM_DIR_BOTTOM;
    } else {
        return false;
    }
    return true;
}

/* bezier = name, x0, y0, x1, y1 */
static void handle_bezier(struct loader *l, const char *value)
{
    struct config *c = l->c;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", value);
    char *fields[5];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < 6; tok = strtok_r(NULL, ",", &save)) {
        if (n < 5) {
            fields[n] = trim(tok);
        }
        n++;
    }
    if (n != 5) {
        report(l, CONFIG_ERROR, "bezier: use 'name, x0, y0, x1, y1' (got %d value%s)", n, n == 1 ? "" : "s");
        return;
    }
    const char *name = fields[0];
    size_t nl = strlen(name);
    bool ok = nl > 0 && nl < sizeof c->curves[0].name;
    for (const char *p = name; ok && *p; p++) {
        ok = isalnum((unsigned char)*p) || *p == '_' || *p == '-';
    }
    if (!ok) {
        report(l, CONFIG_ERROR, "bezier: '%s' is not a curve name (letters, digits, - and _, up to 23)", name);
        return;
    }
    struct anim_curve cv;
    double *p[4] = {&cv.x0, &cv.y0, &cv.x1, &cv.y1};
    for (int i = 0; i < 4; i++) {
        if (!parse_double(fields[i + 1], -10, 10, p[i])) {
            report(l, CONFIG_ERROR, "bezier %s: '%s' is not a number", name, fields[i + 1]);
            return;
        }
    }
    if (!anim_curve_valid(&cv)) {
        report(l, CONFIG_ERROR, "bezier %s: x0 and x1 must be between 0 and 1", name);
        return;
    }
    for (int i = 0; i < c->n_curves; i++) {
        if (!strcmp(c->curves[i].name, name)) { /* last definition wins */
            c->curves[i].curve = cv;
            return;
        }
    }
    if (c->n_curves >= MAX_CURVES) {
        report(l, CONFIG_ERROR, "bezier: at most %d curves can be defined", MAX_CURVES);
        return;
    }
    snprintf(c->curves[c->n_curves].name, sizeof c->curves[0].name, "%s", name);
    c->curves[c->n_curves++].curve = cv;
}

/* animation = type, on, speed, curve[, style] */
static void handle_animation_rule(struct loader *l, const char *value)
{
    struct config *c = l->c;
    char buf[200];
    snprintf(buf, sizeof buf, "%s", value);
    char *fields[5];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < 6; tok = strtok_r(NULL, ",", &save)) {
        if (n < 5) {
            fields[n] = trim(tok);
        }
        n++;
    }
    if (n < 2 || n > 5) {
        report(l, CONFIG_ERROR, "animation: use 'type, on, speed, curve[, style]'");
        return;
    }
    const char *name = fields[0];
    bool global = !strcmp(name, "global");
    const enum anim_type *types = NULL;
    for (unsigned i = 0; i < sizeof anim_names / sizeof *anim_names; i++) {
        if (!strcmp(name, anim_names[i].name)) {
            types = anim_names[i].types;
        }
    }
    if (!global && !types) {
        for (unsigned i = 0; i < sizeof anim_ignored / sizeof *anim_ignored; i++) {
            if (!strcmp(name, anim_ignored[i])) {
                report(l, CONFIG_WARNING, "animation %s is not supported and is ignored", name);
                return;
            }
        }
        report(l, CONFIG_ERROR, "animation: unknown type '%s'", name);
        return;
    }
    struct anim_rule r = {.set = true, .on = true, .speed = 5, .style = ANIM_STYLE_DEFAULT};
    snprintf(r.curve, sizeof r.curve, "%s", "default");
    if (!parse_bool(fields[1], &r.on)) {
        report(l, CONFIG_ERROR, "animation %s: '%s' is not 0/1", name, fields[1]);
        return;
    }
    if (r.on) {
        if (n < 4) {
            report(l, CONFIG_ERROR, "animation %s: needs speed and curve (or 0 to turn it off)", name);
            return;
        }
        if (!parse_double(fields[2], 0.01, 100, &r.speed)) {
            report(l, CONFIG_ERROR, "animation %s: speed '%s' is not a number between 0.01 and 100", name, fields[2]);
            return;
        }
        struct anim_curve probe;
        if (strlen(fields[3]) >= sizeof r.curve || !config_find_curve(c, fields[3], &probe)) {
            report(l, CONFIG_ERROR, "animation %s: unknown curve '%s' (define it with bezier = ... first)", name, fields[3]);
            return;
        }
        snprintf(r.curve, sizeof r.curve, "%s", fields[3]);
    }
    if (n == 5 && r.on) {
        enum anim_style style;
        if (global) {
            report(l, CONFIG_ERROR, "animation global: takes no style");
            return;
        }
        if (!parse_style(fields[4], &style, &r.percent, &r.dir)) {
            report(l, CONFIG_ERROR, "animation %s: bad style '%s' (popin 80%%, slide [left|right|top|bottom], slidefade 20%%, slidevert, fade)", name, fields[4]);
            return;
        }
        bool applies = false; /* at least one of the types must be able to use the style */
        for (const enum anim_type *t = types; *t != ANIMT_COUNT; t++) {
            applies = applies || style_allowed(*t, style);
        }
        if (!applies) {
            report(l, CONFIG_ERROR, "animation %s: style '%s' does not apply here", name, fields[4]);
            return;
        }
        r.style = style;
        /* windows slide towards an edge (a direction), workspaces slide a part of the screen (a percent) */
        bool windows = types[0] == ANIMT_WINDOWS_IN || types[0] == ANIMT_WINDOWS_OUT;
        if (style == ANIM_STYLE_SLIDE && ((windows && r.percent) || (!windows && r.dir != ANIM_DIR_AUTO))) {
            report(l, CONFIG_ERROR, "animation %s: bad style '%s' (%s)", name, fields[4],
                   windows ? "slide takes left, right, top or bottom" : "slide takes a percentage");
            return;
        }
    }
    if (global) {
        c->anim_global = r;
        return;
    }
    for (const enum anim_type *t = types; *t != ANIMT_COUNT; t++) {
        c->anim[*t] = r;
        if (r.style != ANIM_STYLE_DEFAULT && !style_allowed(*t, r.style)) {
            c->anim[*t].style = ANIM_STYLE_DEFAULT; /* `windows` carries a style only windowsIn/Out take */
            c->anim[*t].percent = 0;
        }
    }
}

/* preset = hyprland | minimal | none: a ready-made set of rules, later lines can change them */
static void apply_preset(struct loader *l, const char *name)
{
    static const char *const hyprland[] = {
        "bezier = easeOutQuint, 0.23, 1, 0.32, 1",
        "bezier = easeInOutCubic, 0.65, 0.05, 0.36, 1",
        "bezier = almostLinear, 0.5, 0.5, 0.75, 1",
        "bezier = quick, 0.15, 0, 0.1, 1",
        "windows, 1, 4.79, easeOutQuint",
        "windowsIn, 1, 4.1, easeOutQuint, popin 87%",
        "windowsOut, 1, 1.49, linear, popin 87%",
        "fadeIn, 1, 1.73, almostLinear",
        "fadeOut, 1, 1.46, almostLinear",
        "border, 1, 5.39, easeOutQuint",
        "layers, 1, 3.81, easeOutQuint",
        "workspaces, 1, 1.94, easeOutQuint, slide",
        NULL};
    static const char *const minimal[] = {
        "bezier = quick, 0.15, 0, 0.1, 1",
        "windowsIn, 1, 2, quick, popin 95%",
        "windowsOut, 1, 2, quick, popin 95%",
        "windowsMove, 1, 2, quick",
        "fade, 1, 2, quick",
        "workspaces, 1, 2, quick, fade",
        NULL};
    const char *const *lines = NULL;
    if (!strcmp(name, "hyprland")) {
        lines = hyprland;
    } else if (!strcmp(name, "minimal")) {
        lines = minimal;
    } else if (!strcmp(name, "none")) {
        for (int i = 0; i < ANIMT_COUNT; i++) {
            l->c->anim[i] = (struct anim_rule){.set = true, .on = false};
        }
        return;
    } else {
        report(l, CONFIG_ERROR, "preset: unknown preset '%s' (hyprland, minimal, none)", name);
        return;
    }
    for (; *lines; lines++) {
        const char *line = *lines;
        if (!strncmp(line, "bezier = ", 9)) {
            handle_bezier(l, line + 9);
        } else {
            handle_animation_rule(l, line);
        }
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
    } else if (!strcmp(key, "bezier")) {
        handle_bezier(l, v);
    } else if (!strcmp(key, "animation")) {
        handle_animation_rule(l, v);
    } else if (!strcmp(key, "preset")) {
        apply_preset(l, v);
    } else if (!strcmp(key, "fire_particles")) {
        set_int(l, key, v, 20, 2000, &c->fire_particles);
    } else if (!strcmp(key, "fire_size")) {
        set_int(l, key, v, 4, 60, &c->fire_size);
    } else if (!strcmp(key, "fire_color")) {
        const char *h = v + (*v == '#');
        char *end;
        unsigned long rgb = strtoul(h, &end, 16);
        if (strlen(h) != 6 || *end) {
            report(l, CONFIG_ERROR, "fire_color: '%s' is not a color (use #rrggbb)", v);
        } else {
            c->fire_color = (uint32_t)rgb;
        }
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
        struct config *c = l->c;
        if (!strcmp(name, "enabled")) {
            set_bool(l, name, value, &c->templates_enabled);
            return 1;
        }
        bool on = true;
        bool ok = *name != 0;
        for (const char *p = name; *p; p++) {
            ok = ok && (isalnum((unsigned char)*p) || *p == '_' || *p == '-');
        }
        if (!ok) {
            report(l, CONFIG_WARNING, "[templates]: '%s' is not a template name", name);
            return 1;
        }
        if (!parse_bool(value, &on)) {
            report(l, CONFIG_ERROR, "%s: '%s' is not a boolean (use true/false)", name, value);
            return 1;
        }
        size_t i = 0;
        while (i < c->n_templates_off && strcmp(c->templates_off[i], name) != 0) {
            i++;
        }
        if (on && i < c->n_templates_off) { /* last line wins */
            free(c->templates_off[i]);
            c->templates_off[i] = c->templates_off[--c->n_templates_off];
        } else if (!on && i == c->n_templates_off) {
            c->templates_off = xrealloc(c->templates_off, (c->n_templates_off + 1) * sizeof(char *));
            c->templates_off[c->n_templates_off++] = xstrdup(name);
        }
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
