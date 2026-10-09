/* Server-side decorations: titlebar, borders and shadow drawn into scene buffers. */
#include "server.h"

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

/* Show a cairo image in a scene buffer (takes ownership of the surface). */
void scene_buffer_set_cairo(struct wlr_scene_buffer *node, cairo_surface_t *surface)
{
    struct wlr_buffer *buffer = cairo_buffer_create(surface);
    wlr_scene_buffer_set_buffer(node, buffer);
    wlr_buffer_drop(buffer); /* the scene node holds its own lock */
}

/* The shadow must not catch pointer input. */
bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return false;
}

struct deco_insets toplevel_insets(struct toplevel *t)
{
    struct deco_insets none = {0};
    if (!t->ssd || t->fullscreen) {
        return none;
    }
    return deco_insets(&t->server->theme);
}

/* Window geometry including the server-side frame, in layout coordinates. */
struct wlr_box toplevel_outer(struct toplevel *t)
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
    t->focus_known = false; /* a new frame starts with the right colors, no transition */
}

/* (Re)create the frame for the current size, focus, title and theme; no-ops when nothing
 * changed. Removes the frame when the window is not server-side decorated or fullscreen. */
void frame_refresh(struct toplevel *t)
{
    struct server *server = t->server;
    if (!t->ssd || t->fullscreen || !t->xdg_toplevel->base->surface->mapped) {
        frame_destroy(t);
        return;
    }
    const struct theme *theme = &server->theme;
    struct wlr_box geo;
    geo = t->xdg_toplevel->base->geometry; /* wlroots 0.20: kept up to date on commit */
    if (geo.width <= 0 || geo.height <= 0) {
        return;
    }
    bool focused = server->seat->keyboard_state.focused_surface == t->xdg_toplevel->base->surface;
    double scale = frame_scale(server);
    const char *title = t->xdg_toplevel->title ? t->xdg_toplevel->title : "";

    if (!t->frame_tree) {
        t->frame_tree = wlr_scene_tree_create(t->scene_tree);
        t->shadow = wlr_scene_buffer_create(t->frame_tree, NULL);
        t->shadow->point_accepts_input = scene_buffer_no_input;
        t->chrome = wlr_scene_buffer_create(t->frame_tree, NULL);
        wlr_scene_node_lower_to_bottom(&t->frame_tree->node); /* behind the client surface */
        t->frame_cw = t->frame_ch = 0;
    }
    struct deco_insets in = deco_insets(theme);
    int ow = geo.width + in.left + in.right, oh = geo.height + in.top + in.bottom;

    /* the focus look: switches at once, or a plugin fades it (toplevel_focus) */
    double target = focused ? 1.0 : 0.0;
    if (!t->focus_known) {
        t->focus_known = true;
        t->focus_target = t->focus_mix = target;
    } else if (t->focus_target != target) {
        t->focus_target = target;
        if (!plugins_toplevel_focus(t, t->focus_mix, target)) {
            t->focus_mix = target;
        }
    }

    bool title_changed = !t->frame_title || strcmp(t->frame_title, title) != 0;
    if (t->frame_cw != geo.width || t->frame_ch != geo.height || t->frame_mix != t->focus_mix ||
        t->frame_scale != scale || t->frame_gen != server->theme_gen || title_changed) {
        cairo_surface_t *surf = deco_render_chrome(theme, geo.width, geo.height, t->focus_mix, title, scale);
        scene_buffer_set_cairo(t->chrome, surf);
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
        scene_buffer_set_cairo(t->shadow, surf);
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
    t->frame_mix = t->focus_mix;
    t->frame_scale = scale;
    t->frame_gen = server->theme_gen;
}

/* Answer the client's decoration request according to the config. */
void decoration_apply(struct toplevel *t)
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

void server_new_toplevel_decoration(struct wl_listener *listener, void *data)
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
