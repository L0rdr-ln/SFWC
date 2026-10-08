/*
 * SFWC themes: colors, window frame geometry, shadow and font.
 * No wlroots dependency (unit-tested in tests/test_theme.c). The built-in "default"
 * theme is always available; theme files (format = 1) override it.
 */
#ifndef SFWC_THEME_H
#define SFWC_THEME_H

#include <stdbool.h>

#include "inifile.h"

#define THEME_FORMAT 1

struct color {
    float r, g, b, a; /* 0..1, not premultiplied */
};

struct theme {
    char *name;
    /* [colors] */
    struct color background;
    struct color border_focused, border_unfocused;
    struct color titlebar_focused, titlebar_unfocused;
    struct color title_text;
    struct color close_button, maximize_button, minimize_button;
    /* [geometry] */
    int border_width;
    int titlebar_height;
    int corner_radius;
    int button_size;
    int button_spacing;
    /* [shadow] */
    bool shadow_enabled;
    int shadow_radius;
    int shadow_offset_y;
    struct color shadow_color;
    /* [font] */
    char *font_family;
    int font_size;
};

/* Fill with the built-in default theme. */
void theme_init_default(struct theme *t);
void theme_finish(struct theme *t);

/* Apply a theme file on top of `t` (call theme_init_default first). Problems are reported
 * with line numbers through `log`; invalid entries keep their value. Returns false only if
 * the file cannot be read. */
bool theme_load_file(struct theme *t, const char *path, ini_log_fn log, void *data);
bool theme_load_string(struct theme *t, const char *text, ini_log_fn log, void *data);

/* "#rrggbb" or "#rrggbbaa" */
bool color_parse(const char *s, struct color *out);

/* Locate <name>.theme: <config_dir>/themes, $XDG_CONFIG_HOME/sfwc/themes,
 * ~/.config/sfwc/themes, $XDG_DATA_HOME/sfwc/themes (~/.local/share), /usr/local/share/sfwc/themes,
 * /usr/share/sfwc/themes. `config_dir` may be NULL. Returns a malloc'ed path or NULL.
 * Names containing '/' are rejected. */
char *theme_find(const char *name, const char *config_dir);

#endif
