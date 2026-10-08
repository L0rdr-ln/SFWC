#define _POSIX_C_SOURCE 200809L
#include "template.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int to8(float v)
{
    int i = (int)lroundf(v * 255.0f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

static bool format_color(const struct color *c, const char *mod, char *out, size_t n)
{
    int r = to8(c->r), g = to8(c->g), b = to8(c->b), a = to8(c->a);
    if (!mod || !*mod) {
        if (a == 255) {
            snprintf(out, n, "#%02x%02x%02x", r, g, b);
        } else {
            snprintf(out, n, "#%02x%02x%02x%02x", r, g, b, a);
        }
    } else if (!strcmp(mod, "hex")) {
        snprintf(out, n, "%02x%02x%02x", r, g, b);
    } else if (!strcmp(mod, "hexa")) {
        snprintf(out, n, "%02x%02x%02x%02x", r, g, b, a);
    } else if (!strcmp(mod, "rgb")) {
        snprintf(out, n, "%d, %d, %d", r, g, b);
    } else if (!strcmp(mod, "rgba")) {
        snprintf(out, n, "rgba(%d, %d, %d, %.2f)", r, g, b, a / 255.0);
    } else {
        snprintf(out, n, "unknown modifier ':%s' (use hex, hexa, rgb or rgba)", mod);
        return false;
    }
    return true;
}

bool theme_value(const struct theme *t, const char *key, const char *modifier, char *out, size_t n)
{
    static const struct {
        const char *key;
        size_t off;
    } colors[] = {
        {"colors.background", offsetof(struct theme, background)},
        {"colors.border_focused", offsetof(struct theme, border_focused)},
        {"colors.border_unfocused", offsetof(struct theme, border_unfocused)},
        {"colors.titlebar_focused", offsetof(struct theme, titlebar_focused)},
        {"colors.titlebar_unfocused", offsetof(struct theme, titlebar_unfocused)},
        {"colors.title_text", offsetof(struct theme, title_text)},
        {"colors.close_button", offsetof(struct theme, close_button)},
        {"colors.maximize_button", offsetof(struct theme, maximize_button)},
        {"colors.minimize_button", offsetof(struct theme, minimize_button)},
        {"shadow.color", offsetof(struct theme, shadow_color)},
    };
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
        if (!strcmp(key, colors[i].key)) {
            return format_color((const struct color *)((const char *)t + colors[i].off), modifier,
                                out, n);
        }
    }
    const struct {
        const char *key;
        int value;
    } ints[] = {
        {"geometry.border_width", t->border_width},
        {"geometry.titlebar_height", t->titlebar_height},
        {"geometry.corner_radius", t->corner_radius},
        {"geometry.button_size", t->button_size},
        {"geometry.button_spacing", t->button_spacing},
        {"shadow.radius", t->shadow_radius},
        {"shadow.offset_y", t->shadow_offset_y},
        {"font.size", t->font_size},
    };
    const char *text = NULL;
    char num[16];
    for (size_t i = 0; i < sizeof ints / sizeof *ints; i++) {
        if (!strcmp(key, ints[i].key)) {
            snprintf(num, sizeof num, "%d", ints[i].value);
            text = num;
        }
    }
    if (!text && !strcmp(key, "shadow.enabled")) {
        text = t->shadow_enabled ? "true" : "false";
    } else if (!text && !strcmp(key, "font.family")) {
        text = t->font_family ? t->font_family : "sans";
    } else if (!text && !strcmp(key, "theme.name")) {
        text = t->name ? t->name : "default";
    }
    if (!text) {
        snprintf(out, n, "unknown theme key '%s'", key);
        return false;
    }
    if (modifier && *modifier) {
        snprintf(out, n, "'%s' is not a color, so it takes no ':%s' modifier", key, modifier);
        return false;
    }
    snprintf(out, n, "%s", text);
    return true;
}

struct buf {
    char *p;
    size_t len, cap;
};

static void put(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) {
            abort();
        }
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static bool key_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.';
}

char *template_render(const struct theme *t, const char *in, bool strict, char *err, size_t err_n)
{
    struct buf b = {0};
    put(&b, "", 0);
    int line = 1;
    for (const char *p = in; *p;) {
        if (*p == '\n') {
            line++;
        }
        if (*p != '@') {
            put(&b, p, 1);
            p++;
            continue;
        }
        if (strict && p[1] == '@') {
            put(&b, "@", 1);
            p += 2;
            continue;
        }
        /* @key@ or @key:modifier@ */
        const char *q = p + 1;
        while (key_char(*q)) {
            q++;
        }
        char key[64] = "", mod[16] = "";
        size_t klen = (size_t)(q - (p + 1));
        const char *end = NULL;
        if (klen > 0 && klen < sizeof key) {
            memcpy(key, p + 1, klen);
            key[klen] = 0;
            if (*q == '@') {
                end = q;
            } else if (*q == ':') {
                const char *m = q + 1, *r = m;
                while (isalpha((unsigned char)*r)) {
                    r++;
                }
                if (*r == '@' && r > m && (size_t)(r - m) < sizeof mod) {
                    memcpy(mod, m, (size_t)(r - m));
                    mod[r - m] = 0;
                    end = r;
                }
            }
        }
        char val[128];
        if (end && theme_value(t, key, mod, val, sizeof val)) {
            put(&b, val, strlen(val));
            p = end + 1;
            continue;
        }
        if (!strict) { /* not ours: keep the '@' and go on after it */
            put(&b, "@", 1);
            p++;
            continue;
        }
        if (err && err_n) {
            if (end) {
                snprintf(err, err_n, "line %d: %s", line, val);
            } else {
                snprintf(err, err_n, "line %d: '@' without a closing '@' (write '@@' for a literal @)", line);
            }
        }
        free(b.p);
        return NULL;
    }
    return b.p;
}
