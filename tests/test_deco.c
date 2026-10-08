/* Unit tests for src/deco.c: layout, hit-testing and the rendered pixels. */
#define _DEFAULT_SOURCE
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deco.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* un-premultiplied RGBA of a pixel */
static void pixel(cairo_surface_t *s, int x, int y, int *r, int *g, int *b, int *a)
{
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    uint32_t p = *(uint32_t *)(d + y * stride + x * 4);
    *a = p >> 24;
    int pr = (p >> 16) & 0xff, pg = (p >> 8) & 0xff, pb = p & 0xff;
    if (*a > 0 && *a < 255) {
        pr = pr * 255 / *a;
        pg = pg * 255 / *a;
        pb = pb * 255 / *a;
    }
    *r = pr;
    *g = pg;
    *b = pb;
}

static int is_color(cairo_surface_t *s, int x, int y, unsigned rgb)
{
    int r, g, b, a;
    pixel(s, x, y, &r, &g, &b, &a);
    return a == 255 && abs(r - (int)((rgb >> 16) & 0xff)) <= 2 && abs(g - (int)((rgb >> 8) & 0xff)) <= 2 &&
           abs(b - (int)(rgb & 0xff)) <= 2;
}

static int alpha_at(cairo_surface_t *s, int x, int y)
{
    int r, g, b, a;
    pixel(s, x, y, &r, &g, &b, &a);
    return a;
}

static void test_layout_and_hits(void)
{
    struct theme t;
    theme_init_default(&t); /* border 2, titlebar 28, button 14, spacing 8 */
    struct deco_insets in = deco_insets(&t);
    CHECK(in.left == 2 && in.right == 2 && in.top == 30 && in.bottom == 2);

    uint32_t e;
    const int cw = 200, ch = 100;
    CHECK(deco_hit_test(&t, cw, ch, 100, 50, &e) == DECO_CLIENT && e == 0);
    CHECK(deco_hit_test(&t, cw, ch, 100, -10, &e) == DECO_TITLE && e == 0);
    CHECK(deco_hit_test(&t, cw, ch, 186, -14, &e) == DECO_CLOSE);
    CHECK(deco_hit_test(&t, cw, ch, 164, -14, &e) == DECO_MAXIMIZE);
    CHECK(deco_hit_test(&t, cw, ch, 142, -14, &e) == DECO_MINIMIZE);
    CHECK(deco_hit_test(&t, cw, ch, 100, -29, &e) == DECO_BORDER && e == DECO_EDGE_TOP);
    CHECK(deco_hit_test(&t, cw, ch, -1, 50, &e) == DECO_BORDER && e == DECO_EDGE_LEFT);
    CHECK(deco_hit_test(&t, cw, ch, 201, 50, &e) == DECO_BORDER && e == DECO_EDGE_RIGHT);
    CHECK(deco_hit_test(&t, cw, ch, 100, 101, &e) == DECO_BORDER && e == DECO_EDGE_BOTTOM);
    CHECK(deco_hit_test(&t, cw, ch, -1, -29, &e) == DECO_BORDER && e == (DECO_EDGE_TOP | DECO_EDGE_LEFT));
    CHECK(deco_hit_test(&t, cw, ch, 201, 101, &e) == DECO_BORDER && e == (DECO_EDGE_BOTTOM | DECO_EDGE_RIGHT));
    CHECK(deco_hit_test(&t, cw, ch, 300, 0, &e) == DECO_OUTSIDE);
    CHECK(deco_hit_test(&t, cw, ch, 100, -31, &e) == DECO_OUTSIDE);

    /* narrow window: only the close button fits */
    double cx, cy;
    CHECK(deco_button_center(&t, 100, DECO_CLOSE, &cx, &cy));
    CHECK(!deco_button_center(&t, 100, DECO_MAXIMIZE, &cx, &cy));
    CHECK(!deco_button_center(&t, 50, DECO_CLOSE, &cx, &cy));

    /* no frame at all: nothing but the client */
    t.border_width = 0;
    t.titlebar_height = 0;
    in = deco_insets(&t);
    CHECK(in.left == 0 && in.top == 0 && in.bottom == 0 && in.right == 0);
    CHECK(deco_hit_test(&t, cw, ch, 10, 10, &e) == DECO_CLIENT);
    CHECK(deco_hit_test(&t, cw, ch, -1, 10, &e) == DECO_OUTSIDE);
    theme_finish(&t);
}

static void test_chrome_pixels(void)
{
    struct theme t;
    theme_init_default(&t);
    const int cw = 200, ch = 100;
    cairo_surface_t *s = deco_render_chrome(&t, cw, ch, true, "Hello", 1.0);
    CHECK(cairo_image_surface_get_width(s) == 204 && cairo_image_surface_get_height(s) == 132);

    CHECK(is_color(s, 102, 0, 0x89b4fa));  /* top border (focused) */
    CHECK(is_color(s, 1, 80, 0x89b4fa));   /* left border */
    CHECK(is_color(s, 102, 131, 0x89b4fa)); /* bottom border */
    CHECK(alpha_at(s, 0, 0) == 0);          /* rounded top-left corner is transparent */
    CHECK(alpha_at(s, 203, 0) == 0);        /* top-right */
    CHECK(alpha_at(s, 0, 131) == 0);        /* rounded bottom-left corner */
    CHECK(is_color(s, 100, 25, 0x313244));  /* titlebar, away from text and buttons */
    CHECK(alpha_at(s, 100, 80) == 0);       /* content area is transparent */
    CHECK(alpha_at(s, 3, 31) == 0);         /* content's top-left corner */
    CHECK(is_color(s, 188, 16, 0xf38ba8));  /* close button centre */
    CHECK(is_color(s, 166, 16, 0xa6e3a1));  /* maximize */
    CHECK(is_color(s, 144, 16, 0xf9e2af));  /* minimize */

    /* the title text puts pixels of another color into the titlebar (when fonts exist) */
    FcFontSet *fonts = FcConfigGetFonts(NULL, FcSetSystem);
    if (fonts && fonts->nfont > 0) {
        int differing = 0;
        for (int y = 8; y < 24; y++) {
            for (int x = 12; x < 60; x++) {
                if (!is_color(s, x, y, 0x313244)) {
                    differing++;
                }
            }
        }
        CHECK(differing > 20);
    } else {
        fprintf(stderr, "note: no fonts installed, title text is not checked\n");
    }
    cairo_surface_destroy(s);

    /* unfocused colors */
    s = deco_render_chrome(&t, cw, ch, false, "Hello", 1.0);
    CHECK(is_color(s, 102, 0, 0x45475a));
    CHECK(is_color(s, 100, 25, 0x1e1e2e));
    CHECK(is_color(s, 188, 16, 0x45475a)); /* buttons are dimmed when not focused */
    cairo_surface_destroy(s);

    /* scale 2 */
    s = deco_render_chrome(&t, cw, ch, true, NULL, 2.0);
    CHECK(cairo_image_surface_get_width(s) == 408 && cairo_image_surface_get_height(s) == 264);
    CHECK(is_color(s, 204, 1, 0x89b4fa));
    CHECK(is_color(s, 376, 32, 0xf38ba8));
    CHECK(alpha_at(s, 200, 160) == 0);
    cairo_surface_destroy(s);

    /* no corner radius: opaque corners */
    t.corner_radius = 0;
    s = deco_render_chrome(&t, cw, ch, true, NULL, 1.0);
    CHECK(is_color(s, 0, 0, 0x89b4fa) && is_color(s, 203, 131, 0x89b4fa));
    cairo_surface_destroy(s);

    /* translucent border color keeps its alpha */
    theme_load_string(&t, "[colors]\nborder_focused = #ff000080\n", NULL, NULL);
    s = deco_render_chrome(&t, cw, ch, true, NULL, 1.0);
    int r, g, b, a;
    pixel(s, 102, 0, &r, &g, &b, &a);
    CHECK(abs(a - 128) <= 2 && r >= 250 && g <= 3 && b <= 3);
    cairo_surface_destroy(s);
    theme_finish(&t);
}

static void test_shadow(void)
{
    struct theme t;
    theme_init_default(&t); /* radius 20, color #00000066 */
    cairo_surface_t *s = deco_render_shadow(&t, 204, 132, 4);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    CHECK(w == (204 + 40 + 3) / 4 && h == (132 + 40 + 3) / 4);
    int a_center = alpha_at(s, w / 2, h / 2);
    CHECK(abs(a_center - 0x66) <= 6);       /* solid under the window */
    CHECK(alpha_at(s, 0, 0) < 8);           /* fades out at the corner */
    CHECK(alpha_at(s, w / 2, 0) < 12);      /* and at the edge */
    int a_edge_in = alpha_at(s, w / 2, 5 + 2); /* 7 px (28 px at scale) into the margin */
    CHECK(a_edge_in > alpha_at(s, w / 2, 1));  /* monotonic fade-in */
    int r, g, b, a;
    pixel(s, w / 2, h / 2, &r, &g, &b, &a);
    CHECK(r <= 2 && g <= 2 && b <= 2); /* black shadow */
    cairo_surface_destroy(s);

    /* colored shadow */
    theme_load_string(&t, "[shadow]\ncolor = #3050ff80\n", NULL, NULL);
    s = deco_render_shadow(&t, 204, 132, 4);
    pixel(s, w / 2, h / 2, &r, &g, &b, &a);
    CHECK(abs(a - 128) <= 6 && abs(r - 0x30) <= 6 && abs(g - 0x50) <= 6 && abs(b - 0xff) <= 6);
    cairo_surface_destroy(s);
    theme_finish(&t);
}

int main(void)
{
    test_layout_and_hits();
    test_chrome_pixels();
    test_shadow();
    /* release pango/fontconfig globals so that leak checking only sees our own allocations */
    pango_cairo_font_map_set_default(NULL);
    cairo_debug_reset_static_data();
    FcFini();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_deco: all checks passed\n");
    return 0;
}
