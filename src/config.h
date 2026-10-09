/*
 * SFWC configuration: parsing of sfwc.conf, keybind/mousebind tables, defaults.
 * No wlroots dependency so it can be unit-tested on its own (tests/test_config.c).
 */
#ifndef SFWC_CONFIG_H
#define SFWC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"
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

/*
 * Hyprland-style animation rules: `animation = <type>, <on>, <speed>, <curve>[, <style>]`.
 * Speed is in units of 100 ms, as in Hyprland.
 */
enum anim_type {
    ANIMT_WINDOWS_IN,
    ANIMT_WINDOWS_OUT,
    ANIMT_WINDOWS_MOVE,
    ANIMT_FADE_IN,
    ANIMT_FADE_OUT,
    ANIMT_BORDER,
    ANIMT_WORKSPACES,
    ANIMT_LAYERS_IN,
    ANIMT_LAYERS_OUT,
    ANIMT_COUNT,
};

enum anim_style {
    ANIM_STYLE_DEFAULT, /* the type's own default */
    ANIM_STYLE_POPIN,   /* grow/shrink around the center, `percent` = size at the start */
    ANIM_STYLE_SLIDE,   /* move in from / out to the nearest screen edge (or `dir`) */
    ANIM_STYLE_SLIDEVERT,     /* workspaces: slide up/down */
    ANIM_STYLE_SLIDEFADE,     /* slide by `percent` of the size while fading */
    ANIM_STYLE_SLIDEFADEVERT, /* workspaces: slidefade up/down */
    ANIM_STYLE_FADE,
    ANIM_STYLE_ZOOM,    /* popin that also fades */
    ANIM_STYLE_SQUEEZE, /* collapse to a line, then to nothing, like a switched off TV */
    ANIM_STYLE_FIRE,    /* burn away from the bottom, with flames */
};

enum anim_dir { ANIM_DIR_AUTO, ANIM_DIR_LEFT, ANIM_DIR_RIGHT, ANIM_DIR_TOP, ANIM_DIR_BOTTOM };

struct anim_rule {
    bool set;  /* given in the config (otherwise the legacy keys / the global rule apply) */
    bool on;
    double speed; /* x 100 ms */
    char curve[24];
    enum anim_style style;
    int percent; /* 0 = the style's default */
    enum anim_dir dir;
};

#define MAX_CURVES 16
struct anim_curve_def {
    char name[24];
    struct anim_curve curve;
};

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
    char **keys, **values;
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
    /* [keyboard]; NULL strings mean the xkb default */
    char *kb_rules, *kb_model, *kb_layout, *kb_variant, *kb_options;
    int repeat_rate;  /* characters per second, 0 = no repeat */
    int repeat_delay; /* ms before repeating starts */
        /* [output:NAME] */
    struct output_cfg *outputs;
    size_t n_outputs;
    /* [animations] (parsed now, used from the animation milestone on) */
    bool anim_enabled;
    char anim_open[24], anim_close[24], anim_easing[24];
    bool anim_move, anim_resize;
    int anim_duration_ms;
    /* [animations] bezier = ..., animation = ... */
    struct anim_curve_def curves[MAX_CURVES];
    int n_curves;
    struct anim_rule anim[ANIMT_COUNT];
    struct anim_rule anim_global; /* `animation = global, ...`: default for types without a rule */
    /* the `fire` style */
    int fire_particles; /* at most this many flames at a time */
    int fire_size;      /* radius of a flame in px */
    uint32_t fire_color; /* 0xRRGGBB, the main color of the flames */
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
/* The value of `key` in [plugin:NAME], or NULL. */
const char *config_plugin_get(const struct config *c, const char *plugin, const char *key);

const char *action_name(enum action a);

/* The curve called `name`: defined with `bezier = ...` in the config, else a built-in one. */
bool config_find_curve(const struct config *c, const char *name, struct anim_curve *out);

/* The rule for `type`: its own, else the global one (with the type's style), else false. */
bool config_anim_rule(const struct config *c, enum anim_type type, struct anim_rule *out);

#endif
