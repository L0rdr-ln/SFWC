/* Window animations: open, close and move, driven by the output frame callbacks. */
#include "server.h"

/* config `enabled` (reduced motion) and the SFWC_NO_ANIMATIONS=1 environment variable */
bool animations_enabled(struct server *server)
{
    const char *off = getenv("SFWC_NO_ANIMATIONS");
    return server->config.anim_enabled && !(off && !strcmp(off, "1"));
}

static void set_opacity_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

static void tree_set_opacity(struct wlr_scene_tree *tree, double opacity)
{
    float o = opacity < 0 ? 0 : opacity > 1 ? 1 : (float)opacity;
    wlr_scene_node_for_each_buffer(&tree->node, set_opacity_iter, &o);
}

static void animation_apply(struct animation *a, double e)
{
    wlr_scene_node_set_position(&a->tree->node, (int)lround(anim_lerp(a->from_x, a->to_x, e)),
                                (int)lround(anim_lerp(a->from_y, a->to_y, e)));
    if (a->from_opacity != a->to_opacity) {
        tree_set_opacity(a->tree, anim_lerp(a->from_opacity, a->to_opacity, e));
    }
}

/* Jump to the end state and drop the animation. */
static void animation_finish(struct animation *a)
{
    if (a->destroy_tree) {
        wlr_scene_node_destroy(&a->tree->node);
    } else {
        animation_apply(a, 1);
        tree_set_opacity(a->tree, 1);
    }
    wl_list_remove(&a->link);
    free(a);
}

static void animations_schedule_frames(struct server *server)
{
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

struct animation *animation_start(struct server *server, enum anim_kind kind,
                                         struct toplevel *t, struct wlr_scene_tree *tree,
                                         double from_x, double from_y, double to_x, double to_y,
                                         double from_opacity, double to_opacity)
{
    struct animation *a = calloc(1, sizeof(*a));
    a->kind = kind;
    a->toplevel = t;
    a->tree = tree;
    a->start_ms = now_msec();
    a->duration_ms = server->config.anim_duration_ms;
    if (!anim_easing_from_name(server->config.anim_easing, &a->easing)) {
        a->easing = EASE_OUT;
    }
    a->from_x = from_x;
    a->from_y = from_y;
    a->to_x = to_x;
    a->to_y = to_y;
    a->from_opacity = from_opacity;
    a->to_opacity = to_opacity;
    wl_list_insert(&server->animations, &a->link);
    animation_apply(a, 0); /* no flash of the final state before the first frame */
    animations_schedule_frames(server);
    return a;
}

/* Called for every output frame, before the scene is committed. */
void animations_tick(struct server *server)
{
    uint32_t now = now_msec();
    struct animation *a, *tmp;
    wl_list_for_each_safe(a, tmp, &server->animations, link) {
        double p = anim_progress(a->start_ms, now, a->duration_ms);
        if (p >= 1) {
            animation_finish(a);
        } else {
            animation_apply(a, anim_ease(a->easing, p));
        }
    }
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
                wl_list_remove(&a->link);
                free(a);
            }
        }
    }
}

/* Slide distance of the "slide" and "fade-scale" opening/closing animations. */
static int slide_distance(const char *style)
{
    return !strcmp(style, "slide") ? 32 : !strcmp(style, "fade-scale") ? 10 : 0;
}

void animate_open(struct toplevel *t)
{
    struct server *server = t->server;
    const char *style = server->config.anim_open;
    if (!animations_enabled(server) || !strcmp(style, "none") ||
        server->config.anim_duration_ms == 0) {
        return;
    }
    double x = t->scene_tree->node.x, y = t->scene_tree->node.y;
    animation_start(server, ANIM_OPEN, t, t->scene_tree, x, y + slide_distance(style), x, y, 0, 1);
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
    if (!animations_enabled(server) || !strcmp(server->config.anim_close, "none") ||
        server->config.anim_duration_ms == 0) {
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

/* The window is going away: fade out the copy of what it showed. */
void animate_close(struct toplevel *t)
{
    struct server *server = t->server;
    struct wlr_scene_tree *snap = t->last_frame;
    t->last_frame = NULL;
    const char *style = server->config.anim_close;
    if (!snap) {
        return;
    }
    if (!animations_enabled(server) || !strcmp(style, "none") ||
        server->config.anim_duration_ms == 0) {
        wlr_scene_node_destroy(&snap->node);
        return;
    }
    int lx, ly;
    wlr_scene_node_coords(&t->scene_tree->node, &lx, &ly);
    wlr_scene_node_set_position(&snap->node, lx, ly);
    wlr_scene_node_set_enabled(&snap->node, true);
    wlr_scene_node_raise_to_top(&snap->node);
    struct animation *a = animation_start(server, ANIM_CLOSE, NULL, snap, lx, ly, lx,
                                          ly + slide_distance(style), 1, 0);
    a->destroy_tree = true;
}

uint32_t now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
