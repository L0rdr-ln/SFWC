#define _DEFAULT_SOURCE
/* Unit tests for src/theme.c (no compositor needed). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "theme.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct msg {
    int level, line;
    char text[200];
};
struct log {
    struct msg m[32];
    int n;
};

static void collect(int level, int line, const char *text, void *data)
{
    struct log *l = data;
    if (l->n < 32) {
        l->m[l->n].level = level;
        l->m[l->n].line = line;
        snprintf(l->m[l->n].text, sizeof l->m[l->n].text, "%s", text);
        l->n++;
    }
}

static int has_msg(const struct log *l, int level, int line, const char *needle)
{
    for (int i = 0; i < l->n; i++) {
        if (l->m[i].level == level && l->m[i].line == line && strstr(l->m[i].text, needle)) {
            return 1;
        }
    }
    return 0;
}

static int color_eq(struct color a, struct color b)
{
    return fabsf(a.r - b.r) < 0.003f && fabsf(a.g - b.g) < 0.003f && fabsf(a.b - b.b) < 0.003f &&
           fabsf(a.a - b.a) < 0.003f;
}

static int theme_eq(const struct theme *a, const struct theme *b)
{
    return color_eq(a->background, b->background) &&
           color_eq(a->border_focused, b->border_focused) &&
           color_eq(a->border_unfocused, b->border_unfocused) &&
           color_eq(a->titlebar_focused, b->titlebar_focused) &&
           color_eq(a->titlebar_unfocused, b->titlebar_unfocused) &&
           color_eq(a->title_text, b->title_text) && color_eq(a->close_button, b->close_button) &&
           color_eq(a->maximize_button, b->maximize_button) &&
           color_eq(a->minimize_button, b->minimize_button) &&
           a->border_width == b->border_width && a->titlebar_height == b->titlebar_height &&
           a->corner_radius == b->corner_radius && a->button_size == b->button_size &&
           a->button_spacing == b->button_spacing && a->shadow_enabled == b->shadow_enabled &&
           a->shadow_radius == b->shadow_radius && a->shadow_offset_y == b->shadow_offset_y &&
           color_eq(a->shadow_color, b->shadow_color) && !strcmp(a->font_family, b->font_family) &&
           a->font_size == b->font_size;
}

static void test_color_parse(void)
{
    struct color c;
    CHECK(color_parse("#ff8000", &c) && c.r == 1.0f && fabsf(c.g - 128 / 255.0f) < 0.001f && c.b == 0 && c.a == 1.0f);
    CHECK(color_parse("#00000066", &c) && fabsf(c.a - 0x66 / 255.0f) < 0.001f);
    CHECK(!color_parse("ff8000", &c));    /* needs the # */
    CHECK(!color_parse("#ff80", &c));     /* wrong length */
    CHECK(!color_parse("#gg0000", &c));   /* not hex */
    CHECK(!color_parse("#ff8000zz", &c)); /* not hex */
}

static void test_shipped_default_matches_builtin(void)
{
    struct theme builtin, t;
    theme_init_default(&builtin);
    theme_init_default(&t);
    /* scramble every field so that only the file can restore it */
    struct color junk = {0.5f, 0.5f, 0.5f, 0.5f};
    t.background = t.border_focused = t.border_unfocused = t.titlebar_focused = junk;
    t.titlebar_unfocused = t.title_text = t.close_button = t.maximize_button = junk;
    t.minimize_button = t.shadow_color = junk;
    t.border_width = t.titlebar_height = t.corner_radius = t.button_size = 7;
    t.button_spacing = t.shadow_radius = t.shadow_offset_y = t.font_size = 7;
    t.shadow_enabled = false;
    free(t.font_family);
    t.font_family = strdup("junk");

    struct log l = {0};
    char path[1024];
    snprintf(path, sizeof path, "%s/themes/default.theme", SRC_ROOT);
    CHECK(theme_load_file(&t, path, collect, &l));
    CHECK(l.n == 0);
    CHECK(theme_eq(&builtin, &t)); /* the built-in fallback is the shipped default theme */
    theme_finish(&builtin);
    theme_finish(&t);
}

static void test_shipped_light(void)
{
    struct theme t;
    struct log l = {0};
    theme_init_default(&t);
    char path[1024];
    snprintf(path, sizeof path, "%s/themes/light.theme", SRC_ROOT);
    CHECK(theme_load_file(&t, path, collect, &l));
    CHECK(l.n == 0);
    CHECK(!strcmp(t.name, "Light") && t.border_width == 1 && t.corner_radius == 4);
    theme_finish(&t);
}

static void test_errors(void)
{
    const char *text = "format = 1\n"              /* 1 */
                       "colour = red\n"            /* 2 unknown top-level key */
                       "[colors]\n"                /* 3 */
                       "border_focused = blue\n"   /* 4 bad color */
                       "title_text = #123456\n"    /* 5 ok */
                       "shiny = #ffffff\n"         /* 6 unknown */
                       "[geometry]\n"              /* 7 */
                       "border_width = 99\n"       /* 8 out of range */
                       "corner_radius = round\n"   /* 9 not a number */
                       "button_size = 2\n"         /* 10 too small */
                       "[shadow]\n"                /* 11 */
                       "enabled = perhaps\n"       /* 12 */
                       "radius = 10\n"             /* 13 ok */
                       "[font]\n"                  /* 14 */
                       "size = 0\n"                /* 15 too small */
                       "no equals sign here\n"     /* 16 syntax */
                       "[other]\n"                 /* 17 */
                       "x = 1\n";                  /* 18 */
    struct theme t, ref;
    struct log l = {0};
    theme_init_default(&t);
    theme_init_default(&ref);
    CHECK(theme_load_string(&t, text, collect, &l));
    CHECK(has_msg(&l, INI_WARNING, 2, "unknown key 'colour'"));
    CHECK(has_msg(&l, INI_ERROR, 4, "border_focused"));
    CHECK(has_msg(&l, INI_WARNING, 6, "unknown key 'shiny' in [colors]"));
    CHECK(has_msg(&l, INI_ERROR, 8, "border_width"));
    CHECK(has_msg(&l, INI_ERROR, 9, "corner_radius"));
    CHECK(has_msg(&l, INI_ERROR, 10, "button_size"));
    CHECK(has_msg(&l, INI_ERROR, 12, "enabled"));
    CHECK(has_msg(&l, INI_ERROR, 15, "size"));
    CHECK(has_msg(&l, INI_ERROR, 16, "syntax"));
    CHECK(has_msg(&l, INI_WARNING, 18, "unknown section [other]"));
    /* invalid values kept the defaults, valid ones applied */
    CHECK(color_eq(t.border_focused, ref.border_focused));
    CHECK(t.border_width == ref.border_width && t.corner_radius == ref.corner_radius);
    CHECK(t.button_size == ref.button_size && t.font_size == ref.font_size);
    CHECK(color_eq(t.title_text, (struct color){0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f, 1}));
    CHECK(t.shadow_radius == 10);
    theme_finish(&t);

    /* unsupported format */
    struct log l2 = {0};
    theme_init_default(&t);
    theme_load_string(&t, "format = 2\n", collect, &l2);
    CHECK(has_msg(&l2, INI_ERROR, 1, "unsupported theme format 2"));
    theme_finish(&t);
    theme_finish(&ref);
}

/* name and font family end up in shell commands: only plain text is accepted */
static void test_unsafe_text(void)
{
    const char *text = "name = x; rm -rf ~\n"                     /* 1 rejected */
                       "[font]\n"                                 /* 2 */
                       "family = sans$(touch /tmp/pwned)\n"       /* 3 rejected */
                       "family = Noto Sans CJK JP\n"              /* 4 ok */
                       "family = M+ 1p, DejaVu Sans_Mono-2.37\n"  /* 5 ok */
                       "family = \"sans\"\n"                      /* 6 rejected */
                       "family = Ünïcödé Sans\n";                 /* 7 ok (UTF-8) */
    struct theme t;
    struct log l = {0};
    theme_init_default(&t);
    CHECK(theme_load_string(&t, text, collect, &l));
    CHECK(has_msg(&l, INI_ERROR, 1, "name"));
    CHECK(has_msg(&l, INI_ERROR, 3, "family"));
    CHECK(has_msg(&l, INI_ERROR, 6, "family"));
    CHECK(l.n == 3);
    CHECK(!strcmp(t.name, "default"));              /* rejected value kept the old one */
    CHECK(!strcmp(t.font_family, "Ünïcödé Sans")); /* last valid value wins */
    theme_finish(&t);
}

static void test_find(void)
{
    char dir[] = "/tmp/sfwc-theme-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char tdir[300], file[400];
    snprintf(tdir, sizeof tdir, "%s/themes", dir);
    CHECK(mkdir(tdir, 0700) == 0);
    snprintf(file, sizeof file, "%s/mine.theme", tdir);
    FILE *f = fopen(file, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("format = 1\n", f);
        fclose(f);
    }
    char *p = theme_find("mine", dir);
    CHECK(p && !strcmp(p, file));
    free(p);
    CHECK(theme_find("nope-not-there", dir) == NULL);
    CHECK(theme_find("../mine", dir) == NULL); /* no path tricks */
    CHECK(theme_find("a/b", dir) == NULL);
    CHECK(theme_find("", dir) == NULL);
    unlink(file);
    rmdir(tdir);
    rmdir(dir);
}

int main(void)
{
    test_color_parse();
    test_shipped_default_matches_builtin();
    test_shipped_light();
    test_errors();
    test_unsafe_text();
    test_find();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_theme: all checks passed\n");
    return 0;
}
