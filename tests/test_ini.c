/*
 * Checks the parser rules documented in docs/THEMES.md and that every shipped
 * config/theme file parses without errors.
 */
#include <stdio.h>
#include <string.h>

#include "ini.h"

struct state {
    int entries;
    int color_ok;     /* values starting with '#' kept intact */
    int comment_leak; /* values that contain comment text */
};

static int handler(void *user, const char *section, const char *name, const char *value)
{
    struct state *st = user;
    (void)section;
    (void)name;
    st->entries++;
    if (value[0] == '#' && strlen(value) >= 7) {
        st->color_ok++;
    }
    if (strstr(value, "click |") || strstr(value, "ease-in |")) {
        st->comment_leak++;
    }
    return 1;
}

static int check(const char *path, int expect_colors)
{
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", SRC_ROOT, path);
    struct state st = {0};
    int rc = ini_parse(full, handler, &st);
    if (rc != 0) {
        fprintf(stderr, "FAIL %s: parse error at line %d\n", path, rc);
        return 1;
    }
    if (st.entries == 0 || st.comment_leak || (expect_colors && st.color_ok == 0)) {
        fprintf(stderr, "FAIL %s: entries=%d colors=%d leak=%d\n", path, st.entries, st.color_ok,
                st.comment_leak);
        return 1;
    }
    printf("ok   %s (%d entries, %d colors)\n", path, st.entries, st.color_ok);
    return 0;
}

static int check_string(void)
{
    /* '#' inside a value must NOT start an inline comment. */
    struct state st = {0};
    const char *s = "[colors]\n# full-line comment\n; another\nborder = #89b4fa\n";
    if (ini_parse_string(s, handler, &st) != 0 || st.entries != 1 || st.color_ok != 1) {
        fprintf(stderr, "FAIL inline '#' handling\n");
        return 1;
    }
    printf("ok   '#rrggbb' values survive\n");
    return 0;
}

int main(void)
{
    int fails = 0;
    fails += check_string();
    fails += check("config/sfwc.conf", 0);
    fails += check("themes/default.theme", 1);
    fails += check("themes/light.theme", 1);
    return fails ? 1 : 0;
}
