#define _DEFAULT_SOURCE
/*
 * Garbage-in test for the parsers: random bytes, mutated copies of the shipped files and
 * line-shaped nonsense go through the config, theme and template code. The point is that
 * nothing crashes or trips the sanitizers (run with -Db_sanitize=address,undefined) and that
 * the values that come out are still inside their documented limits. Deterministic: the same
 * seeds every run, so a failure can be reproduced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "template.h"
#include "theme.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s (iteration %d)\n", __FILE__, __LINE__, #cond, iter);   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static int iter;
static unsigned long long state = 0x9e3779b97f4a7c15ull;

static unsigned rnd(unsigned n)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (unsigned)(state % n);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

static const char *const SNIPPETS[] = {
    "[general]", "[windows]", "[keybinds]", "[mouse]", "[autostart]", "[templates]", "[output:X]",
    "[colors]", "[geometry]", "[shadow]", "[font]", "[animations]", "[keyboard]",
    "=", " = ", "#", ";", "$mod+", "Alt+", "Shift+", "Super+Ctrl+", "spawn:", "workspace:", "move-to-workspace:",
    "true", "false", "-1", "99999999999999999999", "0", "#ffffff", "#12345", "#gggggg", "@", "@@",
    "@colors.background@", "@colors.nope@", "@font.family:hex@", ":hex@", "\\", "\"", "'", "\n", "\r\n", "\t",
    "\nworkspaces = 12\n", "\nworkspaces = 0\n", "\ngap = 5000\n", "\nsnap_distance = -4\n",
    "\nrepeat_rate = 99999\n", "\nduration_ms = 99999\n", "\nborder_width = 500\n",
    "\ntitlebar_height = -1\n", "\ncorner_radius = 1000\n", "\nbutton_size = 1\n", "\nsize = 3\n",
    "\nfamily = $(id)\n", "\nname = a;b\n", "\nAlt+1 = workspace:12\n", "\nAlt+2 = spawn:\n",
    "\nAlt+3 = move-to-workspace:0\n", "\ncolor = #ffffff00ff\n",
    "\xff\xfe", "\xc3\x28", "$$", "$terminal", "$runtime", "exec", "format = 1", "format = 7", "name",
};
#define N_SNIPPETS (sizeof SNIPPETS / sizeof *SNIPPETS)

/* a buffer of random bytes, snippets or a mutated copy of `seed` */
static char *make_input(const char *seed, size_t seed_len, size_t *out_len)
{
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    unsigned kind = rnd(4);
    if (kind == 0 || !seed) { /* random bytes */
        len = rnd(600);
        for (size_t i = 0; i < len; i++) {
            buf[i] = (char)rnd(256);
        }
    } else if (kind == 1) { /* snippets glued together */
        unsigned n = 1 + rnd(40);
        for (unsigned i = 0; i < n; i++) {
            const char *s = SNIPPETS[rnd(N_SNIPPETS)];
            size_t sl = strlen(s);
            if (len + sl + 2 >= cap) {
                break;
            }
            memcpy(buf + len, s, sl);
            len += sl;
            if (rnd(3) == 0) {
                buf[len++] = '\n';
            }
        }
    } else { /* the shipped file with a few edits */
        len = seed_len < cap - 64 ? seed_len : cap - 64;
        memcpy(buf, seed, len);
        unsigned edits = 1 + rnd(8);
        for (unsigned e = 0; e < edits && len > 0; e++) {
            size_t at = rnd((unsigned)len);
            switch (rnd(4)) {
            case 0:
                buf[at] = (char)rnd(256);
                break;
            case 1: { /* delete a run */
                size_t n = 1 + rnd(20);
                if (at + n > len) {
                    n = len - at;
                }
                memmove(buf + at, buf + at + n, len - at - n);
                len -= n;
                break;
            }
            case 2: { /* insert a snippet */
                const char *s = SNIPPETS[rnd(N_SNIPPETS)];
                size_t sl = strlen(s);
                if (len + sl < cap - 1) {
                    memmove(buf + at + sl, buf + at, len - at);
                    memcpy(buf + at, s, sl);
                    len += sl;
                }
                break;
            }
            default: { /* duplicate a run */
                size_t n = 1 + rnd(40);
                if (at + n > len) {
                    n = len - at;
                }
                if (len + n < cap - 1) {
                    memmove(buf + at + n, buf + at, len - at);
                    memcpy(buf + at + n, buf + at, 0);
                    len += n;
                }
                break;
            }
            }
        }
    }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static void nolog(int level, int line, const char *text, void *data)
{
    (void)level;
    (void)line;
    (void)text;
    (void)data;
}

static void check_theme_limits(const struct theme *t)
{
    CHECK(t->border_width >= 0 && t->border_width <= 50);
    CHECK(t->titlebar_height >= 0 && t->titlebar_height <= 200);
    CHECK(t->corner_radius >= 0 && t->corner_radius <= 100);
    CHECK(t->button_size >= 4 && t->button_size <= 100);
    CHECK(t->font_size >= 4 && t->font_size <= 100);
    CHECK(t->shadow_radius >= 0 && t->shadow_radius <= 200);
    CHECK(t->name && t->font_family);
    const struct color *c[] = {&t->background, &t->border_focused, &t->title_text, &t->shadow_color};
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        CHECK(c[i]->r >= 0 && c[i]->r <= 1 && c[i]->a >= 0 && c[i]->a <= 1);
    }
    /* free text values never carry shell syntax */
    for (const char *p = t->font_family; *p; p++) {
        CHECK(!strchr("$`\"'\\;|&<>(){}*?\n", *p));
    }
}

static void check_config_limits(const struct config *c)
{
    CHECK(c->workspaces >= 1 && c->workspaces <= 9);
    CHECK(c->gap >= 0 && c->gap <= 200);
    CHECK(c->snap_distance >= 0 && c->snap_distance <= 200);
    CHECK(c->repeat_rate >= 0 && c->repeat_rate <= 1000);
    CHECK(c->theme && c->terminal);
    for (size_t i = 0; i < c->n_binds; i++) {
        const struct keybind *b = &c->binds[i];
        if (b->action == ACTION_SPAWN) {
            CHECK(b->arg && *b->arg);
        }
        if (b->action == ACTION_WORKSPACE || b->action == ACTION_MOVE_WORKSPACE) {
            CHECK(b->arg && b->arg[0] >= '1' && b->arg[0] <= '9' && !b->arg[1]);
        }
    }
}

int main(void)
{
    char path[1024];
    size_t conf_len = 0, theme_len = 0, tpl_len = 0;
    snprintf(path, sizeof path, "%s/config/sfwc.conf", SRC_ROOT);
    char *conf = slurp(path, &conf_len);
    snprintf(path, sizeof path, "%s/themes/light.theme", SRC_ROOT);
    char *themef = slurp(path, &theme_len);
    snprintf(path, sizeof path, "%s/themes/templates/waybar.css.in", SRC_ROOT);
    char *tpl = slurp(path, &tpl_len);
    if (!conf || !themef || !tpl) {
        fprintf(stderr, "FAIL: cannot read the shipped files\n");
        return 1;
    }

    enum { ROUNDS = 4000 };
    for (iter = 0; iter < ROUNDS; iter++) {
        size_t len;
        char *in = make_input(conf, conf_len, &len);
        struct config c;
        config_init_defaults(&c);
        config_load_string(&c, in, nolog, NULL);
        check_config_limits(&c);
        config_find_keybind(&c, CFG_MOD_ALT, 'q');
        char *x = config_expand(&c, in, "/run/user/1000");
        free(x);
        config_finish(&c);
        free(in);

        in = make_input(themef, theme_len, &len);
        struct theme t;
        theme_init_default(&t);
        theme_load_string(&t, in, nolog, NULL);
        check_theme_limits(&t);

        char err[200];
        for (int mode = TEMPLATE_STRICT; mode <= TEMPLATE_SHELL; mode++) {
            char *out = template_render(&t, in, (enum template_mode)mode, err, sizeof err);
            CHECK(out || mode == TEMPLATE_STRICT);
            free(out);
        }
        free(in);
        in = make_input(tpl, tpl_len, &len);
        for (int mode = TEMPLATE_STRICT; mode <= TEMPLATE_SHELL; mode++) {
            char *out = template_render(&t, in, (enum template_mode)mode, err, sizeof err);
            CHECK(out || mode == TEMPLATE_STRICT);
            free(out);
        }
        struct color col;
        color_parse(in, &col);
        free(in);
        theme_finish(&t);
    }
    free(conf);
    free(themef);
    free(tpl);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("fuzz: %d rounds OK\n", ROUNDS);
    return 0;
}
