/*
 * Animations, Hyprland style: open/close (popin, slide, slidefade), window moves, a fade on its own
 * timeline, the border color following the focus, workspace switches and layer surfaces appearing.
 * Driven by the output frame callbacks (animations_tick), so skipped frames only make an
 * animation less smooth, never longer.
 *
 * How it works: every animation has two tracks, a geometry track (position and scale) and an
 * opacity track, each with its own duration and Bézier curve, like `windowsIn` and `fadeIn` in
 * Hyprland. wlroots' scene graph cannot scale a tree, so popin scales every buffer of the
 * animated tree around the tree's center (dest size and position), re-applied before every frame
 * because the client may reset it with a commit.
 *
 * Without `animation = ...` rules the old keys (open, close, move, duration_ms, easing) apply.
 */
#include "server.h"

/* config `enabled` (reduced motion) and the SFWC_NO_ANIMATIONS=1 environment variable */
bool animations_enabled(struct server *server)
{
    const char *off = getenv("SFWC_NO_ANIMATIONS");
    return server->config.anim_enabled && !(off && !strcmp(off, "1"));
}

/* ------------------------------------------------------------ settings */

/* What one animation type is configured to do, from a rule or from the legacy keys. */
struct anim_cfg {
    uint32_t duration_ms;
    struct anim_curve curve;
    enum anim_style style;
    int percent;
    enum anim_dir dir;
    bool legacy;  /* from the old keys */
    int slide_px; /* legacy: how far the window slides */
};

/* the old `easing` names as Bézier curves (the same shapes: cubic ease in / out / in-out) */
static struct anim_curve legacy_curve(const char *easing)
{
    if (!strcmp(easing, "linear")) {
        return (struct anim_curve){0, 0, 1, 1};
    } else if (!strcmp(easing, "ease-in")) {
        return (struct anim_curve){0.55, 0.055, 0.675, 0.19};
    } else if (!strcmp(easing, "ease-in-out")) {
        return (struct anim_curve){0.645, 0.045, 0.355, 1};
    }
    return (struct anim_curve){0.215, 0.61, 0.355, 1}; /* ease-out, the default */
}

/* false when this type of animation is off */
static bool cfg_get(struct server *server, enum anim_type type, struct anim_cfg *out)
{
    const struct config *c = &server->config;
    if (!animations_enabled(server)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    struct anim_rule r;
    if (config_anim_rule(c, type, &r)) {
        struct anim_curve curve;
        if (!r.on || !config_find_curve(c, r.curve, &curve)) {
            return false;
        }
        out->duration_ms = (uint32_t)(r.speed * 100.0 + 0.5);
        out->curve = curve;
        out->style = r.style;
        out->percent = r.percent;
        out->dir = r.dir;
        return out->duration_ms > 0;
    }
    /* legacy keys */
    out->legacy = true;
    out->duration_ms = (uint32_t)c->anim_duration_ms;
    out->curve = legacy_curve(c->anim_easing);
    if (out->duration_ms == 0) {
        return false;
    }
    const char *style = NULL;
    switch (type) {
    case ANIMT_WINDOWS_IN:
    case ANIMT_FADE_IN:
        style = c->anim_open;
        break;
    case ANIMT_WINDOWS_OUT:
    case ANIMT_FADE_OUT:
        style = c->anim_close;
        break;
    case ANIMT_WINDOWS_MOVE:
        return c->anim_move;
    default:
        return false; /* border, workspaces and layers only exist as rules */
    }
    if (!strcmp(style, "none")) {
        return false;
    }
    if (type == ANIMT_WINDOWS_IN || type == ANIMT_WINDOWS_OUT) {
        out->slide_px = !strcmp(style, "slide") ? 32 : !strcmp(style, "fade-scale") ? 10 : 0;
        return out->slide_px > 0; /* "fade" has no movement; the fade track does it */
    }
    return true;
}

/* -------------------------------------------------------------- tracks */

static void track_start(struct anim_track *t, const struct anim_cfg *c, uint32_t now)
{
    t->on = true;
    t->start_ms = now;
    t->duration_ms = c->duration_ms;
    t->curve = c->curve;
}

/* curve value at `now`; *done tells whether the track has run its course */
static double track_value(const struct anim_track *t, uint32_t now, bool *done)
{
    if (!t->on) {
        *done = true;
        return 1;
    }
    double p = anim_progress(t->start_ms, now, t->duration_ms);
    *done = p >= 1;
    return anim_curve_eval(&t->curve, p);
}

/* ------------------------------------------------------------- opacity */

static void set_opacity_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

static void tree_set_opacity(struct wlr_scene_tree *tree, double opacity)
{
    float o = opacity < 0 ? 0 : opacity > 1 ? 1 : (float)opacity;
    wlr_scene_node_for_each_buffer(&tree->node, set_opacity_iter, &o);
}

/* ------------------------------------------------------------- scaling */

static void scale_rec_destroy(struct wl_listener *listener, void *data)
{
    struct scale_rec *rec = wl_container_of(listener, rec, destroy);
    wl_list_remove(&rec->destroy.link);
    wl_list_remove(&rec->link);
    free(rec);
}

static void scale_recs_free(struct animation *a)
{
    struct scale_rec *rec, *tmp;
    wl_list_for_each_safe(rec, tmp, &a->scale_recs, link) {
        wl_list_remove(&rec->destroy.link);
        wl_list_remove(&rec->link);
        free(rec);
    }
}

static void scale_collect_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
    struct animation *a = data;
    struct scale_rec *rec;
    wl_list_for_each(rec, &a->scale_recs, link) {
        if (rec->buffer == sb) {
            return;
        }
    }
    int w = sb->dst_width ? sb->dst_width : (sb->buffer ? sb->buffer->width : 0);
    int h = sb->dst_height ? sb->dst_height : (sb->buffer ? sb->buffer->height : 0);
    if (w <= 0 || h <= 0) {
        return;
    }
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        return;
    }
    rec->buffer = sb;
    rec->node_x = sb->node.x;
    rec->node_y = sb->node.y;
    rec->sx = sx;
    rec->sy = sy;
    rec->w = w;
    rec->h = h;
    rec->dst_w = sb->dst_width;
    rec->dst_h = sb->dst_height;
    rec->destroy.notify = scale_rec_destroy;
    wl_signal_add(&sb->node.events.destroy, &rec->destroy);
    wl_list_insert(&a->scale_recs, &rec->link);
}

/* Scale every buffer of the tree by `s` around the center of the tree (s == 1 restores). */
static void scale_apply(struct animation *a, double s)
{
    wlr_scene_node_for_each_buffer(&a->tree->node, scale_collect_iter, a);
    struct scale_rec *rec;
    if (!a->centered) {
        int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        wl_list_for_each(rec, &a->scale_recs, link) {
            x0 = rec->sx < x0 ? rec->sx : x0;
            y0 = rec->sy < y0 ? rec->sy : y0;
            x1 = rec->sx + rec->w > x1 ? rec->sx + rec->w : x1;
            y1 = rec->sy + rec->h > y1 ? rec->sy + rec->h : y1;
        }
        if (x0 > x1) {
            return; /* nothing to scale yet */
        }
        a->cx = (x0 + x1) / 2.0;
        a->cy = (y0 + y1) / 2.0;
        a->centered = true;
    }
    wl_list_for_each(rec, &a->scale_recs, link) {
        if (s == 1.0) {
            wlr_scene_node_set_position(&rec->buffer->node, rec->node_x, rec->node_y);
            wlr_scene_buffer_set_dest_size(rec->buffer, rec->dst_w, rec->dst_h);
            continue;
        }
        int dx = (int)lround((rec->sx - a->cx) * (s - 1));
        int dy = (int)lround((rec->sy - a->cy) * (s - 1));
        int w = (int)lround(rec->w * s), h = (int)lround(rec->h * s);
        wlr_scene_node_set_position(&rec->buffer->node, rec->node_x + dx, rec->node_y + dy);
        wlr_scene_buffer_set_dest_size(rec->buffer, w < 1 ? 1 : w, h < 1 ? 1 : h);
    }
}

/* ------------------------------------------------------- the animations */

static struct animation *anim_new(struct server *server, enum anim_kind kind, struct toplevel *t,
                                  struct wlr_scene_tree *tree)
{
    struct animation *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->kind = kind;
    a->toplevel = t;
    a->tree = tree;
    a->from_x = a->to_x = tree->node.x;
    a->from_y = a->to_y = tree->node.y;
    a->from_scale = a->to_scale = 1;
    a->from_opacity = a->to_opacity = 1;
    wl_list_init(&a->scale_recs);
    wl_list_insert(&server->animations, &a->link);
    return a;
}

/* Apply the state at time `now`; true when the animation has run its course. */
static bool animation_apply(struct animation *a, uint32_t now)
{
    bool geo_done, fade_done;
    double eg = track_value(&a->geo, now, &geo_done);
    double ef = track_value(&a->fade, now, &fade_done);
    wlr_scene_node_set_position(&a->tree->node, (int)lround(anim_lerp(a->from_x, a->to_x, eg)),
                                (int)lround(anim_lerp(a->from_y, a->to_y, eg)));
    if (a->from_scale != 1 || a->to_scale != 1) {
        scale_apply(a, anim_lerp(a->from_scale, a->to_scale, eg));
    }
    if (a->from_opacity != a->to_opacity) {
        tree_set_opacity(a->tree, anim_lerp(a->from_opacity, a->to_opacity, ef));
    }
    return geo_done && fade_done;
}

static void animation_free(struct animation *a)
{
    scale_recs_free(a);
    wl_list_remove(&a->link);
    free(a);
}

/* Jump to the end state and drop the animation. */
static void animation_finish(struct animation *a)
{
    if (a->destroy_tree) {
        scale_recs_free(a); /* the buffers go with the tree */
        wlr_scene_node_destroy(&a->tree->node);
        wl_list_remove(&a->link);
        free(a);
        return;
    }
    if (a->from_scale != 1 || a->to_scale != 1) {
        scale_apply(a, 1);
    }
    tree_set_opacity(a->tree, 1);
    if (a->hide_at_end) {
        wlr_scene_node_set_position(&a->tree->node, (int)lround(a->rest_x), (int)lround(a->rest_y));
        if (a->toplevel) {
            wlr_scene_node_set_enabled(&a->tree->node, toplevel_shown(a->toplevel));
        }
    } else {
        wlr_scene_node_set_position(&a->tree->node, (int)lround(a->to_x), (int)lround(a->to_y));
    }
    animation_free(a);
}

static void animations_schedule_frames(struct server *server)
{
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

/* Show the first frame's state right away (no flash of the end state), and ask for frames. */
static void anim_begin(struct server *server, struct animation *a)
{
    animation_apply(a, a->geo.on ? a->geo.start_ms : a->fade.start_ms);
    animations_schedule_frames(server);
}

static void border_tick(struct server *server, uint32_t now)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (!t->border_anim.active) {
            continue;
        }
        double p = anim_progress(t->border_anim.start_ms, now, t->border_anim.duration_ms);
        if (p >= 1) {
            t->border_anim.active = false;
            t->focus_mix = t->border_anim.to;
        } else {
            t->focus_mix = anim_lerp(t->border_anim.from, t->border_anim.to,
                                     anim_curve_eval(&t->border_anim.curve, p));
        }
        frame_refresh(t);
    }
}

/* Called for every output frame, before the scene is committed. */
void animations_tick(struct server *server)
{
    uint32_t now = now_msec();
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &server->animations, link) {
        if (animation_apply(a, now)) {
            animation_finish(a);
        }
    }
    border_tick(server, now);
}

/* A window that is about to be moved, resized or destroyed by other code. */
void animations_cancel(struct toplevel *t, bool finish)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &t->server->animations, link) {
        if (a->toplevel == t) {
            if (finish) {
                animation_finish(a);
            } else {
                animation_free(a);
            }
        }
    }
    t->border_anim.active = false;
}

/* A new workspace switch while the last one is still sliding: jump to its end first. */
void animations_finish_workspace(struct server *server)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &server->animations, link) {
        if (a->kind == ANIM_WORKSPACE) {
            animation_finish(a);
        }
    }
}

/* The tree (a layer surface) is about to go away. */
void animations_cancel_tree(struct server *server, struct wlr_scene_tree *tree)
{
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &server->animations, link) {
        if (a->tree == tree && !a->destroy_tree) {
            animation_free(a);
        }
    }
}

/* ------------------------------------------------------ geometry styles */

/* How far the window slides to leave through (or arrive from) the nearest screen edge; `fraction`
 * is the part of the way (1 = completely off the screen). */
static void slide_offset(const struct wlr_box *box, const struct wlr_box *out, enum anim_dir dir,
                         double fraction, double *dx, double *dy)
{
    *dx = *dy = 0;
    if (out->width <= 0 || out->height <= 0) {
        return;
    }
    if (dir == ANIM_DIR_AUTO) {
        double cx = box->x + box->width / 2.0, cy = box->y + box->height / 2.0;
        double left = cx - out->x, right = out->x + out->width - cx;
        double top = cy - out->y, bottom = out->y + out->height - cy;
        double best = left;
        dir = ANIM_DIR_LEFT;
        if (right < best) {
            best = right;
            dir = ANIM_DIR_RIGHT;
        }
        if (top < best) {
            best = top;
            dir = ANIM_DIR_TOP;
        }
        if (bottom < best) {
            dir = ANIM_DIR_BOTTOM;
        }
    }
    switch (dir) {
    case ANIM_DIR_LEFT:
        *dx = -(box->x + box->width - out->x) * fraction;
        break;
    case ANIM_DIR_RIGHT:
        *dx = (out->x + out->width - box->x) * fraction;
        break;
    case ANIM_DIR_TOP:
        *dy = -(box->y + box->height - out->y) * fraction;
        break;
    default:
        *dy = (out->y + out->height - box->y) * fraction;
        break;
    }
}

/* Set up the geometry track of an opening or closing window from its rule. Returns true when
 * the style fades the window by itself (slidefade), so the caller makes sure a fade runs. */
static bool style_geometry(struct animation *a, const struct anim_cfg *g, const struct wlr_box *box,
                           const struct wlr_box *out, bool closing, uint32_t now)
{
    double rest_x = a->tree->node.x, rest_y = a->tree->node.y;
    double dx = 0, dy = 0, scale = 1;
    bool fades = false;
    if (g->legacy) {
        dy = g->slide_px;
    } else {
        enum anim_style style = g->style == ANIM_STYLE_DEFAULT ? ANIM_STYLE_SLIDE : g->style;
        switch (style) {
        case ANIM_STYLE_POPIN:
            scale = (g->percent ? g->percent : 80) / 100.0;
            break;
        case ANIM_STYLE_SLIDEFADE:
            slide_offset(box, out, g->dir, (g->percent ? g->percent : 20) / 100.0, &dx, &dy);
            /* a slidefade moves by a part of the window's size, not of the way off screen */
            if (dx != 0) {
                dx = (dx < 0 ? -1 : 1) * box->width * (g->percent ? g->percent : 20) / 100.0;
            }
            if (dy != 0) {
                dy = (dy < 0 ? -1 : 1) * box->height * (g->percent ? g->percent : 20) / 100.0;
            }
            fades = true;
            break;
        default:
            slide_offset(box, out, g->dir, 1.0, &dx, &dy);
            break;
        }
    }
    track_start(&a->geo, g, now);
    if (closing) {
        a->from_x = rest_x;
        a->from_y = rest_y;
        a->to_x = rest_x + dx;
        a->to_y = rest_y + dy;
        a->from_scale = 1;
        a->to_scale = scale;
    } else {
        a->from_x = rest_x + dx;
        a->from_y = rest_y + dy;
        a->to_x = rest_x;
        a->to_y = rest_y;
        a->from_scale = scale;
        a->to_scale = 1;
    }
    return fades;
}

/* The window's whole look (frame included) and the output it is on, in layout coordinates. */
static void window_boxes(struct toplevel *t, struct wlr_box *box, struct wlr_box *out)
{
    *box = toplevel_outer(t);
    *out = output_box_at(t->server, box->x + box->width / 2.0, box->y + box->height / 2.0);
}

/* Both tracks of an opening or closing animation. */
static struct animation *open_close(struct toplevel *t, struct wlr_scene_tree *tree,
                                    enum anim_kind kind, const struct anim_cfg *g, bool have_g,
                                    const struct anim_cfg *f, bool have_f)
{
    struct server *server = t->server;
    bool closing = kind == ANIM_CLOSE;
    struct animation *a = anim_new(server, kind, closing ? NULL : t, tree);
    if (!a) {
        return NULL;
    }
    uint32_t now = now_msec();
    struct wlr_box box, out;
    window_boxes(t, &box, &out);
    bool forces_fade = false;
    if (have_g) {
        forces_fade = style_geometry(a, g, &box, &out, closing, now);
    }
    if (have_f) {
        track_start(&a->fade, f, now);
    } else if (forces_fade) {
        track_start(&a->fade, g, now); /* slidefade fades with the movement's timing */
    }
    if (a->fade.on) {
        a->from_opacity = closing ? 1 : 0;
        a->to_opacity = closing ? 0 : 1;
    }
    a->destroy_tree = closing;
    anim_begin(server, a);
    return a;
}

/* ------------------------------------------------------------ open/close */

void animate_open(struct toplevel *t)
{
    struct anim_cfg g, f;
    bool have_g = cfg_get(t->server, ANIMT_WINDOWS_IN, &g);
    bool have_f = cfg_get(t->server, ANIMT_FADE_IN, &f);
    if (!have_g && !have_f) {
        return;
    }
    open_close(t, t->scene_tree, ANIM_OPEN, &g, have_g, &f, have_f);
}

struct snapshot_ctx {
    struct wlr_scene_tree *snap;
    int count;
};

static void snapshot_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
    struct snapshot_ctx *ctx = data;
    if (!sb->buffer) {
        return;
    }
    struct wlr_scene_buffer *copy = wlr_scene_buffer_create(ctx->snap, sb->buffer);
    wlr_scene_node_set_position(&copy->node, sx, sy);
    wlr_scene_buffer_set_dest_size(copy, sb->dst_width, sb->dst_height);
    wlr_scene_buffer_set_source_box(copy, &sb->src_box);
    wlr_scene_buffer_set_transform(copy, sb->transform);
    ctx->count++;
}

/* Keep a hidden, reference-only copy of the window's current buffers (no pixels are copied).
 * By the time a window unmaps, the scene has already dropped them, so the copy is made while
 * the window is still showing. Refreshed on commits, at most every 50 ms. */
void snapshot_refresh(struct toplevel *t)
{
    struct server *server = t->server;
    struct anim_cfg g, f;
    if (!cfg_get(server, ANIMT_WINDOWS_OUT, &g) && !cfg_get(server, ANIMT_FADE_OUT, &f)) {
        if (t->last_frame) {
            wlr_scene_node_destroy(&t->last_frame->node);
            t->last_frame = NULL;
        }
        return;
    }
    uint32_t now = now_msec();
    if (t->last_frame && now - t->last_frame_ms < 50) {
        return;
    }
    if (!t->xdg_toplevel->base->surface->mapped) {
        return; /* keep the last good copy */
    }
    struct wlr_scene_tree *copy = wlr_scene_tree_create(server->windows_tree);
    wlr_scene_node_set_enabled(&copy->node, false);
    struct snapshot_ctx ctx = {copy, 0};
    wlr_scene_node_for_each_buffer(&t->scene_tree->node, snapshot_iter, &ctx);
    if (ctx.count == 0) {
        wlr_scene_node_destroy(&copy->node);
        return;
    }
    if (t->last_frame) {
        wlr_scene_node_destroy(&t->last_frame->node);
    }
    t->last_frame = copy;
    t->last_frame_ms = now;
}

/* The window is going away: animate the copy of what it showed. */
void animate_close(struct toplevel *t)
{
    struct server *server = t->server;
    struct wlr_scene_tree *snap = t->last_frame;
    t->last_frame = NULL;
    if (!snap) {
        return;
    }
    struct anim_cfg g, f;
    bool have_g = cfg_get(server, ANIMT_WINDOWS_OUT, &g);
    bool have_f = cfg_get(server, ANIMT_FADE_OUT, &f);
    if (!have_g && !have_f) {
        wlr_scene_node_destroy(&snap->node);
        return;
    }
    int lx, ly;
    wlr_scene_node_coords(&t->scene_tree->node, &lx, &ly);
    wlr_scene_node_set_position(&snap->node, lx, ly);
    wlr_scene_node_set_enabled(&snap->node, true);
    wlr_scene_node_raise_to_top(&snap->node);
    if (!open_close(t, snap, ANIM_CLOSE, &g, have_g, &f, have_f)) {
        wlr_scene_node_destroy(&snap->node);
    }
}

/* ------------------------------------------------------------------ move */

/* Slide a window from one position to another (maximize, restore, to the next output). */
bool animate_move(struct toplevel *t, double from_x, double from_y, double to_x, double to_y)
{
    struct anim_cfg g;
    if (!cfg_get(t->server, ANIMT_WINDOWS_MOVE, &g)) {
        return false;
    }
    struct animation *a = anim_new(t->server, ANIM_MOVE, t, t->scene_tree);
    if (!a) {
        return false;
    }
    track_start(&a->geo, &g, now_msec());
    a->from_x = from_x;
    a->from_y = from_y;
    a->to_x = to_x;
    a->to_y = to_y;
    anim_begin(t->server, a);
    return true;
}

/* ---------------------------------------------------------------- border */

/* The frame colors move from the look at `from` to the look at `to` (0 unfocused, 1 focused). */
void animate_border(struct toplevel *t, double from, double to)
{
    struct anim_cfg b;
    if (!cfg_get(t->server, ANIMT_BORDER, &b)) {
        t->border_anim.active = false;
        t->focus_mix = to;
        return;
    }
    t->border_anim.active = true;
    t->border_anim.start_ms = now_msec();
    t->border_anim.duration_ms = b.duration_ms;
    t->border_anim.from = from;
    t->border_anim.to = to;
    t->border_anim.curve = b.curve;
    t->focus_mix = from;
    animations_schedule_frames(t->server);
}

/* ------------------------------------------------------------ workspaces */

/* Slide or fade the windows of the workspace we leave out and those of the one we enter in. */
void animate_workspace_switch(struct server *server, int old_ws, int new_ws)
{
    struct anim_cfg g;
    if (!cfg_get(server, ANIMT_WORKSPACES, &g)) {
        return;
    }
    enum anim_style style = g.style == ANIM_STYLE_DEFAULT ? ANIM_STYLE_SLIDE : g.style;
    bool vertical = style == ANIM_STYLE_SLIDEVERT || style == ANIM_STYLE_SLIDEFADEVERT;
    bool fades = style == ANIM_STYLE_FADE || style == ANIM_STYLE_SLIDEFADE ||
                 style == ANIM_STYLE_SLIDEFADEVERT;
    bool moves = style != ANIM_STYLE_FADE;
    double part = style == ANIM_STYLE_SLIDEFADE || style == ANIM_STYLE_SLIDEFADEVERT
                      ? (g.percent ? g.percent : 20) / 100.0
                      : (g.percent ? g.percent : 100) / 100.0;
    double forward = new_ws > old_ws ? 1 : -1; /* the content moves against the direction of travel */
    uint32_t now = now_msec();

    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        bool leaving = t->ws == old_ws, entering = t->ws == new_ws;
        if (!t->mapped || t->minimized || (!leaving && !entering)) {
            continue;
        }
        struct animation *a = anim_new(server, ANIM_WORKSPACE, t, t->scene_tree);
        if (!a) {
            continue;
        }
        struct wlr_box box, out;
        window_boxes(t, &box, &out);
        double dist = out.width > 0 ? (vertical ? out.height : out.width) * part : 0;
        a->rest_x = a->from_x = a->to_x = t->scene_tree->node.x;
        a->rest_y = a->from_y = a->to_y = t->scene_tree->node.y;
        a->hide_at_end = true;
        if (moves) {
            double *from = vertical ? &a->from_y : &a->from_x, *to = vertical ? &a->to_y : &a->to_x;
            if (leaving) {
                *to += -forward * dist;
            } else {
                *from += forward * dist;
            }
        }
        track_start(&a->geo, &g, now);
        if (fades) {
            track_start(&a->fade, &g, now);
            a->from_opacity = leaving ? 1 : 0;
            a->to_opacity = leaving ? 0 : 1;
        }
        wlr_scene_node_set_enabled(&t->scene_tree->node, true); /* the old workspace stays until it is out */
        anim_begin(server, a);
    }
}

/* ---------------------------------------------------------------- layers */

/* A panel, launcher or notification appears: fade it in (popin also grows it). */
void animate_layer_open(struct server *server, struct wlr_scene_tree *tree)
{
    struct anim_cfg g;
    if (!cfg_get(server, ANIMT_LAYERS_IN, &g)) {
        return;
    }
    struct animation *a = anim_new(server, ANIM_LAYER, NULL, tree);
    if (!a) {
        return;
    }
    uint32_t now = now_msec();
    track_start(&a->geo, &g, now);
    track_start(&a->fade, &g, now);
    a->from_opacity = 0;
    a->to_opacity = 1;
    if (g.style == ANIM_STYLE_POPIN) {
        a->from_scale = (g.percent ? g.percent : 80) / 100.0;
    }
    anim_begin(server, a);
}

uint32_t now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
