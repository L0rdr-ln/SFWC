#define _DEFAULT_SOURCE
#include "deco.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#define GRIP_MIN 5

struct deco_insets deco_insets(const struct theme *t)
{
    return (struct deco_insets){t->border_width, t->border_width,
                                t->border_width + t->titlebar_height, t->border_width};
}

/* ------------------------------------------------------------------ buttons */

static int button_margin(const struct theme *t)
{
    int m = (t->titlebar_height - t->button_size) / 2;
    return m < 6 ? 6 : m;
}

bool deco_button_center(const struct theme *t, int cw, enum deco_part button, double *cx,
                        double *cy)
{
    if (t->titlebar_height <= 0 || cw < 60) {
        return false;
    }
    int slot;
    switch (button) {
    case DECO_CLOSE:
        slot = 0;
        break;
    case DECO_MAXIMIZE:
        slot = 1;
        break;
    case DECO_MINIMIZE:
        slot = 2;
        break;
    default:
        return false;
    }
    if (slot > 0 && cw < 120) {
        return false; /* only the close button fits */
    }
    double step = t->button_size + t->button_spacing;
    *cx = cw - button_margin(t) - t->button_size / 2.0 - slot * step;
    *cy = -t->titlebar_height / 2.0;
    return true;
}

/* ---------------------------------------------------------------- hit-testing */

enum deco_part deco_hit_test(const struct theme *t, int cw, int ch, double x, double y,
                             uint32_t *edges)
{
    struct deco_insets in = deco_insets(t);
    uint32_t e = 0;
    if (edges) {
        *edges = 0;
    }
    if (x < -in.left || x >= cw + in.right || y < -in.top || y >= ch + in.bottom) {
        return DECO_OUTSIDE;
    }
    if (x >= 0 && x < cw && y >= 0 && y < ch) {
        return DECO_CLIENT;
    }

    static const enum deco_part buttons[] = {DECO_CLOSE, DECO_MAXIMIZE, DECO_MINIMIZE};
    for (int i = 0; i < 3; i++) {
        double cx, cy;
        if (deco_button_center(t, cw, buttons[i], &cx, &cy)) {
            double r = t->button_size / 2.0 + 2;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                return buttons[i];
            }
        }
    }

    int grip = t->border_width > GRIP_MIN ? t->border_width : GRIP_MIN;
    if (y < -in.top + grip) {
        e |= DECO_EDGE_TOP;
    }
    if (y >= ch + in.bottom - grip) {
        e |= DECO_EDGE_BOTTOM;
    }
    if (x < -in.left + grip) {
        e |= DECO_EDGE_LEFT;
    }
    if (x >= cw + in.right - grip) {
        e |= DECO_EDGE_RIGHT;
    }
    if (edges) {
        *edges = e;
    }
    if (e) {
        return DECO_BORDER;
    }
    if (y < 0 && y >= -t->titlebar_height) {
        return DECO_TITLE;
    }
    return DECO_BORDER; /* thin border strip between grips: no resize zone */
}

/* ----------------------------------------------------------------- rendering */

static void set_color(cairo_t *cr, struct color c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

/* Rounded rectangle with individual corner radii. */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double tl, double tr,
                  double br, double bl)
{
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + tl, y);
    cairo_line_to(cr, x + w - tr, y);
    if (tr > 0) {
        cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
    }
    cairo_line_to(cr, x + w, y + h - br);
    if (br > 0) {
        cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
    }
    cairo_line_to(cr, x + bl, y + h);
    if (bl > 0) {
        cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
    }
    cairo_line_to(cr, x, y + tl);
    if (tl > 0) {
        cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
    }
    cairo_close_path(cr);
}

/* Outer corner radii; the bottom corners shrink so the (square) client content never
 * pokes out of the rounded border. */
static void corner_radii(const struct theme *t, int ow, int oh, double *top, double *bottom)
{
    double r = t->corner_radius;
    double max_r = (ow < oh ? ow : oh) / 2.0;
    if (r > max_r) {
        r = max_r;
    }
    *top = r;
    double limit = t->border_width / 0.2929; /* content corner stays inside the arc */
    *bottom = r < limit ? r : floor(limit);
}

static struct color mix_color(struct color a, struct color b, double f)
{
    if (f <= 0) {
        return a;
    }
    if (f >= 1) {
        return b;
    }
    return (struct color){(float)(a.r + (b.r - a.r) * f), (float)(a.g + (b.g - a.g) * f),
                         (float)(a.b + (b.b - a.b) * f), (float)(a.a + (b.a - a.a) * f)};
}

static void box_blur_pass(unsigned char *data, int w, int h, int stride, int radius, bool horizontal);

static struct color shade(struct color c, double f) /* f > 0 lighter, f < 0 darker */
{
    if (f >= 0) {
        return (struct color){(float)(c.r + (1 - c.r) * f), (float)(c.g + (1 - c.g) * f),
                             (float)(c.b + (1 - c.b) * f), c.a};
    }
    return (struct color){(float)(c.r * (1 + f)), (float)(c.g * (1 + f)), (float)(c.b * (1 + f)), c.a};
}

/* A glossy round button: a lit sphere with a rim and a glint on the upper half. */
static void draw_orb(cairo_t *cr, double cx, double cy, double d, struct color c)
{
    double r = d / 2.0;
    cairo_pattern_t *p = cairo_pattern_create_radial(cx - r * 0.3, cy - r * 0.45, r * 0.1, cx, cy, r);
    struct color hi = shade(c, 0.55), lo = shade(c, -0.35);
    cairo_pattern_add_color_stop_rgba(p, 0.0, hi.r, hi.g, hi.b, c.a);
    cairo_pattern_add_color_stop_rgba(p, 0.55, c.r, c.g, c.b, c.a);
    cairo_pattern_add_color_stop_rgba(p, 1.0, lo.r, lo.g, lo.b, c.a);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * c.a); /* rim */
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    /* glint: a lens shape on the upper half */
    cairo_save(cr);
    cairo_translate(cr, cx, cy - r * 0.42);
    cairo_scale(cr, r * 0.72, r * 0.42);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(cx, cy - r, cx, cy);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.85 * c.a);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.10 * c.a);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

cairo_surface_t *deco_render_chrome(const struct theme *t, int cw, int ch, double focus,
                                    const char *title, double scale)
{
    int bw = t->border_width, th = t->titlebar_height;
    int ow = cw + 2 * bw, oh = ch + th + 2 * bw;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(ow * scale), (int)ceil(oh * scale));
    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, scale, scale);

    double r_top, r_bottom;
    corner_radii(t, ow, oh, &r_top, &r_bottom);

    /* border: the whole outer shape */
    set_color(cr, mix_color(t->border_unfocused, t->border_focused, focus));
    rrect(cr, 0, 0, ow, oh, r_top, r_top, r_bottom, r_bottom);
    cairo_fill(cr);

    /* titlebar */
    if (th > 0) {
        double rt = r_top - bw;
        if (rt < 0) {
            rt = 0;
        }
        struct color tb = mix_color(t->titlebar_unfocused, t->titlebar_focused, focus);
        rrect(cr, bw, bw, cw, th, rt, rt, 0, 0);
        if (t->gradient > 0) {
            struct color top = shade(tb, 0.35 * t->gradient), bot = shade(tb, -0.25 * t->gradient);
            cairo_pattern_t *p = cairo_pattern_create_linear(0, bw, 0, bw + th);
            cairo_pattern_add_color_stop_rgba(p, 0, top.r, top.g, top.b, tb.a);
            cairo_pattern_add_color_stop_rgba(p, 1, bot.r, bot.g, bot.b, tb.a);
            cairo_set_source(cr, p);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        } else {
            set_color(cr, tb);
            cairo_fill(cr);
        }
        if (t->gloss > 0) {
            /* the classic glass band: the upper half, its lower edge curving down at the right */
            cairo_save(cr);
            rrect(cr, bw, bw, cw, th, rt, rt, 0, 0);
            cairo_clip(cr);
            cairo_new_path(cr);
            cairo_move_to(cr, bw, bw);
            cairo_line_to(cr, bw + cw, bw);
            cairo_line_to(cr, bw + cw, bw + th * 0.62);
            cairo_curve_to(cr, bw + cw * 0.7, bw + th * 0.40, bw + cw * 0.3, bw + th * 0.52, bw, bw + th * 0.40);
            cairo_close_path(cr);
            cairo_pattern_t *g = cairo_pattern_create_linear(0, bw, 0, bw + th * 0.6);
            double a = (focus * 0.6 + 0.4) * t->gloss;
            cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.65 * a);
            cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.12 * a);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            cairo_restore(cr);
        }
    }

    if (t->inner_highlight > 0) { /* a thin light line just inside the border: the glass edge */
        double inset = bw + 0.5;
        if (ow > 2 * inset && oh > 2 * inset) {
            double ri = r_top - bw > 0 ? r_top - bw : 0;
            rrect(cr, inset, inset, ow - 2 * inset, oh - 2 * inset, ri, ri, 0, 0);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.55 * t->inner_highlight);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }
    }

    /* content area stays transparent (a translucent client shows what is behind it) */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, bw, bw + th, cw, ch);
    cairo_fill(cr);
    cairo_restore(cr);

    if (th > 0) {
        /* buttons: coordinates from deco_button_center are relative to the content */
        static const enum deco_part buttons[] = {DECO_CLOSE, DECO_MAXIMIZE, DECO_MINIMIZE};
        double left_most = cw - 10;
        for (int i = 0; i < 3; i++) {
            double cx, cy;
            if (!deco_button_center(t, cw, buttons[i], &cx, &cy)) {
                continue;
            }
            struct color c = buttons[i] == DECO_CLOSE      ? t->close_button
                             : buttons[i] == DECO_MAXIMIZE ? t->maximize_button
                                                           : t->minimize_button;
            struct color bc = mix_color(t->border_unfocused, c, focus); /* dimmed when unfocused */
            if (t->orb_buttons) {
                draw_orb(cr, bw + cx, bw + th + cy, t->button_size, bc);
            } else {
                set_color(cr, bc);
                cairo_arc(cr, bw + cx, bw + th + cy, t->button_size / 2.0, 0, 2 * M_PI);
                cairo_fill(cr);
            }
            if (cx - t->button_size / 2.0 < left_most) {
                left_most = cx - t->button_size / 2.0;
            }
        }

        /* title text, left aligned, clipped before the buttons */
        if (title && *title) {
            double x0 = bw + 10;
            double avail = bw + left_most - t->button_spacing - x0;
            if (avail > 8) {
                PangoLayout *layout = pango_cairo_create_layout(cr);
                PangoFontDescription *fd = pango_font_description_new();
                pango_font_description_set_family(fd, t->font_family);
                pango_font_description_set_size(fd, t->font_size * PANGO_SCALE);
                pango_layout_set_font_description(layout, fd);
                pango_layout_set_text(layout, title, -1);
                pango_layout_set_width(layout, (int)(avail * PANGO_SCALE));
                pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
                pango_layout_set_single_paragraph_mode(layout, TRUE);
                int tw, thh;
                pango_layout_get_pixel_size(layout, &tw, &thh);
                if (t->text_glow.a > 0) { /* a soft halo behind the text keeps it readable on glass */
                    int g = t->text_glow_radius, mw = tw + 2 * g + 2, mh = thh + 2 * g + 2;
                    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, mw, mh);
                    cairo_t *mc = cairo_create(mask);
                    cairo_set_source_rgba(mc, 0, 0, 0, 1);
                    cairo_move_to(mc, g + 1, g + 1);
                    pango_cairo_show_layout(mc, layout);
                    cairo_destroy(mc);
                    cairo_surface_flush(mask);
                    unsigned char *md = cairo_image_surface_get_data(mask);
                    int ms = cairo_image_surface_get_stride(mask);
                    for (int pass = 0; pass < 2; pass++) {
                        box_blur_pass(md, mw, mh, ms, g > 1 ? g / 2 : 1, true);
                        box_blur_pass(md, mw, mh, ms, g > 1 ? g / 2 : 1, false);
                    }
                    cairo_surface_mark_dirty(mask);
                    set_color(cr, t->text_glow);
                    for (int k = 0; k < 3; k++) { /* the blur thins the halo out: stack it */
                        cairo_mask_surface(cr, mask, x0 - g - 1, bw + (th - thh) / 2.0 - g - 1);
                    }
                    cairo_surface_destroy(mask);
                }
                set_color(cr, t->title_text);
                cairo_move_to(cr, x0, bw + (th - thh) / 2.0);
                pango_cairo_show_layout(cr, layout);
                pango_font_description_free(fd);
                g_object_unref(layout);
            }
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    return surf;
}

/* One horizontal or vertical box-blur pass on an A8 image, edges treated as transparent. */
static void box_blur_pass(unsigned char *data, int w, int h, int stride, int radius, bool horizontal)
{
    int len = horizontal ? w : h;
    int lines = horizontal ? h : w;
    unsigned char *tmp = malloc(len);
    if (!tmp) {
        return;
    }
    int win = 2 * radius + 1;
    for (int l = 0; l < lines; l++) {
#define PIX(i) (horizontal ? data[l * stride + (i)] : data[(i) * stride + l])
        int sum = 0;
        for (int i = -radius; i <= radius; i++) {
            if (i >= 0 && i < len) {
                sum += PIX(i);
            }
        }
        for (int i = 0; i < len; i++) {
            tmp[i] = (unsigned char)(sum / win);
            int add = i + radius + 1, sub = i - radius;
            if (add < len) {
                sum += PIX(add);
            }
            if (sub >= 0) {
                sum -= PIX(sub);
            }
        }
        for (int i = 0; i < len; i++) {
            if (horizontal) {
                data[l * stride + i] = tmp[i];
            } else {
                data[i * stride + l] = tmp[i];
            }
        }
#undef PIX
    }
    free(tmp);
}

cairo_surface_t *deco_render_shadow(const struct theme *t, int outer_w, int outer_h, int divisor)
{
    if (divisor < 1) {
        divisor = 1;
    }
    int R = t->shadow_radius;
    int w = (outer_w + 2 * R + divisor - 1) / divisor;
    int h = (outer_h + 2 * R + divisor - 1) / divisor;
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *cr = cairo_create(mask);
    cairo_scale(cr, 1.0 / divisor, 1.0 / divisor);
    double r_top, r_bottom;
    corner_radii(t, outer_w, outer_h, &r_top, &r_bottom);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    rrect(cr, R, R, outer_w, outer_h, r_top, r_top, r_top, r_top);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(mask);

    int radius = R / divisor / 2;
    if (radius < 1) {
        radius = 1;
    }
    unsigned char *d = cairo_image_surface_get_data(mask);
    int stride = cairo_image_surface_get_stride(mask);
    for (int pass = 0; pass < 3; pass++) { /* three box blurs approximate a gaussian */
        box_blur_pass(d, w, h, stride, radius, true);
        box_blur_pass(d, w, h, stride, radius, false);
    }
    cairo_surface_mark_dirty(mask);

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cr = cairo_create(out);
    set_color(cr, t->shadow_color);
    cairo_mask_surface(cr, mask, 0, 0);
    cairo_destroy(cr);
    cairo_surface_destroy(mask);
    cairo_surface_flush(out);
    return out;
}
