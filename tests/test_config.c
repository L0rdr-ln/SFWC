/* Unit tests for src/config.c (no compositor needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

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

static void dump(const struct log *l)
{
    for (int i = 0; i < l->n; i++) {
        fprintf(stderr, "  [%d] line %d: %s\n", l->m[i].level, l->m[i].line, l->m[i].text);
    }
}

static void test_defaults(void)
{
    struct config c;
    config_init_defaults(&c);
    CHECK(c.gap == 8 && c.snap_distance == 12 && c.snap_to_edges);
    CHECK(c.n_binds == 9 && c.n_mbinds == 2 && c.n_autostart == 0);
    const struct keybind *b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_q);
    CHECK(b && b->action == ACTION_CLOSE);
    b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_Q); /* case-insensitive on the key */
    CHECK(b && b->action == ACTION_CLOSE);
    b = config_find_keybind(&c, CFG_MOD_ALT | CFG_MOD_SHIFT, XKB_KEY_m);
    CHECK(b && b->action == ACTION_RESTORE);
    CHECK(!config_find_keybind(&c, 0, XKB_KEY_q));              /* modifier required */
    CHECK(!config_find_keybind(&c, CFG_MOD_ALT | 2, XKB_KEY_x)); /* nothing bound to x */
    b = config_find_keybind(&c, CFG_MOD_ALT | 2 /* caps lock is ignored */, XKB_KEY_f);
    CHECK(b && b->action == ACTION_TOGGLE_MAXIMIZE);
    const struct mousebind *mb = config_find_mousebind(&c, CFG_MOD_ALT, BTN_RIGHT);
    CHECK(mb && mb->action == ACTION_RESIZE);
    config_finish(&c);
}

static void test_shipped_config(void)
{
    struct config c;
    struct log l = {0};
    config_init_defaults(&c);
    char path[1024];
    snprintf(path, sizeof path, "%s/config/sfwc.conf", SRC_ROOT);
    CHECK(config_load_file(&c, path, collect, &l));
    if (l.n) {
        fprintf(stderr, "shipped config produced messages:\n");
        dump(&l);
    }
    CHECK(l.n == 0);
    CHECK(c.n_binds == 9 && c.n_mbinds == 2);
    const struct keybind *b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_Return);
    CHECK(b && b->action == ACTION_SPAWN && !strcmp(b->arg, "$terminal"));
    config_finish(&c);
}

static void test_errors_have_line_numbers(void)
{
    const char *text = "[general]\n"           /* 1 */
                       "focus = sideways\n"     /* 2 bad choice */
                       "colour = red\n"         /* 3 unknown key */
                       "[windows]\n"            /* 4 */
                       "gap = lots\n"           /* 5 bad number */
                       "snap_distance = 5000\n" /* 6 out of range */
                       "snap_to_edges = maybe\n" /* 7 bad bool */
                       "this is not valid\n"    /* 8 syntax */
                       "[nothing]\n"            /* 9 */
                       "x = 1\n"                /* 10 */
                       "[keybinds]\n"           /* 11 */
                       "Alt+q = frobnicate\n"   /* 12 bad action */
                       "Hyper+q = close\n"      /* 13 bad modifier */
                       "Alt+nosuchkey = close\n" /* 14 bad key */
                       "Alt+w = spawn:\n"       /* 15 empty command */
                       "Alt+e = move\n"         /* 16 mouse action on key */
                       "[mouse]\n"              /* 17 */
                       "Alt+Fourth = move\n"    /* 18 bad button */
                       "Alt+Left = close\n";    /* 19 key action on mouse */
    struct config c;
    struct log l = {0};
    config_init_defaults(&c);
    CHECK(config_load_string(&c, text, collect, &l));
    CHECK(has_msg(&l, CONFIG_ERROR, 2, "focus"));
    CHECK(has_msg(&l, CONFIG_WARNING, 3, "unknown key 'colour'"));
    CHECK(has_msg(&l, CONFIG_ERROR, 5, "gap"));
    CHECK(has_msg(&l, CONFIG_ERROR, 6, "snap_distance"));
    CHECK(has_msg(&l, CONFIG_ERROR, 7, "snap_to_edges"));
    CHECK(has_msg(&l, CONFIG_ERROR, 8, "syntax"));
    CHECK(has_msg(&l, CONFIG_WARNING, 10, "unknown section [nothing]"));
    CHECK(has_msg(&l, CONFIG_ERROR, 12, "unknown action"));
    CHECK(has_msg(&l, CONFIG_ERROR, 13, "unknown modifier"));
    CHECK(has_msg(&l, CONFIG_ERROR, 14, "unknown key name"));
    CHECK(has_msg(&l, CONFIG_ERROR, 15, "needs a command"));
    CHECK(has_msg(&l, CONFIG_ERROR, 16, "cannot be used"));
    CHECK(has_msg(&l, CONFIG_ERROR, 18, "unknown mouse button"));
    CHECK(has_msg(&l, CONFIG_ERROR, 19, "cannot be used"));
    if (failures) {
        dump(&l);
    }
    /* invalid values keep the defaults */
    CHECK(c.gap == 8 && c.snap_distance == 12 && c.snap_to_edges && c.focus == FOCUS_CLICK);
    config_finish(&c);
}

static void test_replace_and_last_wins(void)
{
    struct config c;
    config_init_defaults(&c);
    CHECK(config_load_string(&c,
                             "[keybinds]\nAlt+q = close\nAlt+q = quit\nCtrl+Alt+t = spawn:xterm -e top\n"
                             "[mouse]\nAlt+Middle = move\n[autostart]\nexec = a\nexec = b --x\n",
                             NULL, NULL));
    CHECK(c.n_binds == 3); /* defaults were replaced */
    const struct keybind *b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_q);
    CHECK(b && b->action == ACTION_QUIT);
    b = config_find_keybind(&c, CFG_MOD_CTRL | CFG_MOD_ALT, XKB_KEY_t);
    CHECK(b && b->action == ACTION_SPAWN && !strcmp(b->arg, "xterm -e top"));
    CHECK(!config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_f)); /* default gone */
    CHECK(c.n_mbinds == 1 && config_find_mousebind(&c, CFG_MOD_ALT, BTN_MIDDLE));
    CHECK(c.n_autostart == 2 && !strcmp(c.autostart[1], "b --x"));
    config_finish(&c);
}

static void test_mod_rebuilds_defaults(void)
{
    struct config c;
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[general]\nmod = Super\n", NULL, NULL));
    CHECK(c.mod == CFG_MOD_LOGO);
    CHECK(config_find_keybind(&c, CFG_MOD_LOGO, XKB_KEY_q));
    CHECK(!config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_q));
    CHECK(config_find_mousebind(&c, CFG_MOD_LOGO, BTN_LEFT));
    config_finish(&c);

    /* $mod in custom binds */
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[general]\nmod = Ctrl\n[keybinds]\n$mod+Shift+x = quit\n", NULL, NULL));
    CHECK(config_find_keybind(&c, CFG_MOD_CTRL | CFG_MOD_SHIFT, XKB_KEY_x));
    config_finish(&c);

    /* bad modifier name is an error and changes nothing */
    struct log l = {0};
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[general]\nmod = Hyper\n", collect, &l));
    CHECK(l.n >= 1 && l.m[0].line == 2 && c.mod == CFG_MOD_ALT);
    config_finish(&c);
}

static void test_values_and_hash_colors(void)
{
    struct config c;
    config_init_defaults(&c);
    CHECK(config_load_string(&c,
                             "[general]\nfocus = follow-mouse\nterminal = #notacomment\ntheme = nord\n"
                             "[windows]\ngap = 0\nsnap_to_edges = off\nsnap_distance = 30\n"
                             "[animations]\nenabled = no\nopen = slide\neasing = linear\nduration_ms = 500\n",
                             NULL, NULL));
    CHECK(c.focus == FOCUS_FOLLOW_MOUSE);
    CHECK(!strcmp(c.terminal, "#notacomment")); /* '#' inside a value is kept */
    CHECK(!strcmp(c.theme, "nord"));
    CHECK(c.gap == 0 && !c.snap_to_edges && c.snap_distance == 30);
    CHECK(!c.anim_enabled && !strcmp(c.anim_open, "slide") && !strcmp(c.anim_easing, "linear") &&
          c.anim_duration_ms == 500);
    config_finish(&c);
}

static void test_expand(void)
{
    struct config c;
    config_init_defaults(&c);
    config_load_string(&c, "[general]\nterminal = kitty\ntheme = nord\n", NULL, NULL);
    char *s = config_expand(&c, "$terminal -e 'echo $$HOME' $runtime/x $theme $unknown", "/run/user/1");
    CHECK(s && !strcmp(s, "kitty -e 'echo $HOME' /run/user/1/x nord $unknown"));
    free(s);
    s = config_expand(&c, "plain", NULL);
    CHECK(s && !strcmp(s, "plain"));
    free(s);
    config_finish(&c);
}

static void test_missing_file(void)
{
    struct config c;
    config_init_defaults(&c);
    CHECK(!config_load_file(&c, "/nonexistent/sfwc.conf", NULL, NULL));
    CHECK(c.n_binds == 9); /* defaults untouched */
    config_finish(&c);
}

int main(void)
{
    test_defaults();
    test_shipped_config();
    test_errors_have_line_numbers();
    test_replace_and_last_wins();
    test_mod_rebuilds_defaults();
    test_values_and_hash_colors();
    test_expand();
    test_missing_file();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_config: all checks passed\n");
    return 0;
}
