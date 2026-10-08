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
    /* [keybinds] [mouse] [autostart] */
    struct keybind *binds;
    size_t n_binds;
    struct mousebind *mbinds;
    size_t n_mbinds;
    char **autostart;
    size_t n_autostart;
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

const char *action_name(enum action a);

#endif
