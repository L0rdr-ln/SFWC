/*
 * SFWC configuration: parsing of sfwc.conf, keybind/mousebind tables, defaults.
 * No wlroots dependency so it can be unit-tested on its own (tests/test_config.c).
 */
#ifndef SFWC_CONFIG_H
#define SFWC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inifile.h"

/* Modifier bits; identical to wlroots' WLR_MODIFIER_* (checked in main.c). */
#define CFG_MOD_SHIFT 1u
#define CFG_MOD_CTRL 4u
#define CFG_MOD_ALT 8u
#define CFG_MOD_LOGO 64u
#define CFG_MOD_MASK (CFG_MOD_SHIFT | CFG_MOD_CTRL | CFG_MOD_ALT | CFG_MOD_LOGO)

enum action {
    ACTION_SPAWN,
    ACTION_CLOSE,
    ACTION_TOGGLE_MAXIMIZE,
    ACTION_TOGGLE_FULLSCREEN,
    ACTION_MINIMIZE,
    ACTION_RESTORE,
    ACTION_CYCLE,
    ACTION_RELOAD,
    ACTION_QUIT,
    ACTION_MOVE_OUTPUT,  /* move the focused window to the next output */
    ACTION_FOCUS_OUTPUT, /* warp to the next output and focus its top window */
    ACTION_WORKSPACE,      /* switch to workspace `arg` (1-based) */
    ACTION_MOVE_WORKSPACE, /* send the focused window to workspace `arg` */
    ACTION_MOVE,   /* mouse only */
    ACTION_RESIZE, /* mouse only */
};

enum focus_mode { FOCUS_CLICK, FOCUS_FOLLOW_MOUSE };

struct keybind {
    uint32_t mods;
    uint32_t sym; /* lower-case base-level keysym */
    enum action action;
    char *arg; /* command for ACTION_SPAWN, workspace number for ACTION_*WORKSPACE */
};

struct mousebind {
    uint32_t mods;
    uint32_t button; /* BTN_* code */
    enum action action;
};

/* [plugin:NAME]: free-form settings that only the plugin itself understands */
struct plugin_cfg {
    char *name;
    char **keys, **values; /* in file order; a key may repeat (lists) */
    int *lines;
    size_t n;
};

/* [output:NAME] */
struct output_cfg {
    char *name;
    double scale; /* 0 = not set */
    bool has_pos;
    int x, y;
    bool enabled;
};

/* [input] (every pointing device) and [input:touchpad] (on top of it, for touchpads). A tri-state
 * is -1 = leave what the device does by default, 0 = off, 1 = on; an empty string likewise. */
struct input_cfg {
    int tap, tap_drag, natural_scroll, disable_while_typing, middle_emulation, left_handed;
    bool has_accel;
    double accel_speed;       /* -1 .. 1 */
    char accel_profile[12];   /* adaptive | flat */
    char click_method[16];    /* button-areas | clickfinger */
    char scroll_method[20];   /* two-finger | edge | on-button-down | none */
    char tap_button_map[4];   /* lrm | lmr */
};
void input_cfg_init(struct input_cfg *c);
/* out = base with every setting that `over` makes replaced. */
void input_cfg_merge(struct input_cfg *out, const struct input_cfg *base, const struct input_cfg *over);

struct config {
    /* [general] */
    char *theme;
    char *terminal;
    uint32_t mod; /* what $mod expands to in binds */
    int workspaces; /* 1..9 */
    enum focus_mode focus;
    /* [windows] */
    bool snap_to_edges;
    bool snap_to_windows;
    bool decorations; /* draw title bars/borders for clients that agree to server-side decorations */
    int snap_distance;
    int gap;
    struct input_cfg input, touchpad;
    /* [keyboard]; NULL strings mean the xkb default */
    char *kb_rules, *kb_model, *kb_layout, *kb_variant, *kb_options;
    int repeat_rate;  /* characters per second, 0 = no repeat */
    int repeat_delay; /* ms before repeating starts */
        /* [output:NAME] */
    struct output_cfg *outputs;
    size_t n_outputs;
    /* [keybinds] [mouse] [autostart] */
    struct keybind *binds;
    size_t n_binds;
    struct mousebind *mbinds;
    size_t n_mbinds;
    char **autostart;
    size_t n_autostart;
    /* [plugins] load = NAME (loaded from the plugin search path) and the [plugin:NAME] sections */
    char **plugins;
    size_t n_plugins;
    struct plugin_cfg *plugin_cfgs;
    size_t n_plugin_cfgs;
    /* [templates]: render the theme's templates for the companion tools */
    bool templates_enabled;
    char **templates_off; /* names switched off individually (waybar = false) */
    size_t n_templates_off;
};

enum { CONFIG_WARNING = INI_WARNING, CONFIG_ERROR = INI_ERROR };
typedef ini_log_fn config_log_fn;

/* Fill `c` with the built-in defaults (also the default keybinds). */
void config_init_defaults(struct config *c);
void config_finish(struct config *c);

/*
 * Apply a config file / string on top of `c` (call config_init_defaults first).
 * Problems are reported through `log` with the 1-based line number; invalid
 * entries are skipped and the default stays. Returns false only if the file
 * cannot be read. A [keybinds], [mouse] or [autostart] section replaces the
 * built-in list for that section.
 */
bool config_load_file(struct config *c, const char *path, config_log_fn log, void *data);
bool config_load_string(struct config *c, const char *text, config_log_fn log, void *data);

/* Exact-modifier match; `sym` is the key's base-level keysym (any case). */
const struct keybind *config_find_keybind(const struct config *c, uint32_t mods, uint32_t sym);
const struct output_cfg *config_find_output(const struct config *c, const char *name);
const struct mousebind *config_find_mousebind(const struct config *c, uint32_t mods,
                                              uint32_t button);

/* Expands $terminal, $theme, $runtime, $$ in `in`. Caller frees. */
char *config_expand(const struct config *c, const char *in, const char *runtime_dir);

/* Config path: $SFWC_CONFIG, else $XDG_CONFIG_HOME/sfwc/sfwc.conf, else
 * ~/.config/sfwc/sfwc.conf, else NULL. Caller frees. */
char *config_default_path(void);

/* A plugin name is 1-32 characters of a-z, 0-9, '_' and '-': it becomes part of a file name. */
bool config_valid_plugin_name(const char *name);
/* The value of `key` in [plugin:NAME] (the last one if repeated), or NULL. The section [animations]
 * is read as [plugin:animations]. */
const char *config_plugin_get(const struct config *c, const char *plugin, const char *key);
/* The section of a plugin, or NULL if the config has none. */
const struct plugin_cfg *config_plugin_section(const struct config *c, const char *plugin);

const char *action_name(enum action a);

#endif
