/* wlr-layer-shell: panels, wallpapers, launchers and the area they leave for windows. */
#include "server.h"

void focus_layer_surface(struct server *server, struct wlr_layer_surface_v1 *layer)
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
void arrange_layers(struct output *output)
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
    animate_layer_open(ls->server, ls->scene->tree);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data)
{
    struct layer_surface *ls = wl_container_of(listener, ls, unmap);
    struct server *server = ls->server;
    wlr_log(WLR_INFO, "layer surface unmapped: namespace=%s", ls->layer->namespace);
    animations_cancel_tree(server, ls->scene->tree);
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

void server_new_layer_surface(struct wl_listener *listener, void *data)
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
