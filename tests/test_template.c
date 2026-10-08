#define _DEFAULT_SOURCE
/* Unit tests for src/template.c (no compositor needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "template.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static char *render(const struct theme *t, const char *in, bool strict, char *err)
{
    err[0] = 0;
    return template_render(t, in, strict, err, 200);
}

int main(void)
{
    struct theme t;
    theme_init_default(&t);
    char v[128], err[200];

    /* values and formats */
    CHECK(theme_value(&t, "colors.border_focused", NULL, v, sizeof v) && !strcmp(v, "#89b4fa"));
    CHECK(theme_value(&t, "colors.border_focused", "hex", v, sizeof v) && !strcmp(v, "89b4fa"));
    CHECK(theme_value(&t, "colors.border_focused", "hexa", v, sizeof v) && !strcmp(v, "89b4faff"));
    CHECK(theme_value(&t, "colors.border_focused", "rgb", v, sizeof v) && !strcmp(v, "137, 180, 250"));
    CHECK(theme_value(&t, "colors.border_focused", "rgba", v, sizeof v) &&
          !strcmp(v, "rgba(137, 180, 250, 1.00)"));
    CHECK(theme_value(&t, "shadow.color", NULL, v, sizeof v) && !strcmp(v, "#00000066"));
    CHECK(theme_value(&t, "geometry.border_width", NULL, v, sizeof v) && !strcmp(v, "2"));
    CHECK(theme_value(&t, "shadow.enabled", NULL, v, sizeof v) && !strcmp(v, "true"));
    CHECK(theme_value(&t, "font.family", NULL, v, sizeof v) && !strcmp(v, "sans"));
    CHECK(theme_value(&t, "theme.name", NULL, v, sizeof v) && !strcmp(v, "default"));
    CHECK(!theme_value(&t, "colors.nope", NULL, v, sizeof v) && strstr(v, "unknown theme key"));
    CHECK(!theme_value(&t, "colors.title_text", "bogus", v, sizeof v) && strstr(v, "unknown modifier"));
    CHECK(!theme_value(&t, "font.size", "hex", v, sizeof v) && strstr(v, "not a color"));

    /* every key the docs and the shipped templates promise exists */
    static const char *const keys[] = {
        "colors.background", "colors.border_focused", "colors.border_unfocused",
        "colors.titlebar_focused", "colors.titlebar_unfocused", "colors.title_text",
        "colors.close_button", "colors.maximize_button", "colors.minimize_button",
        "geometry.border_width", "geometry.titlebar_height", "geometry.corner_radius",
        "geometry.button_size", "geometry.button_spacing", "shadow.enabled", "shadow.radius",
        "shadow.offset_y", "shadow.color", "font.family", "font.size", "theme.name",
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        CHECK(theme_value(&t, keys[i], NULL, v, sizeof v));
    }

    /* strict rendering (template files) */
    char *out = render(&t, "bg=@colors.background@ w=@geometry.border_width@px c=@colors.title_text:hex@\n", true, err);
    CHECK(out && !strcmp(out, "bg=#1e1e2e w=2px c=cdd6f4\n"));
    free(out);
    out = render(&t, "mail me@@example.org\n", true, err);
    CHECK(out && !strcmp(out, "mail me@example.org\n"));
    free(out);
    out = render(&t, "a\nb\nc=@colors.nope@\n", true, err);
    CHECK(!out && strstr(err, "line 3") && strstr(err, "unknown theme key"));
    out = render(&t, "x\n@colors.background\n", true, err);
    CHECK(!out && strstr(err, "line 2") && strstr(err, "closing"));
    out = render(&t, "@colors.background:bogus@", true, err);
    CHECK(!out && strstr(err, "unknown modifier"));
    out = render(&t, "no placeholders at all", true, err);
    CHECK(out && !strcmp(out, "no placeholders at all"));
    free(out);

    /* lenient rendering (commands in the config): foreign @ signs stay */
    out = render(&t, "swaybg -c @colors.background:hex@ ; ssh me@host @@ @colors.nope@ @", false, err);
    CHECK(out && !strcmp(out, "swaybg -c 1e1e2e ; ssh me@host @@ @colors.nope@ @"));
    free(out);

    /* a theme changes the output */
    struct theme light;
    theme_init_default(&light);
    CHECK(theme_load_string(&light, "[colors]\nbackground = #ffffff\n[font]\nfamily = Iosevka\n", NULL, NULL));
    out = render(&light, "@colors.background@ @font.family@", true, err);
    CHECK(out && !strcmp(out, "#ffffff Iosevka"));
    free(out);

    theme_finish(&t);
    theme_finish(&light);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("template tests: OK\n");
    return 0;
}
