#define _POSIX_C_SOURCE 200809L
#include "theme.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        abort();
    }
    return d;
}

static struct color rgba(unsigned hex, float a)
{
    return (struct color){((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f,
                          (hex & 0xff) / 255.0f, a};
}

/* Same values as themes/default.theme (checked by the unit tests). */
void theme_init_default(struct theme *t)
{
    memset(t, 0, sizeof *t);
    t->name = xstrdup("default");
    t->background = rgba(0x1e1e2e, 1);
    t->border_focused = rgba(0x89b4fa, 1);
    t->border_unfocused = rgba(0x45475a, 1);
    t->titlebar_focused = rgba(0x313244, 1);
    t->titlebar_unfocused = rgba(0x1e1e2e, 1);
    t->title_text = rgba(0xcdd6f4, 1);
    t->close_button = rgba(0xf38ba8, 1);
    t->maximize_button = rgba(0xa6e3a1, 1);
    t->minimize_button = rgba(0xf9e2af, 1);
    t->border_width = 2;
    t->titlebar_height = 28;
    t->corner_radius = 8;
    t->button_size = 14;
    t->button_spacing = 8;
    t->shadow_enabled = true;
    t->shadow_radius = 20;
    t->shadow_offset_y = 6;
    t->shadow_color = rgba(0x000000, 0x66 / 255.0f);
    t->font_family = xstrdup("sans");
    t->font_size = 10;
}

void theme_finish(struct theme *t)
{
    free(t->name);
    free(t->font_family);
    memset(t, 0, sizeof *t);
}

bool color_parse(const char *s, struct color *out)
{
    if (*s != '#') {
        return false;
    }
    size_t n = strlen(s + 1);
    if (n != 6 && n != 8) {
        return false;
    }
    for (size_t i = 1; i <= n; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    unsigned long v = strtoul(s + 1, NULL, 16);
    if (n == 6) {
        *out = rgba((unsigned)v, 1);
    } else {
        *out = rgba((unsigned)(v >> 8), (v & 0xff) / 255.0f);
    }
    return true;
}

/* ----------------------------------------------------------------- parsing */

struct tloader {
    struct theme *t;
    ini_log_fn log;
    void *log_data;
    int line;
};

static void report(struct tloader *l, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void report(struct tloader *l, int level, const char *fmt, ...)
{
    if (!l->log) {
        return;
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    l->log(level, l->line, msg, l->log_data);
}

static void set_color(struct tloader *l, const char *key, const char *v, struct color *out)
{
    if (!color_parse(v, out)) {
        report(l, INI_ERROR, "%s: '%s' is not a color (use #rrggbb or #rrggbbaa)", key, v);
    }
}

static void set_int(struct tloader *l, const char *key, const char *v, int min, int max, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < min || n > max) {
        report(l, INI_ERROR, "%s: '%s' is not a number between %d and %d", key, v, min, max);
        return;
    }
    *out = (int)n;
}

static int theme_cb(void *user, const char *section, const char *name, const char *value, int line)
{
    struct tloader *l = user;
    struct theme *t = l->t;
    l->line = line;
    if (!*section) { /* top-level keys */
        if (!strcmp(name, "format")) {
            int fmt = 0;
            set_int(l, name, value, 1, 1000, &fmt);
            if (fmt && fmt != THEME_FORMAT) {
                report(l, INI_ERROR, "unsupported theme format %d (this build reads format %d)", fmt,
                       THEME_FORMAT);
            }
        } else if (!strcmp(name, "name")) {
            free(t->name);
            t->name = xstrdup(value);
        } else {
            report(l, INI_WARNING, "unknown key '%s'", name);
        }
        return 1;
    }
    if (!strcmp(section, "colors")) {
        struct {
            const char *key;
            struct color *dst;
        } colors[] = {
            {"background", &t->background},
            {"border_focused", &t->border_focused},
            {"border_unfocused", &t->border_unfocused},
            {"titlebar_focused", &t->titlebar_focused},
            {"titlebar_unfocused", &t->titlebar_unfocused},
            {"title_text", &t->title_text},
            {"close_button", &t->close_button},
            {"maximize_button", &t->maximize_button},
            {"minimize_button", &t->minimize_button},
        };
        for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
            if (!strcmp(name, colors[i].key)) {
                set_color(l, name, value, colors[i].dst);
                return 1;
            }
        }
        report(l, INI_WARNING, "unknown key '%s' in [colors]", name);
    } else if (!strcmp(section, "geometry")) {
        if (!strcmp(name, "border_width")) {
            set_int(l, name, value, 0, 50, &t->border_width);
        } else if (!strcmp(name, "titlebar_height")) {
            set_int(l, name, value, 0, 200, &t->titlebar_height);
        } else if (!strcmp(name, "corner_radius")) {
            set_int(l, name, value, 0, 100, &t->corner_radius);
        } else if (!strcmp(name, "button_size")) {
            set_int(l, name, value, 4, 100, &t->button_size);
        } else if (!strcmp(name, "button_spacing")) {
            set_int(l, name, value, 0, 100, &t->button_spacing);
        } else {
            report(l, INI_WARNING, "unknown key '%s' in [geometry]", name);
        }
    } else if (!strcmp(section, "shadow")) {
        if (!strcmp(name, "enabled")) {
            if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") || !strcasecmp(value, "on") || !strcmp(value, "1")) {
                t->shadow_enabled = true;
            } else if (!strcasecmp(value, "false") || !strcasecmp(value, "no") || !strcasecmp(value, "off") || !strcmp(value, "0")) {
                t->shadow_enabled = false;
            } else {
                report(l, INI_ERROR, "enabled: '%s' is not a boolean (use true/false)", value);
            }
        } else if (!strcmp(name, "radius")) {
            set_int(l, name, value, 0, 200, &t->shadow_radius);
        } else if (!strcmp(name, "offset_y")) {
            set_int(l, name, value, -100, 100, &t->shadow_offset_y);
        } else if (!strcmp(name, "color")) {
            set_color(l, name, value, &t->shadow_color);
        } else {
            report(l, INI_WARNING, "unknown key '%s' in [shadow]", name);
        }
    } else if (!strcmp(section, "font")) {
        if (!strcmp(name, "family")) {
            free(t->font_family);
            t->font_family = xstrdup(value);
        } else if (!strcmp(name, "size")) {
            set_int(l, name, value, 4, 100, &t->font_size);
        } else {
            report(l, INI_WARNING, "unknown key '%s' in [font]", name);
        }
    } else {
        report(l, INI_WARNING, "unknown section [%s] ignored", section);
    }
    return 1;
}

/* inih's handler has no line number; recover it from the reader's counter. */
struct cb_ctx {
    struct tloader *loader;
    const struct inifile_reader *reader;
};

static int ini_trampoline(void *user, const char *section, const char *name, const char *value)
{
    struct cb_ctx *c = user;
    return theme_cb(c->loader, section, name, value, c->reader->line);
}

static void run(struct theme *t, struct inifile_reader *r, ini_reader fn, ini_log_fn log, void *data)
{
    struct tloader l = {.t = t, .log = log, .log_data = data};
    struct cb_ctx ctx = {&l, r};
    int err = ini_parse_stream(fn, r, ini_trampoline, &ctx);
    if (err > 0 && log) {
        log(INI_ERROR, err, "syntax error (expected 'key = value' or '[section]')", data);
    }
}

bool theme_load_file(struct theme *t, const char *path, ini_log_fn log, void *data)
{
    struct inifile_reader r = {.fp = fopen(path, "r")};
    if (!r.fp) {
        return false;
    }
    run(t, &r, inifile_file_reader, log, data);
    fclose(r.fp);
    return true;
}

bool theme_load_string(struct theme *t, const char *text, ini_log_fn log, void *data)
{
    struct inifile_reader r = {.text = text};
    run(t, &r, inifile_string_reader, log, data);
    return true;
}

/* ---------------------------------------------------------------- lookup */

char *theme_find(const char *name, const char *config_dir)
{
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        return NULL;
    }
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char dirs[6][768];
    int n = 0;
    if (config_dir) {
        snprintf(dirs[n++], sizeof dirs[0], "%s/themes", config_dir);
    }
    if (xdg_config && *xdg_config) {
        snprintf(dirs[n++], sizeof dirs[0], "%s/sfwc/themes", xdg_config);
    } else if (home && *home) {
        snprintf(dirs[n++], sizeof dirs[0], "%s/.config/sfwc/themes", home);
    }
    if (xdg_data && *xdg_data) {
        snprintf(dirs[n++], sizeof dirs[0], "%s/sfwc/themes", xdg_data);
    } else if (home && *home) {
        snprintf(dirs[n++], sizeof dirs[0], "%s/.local/share/sfwc/themes", home);
    }
    snprintf(dirs[n++], sizeof dirs[0], "/usr/local/share/sfwc/themes");
    snprintf(dirs[n++], sizeof dirs[0], "/usr/share/sfwc/themes");
    for (int i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.theme", dirs[i], name);
        if (access(path, R_OK) == 0) {
            return xstrdup(path);
        }
    }
    return NULL;
}
