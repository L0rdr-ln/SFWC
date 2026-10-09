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
    CHECK(c.n_binds == 19 && c.n_mbinds == 2 && c.n_autostart == 0);
    CHECK(c.snap_to_windows && c.repeat_rate == 25 && c.repeat_delay == 600);
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
    CHECK(c.n_binds == 19 && c.n_mbinds == 2);
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

static void test_keyboard_and_outputs(void)
{
    struct config c;
    struct log l = {0};
    config_init_defaults(&c);
    CHECK(c.repeat_rate == 25 && c.repeat_delay == 600 && c.kb_layout == NULL);
    CHECK(config_load_string(&c,
                             "[keyboard]\n"                       /* 1 */
                             "layout = de\n"                      /* 2 */
                             "variant = nodeadkeys\n"             /* 3 */
                             "options = caps:escape\n"            /* 4 */
                             "repeat_rate = 40\n"                 /* 5 */
                             "repeat_delay = lots\n"              /* 6 bad */
                             "colour = red\n"                     /* 7 unknown */
                             "[output:HDMI-A-1]\n"                /* 8 */
                             "scale = 1.5\n"                      /* 9 */
                             "position = -1920, 0\n"              /* 10 */
                             "[output:DP-1]\n"                    /* 11 */
                             "scale = 99\n"                       /* 12 bad */
                             "position = left\n"                  /* 13 bad */
                             "enabled = no\n"                     /* 14 */
                             "mode = 1920x1080\n"                 /* 15 unknown */
                             "[output:]\n"                        /* 16 */
                             "scale = 2\n"                        /* 17 bad section */
                             "[output:HDMI-A-1]\n"                /* 18 merges */
                             "enabled = true\n"
                             "[windows]\nsnap_to_windows = off\n",
                             collect, &l));
    CHECK(!strcmp(c.kb_layout, "de") && !strcmp(c.kb_variant, "nodeadkeys") &&
          !strcmp(c.kb_options, "caps:escape") && c.kb_model == NULL);
    CHECK(c.repeat_rate == 40 && c.repeat_delay == 600);
    CHECK(has_msg(&l, CONFIG_ERROR, 6, "repeat_delay"));
    CHECK(has_msg(&l, CONFIG_WARNING, 7, "unknown key 'colour' in [keyboard]"));
    CHECK(has_msg(&l, CONFIG_ERROR, 12, "scale"));
    CHECK(has_msg(&l, CONFIG_ERROR, 13, "position"));
    CHECK(has_msg(&l, CONFIG_WARNING, 15, "unknown key 'mode' in [output:DP-1]"));
    CHECK(has_msg(&l, CONFIG_ERROR, 17, "needs an output name")); /* reported on the first key */
    CHECK(c.n_outputs == 2); /* HDMI-A-1 merged, DP-1, the empty name was rejected */
    const struct output_cfg *o = config_find_output(&c, "HDMI-A-1");
    CHECK(o && o->scale == 1.5 && o->has_pos && o->x == -1920 && o->y == 0 && o->enabled);
    o = config_find_output(&c, "DP-1");
    CHECK(o && o->scale == 0 && !o->has_pos && !o->enabled);
    CHECK(!config_find_output(&c, "eDP-1"));
    CHECK(!c.snap_to_windows);
    /* empty value resets to the xkb default */
    CHECK(config_load_string(&c, "[keyboard]\nlayout =\n", NULL, NULL));
    CHECK(c.kb_layout == NULL);
    config_finish(&c);

    /* new actions */
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[keybinds]\nAlt+o = move-to-next-output\nAlt+Shift+o = focus-next-output\n", NULL, NULL));
    const struct keybind *b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_o);
    CHECK(b && b->action == ACTION_MOVE_OUTPUT);
    b = config_find_keybind(&c, CFG_MOD_ALT | CFG_MOD_SHIFT, XKB_KEY_o);
    CHECK(b && b->action == ACTION_FOCUS_OUTPUT);
    config_finish(&c);

    /* [templates] */
    config_init_defaults(&c);
    CHECK(c.templates_enabled && c.n_templates_off == 0);
    CHECK(config_load_string(&c, "[templates]\nwaybar = false\nfuzzel = false\nfuzzel = true\nfoot = no\n", NULL, NULL));
    CHECK(c.templates_enabled && c.n_templates_off == 2);
    int has_waybar = 0, has_foot = 0, has_fuzzel = 0;
    for (size_t i = 0; i < c.n_templates_off; i++) {
        has_waybar |= !strcmp(c.templates_off[i], "waybar");
        has_foot |= !strcmp(c.templates_off[i], "foot");
        has_fuzzel |= !strcmp(c.templates_off[i], "fuzzel");
    }
    CHECK(has_waybar && has_foot && !has_fuzzel);
    CHECK(config_load_string(&c, "[templates]\nenabled = false\nbad name = true\n", NULL, NULL));
    CHECK(!c.templates_enabled);
    config_finish(&c);

    /* workspaces */
    config_init_defaults(&c);
    CHECK(c.workspaces == 4);
    b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_3);
    CHECK(b && b->action == ACTION_WORKSPACE && !strcmp(b->arg, "3"));
    b = config_find_keybind(&c, CFG_MOD_ALT | CFG_MOD_SHIFT, XKB_KEY_2);
    CHECK(b && b->action == ACTION_MOVE_WORKSPACE && !strcmp(b->arg, "2"));
    CHECK(config_load_string(&c, "[general]\nworkspaces = 6\n[keybinds]\nAlt+5 = workspace:5\nAlt+6 = move-to-workspace: 6\n", NULL, NULL));
    CHECK(c.workspaces == 6);
    b = config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_6);
    CHECK(b && b->action == ACTION_MOVE_WORKSPACE && !strcmp(b->arg, "6"));
    /* bad numbers are rejected, a section replaces the defaults */
    CHECK(config_load_string(&c, "[general]\nworkspaces = 12\n", NULL, NULL));
    CHECK(c.workspaces == 6);
    CHECK(config_load_string(&c, "[keybinds]\nAlt+1 = workspace:0\nAlt+2 = workspace:x\nAlt+3 = workspace:10\n", NULL, NULL));
    CHECK(!config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_1));
    CHECK(!config_find_keybind(&c, CFG_MOD_ALT, XKB_KEY_3));
    config_finish(&c);
}

static void test_animation_rules(void)
{
    struct config c;
    struct log l = {0};
    struct anim_rule r;
    struct anim_curve cv;

    /* nothing configured: no rule, the legacy keys apply */
    config_init_defaults(&c);
    CHECK(!config_anim_rule(&c, ANIMT_WINDOWS_IN, &r));
    CHECK(config_find_curve(&c, "linear", &cv) && !config_find_curve(&c, "myBezier", &cv));

    /* a Hyprland style block */
    const char *text =
        "[animations]\n"
        "bezier = myBezier, 0.05, 0.9, 0.1, 1.05\n"         /* 2 */
        "animation = windows, 1, 7, myBezier\n"             /* 3 */
        "animation = windowsOut, 1, 5, default, popin 80%\n" /* 4 */
        "animation = border, 1, 10, default\n"              /* 5 */
        "animation = fade, 0\n"                             /* 6 */
        "animation = workspaces, 1, 6, default, slidefade 20%\n" /* 7 */
        "animation = windowsIn, 1, 4, myBezier, slide left\n"    /* 8 */
        "animation = borderangle, 1, 8, default, loop\n";   /* 9 */
    CHECK(config_load_string(&c, text, collect, &l));
    CHECK(l.n == 1 && has_msg(&l, CONFIG_WARNING, 9, "not supported"));
    CHECK(config_find_curve(&c, "myBezier", &cv) && cv.y1 == 1.05);
    /* `windows` set all three, windowsIn/windowsOut then replaced their own */
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_MOVE, &r) && r.on && r.speed == 7 && !strcmp(r.curve, "myBezier"));
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.speed == 5 && r.style == ANIM_STYLE_POPIN && r.percent == 80);
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && r.speed == 4 && r.style == ANIM_STYLE_SLIDE && r.dir == ANIM_DIR_LEFT);
    CHECK(config_anim_rule(&c, ANIMT_BORDER, &r) && r.on && r.speed == 10);
    CHECK(config_anim_rule(&c, ANIMT_FADE_IN, &r) && !r.on && config_anim_rule(&c, ANIMT_FADE_OUT, &r) && !r.on);
    CHECK(config_anim_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDEFADE && r.percent == 20);
    CHECK(!config_anim_rule(&c, ANIMT_LAYERS_IN, &r));

    /* the global rule is the default for types without their own */
    l.n = 0;
    CHECK(config_load_string(&c, "[animations]\nanimation = global, 1, 3, linear\n", collect, &l));
    CHECK(l.n == 0);
    CHECK(config_anim_rule(&c, ANIMT_LAYERS_IN, &r) && r.speed == 3 && r.style == ANIM_STYLE_DEFAULT);
    CHECK(config_anim_rule(&c, ANIMT_BORDER, &r) && r.speed == 10); /* its own rule wins */
    config_finish(&c);

    /* a later bezier with the same name replaces the earlier one */
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\nbezier = a, 0, 0, 1, 1\nbezier = a, 0.1, 0.2, 0.3, 0.4\n", NULL, NULL));
    CHECK(c.n_curves == 1 && config_find_curve(&c, "a", &cv) && cv.y0 == 0.2);
    config_finish(&c);

    /* mistakes: each is reported on its line and changes nothing */
    config_init_defaults(&c);
    memset(&l, 0, sizeof l);
    text = "[animations]\n"
           "bezier = x, 0, 0, 1\n"                      /* 2 too few values */
           "bezier = x, 1.5, 0, 0.5, 1\n"               /* 3 x out of range */
           "bezier = x, 0, 0, a, 1\n"                   /* 4 not a number */
           "bezier = bad name!, 0, 0, 1, 1\n"           /* 5 bad name */
           "animation = windows, 1, 4, nosuchcurve\n"   /* 6 unknown curve */
           "animation = windows, 1, fast, linear\n"     /* 7 bad speed */
           "animation = windows, 1, 0, linear\n"        /* 8 speed too small */
           "animation = windows, maybe, 4, linear\n"    /* 9 bad on/off */
           "animation = teleport, 1, 4, linear\n"       /* 10 unknown type */
           "animation = windows, 1, 4, linear, spin\n"  /* 11 unknown style */
           "animation = border, 1, 4, linear, popin\n"  /* 12 style not allowed */
           "animation = windowsIn, 1, 4, linear, popin 80\n" /* 13 percent needs % */
           "animation = windowsIn, 1, 4, linear, slide 50%\n" /* 14 slide takes a direction */
           "animation = workspaces, 1, 4, linear, popin\n"   /* 15 not for workspaces */
           "animation = global, 1, 4, linear, fade\n"        /* 16 global takes no style */
           "animation = windows, 1\n"                        /* 17 speed and curve missing */
           "preset = flashy\n";                              /* 18 */
    CHECK(config_load_string(&c, text, collect, &l));
    CHECK(c.n_curves == 0 && !config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && !c.anim_global.set);
    CHECK(has_msg(&l, CONFIG_ERROR, 2, "name, x0"));
    CHECK(has_msg(&l, CONFIG_ERROR, 3, "between 0 and 1"));
    CHECK(has_msg(&l, CONFIG_ERROR, 4, "not a number"));
    CHECK(has_msg(&l, CONFIG_ERROR, 5, "curve name"));
    CHECK(has_msg(&l, CONFIG_ERROR, 6, "unknown curve"));
    CHECK(has_msg(&l, CONFIG_ERROR, 7, "speed"));
    CHECK(has_msg(&l, CONFIG_ERROR, 8, "speed"));
    CHECK(has_msg(&l, CONFIG_ERROR, 9, "0/1"));
    CHECK(has_msg(&l, CONFIG_ERROR, 10, "unknown type"));
    CHECK(has_msg(&l, CONFIG_ERROR, 11, "bad style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 12, "does not apply"));
    CHECK(has_msg(&l, CONFIG_ERROR, 13, "bad style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 14, "bad style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 15, "does not apply"));
    CHECK(has_msg(&l, CONFIG_ERROR, 16, "no style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 17, "needs speed"));
    CHECK(has_msg(&l, CONFIG_ERROR, 18, "unknown preset"));
    config_finish(&c);

    /* `animation = windows, 1, 4, linear, popin 80%` is fine: the style only applies where it can */
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\nanimation = windows, 1, 4, linear, popin 80%\n", collect, &(struct log){0}));
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && r.style == ANIM_STYLE_POPIN && r.percent == 80);
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_MOVE, &r) && r.style == ANIM_STYLE_DEFAULT && r.percent == 0);
    config_finish(&c);

    /* presets */
    config_init_defaults(&c);
    memset(&l, 0, sizeof l);
    CHECK(config_load_string(&c, "[animations]\npreset = hyprland\nanimation = border, 0\n", collect, &l));
    CHECK(l.n == 0);
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && r.on && r.style == ANIM_STYLE_POPIN && r.percent == 87);
    CHECK(config_anim_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDE);
    CHECK(config_anim_rule(&c, ANIMT_BORDER, &r) && !r.on); /* a line after the preset wins */
    CHECK(config_find_curve(&c, "easeOutQuint", &cv) && cv.x0 == 0.23);
    CHECK(c.anim_enabled);
    config_finish(&c);
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\npreset = none\n", NULL, NULL));
    for (int i = 0; i < ANIMT_COUNT; i++) {
        CHECK(config_anim_rule(&c, i, &r) && !r.on);
    }
    config_finish(&c);
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\npreset = minimal\n", NULL, NULL));
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && r.percent == 95);
    config_finish(&c);

    /* Wayfire style effects, and which style takes which argument */
    config_init_defaults(&c);
    CHECK(c.fire_particles == 400 && c.fire_size == 14 && c.fire_color == 0xff7a18);
    memset(&l, 0, sizeof l);
    text = "[animations]\n"
           "animation = windowsOut, 1, 6, ease, fire\n"            /* 2 */
           "animation = windowsIn, 1, 4, ease, squeeze\n"          /* 3 */
           "animation = windowsMove, 1, 3, ease\n"                 /* 4 */
           "animation = windowsOut, 1, 6, ease, fire 50%\n"        /* 5 fire takes no argument */
           "animation = windowsIn, 1, 6, ease, squeeze left\n"     /* 6 squeeze takes none either */
           "animation = windowsIn, 1, 6, ease, zoom 70%\n"         /* 7 ok */
           "animation = windowsIn, 1, 6, ease, slide 50%\n"        /* 8 a window slide has a direction */
           "animation = workspaces, 1, 6, ease, slide 50%\n"       /* 9 a workspace slide a percentage */
           "animation = workspaces, 1, 6, ease, slide left\n"      /* 10 not a direction */
           "animation = border, 1, 6, ease, fire\n"                /* 11 not for borders */
           "fire_particles = 900\nfire_size = 20\nfire_color = #3060ff\n"
           "fire_particles = 5\nfire_size = 500\nfire_color = red\n"; /* 15-17 out of range / bad */
    CHECK(config_load_string(&c, text, collect, &l));
    CHECK(has_msg(&l, CONFIG_ERROR, 5, "bad style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 6, "bad style"));
    CHECK(has_msg(&l, CONFIG_ERROR, 8, "left, right"));
    CHECK(has_msg(&l, CONFIG_ERROR, 10, "percentage"));
    CHECK(has_msg(&l, CONFIG_ERROR, 11, "does not apply"));
    CHECK(has_msg(&l, CONFIG_ERROR, 15, "between"));
    CHECK(has_msg(&l, CONFIG_ERROR, 16, "between"));
    CHECK(has_msg(&l, CONFIG_ERROR, 17, "#rrggbb"));
    CHECK(l.n == 8);
    CHECK(c.fire_particles == 900 && c.fire_size == 20 && c.fire_color == 0x3060ff); /* bad ones changed nothing */
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.style == ANIM_STYLE_FIRE && r.speed == 6);
    CHECK(config_anim_rule(&c, ANIMT_WORKSPACES, &r) && r.style == ANIM_STYLE_SLIDE && r.percent == 50);
    config_finish(&c);
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\nanimation = windowsIn, 1, 6, ease, zoom 70%\nanimation = windowsOut, 1, 6, ease, squeeze\n", NULL, NULL));
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_IN, &r) && r.style == ANIM_STYLE_ZOOM && r.percent == 70);
    CHECK(config_anim_rule(&c, ANIMT_WINDOWS_OUT, &r) && r.style == ANIM_STYLE_SQUEEZE);
    config_finish(&c);

    /* the legacy keys still work next to the new ones */
    config_init_defaults(&c);
    CHECK(config_load_string(&c, "[animations]\nopen = slide\nduration_ms = 400\nanimation = border, 1, 5, ease\n", NULL, NULL));
    CHECK(!strcmp(c.anim_open, "slide") && c.anim_duration_ms == 400 && c.anim[ANIMT_BORDER].on);
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
    CHECK(c.n_binds == 19); /* defaults untouched */
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
    test_keyboard_and_outputs();
    test_animation_rules();
    test_expand();
    test_missing_file();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_config: all checks passed\n");
    return 0;
}
