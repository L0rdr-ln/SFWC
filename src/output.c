/* Outputs (monitors): layout, the work area, "next output" helpers, frame callbacks. */
#include "server.h"

/* Box of the output containing (x, y), or the center output; zero-sized if none. */
struct wlr_box output_box_at(struct server *server, double x, double y)
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
struct wlr_box work_area_at(struct server *server, double x, double y)
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

void move_to_next_output(struct toplevel *t)
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
void focus_next_output(struct server *server)
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

/* -------------------------------------------------------------- outputs */
static void output_frame(struct wl_listener *listener, void *data)
{
    struct output *output = wl_container_of(listener, output, frame);
    plugins_frame(output->server, now_msec());
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

void server_new_output(struct wl_listener *listener, void *data)
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

/* ------------------------------------------------------------ layer shell */
struct output *output_of(struct server *server, struct wlr_output *wlr_output)
{
    struct output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output == wlr_output) {
            return o;
        }
    }
    return NULL;
}
