/*
 * SFWC - Simple Floating Wayland Compositor
 *
 * M1/M2: outputs + scene graph, xdg-shell windows, keyboard/pointer input,
 * click-to-focus, Alt+drag move/resize, maximize/fullscreen/minimize, edge
 * snapping, cascading placement, a few hard-coded keybinds.
 * Config, themes, decorations and animations come in later milestones
 * (see docs/ROADMAP.md). Structure follows wlroots' tinywl example.
 *
 * Configuration: sfwc.conf (see config/sfwc.conf and src/config.c). Without a
 * config file the built-in defaults apply: modifier Alt (works when nested in
 * another compositor that owns Super), Return terminal, q close, Tab cycle,
 * f maximize, F11 fullscreen, m minimize, Shift+m restore, Shift+r reload,
 * Escape quit, mod+Left/Right drag = move/resize. The config is reloaded when
 * the file is saved.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "anim.h"
#include "deco.h"
#include "template.h"
#include "theme.h"

#define CASCADE_STEP 32

/* config.h uses its own copy of the modifier bits; they must match wlroots. */
_Static_assert(DECO_EDGE_TOP == WLR_EDGE_TOP && DECO_EDGE_BOTTOM == WLR_EDGE_BOTTOM &&
                   DECO_EDGE_LEFT == WLR_EDGE_LEFT && DECO_EDGE_RIGHT == WLR_EDGE_RIGHT,
               "edge bits differ from wlroots");
_Static_assert(CFG_MOD_SHIFT == WLR_MODIFIER_SHIFT && CFG_MOD_CTRL == WLR_MODIFIER_CTRL &&
                   CFG_MOD_ALT == WLR_MODIFIER_ALT && CFG_MOD_LOGO == WLR_MODIFIER_LOGO,
               "modifier bits differ from wlroots");

enum cursor_mode {
    CURSOR_PASSTHROUGH,
    CURSOR_MOVE,
    CURSOR_RESIZE,
};

struct server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels; /* top of the stack first */

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
    struct wl_listener new_idle_inhibitor;
    int idle_inhibitors;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
    struct wl_list keyboards;
    /* Virtual input (tests only, SFWC_ENABLE_VIRTUAL_INPUT=1): any client could
     * otherwise inject keystrokes. */
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_virtual_keyboard;
    enum cursor_mode cursor_mode;
    struct toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;
    int cascade; /* offset of the next new window */
    int ws_current; /* 0-based index of the visible workspace */

    /* ext-session-lock: while `locked`, only the lock client's surfaces are visible and
     * receive input; a lock client that dies leaves the session locked. */
    struct wlr_session_lock_manager_v1 *lock_mgr;
    struct wl_listener new_lock;
    struct wlr_session_lock_v1 *cur_lock;
    struct wl_listener lock_new_surface;
    struct wl_listener lock_unlock;
    struct wl_listener lock_destroy;
    struct wlr_scene_tree *lock_tree; /* above everything, disabled while unlocked */
    struct wlr_scene_rect *lock_bg;   /* black, covers all outputs */
    bool locked;

    struct config config;
    struct theme theme;
    struct wl_list animations; /* struct animation */
    /* z-order, bottom to top: background, bottom, windows, top, overlay */
    struct wlr_scene_tree *layer_trees[4];
    struct wlr_scene_tree *windows_tree;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces; /* struct layer_surface */
    unsigned theme_gen; /* bumped when the theme changes, so frames re-render */
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
    struct wl_listener new_toplevel_decoration;
    struct wl_listener keyboard_focus_change;
    char *config_path; /* NULL: no config location, defaults only */
    char *config_name; /* basename of config_path, matched against inotify events */
    int inotify_fd;
    struct wl_event_source *inotify_source;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
};

struct output {
    struct wl_list link;
    struct server *server;
    struct wlr_output *wlr_output;
    struct wlr_box usable_area; /* the output minus exclusive zones of panels (layer shell) */
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct toplevel {
    struct wl_list link;
    struct server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    bool mapped;
    bool maximized;
    bool fullscreen;
    bool minimized;
    int ws; /* workspace the window lives on */
    struct wlr_box saved; /* window geometry before maximize/fullscreen */

    /* server-side decorations (xdg-decoration) */
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener deco_request_mode;
    struct wl_listener deco_destroy;
    struct wl_listener set_title;
    bool ssd; /* we draw this window's frame */
    struct wlr_scene_tree *last_frame; /* hidden copy of what the window showed, for fade-out */
    uint32_t last_frame_ms;
    struct wlr_scene_tree *frame_tree;
    struct wlr_scene_buffer *chrome;
    struct wlr_scene_buffer *shadow;
    int frame_cw, frame_ch, shadow_w, shadow_h;
    bool frame_focused;
    double frame_scale;
    unsigned frame_gen, shadow_gen;
    char *frame_title;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;

    /* wlr-foreign-toplevel-management (taskbars) */
    struct wlr_foreign_toplevel_handle_v1 *fth;
    struct wl_listener fth_activate;
    struct wl_listener fth_maximize;
    struct wl_listener fth_minimize;
    struct wl_listener fth_fullscreen;
    struct wl_listener fth_close;
};

struct idle_inhibitor {
    struct server *server;
    struct wl_listener destroy;
};

struct layer_surface {
    struct wl_list link;
    struct server *server;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wlr_layer_surface_v1 *layer;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener new_popup;
    struct wl_listener destroy;
};

struct popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void arrange_layers(struct output *output);
static void fth_sync(struct toplevel *t);
static void workspace_refresh(struct server *server);
static void lock_update_bg(struct server *server);
static void reset_cursor_mode(struct server *server);
static void process_cursor_motion(struct server *server, uint32_t time);
static struct toplevel *toplevel_from_xdg(struct wlr_xdg_toplevel *xdg);
static void focus_layer_surface(struct server *server, struct wlr_layer_surface_v1 *layer);

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;
    bool is_virtual; /* virtual keyboards bring their own keymap */
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* ---------------------------------------------------------------- spawn */

/* Run a shell command detached: double fork so we never leave zombies. */
/* $terminal, $theme, $runtime ... and @colors.background:hex@ style theme placeholders. */
static char *expand_command(struct server *server, const char *in)
{
    char *a = config_expand(&server->config, in, getenv("XDG_RUNTIME_DIR"));
    char *b = template_render(&server->theme, a, false, NULL, 0);
    if (!b) {
        return a;
    }
    free(a);
    return b;
}

static void spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "fork failed");
        return;
    }
    if (pid == 0) {
        setsid();
        pid_t pid2 = fork();
        if (pid2 == 0) {
            execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(pid2 < 0 ? 1 : 0);
    }
    waitpid(pid, NULL, 0);
}


/* ---------------------------------------------------------- geometry */

/* Window geometry (without client-side shadows) in layout coordinates. */
static struct wlr_box toplevel_geometry(struct toplevel *t)
{
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    geo.x += t->scene_tree->node.x;
    geo.y += t->scene_tree->node.y;
    return geo;
}

/* Move the window so its geometry's top-left corner is at (x, y). */
/* -------------------------------------------------------------- animations */

enum anim_kind { ANIM_OPEN, ANIM_CLOSE, ANIM_MOVE };

struct animation {
    struct wl_list link;
    enum anim_kind kind;
    struct toplevel *toplevel; /* OPEN and MOVE; NULL for CLOSE */
    struct wlr_scene_tree *tree;
    uint32_t start_ms, duration_ms;
    enum anim_easing easing;
    double from_x, from_y, to_x, to_y; /* position of `tree` */
    double from_opacity, to_opacity;
    bool destroy_tree; /* CLOSE: the tree is a snapshot that goes away at the end */
};

static uint32_t now_msec(void);

/* config `enabled` (reduced motion) and the SFWC_NO_ANIMATIONS=1 environment variable */
static bool animations_enabled(struct server *server)
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

static struct animation *animation_start(struct server *server, enum anim_kind kind,
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
static void animations_tick(struct server *server)
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
static void animations_cancel(struct toplevel *t, bool finish)
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

static void animate_open(struct toplevel *t)
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
static void snapshot_refresh(struct toplevel *t)
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
static void animate_close(struct toplevel *t)
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

/* Move the window (content top-left to x, y), with a tween when `animate` is set. */
static void toplevel_move_to_ex(struct toplevel *t, int x, int y, bool animate)
{
    struct server *server = t->server;
    animations_cancel(t, true);
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    int nx = x - geo.x, ny = y - geo.y;
    if (animate && t->mapped && server->config.anim_move && animations_enabled(server) &&
        server->config.anim_duration_ms > 0 &&
        (abs(nx - t->scene_tree->node.x) > 2 || abs(ny - t->scene_tree->node.y) > 2)) {
        animation_start(server, ANIM_MOVE, t, t->scene_tree, t->scene_tree->node.x,
                        t->scene_tree->node.y, nx, ny, 1, 1);
    } else {
        wlr_scene_node_set_position(&t->scene_tree->node, nx, ny);
    }
}

static void toplevel_move_to(struct toplevel *t, int x, int y)
{
    toplevel_move_to_ex(t, x, y, false);
}

/* Box of the output containing (x, y), or the center output; zero-sized if none. */
static struct wlr_box output_box_at(struct server *server, double x, double y)
{
    struct wlr_box box = {0};
    struct wlr_output *out = wlr_output_layout_output_at(server->output_layout, x, y);
    if (!out) {
        out = wlr_output_layout_get_center_output(server->output_layout);
    }
    if (out) {
        wlr_output_layout_get_box(server->output_layout, out, &box);
    }
    return box;
}

/* The part of the output at (x, y) not covered by panels (exclusive zones of layer surfaces). */
static struct wlr_box work_area_at(struct server *server, double x, double y)
{
    struct wlr_output *out = wlr_output_layout_output_at(server->output_layout, x, y);
    if (!out) {
        out = wlr_output_layout_get_center_output(server->output_layout);
    }
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output == out && o->usable_area.width > 0) {
            return o->usable_area;
        }
    }
    return output_box_at(server, x, y);
}


/* ------------------------------------------------------ window frames */

/* A wlr_buffer backed by a cairo image surface (the frame is drawn in software). */
struct cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *buffer)
{
    struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
    cairo_surface_destroy(cb->surface);
    free(cb);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
                                               void **data, uint32_t *format, size_t *stride)
{
    struct cairo_buffer *cb = wl_container_of(buffer, cb, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *data = cairo_image_surface_get_data(cb->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(cb->surface);
    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

/* Takes ownership of `surface`. */
static struct wlr_buffer *cairo_buffer_create(cairo_surface_t *surface)
{
    struct cairo_buffer *cb = calloc(1, sizeof(*cb));
    wlr_buffer_init(&cb->base, &cairo_buffer_impl, cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));
    cb->surface = surface;
    return &cb->base;
}

static void set_scene_buffer_from_surface(struct wlr_scene_buffer *node, cairo_surface_t *surface)
{
    struct wlr_buffer *buffer = cairo_buffer_create(surface);
    wlr_scene_buffer_set_buffer(node, buffer);
    wlr_buffer_drop(buffer); /* the scene node holds its own lock */
}

/* The shadow must not catch pointer input. */
static bool no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return false;
}

static struct deco_insets toplevel_insets(struct toplevel *t)
{
    struct deco_insets none = {0};
    if (!t->ssd || t->fullscreen) {
        return none;
    }
    return deco_insets(&t->server->theme);
}

/* Window geometry including the server-side frame, in layout coordinates. */
static struct wlr_box toplevel_outer(struct toplevel *t)
{
    struct wlr_box g = toplevel_geometry(t);
    struct deco_insets in = toplevel_insets(t);
    return (struct wlr_box){g.x - in.left, g.y - in.top, g.width + in.left + in.right,
                            g.height + in.top + in.bottom};
}

static double frame_scale(struct server *server)
{
    double scale = 1;
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output->scale > scale) {
            scale = o->wlr_output->scale;
        }
    }
    return ceil(scale);
}

static void frame_destroy(struct toplevel *t)
{
    if (t->frame_tree) {
        wlr_scene_node_destroy(&t->frame_tree->node);
        t->frame_tree = NULL;
        t->chrome = t->shadow = NULL;
    }
    free(t->frame_title);
    t->frame_title = NULL;
}

/* (Re)create the frame for the current size, focus, title and theme; no-ops when nothing
 * changed. Removes the frame when the window is not server-side decorated or fullscreen. */
static void frame_refresh(struct toplevel *t)
{
    struct server *server = t->server;
    if (!t->ssd || t->fullscreen || !t->xdg_toplevel->base->surface->mapped) {
        frame_destroy(t);
        return;
    }
    const struct theme *theme = &server->theme;
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    if (geo.width <= 0 || geo.height <= 0) {
        return;
    }
    bool focused = server->seat->keyboard_state.focused_surface == t->xdg_toplevel->base->surface;
    double scale = frame_scale(server);
    const char *title = t->xdg_toplevel->title ? t->xdg_toplevel->title : "";

    if (!t->frame_tree) {
        t->frame_tree = wlr_scene_tree_create(t->scene_tree);
        t->shadow = wlr_scene_buffer_create(t->frame_tree, NULL);
        t->shadow->point_accepts_input = no_input;
        t->chrome = wlr_scene_buffer_create(t->frame_tree, NULL);
        wlr_scene_node_lower_to_bottom(&t->frame_tree->node); /* behind the client surface */
        t->frame_cw = t->frame_ch = 0;
    }
    struct deco_insets in = deco_insets(theme);
    int ow = geo.width + in.left + in.right, oh = geo.height + in.top + in.bottom;

    bool title_changed = !t->frame_title || strcmp(t->frame_title, title) != 0;
    if (t->frame_cw != geo.width || t->frame_ch != geo.height || t->frame_focused != focused ||
        t->frame_scale != scale || t->frame_gen != server->theme_gen || title_changed) {
        cairo_surface_t *surf = deco_render_chrome(theme, geo.width, geo.height, focused, title, scale);
        set_scene_buffer_from_surface(t->chrome, surf);
        wlr_scene_buffer_set_dest_size(t->chrome, ow, oh);
        wlr_scene_node_set_position(&t->chrome->node, -in.left, -in.top);
        free(t->frame_title);
        t->frame_title = strdup(title);
    }

    int R = theme->shadow_radius;
    wlr_scene_node_set_enabled(&t->shadow->node, theme->shadow_enabled && R > 0);
    if (theme->shadow_enabled && R > 0 &&
        (t->shadow_w != ow || t->shadow_h != oh || t->shadow_gen != server->theme_gen)) {
        cairo_surface_t *surf = deco_render_shadow(theme, ow, oh, 4);
        set_scene_buffer_from_surface(t->shadow, surf);
        wlr_scene_buffer_set_dest_size(t->shadow, ow + 2 * R, oh + 2 * R);
        wlr_scene_node_set_position(&t->shadow->node, -in.left - R,
                                    -in.top - R + theme->shadow_offset_y);
        t->shadow_w = ow;
        t->shadow_h = oh;
        t->shadow_gen = server->theme_gen;
    }
    t->frame_cw = geo.width;
    t->frame_ch = geo.height;
    t->frame_focused = focused;
    t->frame_scale = scale;
    t->frame_gen = server->theme_gen;
}

/* On the current workspace and not minimized. */
static bool toplevel_shown(const struct toplevel *t)
{
    return !t->minimized && t->ws == t->server->ws_current;
}

static struct toplevel *top_visible(struct server *server)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (toplevel_shown(t)) {
            return t;
        }
    }
    return NULL;
}

/* The window with keyboard focus (the front window when nothing is focused). With
 * focus=follow-mouse this is not necessarily the front window. */
static struct toplevel *focused_visible(struct server *server)
{
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (toplevel_shown(t) && t->xdg_toplevel->base->surface == focus) {
            return t;
        }
    }
    return top_visible(server);
}

/* Apply maximized/fullscreen state; restores the saved geometry when both are off. */
static void toplevel_apply_state(struct toplevel *t, bool max, bool fs)
{
    bool was_normal = !t->maximized && !t->fullscreen;
    struct wlr_box ref = was_normal ? toplevel_geometry(t) : t->saved;
    if (was_normal && (max || fs)) {
        t->saved = ref;
    }
    t->maximized = max;
    t->fullscreen = fs;
    wlr_xdg_toplevel_set_maximized(t->xdg_toplevel, max);
    wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, fs);
    frame_refresh(t); /* fullscreen windows lose their frame */
    struct deco_insets ins = toplevel_insets(t);

    double cx = ref.x + ref.width / 2.0, cy = ref.y + ref.height / 2.0;
    struct wlr_box box = fs ? output_box_at(t->server, cx, cy) : work_area_at(t->server, cx, cy);
    wlr_log(WLR_DEBUG, "apply state max=%d fs=%d, output box %d,%d %dx%d (window centre %.0f,%.0f)",
            max, fs, box.x, box.y, box.width, box.height, ref.x + ref.width / 2.0,
            ref.y + ref.height / 2.0);
    if ((fs || max) && box.width > 0) {
        int inset = fs ? 0 : t->server->config.gap;
        toplevel_move_to_ex(t, box.x + inset + ins.left, box.y + inset + ins.top, true);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, box.width - 2 * inset - ins.left - ins.right,
                                  box.height - 2 * inset - ins.top - ins.bottom);
    } else if (!fs && !max) {
        toplevel_move_to_ex(t, t->saved.x, t->saved.y, true);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, t->saved.width, t->saved.height);
    }
    if (t->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
    }
    fth_sync(t);
}

/* Cascade new windows from the top-left of the output under the cursor. */
static void place_new_toplevel(struct toplevel *t)
{
    struct server *server = t->server;
    struct wlr_box box = work_area_at(server, server->cursor->x, server->cursor->y);
    if (box.width <= 0) {
        return;
    }
    struct wlr_box geo = toplevel_outer(t); /* including the frame */
    struct deco_insets ins = toplevel_insets(t);
    int x, y;
    if (t->xdg_toplevel->parent) { /* dialog: center */
        x = box.x + (box.width - geo.width) / 2;
        y = box.y + (box.height - geo.height) / 2;
    } else {
        x = box.x + 48 + server->cascade;
        y = box.y + 48 + server->cascade;
        server->cascade = (server->cascade + CASCADE_STEP) % (6 * CASCADE_STEP);
    }
    int gap = server->config.gap;
    int max_x = box.x + box.width - gap - geo.width;
    int max_y = box.y + box.height - gap - geo.height;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < box.x + gap) {
        x = box.x + gap;
    }
    if (y < box.y + gap) {
        y = box.y + gap;
    }
    toplevel_move_to(t, x + ins.left, y + ins.top);
}

/* -------------------------------------------------------------- focusing */

static void focus_toplevel_ex(struct toplevel *toplevel, bool raise)
{
    if (toplevel == NULL || toplevel->server->locked) {
        return;
    }
    struct server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev != NULL) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);

    if (raise) { /* raise to the top of the stacking order */
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
        wl_list_remove(&toplevel->link);
        wl_list_insert(&server->toplevels, &toplevel->link);
    }
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
}

static void focus_toplevel(struct toplevel *toplevel)
{
    focus_toplevel_ex(toplevel, true);
}


static void toplevel_set_minimized(struct toplevel *t, bool minimize)
{
    struct server *server = t->server;
    if (!t->mapped || t->minimized == minimize) {
        return;
    }
    t->minimized = minimize;
    wlr_scene_node_set_enabled(&t->scene_tree->node, !minimize && t->ws == server->ws_current);
    if (minimize) {
        wlr_log(WLR_INFO, "window minimized");
        wlr_xdg_toplevel_set_activated(t->xdg_toplevel, false);
        /* park it at the bottom of the stack */
        wl_list_remove(&t->link);
        wl_list_insert(server->toplevels.prev, &t->link);
        if (t == server->grabbed_toplevel) {
            server->cursor_mode = CURSOR_PASSTHROUGH;
            server->grabbed_toplevel = NULL;
        }
        if (server->seat->keyboard_state.focused_surface == t->xdg_toplevel->base->surface) {
            struct toplevel *next = top_visible(server);
            if (next) {
                focus_toplevel(next);
            } else {
                wlr_seat_keyboard_notify_clear_focus(server->seat);
            }
        }
        wlr_seat_pointer_clear_focus(server->seat);
    } else {
        wlr_log(WLR_INFO, "window restored");
        focus_toplevel(t);
    }
    fth_sync(t);
}

/* ------------------------------------------- taskbar protocol, idle, clipboard */

/* Push title, app id and state of a window to the taskbars watching it. */
static void fth_sync(struct toplevel *t)
{
    if (!t->fth) {
        return;
    }
    struct wlr_xdg_toplevel *x = t->xdg_toplevel;
    wlr_foreign_toplevel_handle_v1_set_title(t->fth, x->title ? x->title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(t->fth, x->app_id ? x->app_id : "");
    wlr_foreign_toplevel_handle_v1_set_maximized(t->fth, t->maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(t->fth, t->minimized);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(t->fth, t->fullscreen);
    wlr_foreign_toplevel_handle_v1_set_activated(
        t->fth, t->mapped && !t->minimized &&
                    t->server->seat->keyboard_state.focused_surface == x->base->surface);
}

/* Show the windows of the current workspace, hide the others, and hand the keyboard to the
 * front window that is shown (or to nobody). */
static void workspace_refresh(struct server *server)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        wlr_scene_node_set_enabled(&t->scene_tree->node, toplevel_shown(t));
    }
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct wlr_xdg_toplevel *fx = focus ? wlr_xdg_toplevel_try_from_wlr_surface(focus) : NULL;
    struct toplevel *ft = toplevel_from_xdg(fx);
    if (ft && !toplevel_shown(ft)) {
        wlr_xdg_toplevel_set_activated(ft->xdg_toplevel, false);
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    wlr_seat_pointer_clear_focus(server->seat);
    if (server->grabbed_toplevel && !toplevel_shown(server->grabbed_toplevel)) {
        reset_cursor_mode(server);
    }
    struct toplevel *next = top_visible(server);
    if (next) {
        focus_toplevel(next);
    }
    wl_list_for_each(t, &server->toplevels, link) {
        fth_sync(t);
    }
    process_cursor_motion(server, now_msec());
}

static void switch_workspace(struct server *server, int ws)
{
    if (ws < 0 || ws >= server->config.workspaces || ws == server->ws_current) {
        return;
    }
    wlr_log(WLR_INFO, "workspace %d", ws + 1);
    server->ws_current = ws;
    workspace_refresh(server);
}

static void move_to_workspace(struct toplevel *t, int ws)
{
    struct server *server = t->server;
    if (ws < 0 || ws >= server->config.workspaces || ws == t->ws) {
        return;
    }
    wlr_log(WLR_INFO, "window moved to workspace %d", ws + 1);
    t->ws = ws;
    workspace_refresh(server);
}

/* After a reload that lowered the number of workspaces. */
static void workspace_clamp(struct server *server)
{
    int last = server->config.workspaces - 1;
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t->ws > last) {
            t->ws = last;
        }
    }
    if (server->ws_current > last) {
        server->ws_current = last;
    }
    workspace_refresh(server);
}

/* ----------------------------------------------------------- session lock */

/* Cover the whole layout (all outputs) with black so nothing shows while locked. */
static void lock_update_bg(struct server *server)
{
    if (!server->lock_bg) {
        return;
    }
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    wlr_scene_node_set_position(&server->lock_bg->node, box.x, box.y);
    wlr_scene_rect_set_size(server->lock_bg, box.width, box.height);
}

static void lock_new_surface(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_new_surface);
    struct wlr_session_lock_surface_v1 *surf = data;
    struct wlr_box box = {0};
    if (surf->output) {
        wlr_output_layout_get_box(server->output_layout, surf->output, &box);
    }
    struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(server->lock_tree, surf->surface);
    wlr_scene_node_set_position(&tree->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(surf, box.width, box.height);
    wlr_log(WLR_INFO, "lock surface for %s", surf->output ? surf->output->name : "(no output)");

    struct wlr_seat *seat = server->seat;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    struct wlr_surface *old = seat->keyboard_state.focused_surface;
    struct wlr_session_lock_surface_v1 *old_lock = old ? wlr_session_lock_surface_v1_try_from_wlr_surface(old) : NULL;
    if (keyboard && !old_lock) {
        wlr_seat_keyboard_notify_enter(seat, surf->surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void lock_release_listeners(struct server *server)
{
    wl_list_remove(&server->lock_new_surface.link);
    wl_list_remove(&server->lock_unlock.link);
    wl_list_remove(&server->lock_destroy.link);
    server->cur_lock = NULL;
}

static void lock_handle_unlock(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_unlock);
    wlr_log(WLR_INFO, "session unlocked");
    server->locked = false;
    wlr_scene_node_set_enabled(&server->lock_tree->node, false);
    struct toplevel *next = top_visible(server);
    if (next) {
        focus_toplevel(next);
    } else {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

/* The lock client went away. Without an unlock request the session stays locked. */
static void lock_handle_destroy(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, lock_destroy);
    if (server->locked) {
        wlr_log(WLR_INFO, "lock client went away without unlocking: the session stays locked");
    }
    lock_release_listeners(server);
}

static void server_new_lock(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_lock);
    struct wlr_session_lock_v1 *lock = data;
    if (server->cur_lock) { /* somebody holds the lock already */
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    wlr_log(WLR_INFO, "session locked");
    server->locked = true;
    server->cur_lock = lock;
    reset_cursor_mode(server);
    struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
    struct wlr_xdg_toplevel *fx = focus ? wlr_xdg_toplevel_try_from_wlr_surface(focus) : NULL;
    if (fx) {
        wlr_xdg_toplevel_set_activated(fx, false);
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_scene_node_set_enabled(&server->lock_tree->node, true);
    lock_update_bg(server);

    server->lock_new_surface.notify = lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
    server->lock_unlock.notify = lock_handle_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock_unlock);
    server->lock_destroy.notify = lock_handle_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock_destroy);
    wlr_session_lock_v1_send_locked(lock);
    process_cursor_motion(server, now_msec());
}

static void fth_request_activate(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_activate);
    if (!t->mapped) {
        return;
    }
    if (t->ws != t->server->ws_current) {
        switch_workspace(t->server, t->ws);
    }
    if (t->minimized) {
        toplevel_set_minimized(t, false);
    } else {
        focus_toplevel(t);
    }
}

static void fth_request_maximize(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
    if (t->mapped) {
        toplevel_apply_state(t, ev->maximized, t->fullscreen);
    }
}

static void fth_request_minimize(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
    toplevel_set_minimized(t, ev->minimized);
}

static void fth_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
    if (t->mapped) {
        toplevel_apply_state(t, t->maximized, ev->fullscreen);
    }
}

static void fth_request_close(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, fth_close);
    wlr_xdg_toplevel_send_close(t->xdg_toplevel);
}

static void fth_create(struct toplevel *t)
{
    t->fth = wlr_foreign_toplevel_handle_v1_create(t->server->foreign_toplevel_mgr);
    t->fth_activate.notify = fth_request_activate;
    wl_signal_add(&t->fth->events.request_activate, &t->fth_activate);
    t->fth_maximize.notify = fth_request_maximize;
    wl_signal_add(&t->fth->events.request_maximize, &t->fth_maximize);
    t->fth_minimize.notify = fth_request_minimize;
    wl_signal_add(&t->fth->events.request_minimize, &t->fth_minimize);
    t->fth_fullscreen.notify = fth_request_fullscreen;
    wl_signal_add(&t->fth->events.request_fullscreen, &t->fth_fullscreen);
    t->fth_close.notify = fth_request_close;
    wl_signal_add(&t->fth->events.request_close, &t->fth_close);
    fth_sync(t);
}

static void fth_destroy(struct toplevel *t)
{
    if (!t->fth) {
        return;
    }
    wl_list_remove(&t->fth_activate.link);
    wl_list_remove(&t->fth_maximize.link);
    wl_list_remove(&t->fth_minimize.link);
    wl_list_remove(&t->fth_fullscreen.link);
    wl_list_remove(&t->fth_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(t->fth);
    t->fth = NULL;
}

/* Any input resets the idle timers (ext-idle-notify). */
static void input_activity(struct server *server)
{
    if (server->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    }
}

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
    struct idle_inhibitor *inh = wl_container_of(listener, inh, destroy);
    struct server *server = inh->server;
    wl_list_remove(&inh->destroy.link);
    free(inh);
    server->idle_inhibitors--;
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, server->idle_inhibitors > 0);
}

static void server_new_idle_inhibitor(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inh = data;
    struct idle_inhibitor *inh = calloc(1, sizeof *inh);
    if (!inh) {
        return;
    }
    inh->server = server;
    inh->destroy.notify = idle_inhibitor_destroy;
    wl_signal_add(&wlr_inh->events.destroy, &inh->destroy);
    server->idle_inhibitors++;
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
}

/* ------------------------------------------------------------- keyboard */

static void reload_config(struct server *server);
static void process_cursor_motion(struct server *server, uint32_t time);

static uint32_t now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* --- outputs ordered left-to-right, then top-to-bottom ("next output" is cyclic) --- */

#define MAX_OUTPUTS 16
struct output_box {
    struct wlr_output *output;
    struct wlr_box box;
    struct wlr_box work; /* box minus panels */
};

static int output_box_cmp(const void *a, const void *b)
{
    const struct output_box *x = a, *y = b;
    if (x->box.x != y->box.x) {
        return x->box.x < y->box.x ? -1 : 1;
    }
    if (x->box.y != y->box.y) {
        return x->box.y < y->box.y ? -1 : 1;
    }
    return 0;
}

static size_t layout_outputs(struct server *server, struct output_box *out)
{
    size_t n = 0;
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (n == MAX_OUTPUTS || !wlr_output_layout_get(server->output_layout, o->wlr_output)) {
            continue;
        }
        out[n].output = o->wlr_output;
        wlr_output_layout_get_box(server->output_layout, o->wlr_output, &out[n].box);
        out[n].work = o->usable_area.width > 0 ? o->usable_area : out[n].box;
        n++;
    }
    qsort(out, n, sizeof *out, output_box_cmp);
    return n;
}

static size_t output_index_at(const struct output_box *ob, size_t n, double x, double y)
{
    for (size_t i = 0; i < n; i++) {
        if (x >= ob[i].box.x && x < ob[i].box.x + ob[i].box.width && y >= ob[i].box.y &&
            y < ob[i].box.y + ob[i].box.height) {
            return i;
        }
    }
    return 0;
}

static void clamp_into(const struct wlr_box *area, int gap, int w, int h, int *x, int *y)
{
    int max_x = area->x + area->width - gap - w;
    int max_y = area->y + area->height - gap - h;
    if (*x > max_x) {
        *x = max_x;
    }
    if (*y > max_y) {
        *y = max_y;
    }
    if (*x < area->x + gap) {
        *x = area->x + gap;
    }
    if (*y < area->y + gap) {
        *y = area->y + gap;
    }
}

static void move_to_next_output(struct toplevel *t)
{
    struct server *server = t->server;
    struct output_box obs[MAX_OUTPUTS];
    size_t n = layout_outputs(server, obs);
    if (n < 2) {
        return;
    }
    bool fitted = t->maximized || t->fullscreen;
    struct deco_insets ins = toplevel_insets(t);
    struct wlr_box now = toplevel_outer(t);
    /* the geometry the window will have again when it is not fitted, as an outer rectangle */
    struct wlr_box ref = now;
    if (fitted) {
        ref = (struct wlr_box){t->saved.x - ins.left, t->saved.y - ins.top,
                               t->saved.width + ins.left + ins.right,
                               t->saved.height + ins.top + ins.bottom};
    }
    size_t cur = output_index_at(obs, n, now.x + now.width / 2.0, now.y + now.height / 2.0);
    const struct wlr_box *cb = &obs[cur].box;
    const struct wlr_box *nb = &obs[(cur + 1) % n].box;
    int x = nb->x + (ref.x - cb->x);
    int y = nb->y + (ref.y - cb->y);
    clamp_into(&obs[(cur + 1) % n].work, server->config.gap, ref.width, ref.height, &x, &y);
    if (fitted) { /* re-fit on the new output through the saved geometry */
        t->saved.x = x + ins.left;
        t->saved.y = y + ins.top;
        toplevel_apply_state(t, t->maximized, t->fullscreen);
    } else {
        toplevel_move_to_ex(t, x + ins.left, y + ins.top, true);
    }
}

/* Warp the pointer to the middle of the next output and focus its top window. */
static void focus_next_output(struct server *server)
{
    struct output_box obs[MAX_OUTPUTS];
    size_t n = layout_outputs(server, obs);
    if (n < 2) {
        return;
    }
    size_t cur = output_index_at(obs, n, server->cursor->x, server->cursor->y);
    const struct wlr_box *nb = &obs[(cur + 1) % n].box;
    wlr_cursor_warp(server->cursor, NULL, nb->x + nb->width / 2.0, nb->y + nb->height / 2.0);
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t->minimized) {
            continue;
        }
        struct wlr_box g = toplevel_geometry(t);
        double cx = g.x + g.width / 2.0, cy = g.y + g.height / 2.0;
        if (cx >= nb->x && cx < nb->x + nb->width && cy >= nb->y && cy < nb->y + nb->height) {
            focus_toplevel(t);
            break;
        }
    }
    process_cursor_motion(server, now_msec());
}

static void dispatch_action(struct server *server, enum action action, const char *arg)
{
    struct toplevel *top = focused_visible(server);
    wlr_log(WLR_DEBUG, "action %s, focused window: %s", action_name(action),
            top && top->xdg_toplevel->title ? top->xdg_toplevel->title : "(none)");
    switch (action) {
    case ACTION_QUIT:
        wl_display_terminate(server->display);
        break;
    case ACTION_SPAWN: {
        char *cmd = expand_command(server, arg);
        spawn(cmd);
        free(cmd);
        break;
    }
    case ACTION_CLOSE:
        if (top) {
            wlr_xdg_toplevel_send_close(top->xdg_toplevel);
        }
        break;
    case ACTION_CYCLE: {
        /* Focus the bottom-most visible window, which raises it. */
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (toplevel_shown(t)) {
                focus_toplevel(t);
                break;
            }
        }
        break;
    }
    case ACTION_TOGGLE_MAXIMIZE:
        if (top) {
            toplevel_apply_state(top, !top->maximized, false);
        }
        break;
    case ACTION_TOGGLE_FULLSCREEN:
        if (top) {
            toplevel_apply_state(top, top->maximized, !top->fullscreen);
        }
        break;
    case ACTION_MINIMIZE:
        if (top) {
            toplevel_set_minimized(top, true);
        }
        break;
    case ACTION_RESTORE: {
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (t->minimized && t->ws == server->ws_current) {
                toplevel_set_minimized(t, false);
                break;
            }
        }
        break;
    }
    case ACTION_RELOAD:
        reload_config(server);
        break;
    case ACTION_MOVE_OUTPUT:
        if (top) {
            move_to_next_output(top);
        }
        break;
    case ACTION_FOCUS_OUTPUT:
        focus_next_output(server);
        break;
    case ACTION_WORKSPACE:
        switch_workspace(server, atoi(arg) - 1);
        break;
    case ACTION_MOVE_WORKSPACE:
        if (top) {
            move_to_workspace(top, atoi(arg) - 1);
        }
        break;
    case ACTION_MOVE:
    case ACTION_RESIZE:
        break; /* mouse-only */
    }
}

/* The key's symbol at shift level 0 ("m" even when Shift is held), so that bindings
 * are written as modifiers + the unshifted key. */
static xkb_keysym_t base_keysym(struct wlr_keyboard *kb, xkb_keycode_t code)
{
    struct xkb_keymap *map = xkb_state_get_keymap(kb->xkb_state);
    xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, code);
    const xkb_keysym_t *syms;
    int n = xkb_keymap_key_get_syms_by_level(map, code, layout, 0, &syms);
    return n > 0 ? syms[0] : XKB_KEY_NoSymbol;
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
    struct keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
    struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;
    input_activity(server);

    uint32_t keycode = event->keycode + 8; /* libinput -> xkb */

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* Ctrl+Alt+F1..F12 switch the virtual terminal (works while locked, too) */
        xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->wlr_keyboard->xkb_state, keycode);
        if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            struct wlr_session *session = wlr_backend_get_session(server->backend);
            if (session) {
                wlr_session_change_vt(session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            }
            return;
        }
        uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
        const struct keybind *bind = config_find_keybind(
            &server->config, modifiers, base_keysym(keyboard->wlr_keyboard, keycode));
        if (bind && (!server->locked || bind->action == ACTION_QUIT)) {
            dispatch_action(server, bind->action, bind->arg);
            handled = true;
        } else if (bind) {
            handled = false; /* locked: the key goes to the lock client like any other */
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data)
{
    struct keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void update_seat_capabilities(struct server *server)
{
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

/* [keyboard] section: layout (xkb rule names) and repeat rate. A layout that does not
 * compile falls back to the default one. Virtual keyboards keep the keymap their client
 * sent, so on reload only physical keyboards get a new keymap. */
static void apply_keyboard_config(struct server *server, struct keyboard *keyboard, bool set_keymap)
{
    const struct config *cfg = &server->config;
    if (set_keymap) {
        struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_rule_names names = {
            .rules = cfg->kb_rules,
            .model = cfg->kb_model,
            .layout = cfg->kb_layout,
            .variant = cfg->kb_variant,
            .options = cfg->kb_options,
        };
        struct xkb_keymap *keymap =
            xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) {
            wlr_log(WLR_ERROR,
                    "cannot compile keymap (layout='%s' variant='%s' options='%s'), "
                    "using the default layout",
                    cfg->kb_layout ? cfg->kb_layout : "", cfg->kb_variant ? cfg->kb_variant : "",
                    cfg->kb_options ? cfg->kb_options : "");
            keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        }
        if (keymap) {
            wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
            xkb_keymap_unref(keymap);
        }
        xkb_context_unref(context);
    }
    wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, cfg->repeat_rate, cfg->repeat_delay);
}

static void server_new_keyboard(struct server *server, struct wlr_keyboard *wlr_keyboard,
                                bool is_virtual)
{
    struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;
    keyboard->is_virtual = is_virtual;
    apply_keyboard_config(server, keyboard, true);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);

    /* A window focused before any keyboard existed never got keyboard.enter. */
    focus_toplevel(top_visible(server));
}

static void server_new_pointer(struct server *server, struct wlr_input_device *device)
{
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, wlr_keyboard_from_input_device(device), false);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }
    update_seat_capabilities(server);
}

static void server_new_virtual_pointer(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    server_new_pointer(server, &event->new_pointer->pointer.base);
    update_seat_capabilities(server);
}

static void server_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vkbd = data;
    server_new_keyboard(server, &vkbd->keyboard, true);
    update_seat_capabilities(server);
}

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* --------------------------------------------------------------- cursor */

/* The window under (lx, ly). When the point is on a client surface, *surface is set and
 * *part is DECO_CLIENT; when it is on a server-side frame, *surface stays NULL and *part
 * (and *edges for resize zones) say which part of the frame it is. */
static struct toplevel *toplevel_at(struct server *server, double lx, double ly,
                                    struct wlr_surface **surface, double *sx, double *sy,
                                    enum deco_part *part, uint32_t *edges)
{
    *part = DECO_OUTSIDE;
    *edges = 0;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);

    /* Walk up to the tree that belongs to a toplevel. */
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    struct toplevel *toplevel = tree ? tree->node.data : NULL;

    if (scene_surface) {
        *surface = scene_surface->surface;
        *part = DECO_CLIENT;
        return toplevel;
    }
    if (toplevel && toplevel->chrome && node == &toplevel->chrome->node) {
        struct wlr_box g = toplevel_geometry(toplevel);
        *part = deco_hit_test(&server->theme, g.width, g.height, lx - g.x, ly - g.y, edges);
        return *part == DECO_OUTSIDE ? NULL : toplevel;
    }
    return NULL;
}

static const char *resize_cursor_name(uint32_t edges)
{
    switch (edges) {
    case WLR_EDGE_TOP:
        return "n-resize";
    case WLR_EDGE_BOTTOM:
        return "s-resize";
    case WLR_EDGE_LEFT:
        return "w-resize";
    case WLR_EDGE_RIGHT:
        return "e-resize";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT:
        return "nw-resize";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT:
        return "ne-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT:
        return "sw-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT:
        return "se-resize";
    default:
        return "default";
    }
}

static void reset_cursor_mode(struct server *server)
{
    server->cursor_mode = CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

static void begin_interactive(struct toplevel *toplevel, enum cursor_mode mode, uint32_t edges,
                              bool check_focus)
{
    struct server *server = toplevel->server;
    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
    if (toplevel->maximized || toplevel->fullscreen) {
        return; /* un-maximize first */
    }
    if (check_focus && (focused == NULL || toplevel->xdg_toplevel->base->surface !=
                                               wlr_surface_get_root_surface(focused))) {
        return; /* a client may only start a grab while the pointer is over its window */
    }
    animations_cancel(toplevel, true); /* the window follows the pointer from where it is */
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
        double border_x = (toplevel->scene_tree->node.x + geo.x) +
                          ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo.y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;
        server->grab_geobox = geo;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;
        server->resize_edges = edges;
    }
}

/* If `target` is closer to `pos` than *best, remember it. */
static void try_snap(double pos, double target, double *best, double *out)
{
    double d = fabs(pos - target);
    if (d < *best) {
        *best = d;
        *out = target;
    }
}

static void process_cursor_move(struct server *server)
{
    struct toplevel *toplevel = server->grabbed_toplevel;
    double nx = server->cursor->x - server->grab_x;
    double ny = server->cursor->y - server->grab_y;

    /* Snap the window's geometry to the output edges and to other windows (keeping
     * `gap` between them); the nearest candidate on each axis wins. */
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    const struct config *cfg = &server->config;
    struct deco_insets ins = toplevel_insets(toplevel);
    /* work on the outer rectangle (content plus frame) */
    double x = nx + geo.x - ins.left, y = ny + geo.y - ins.top;
    int w = geo.width + ins.left + ins.right, h = geo.height + ins.top + ins.bottom;
    double snap = cfg->snap_distance, gap = cfg->gap;
    double best_x = snap, best_y = snap, tx = x, ty = y;
    struct wlr_box box = work_area_at(server, server->cursor->x, server->cursor->y);
    if (box.width > 0 && cfg->snap_to_edges) {
        try_snap(x, box.x + gap, &best_x, &tx);
        try_snap(x, box.x + box.width - gap - w, &best_x, &tx);
        try_snap(y, box.y + gap, &best_y, &ty);
        try_snap(y, box.y + box.height - gap - h, &best_y, &ty);
    }
    if (cfg->snap_to_windows) {
        struct toplevel *o;
        wl_list_for_each(o, &server->toplevels, link) {
            if (o == toplevel || o->minimized) {
                continue;
            }
            struct wlr_box og = toplevel_outer(o);
            bool overlap_y = y < og.y + og.height + snap && y + h > og.y - snap;
            bool overlap_x = x < og.x + og.width + snap && x + w > og.x - snap;
            if (overlap_y) {
                try_snap(x, og.x + og.width + gap, &best_x, &tx);
                try_snap(x, og.x - gap - w, &best_x, &tx);
            }
            if (overlap_x) {
                try_snap(y, og.y + og.height + gap, &best_y, &ty);
                try_snap(y, og.y - gap - h, &best_y, &ty);
            }
        }
    }
    nx = tx + ins.left - geo.x;
    ny = ty + ins.top - geo.y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, nx, ny);
}

static void process_cursor_resize(struct server *server)
{
    struct toplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo.x, new_top - geo.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
}

static void process_cursor_motion(struct server *server, uint32_t time)
{
    if (server->cursor_mode == CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    enum deco_part part;
    uint32_t edges;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface,
                                            &sx, &sy, &part, &edges);
    if (!toplevel) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    } else {
        if (!surface) { /* over a server-side frame: resize cursors on the edges */
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                                   part == DECO_BORDER ? resize_cursor_name(edges) : "default");
        }
        if (server->config.focus == FOCUS_FOLLOW_MOUSE) {
            focus_toplevel_ex(toplevel, false); /* focus without raising */
        }
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    input_activity(server);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    input_activity(server);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    input_activity(server);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
        reset_cursor_mode(server);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    enum deco_part part;
    uint32_t hit_edges;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface,
                                            &sx, &sy, &part, &hit_edges);
    focus_toplevel(toplevel); /* click to focus + raise */

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    const struct mousebind *mbind = config_find_mousebind(&server->config, mods, event->button);
    if (toplevel && mbind) {
        /* modifier+drag: do not forward the click to the client. */
        if (mbind->action == ACTION_MOVE) {
            begin_interactive(toplevel, CURSOR_MOVE, 0, false);
        } else {
            struct wlr_box g = toplevel_geometry(toplevel);
            uint32_t edges = (server->cursor->x - g.x < g.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                             (server->cursor->y - g.y < g.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
            begin_interactive(toplevel, CURSOR_RESIZE, edges, false);
        }
        return;
    }

    if (!toplevel && surface) { /* a panel/launcher that takes keyboard input when clicked */
        struct wlr_layer_surface_v1 *layer =
            wlr_layer_surface_v1_try_from_wlr_surface(wlr_surface_get_root_surface(surface));
        if (layer && layer->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
            focus_layer_surface(server, layer);
        }
    }
    if (toplevel && !surface) { /* the server-side frame */
        if (event->button != BTN_LEFT) {
            return;
        }
        static uint32_t last_click_time;
        static struct toplevel *last_click_toplevel;
        switch (part) {
        case DECO_CLOSE:
            wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
            break;
        case DECO_MAXIMIZE:
            toplevel_apply_state(toplevel, !toplevel->maximized, false);
            break;
        case DECO_MINIMIZE:
            toplevel_set_minimized(toplevel, true);
            break;
        case DECO_TITLE:
            if (last_click_toplevel == toplevel && event->time_msec - last_click_time < 400) {
                toplevel_apply_state(toplevel, !toplevel->maximized, false); /* double click */
                last_click_toplevel = NULL;
            } else {
                last_click_toplevel = toplevel;
                last_click_time = event->time_msec;
                begin_interactive(toplevel, CURSOR_MOVE, 0, false);
            }
            break;
        case DECO_BORDER:
            if (hit_edges) {
                begin_interactive(toplevel, CURSOR_RESIZE, hit_edges, false);
            }
            break;
        default:
            break;
        }
        return;
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    input_activity(server);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* -------------------------------------------------------------- outputs */

static void output_frame(struct wl_listener *listener, void *data)
{
    struct output *output = wl_container_of(listener, output, frame);
    animations_tick(output->server);
    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
    struct output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
    struct output *output = wl_container_of(listener, output, destroy);
    struct layer_surface *ls, *ls_tmp;
    wl_list_for_each_safe(ls, ls_tmp, &output->server->layer_surfaces, link) {
        if (ls->layer->output == output->wlr_output) {
            wlr_layer_surface_v1_destroy(ls->layer); /* the clients' panels cannot live on */
        }
    }
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    const struct output_cfg *oc = config_find_output(&server->config, wlr_output->name);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, !oc || oc->enabled);
    if (oc && oc->scale > 0) {
        wlr_output_state_set_scale(&state, oc->scale);
    }
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);

    if (oc && !oc->enabled) {
        wlr_log(WLR_INFO, "output %s is disabled in the config", wlr_output->name);
        return;
    }
    struct wlr_output_layout_output *l_output =
        (oc && oc->has_pos) ? wlr_output_layout_add(server->output_layout, wlr_output, oc->x, oc->y)
                            : wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    output->usable_area = (struct wlr_box){0};
    arrange_layers(output); /* also sets the usable area */
    lock_update_bg(server);
    wlr_log(WLR_INFO, "output %s added", wlr_output->name);
}

/* ------------------------------------------------------ xdg-shell windows */

/* ----------------------------------------------- server-side decorations */

static struct toplevel *toplevel_from_xdg(struct wlr_xdg_toplevel *xdg)
{
    struct wlr_scene_tree *tree = xdg ? xdg->base->data : NULL;
    return tree ? tree->node.data : NULL;
}

/* Answer the client's decoration request according to the config. */
static void decoration_apply(struct toplevel *t)
{
    if (!t->decoration) {
        return;
    }
    bool server_side = t->server->config.decorations;
    t->ssd = server_side;
    /* wlroots refuses to schedule a configure before the surface's initial commit; the
     * mode is sent from toplevel_commit() then. */
    struct wlr_xdg_surface *base = t->xdg_toplevel->base;
    if (base->initialized || base->initial_commit) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            t->decoration, server_side ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                       : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }
    frame_refresh(t);
}

static void deco_handle_request_mode(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, deco_request_mode);
    decoration_apply(t);
}

static void deco_handle_destroy(struct wl_listener *listener, void *data)
{
    struct toplevel *t = wl_container_of(listener, t, deco_destroy);
    wl_list_remove(&t->deco_request_mode.link);
    wl_list_remove(&t->deco_destroy.link);
    t->decoration = NULL;
    t->ssd = false;
    frame_refresh(t);
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    struct toplevel *t = toplevel_from_xdg(decoration->toplevel);
    if (!t) {
        return;
    }
    t->decoration = decoration;
    t->deco_request_mode.notify = deco_handle_request_mode;
    wl_signal_add(&decoration->events.request_mode, &t->deco_request_mode);
    t->deco_destroy.notify = deco_handle_destroy;
    wl_signal_add(&decoration->events.destroy, &t->deco_destroy);
    decoration_apply(t);
}

/* The frame shows whether its window has keyboard focus. */
static void handle_keyboard_focus_change(struct wl_listener *listener, void *data)
{
    struct wlr_seat_keyboard_focus_change_event *event = data;
    struct wlr_surface *surfaces[2] = {event->old_surface, event->new_surface};
    for (int i = 0; i < 2; i++) {
        if (!surfaces[i]) {
            continue;
        }
        struct wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_try_from_wlr_surface(surfaces[i]);
        struct toplevel *t = toplevel_from_xdg(xdg);
        if (t) {
            frame_refresh(t);
            fth_sync(t);
        }
    }
}

static void toplevel_set_title(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    frame_refresh(toplevel);
    fth_sync(toplevel);
}


static void toplevel_map(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wlr_log(WLR_INFO, "window mapped: title=%s app_id=%s",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(none)",
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(none)");
    toplevel->mapped = true;
    toplevel->ws = toplevel->server->ws_current;
    if (!toplevel->maximized && !toplevel->fullscreen) {
        place_new_toplevel(toplevel);
    }
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    fth_create(toplevel);
    focus_toplevel(toplevel);
    frame_refresh(toplevel);
    animate_open(toplevel);
    snapshot_refresh(toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    wlr_log(WLR_INFO, "window unmapped");
    struct server *server = toplevel->server;
    animations_cancel(toplevel, false);
    animate_close(toplevel);
    if (toplevel == server->grabbed_toplevel) {
        reset_cursor_mode(server);
    }
    wl_list_remove(&toplevel->link);
    toplevel->mapped = false;
    fth_destroy(toplevel);
    /* Hand keyboard focus to the next window. */
    if (server->seat->keyboard_state.focused_surface == toplevel->xdg_toplevel->base->surface) {
        struct toplevel *next = top_visible(server);
        if (next) {
            focus_toplevel(next);
        } else {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }
}

static void toplevel_commit(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit) {
        decoration_apply(toplevel);
        if (toplevel->maximized || toplevel->fullscreen) {
            /* requested before the first commit: size it for the output now */
            toplevel_apply_state(toplevel, toplevel->maximized, toplevel->fullscreen);
        } else {
            /* Let the client pick its own size. */
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
    frame_refresh(toplevel); /* cheap when size, focus and title did not change */
    snapshot_refresh(toplevel);
}

static void toplevel_destroy(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->set_title.link);
    if (toplevel->decoration) {
        wl_list_remove(&toplevel->deco_request_mode.link);
        wl_list_remove(&toplevel->deco_destroy.link);
    }
    animations_cancel(toplevel, false);
    if (toplevel->last_frame) {
        wlr_scene_node_destroy(&toplevel->last_frame->node);
    }
    free(toplevel->frame_title);
    free(toplevel);
}

static void toplevel_request_move(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, CURSOR_MOVE, 0, true);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    begin_interactive(toplevel, CURSOR_RESIZE, event->edges, true);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    toplevel_apply_state(toplevel, toplevel->xdg_toplevel->requested.maximized,
                         toplevel->fullscreen);
}

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    toplevel_apply_state(toplevel, toplevel->maximized,
                         toplevel->xdg_toplevel->requested.fullscreen);
}

static void toplevel_request_minimize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
    toplevel_set_minimized(toplevel, true);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(server->windows_tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
}

static void popup_commit(struct wl_listener *listener, void *data)
{
    struct popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
    struct popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

/* Show an xdg popup under `parent_tree` (a window's or a layer surface's scene tree). */
static void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree)
{
    struct popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_popup *xdg_popup = data;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    popup_create(xdg_popup, parent->data);
}

/* ------------------------------------------------------------ layer shell */

static struct output *output_of(struct server *server, struct wlr_output *wlr_output)
{
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output == wlr_output) {
            return o;
        }
    }
    return NULL;
}

static void focus_layer_surface(struct server *server, struct wlr_layer_surface_v1 *layer)
{
    struct wlr_seat *seat = server->seat;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (!keyboard || server->locked || seat->keyboard_state.focused_surface == layer->surface) {
        return;
    }
    struct wlr_surface *prev = seat->keyboard_state.focused_surface;
    if (prev) {
        struct wlr_xdg_toplevel *prev_top = wlr_xdg_toplevel_try_from_wlr_surface(prev);
        if (prev_top) {
            wlr_xdg_toplevel_set_activated(prev_top, false);
        }
    }
    wlr_seat_keyboard_notify_enter(seat, layer->surface, keyboard->keycodes,
                                   keyboard->num_keycodes, &keyboard->modifiers);
}

/* Lay out the layer surfaces of one output, from the overlay layer down, and compute the
 * area that is left for windows. */
static void arrange_layers(struct output *output)
{
    struct server *server = output->server;
    if (!wlr_output_layout_get(server->output_layout, output->wlr_output)) {
        return;
    }
    struct wlr_box full = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
    struct wlr_box usable = full;
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
         layer--) {
        struct layer_surface *ls;
        wl_list_for_each(ls, &server->layer_surfaces, link) {
            if (ls->layer->output != output->wlr_output || (int)ls->layer->current.layer != layer) {
                continue;
            }
            if (ls->layer->surface->mapped) {
                wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
            } else { /* configure, but a panel that is not shown reserves no space */
                struct wlr_box ignored = usable;
                wlr_scene_layer_surface_v1_configure(ls->scene, &full, &ignored);
            }
        }
    }
    bool changed = memcmp(&output->usable_area, &usable, sizeof usable) != 0;
    output->usable_area = usable;
    if (changed) { /* maximized windows follow the free area */
        struct toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            if (t->maximized || t->fullscreen) {
                toplevel_apply_state(t, t->maximized, t->fullscreen);
            }
        }
    }

    /* a launcher or lock screen on top/overlay that wants exclusive keyboard input gets it */
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; layer--) {
        struct layer_surface *ls;
        wl_list_for_each(ls, &server->layer_surfaces, link) {
            if ((int)ls->layer->current.layer == layer && ls->layer->surface->mapped &&
                ls->layer->current.keyboard_interactive ==
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                focus_layer_surface(server, ls->layer);
                return;
            }
        }
    }
}

static void layer_surface_map(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, map);
    wlr_log(WLR_INFO, "layer surface mapped: namespace=%s layer=%d", ls->layer->namespace,
            ls->layer->current.layer);
    struct output *o = output_of(ls->server, ls->layer->output);
    if (o) {
        arrange_layers(o);
    }
}

static void layer_surface_unmap(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, unmap);
    struct server *server = ls->server;
    wlr_log(WLR_INFO, "layer surface unmapped: namespace=%s", ls->layer->namespace);
    if (server->seat->keyboard_state.focused_surface == ls->layer->surface) {
        struct toplevel *next = top_visible(server);
        if (next) {
            focus_toplevel(next);
        } else {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }
    struct output *o = output_of(server, ls->layer->output);
    if (o) {
        arrange_layers(o);
    }
}

static void layer_surface_commit(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, commit);
    struct wlr_layer_surface_v1 *layer = ls->layer;
    if (!layer->initialized) {
        return;
    }
    /* the layer may change after creation */
    struct wlr_scene_tree *want = ls->server->layer_trees[layer->current.layer];
    if (ls->scene->tree->node.parent != want) {
        wlr_scene_node_reparent(&ls->scene->tree->node, want);
    }
    struct output *o = output_of(ls->server, layer->output);
    if (o && (layer->initial_commit || layer->current.committed != 0 || layer->surface->mapped)) {
        arrange_layers(o);
    }
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
    popup_create(data, ls->scene->tree);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, destroy);
    struct server *server = ls->server;
    struct output *o = output_of(server, ls->layer->output);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->new_popup.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->link);
    free(ls);
    if (o) {
        arrange_layers(o);
    }
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer = data;
    if (!layer->output) { /* the client let us choose: the output under the pointer */
        struct wlr_output *out =
            wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
        if (!out) {
            out = wlr_output_layout_get_center_output(server->output_layout);
        }
        layer->output = out;
    }
    if (!layer->output) {
        wlr_layer_surface_v1_destroy(layer);
        return;
    }
    struct layer_surface *ls = calloc(1, sizeof(*ls));
    ls->server = server;
    ls->layer = layer;
    ls->scene = wlr_scene_layer_surface_v1_create(server->layer_trees[layer->pending.layer], layer);
    ls->scene->tree->node.data = NULL;
    ls->map.notify = layer_surface_map;
    wl_signal_add(&layer->surface->events.map, &ls->map);
    ls->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_surface_commit;
    wl_signal_add(&layer->surface->events.commit, &ls->commit);
    ls->new_popup.notify = layer_surface_new_popup;
    wl_signal_add(&layer->events.new_popup, &ls->new_popup);
    ls->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer->events.destroy, &ls->destroy);
    wl_list_insert(&server->layer_surfaces, &ls->link);
}

/* --------------------------------------------------------------- config */

static void config_log_cb(int level, int line, const char *msg, void *data)
{
    struct server *server = data;
    if (level == CONFIG_ERROR) {
        wlr_log(WLR_ERROR, "config %s:%d: %s", server->config_path, line, msg);
    } else {
        wlr_log(WLR_INFO, "config %s:%d: warning: %s", server->config_path, line, msg);
    }
}

struct theme_log_ctx {
    const char *path;
};

static void theme_log_cb(int level, int line, const char *msg, void *data)
{
    struct theme_log_ctx *ctx = data;
    if (level == INI_ERROR) {
        wlr_log(WLR_ERROR, "theme %s:%d: %s", ctx->path, line, msg);
    } else {
        wlr_log(WLR_INFO, "theme %s:%d: warning: %s", ctx->path, line, msg);
    }
}

/* Render the theme's templates for the companion tools (waybar, fuzzel, ...) with the
 * sfwc-theme-apply helper that is installed next to the compositor. Runs to completion
 * (it takes a few milliseconds) so that autostarted tools see current files. */
static void run_theme_apply(struct server *server)
{
    const char *off = getenv("SFWC_NO_THEME_APPLY");
    if (!server->config.templates_enabled || (off && *off == '1') || !getenv("XDG_RUNTIME_DIR")) {
        return;
    }
    char *argv[4 + 2 * 33 + 1];
    int n = 0;
    char self[1024], helper[1100];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    const char *prog = "sfwc-theme-apply";
    if (len > 0) {
        self[len] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(helper, sizeof helper, "%s/sfwc-theme-apply", self);
            if (access(helper, X_OK) == 0) {
                prog = helper;
            }
        }
    }
    char *config_dir = server->config_path ? strdup(server->config_path) : NULL;
    if (config_dir) {
        char *slash = strrchr(config_dir, '/');
        if (slash) {
            *slash = 0;
        }
    }
    argv[n++] = (char *)prog;
    argv[n++] = "--quiet";
    argv[n++] = "--theme";
    argv[n++] = server->config.theme;
    if (config_dir) {
        argv[n++] = "--config-dir";
        argv[n++] = config_dir;
    }
    for (size_t i = 0; i < server->config.n_templates_off && n < 4 + 2 * 32; i++) {
        argv[n++] = "--skip";
        argv[n++] = server->config.templates_off[i];
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            wlr_log(WLR_DEBUG, "sfwc-theme-apply is not installed, themes only style the windows");
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            wlr_log(WLR_ERROR, "sfwc-theme-apply reported a problem (see its messages above)");
        } else {
            wlr_log(WLR_INFO, "theme templates rendered");
        }
    }
    free(config_dir);
}

static void load_theme_file(struct server *server);

/* Load the theme named in the config and render the templates for the companion tools. */
static void load_theme(struct server *server)
{
    load_theme_file(server);
    run_theme_apply(server);
}

/* Load the theme named in the config (the built-in default if there is no such file). */
static void load_theme_file(struct server *server)
{
    if (server->theme.name) {
        theme_finish(&server->theme);
    }
    theme_init_default(&server->theme);
    server->theme_gen++;

    char *config_dir = NULL;
    if (server->config_path) {
        config_dir = strdup(server->config_path);
        char *slash = strrchr(config_dir, '/');
        if (slash) {
            *slash = '\0';
        }
    }
    char *path = theme_find(server->config.theme, config_dir);
    free(config_dir);
    if (!path) {
        if (strcmp(server->config.theme, "default") != 0) {
            wlr_log(WLR_ERROR, "theme '%s' not found, using the built-in default theme",
                    server->config.theme);
        }
        return;
    }
    struct theme_log_ctx ctx = {path};
    if (theme_load_file(&server->theme, path, theme_log_cb, &ctx)) {
        wlr_log(WLR_INFO, "theme loaded: %s", path);
    } else {
        wlr_log(WLR_ERROR, "cannot read theme %s, using the built-in default theme", path);
    }
    free(path);
}

/* Re-read the config file. A file that cannot be read keeps the current config; invalid
 * entries inside a readable file are reported and fall back to their defaults. */
static void reload_config(struct server *server)
{
    if (!server->config_path) {
        wlr_log(WLR_INFO, "no config file location known, nothing to reload");
        return;
    }
    struct config fresh;
    config_init_defaults(&fresh);
    if (!config_load_file(&fresh, server->config_path, config_log_cb, server)) {
        wlr_log(WLR_ERROR, "cannot read %s, keeping the current configuration",
                server->config_path);
        config_finish(&fresh);
        return;
    }
    config_finish(&server->config);
    server->config = fresh;
    wlr_log(WLR_INFO, "config loaded: %s", server->config_path);

    struct keyboard *kb;
    wl_list_for_each(kb, &server->keyboards, link) {
        apply_keyboard_config(server, kb, !kb->is_virtual);
    }
    /* scale and position of outputs that are already running */
    struct output *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!wlr_output_layout_get(server->output_layout, out->wlr_output)) {
            continue; /* disabled at startup: `enabled` is only read when the output appears */
        }
        const struct output_cfg *oc = config_find_output(&server->config, out->wlr_output->name);
        double want = oc && oc->scale > 0 ? oc->scale : 1.0;
        if (fabs(out->wlr_output->scale - want) > 0.001) {
            struct wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_scale(&state, want);
            if (!wlr_output_commit_state(out->wlr_output, &state)) {
                wlr_log(WLR_ERROR, "cannot set scale %.2f on %s", want, out->wlr_output->name);
            }
            wlr_output_state_finish(&state);
        }
        if (oc && oc->has_pos) {
            wlr_output_layout_add(server->output_layout, out->wlr_output, oc->x, oc->y);
        }
    }

    load_theme(server);
    workspace_clamp(server);
    wl_list_for_each(out, &server->outputs, link) {
        arrange_layers(out); /* output positions may have changed */
    }

    /* windows that are fitted to the output depend on the gap and on the frame */
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        decoration_apply(t);
        if (t->maximized || t->fullscreen) {
            toplevel_apply_state(t, t->maximized, t->fullscreen);
        }
        frame_refresh(t);
    }
}

/* Editors save by writing a temp file and renaming it, so watch the directory. */
static int handle_config_event(int fd, uint32_t mask, void *data)
{
    struct server *server = data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && strcmp(ev->name, server->config_name) == 0) {
                changed = true;
            }
            p += sizeof(*ev) + ev->len;
        }
    }
    if (changed) {
        reload_config(server);
    }
    return 0;
}

static void init_config(struct server *server, struct wl_event_loop *loop)
{
    config_init_defaults(&server->config);
    server->inotify_fd = -1;
    server->config_path = config_default_path();
    if (!server->config_path) {
        wlr_log(WLR_INFO, "no HOME/XDG_CONFIG_HOME, using built-in defaults");
        load_theme(server);
        return;
    }
    if (config_load_file(&server->config, server->config_path, config_log_cb, server)) {
        wlr_log(WLR_INFO, "config loaded: %s", server->config_path);
    } else {
        wlr_log(WLR_INFO, "no config file at %s, using built-in defaults", server->config_path);
    }
    load_theme(server);

    char *dir = strdup(server->config_path);
    char *slash = strrchr(dir, '/');
    if (slash == dir) {
        dir[1] = '\0'; /* "/sfwc.conf" -> watch "/" */
        server->config_name = strdup(server->config_path + 1);
    } else if (slash) {
        *slash = '\0';
        server->config_name = strdup(slash + 1);
    } else {
        strcpy(dir, ".");
        server->config_name = strdup(server->config_path);
    }
    const char *no_watch = getenv("SFWC_NO_CONFIG_WATCH"); /* testing aid */
    int fd = no_watch && !strcmp(no_watch, "1") ? -1 : inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO) >= 0) {
        server->inotify_fd = fd;
        server->inotify_source =
            wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, handle_config_event, server);
    } else {
        wlr_log(WLR_INFO, "cannot watch %s for changes; use the reload-config action", dir);
        if (fd >= 0) {
            close(fd);
        }
    }
    free(dir);
}

/* ----------------------------------------------------------------- main */

/* Testing aid: SFWC_TEST_OUTPUTS=1024x600,800x600 adds headless outputs. */
static void add_test_outputs(struct wlr_backend *backend, void *data)
{
    if (!wlr_backend_is_headless(backend)) {
        return;
    }
    char *spec = strdup(data), *save = NULL;
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned w, h;
        if (sscanf(tok, "%ux%u", &w, &h) == 2) {
            wlr_headless_add_output(backend, w, h);
        }
    }
    free(spec);
}

static int handle_signal(int signo, void *data)
{
    wlr_log(WLR_INFO, "signal %d, shutting down", signo);
    wl_display_terminate(data);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-s startup-command] [-h]\n", argv0);
}

int main(int argc, char *argv[])
{
    char *startup_cmd = NULL;
    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        default:
            usage(argv[0]);
            return c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* SFWC_LOG_LEVEL=debug|info|error|silent (default info) */
    enum wlr_log_importance log_level = WLR_INFO;
    const char *lvl = getenv("SFWC_LOG_LEVEL");
    if (lvl && !strcmp(lvl, "debug")) {
        log_level = WLR_DEBUG;
    } else if (lvl && !strcmp(lvl, "error")) {
        log_level = WLR_ERROR;
    } else if (lvl && !strcmp(lvl, "silent")) {
        log_level = WLR_SILENT;
    }
    wlr_log_init(log_level, NULL);

    struct server server = {0};
    server.display = wl_display_create();
    if (!server.display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return EXIT_FAILURE;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    wl_event_loop_add_signal(loop, SIGINT, handle_signal, server.display);
    wl_event_loop_add_signal(loop, SIGTERM, handle_signal, server.display);
    init_config(&server, loop);

    server.backend = wlr_backend_autocreate(loop, NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return EXIT_FAILURE;
    }

    const char *test_outputs = getenv("SFWC_TEST_OUTPUTS");
    if (test_outputs && *test_outputs) {
        if (wlr_backend_is_multi(server.backend)) {
            wlr_multi_for_each_backend(server.backend, add_test_outputs, (void *)test_outputs);
        } else {
            add_test_outputs(server.backend, (void *)test_outputs);
        }
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return EXIT_FAILURE;
    }
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return EXIT_FAILURE;
    }

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
    wlr_screencopy_manager_v1_create(server.display);

    server.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.display);
    server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
    wl_signal_add(&server.xdg_decoration_mgr->events.new_toplevel_decoration,
                  &server.new_toplevel_decoration);
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    /* z-order, bottom to top: background, bottom, windows, top, overlay */
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&server.scene->tree);
    server.windows_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&server.scene->tree);
    server.lock_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);
    server.lock_bg = wlr_scene_rect_create(server.lock_tree, 0, 0, (float[4]){0, 0, 0, 1});
    wl_list_init(&server.layer_surfaces);
    server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    wl_list_init(&server.toplevels);
    wl_list_init(&server.animations);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    /* Cursor theme/size follow XCURSOR_THEME / XCURSOR_SIZE like other desktops. */
    unsigned cursor_size = 24;
    const char *size_env = getenv("XCURSOR_SIZE");
    if (size_env && atoi(size_env) > 0) {
        cursor_size = (unsigned)atoi(size_env);
    } else {
        setenv("XCURSOR_SIZE", "24", 0);
    }
    server.cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cursor_size);
    server.cursor_mode = CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.display, "seat0");
    server.keyboard_focus_change.notify = handle_keyboard_focus_change;
    wl_signal_add(&server.seat->keyboard_state.events.focus_change, &server.keyboard_focus_change);
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    /* middle-click paste, clipboard managers, idle daemons, taskbars */
    wlr_primary_selection_v1_device_manager_create(server.display);
    server.request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection,
                  &server.request_set_primary_selection);
    wlr_data_control_manager_v1_create(server.display);
    server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.display);
    server.new_idle_inhibitor.notify = server_new_idle_inhibitor;
    wl_signal_add(&server.idle_inhibit_mgr->events.new_inhibitor, &server.new_idle_inhibitor);
    server.foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server.display);
    server.lock_mgr = wlr_session_lock_manager_v1_create(server.display);
    server.new_lock.notify = server_new_lock;
    wl_signal_add(&server.lock_mgr->events.new_lock, &server.new_lock);

    const char *vinput = getenv("SFWC_ENABLE_VIRTUAL_INPUT");
    if (vinput && strcmp(vinput, "1") == 0) {
        wlr_log(WLR_INFO, "virtual input protocols enabled (testing)");
        server.virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(server.display);
        server.new_virtual_pointer.notify = server_new_virtual_pointer;
        wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer,
                      &server.new_virtual_pointer);
        server.virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(server.display);
        server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
        wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
                      &server.new_virtual_keyboard);
    }

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return EXIT_FAILURE;
    }
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return EXIT_FAILURE;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    /* what portals and toolkits look at; keep values the user or the login manager set */
    setenv("XDG_CURRENT_DESKTOP", "SFWC", 0);
    setenv("XDG_SESSION_TYPE", "wayland", 0);
    if (!getenv("SFWC_NO_DBUS_ENV")) { /* tell D-Bus activated programs (portals) where we are */
        spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
              "XDG_SESSION_TYPE >/dev/null 2>&1");
    }
    for (size_t i = 0; i < server.config.n_autostart; i++) {
        char *cmd = expand_command(&server, server.config.autostart[i]);
        wlr_log(WLR_INFO, "autostart: %s", cmd);
        spawn(cmd);
        free(cmd);
    }
    if (startup_cmd) {
        spawn(startup_cmd);
    }
    wlr_log(WLR_INFO, "sfwc running on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.new_idle_inhibitor.link);
    wl_list_remove(&server.new_lock.link);
    if (server.cur_lock) {
        wl_list_remove(&server.lock_new_surface.link);
        wl_list_remove(&server.lock_unlock.link);
        wl_list_remove(&server.lock_destroy.link);
    }
    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_toplevel_decoration.link);
    wl_list_remove(&server.new_layer_surface.link);
    wl_list_remove(&server.keyboard_focus_change.link);
    if (server.virtual_pointer_mgr) {
        wl_list_remove(&server.new_virtual_pointer.link);
        wl_list_remove(&server.new_virtual_keyboard.link);
    }

    struct animation *anim, *anim_tmp;
    wl_list_for_each_safe(anim, anim_tmp, &server.animations, link) {
        wl_list_remove(&anim->link);
        free(anim);
    }
    if (server.inotify_source) {
        wl_event_source_remove(server.inotify_source);
    }
    if (server.inotify_fd >= 0) {
        close(server.inotify_fd);
    }
    free(server.config_path);
    free(server.config_name);
    config_finish(&server.config);
    theme_finish(&server.theme);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return EXIT_SUCCESS;
}
