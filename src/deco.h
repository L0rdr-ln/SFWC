/*
 * Window decorations: frame geometry, hit-testing and software rendering (cairo + pango).
 * No wlroots dependency, so it is unit-tested on its own (tests/test_deco.c).
 *
 * Layout, in logical pixels. The *content* is the client's window geometry. Around it:
 *
 *     +--------------------------------+   outer frame: border_width on every side
 *     | titlebar (titlebar_height)  o o o |   buttons: minimize, maximize, close (right-aligned)
 *     +--------------------------------+
 *     |  content                       |
 *     +--------------------------------+
 *
 * Coordinates passed to deco_hit_test() are relative to the content's top-left corner.
 */
#ifndef SFWC_DECO_H
#define SFWC_DECO_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#include "theme.h"

/* Same values as WLR_EDGE_* (checked in main.c). */
#define DECO_EDGE_TOP 1u
#define DECO_EDGE_BOTTOM 2u
#define DECO_EDGE_LEFT 4u
#define DECO_EDGE_RIGHT 8u

struct deco_insets {
    int left, right, top, bottom;
};

enum deco_part {
    DECO_OUTSIDE,  /* not part of the window */
    DECO_CLIENT,   /* the content */
    DECO_TITLE,    /* titlebar: drag to move */
    DECO_BORDER,   /* border/corner: drag to resize (see edges) */
    DECO_CLOSE,
    DECO_MAXIMIZE,
    DECO_MINIMIZE,
};

/* Space the frame takes around the content. */
struct deco_insets deco_insets(const struct theme *t);

/* Which part of the window is at (x, y) relative to the content's top-left. For
 * DECO_BORDER and DECO_TITLE, *edges receives the DECO_EDGE_* mask of the resize
 * zone (0 for the plain titlebar). */
enum deco_part deco_hit_test(const struct theme *t, int content_w, int content_h, double x,
                             double y, uint32_t *edges);

/* Centre of a titlebar button relative to the content's top-left; false if hidden. */
bool deco_button_center(const struct theme *t, int content_w, enum deco_part button, double *cx,
                        double *cy);

/* Titlebar, borders and buttons as an ARGB32 image of the outer frame
 * ((content_w + 2*bw) x (content_h + titlebar + 2*bw) logical px, times `scale`).
 * The content area is transparent. */
cairo_surface_t *deco_render_chrome(const struct theme *t, int content_w, int content_h,
                                    bool focused, const char *title, double scale);

/* Blurred drop shadow for an outer frame of the given size. The image covers the frame
 * plus `shadow_radius` on every side, at 1/divisor of the resolution (the caller scales it
 * up, which is cheap and looks the same for a blur). */
cairo_surface_t *deco_render_shadow(const struct theme *t, int outer_w, int outer_h, int divisor);

#endif
