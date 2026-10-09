/*
 * wlr-output-management: lets programs such as kanshi and wlr-randr read and change the outputs
 * (monitors): on/off, mode, scale, rotation, position, adaptive sync.
 */
#include "server.h"

/* What the clients see: all outputs with their current state. Called when anything changed. */
void output_mgmt_publish(struct server *server)
{
    if (!server->output_mgr) {
        return;
    }
    struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    if (!config) {
        return;
    }
    struct output *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, out->wlr_output);
        if (!head) {
            wlr_output_configuration_v1_destroy(config);
            return;
        }
        struct wlr_box box;
        if (wlr_output_layout_get(server->output_layout, out->wlr_output)) {
            wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
            head->state.x = box.x;
            head->state.y = box.y;
        } else {
            head->state.enabled = false; /* kept out of the layout: switched off for the user */
        }
    }
    wlr_output_manager_v1_set_configuration(server->output_mgr, config);
}

static void state_of_head(const struct wlr_output_configuration_head_v1 *head, struct wlr_output_state *state)
{
    wlr_output_state_set_enabled(state, head->state.enabled);
    if (!head->state.enabled) {
        return;
    }
    if (head->state.mode) {
        wlr_output_state_set_mode(state, head->state.mode);
    } else if (head->state.custom_mode.width > 0) {
        wlr_output_state_set_custom_mode(state, head->state.custom_mode.width, head->state.custom_mode.height,
                                         head->state.custom_mode.refresh);
    }
    wlr_output_state_set_scale(state, head->state.scale);
    wlr_output_state_set_transform(state, head->state.transform);
    wlr_output_state_set_adaptive_sync_enabled(state, head->state.adaptive_sync_enabled);
}

/* At least one output must stay on, or there would be nothing to see and nowhere to move windows to. */
static bool leaves_an_output(const struct wlr_output_configuration_v1 *config)
{
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        if (head->state.enabled) {
            return true;
        }
    }
    return false;
}

static bool apply_config(struct server *server, struct wlr_output_configuration_v1 *config, bool test)
{
    if (!leaves_an_output(config)) {
        wlr_log(WLR_ERROR, "output configuration refused: it would switch every output off");
        return false;
    }
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        state_of_head(head, &state);
        ok &= test ? wlr_output_test_state(head->state.output, &state)
                   : wlr_output_commit_state(head->state.output, &state);
        wlr_output_state_finish(&state);
    }
    if (test) {
        return ok;
    }

    /* layout: first move windows off outputs that go dark, then place the others */
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output *wo = head->state.output;
        if (head->state.enabled || !wlr_output_layout_get(server->output_layout, wo)) {
            continue;
        }
        struct toplevel *t;
        wl_list_for_each(t, &server->toplevels, link) {
            struct wlr_box b = toplevel_outer(t);
            if (wlr_output_layout_output_at(server->output_layout, b.x + b.width / 2.0, b.y + b.height / 2.0) == wo) {
                move_to_next_output(t);
            }
        }
        struct layer_surface *ls, *tmp;
        wl_list_for_each_safe(ls, tmp, &server->layer_surfaces, link) {
            if (ls->layer->output == wo) {
                wlr_layer_surface_v1_destroy(ls->layer);
            }
        }
        wlr_output_layout_remove(server->output_layout, wo);
    }
    wl_list_for_each(head, &config->heads, link) {
        if (!head->state.enabled) {
            continue;
        }
        struct output *out = output_of(server, head->state.output);
        if (out) {
            output_attach(out, true, head->state.x, head->state.y);
        }
    }

    /* windows that fill their output follow its new size */
    struct output *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (wlr_output_layout_get(server->output_layout, out->wlr_output)) {
            arrange_layers(out);
        }
    }
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t->maximized || t->fullscreen) {
            toplevel_apply_state(t, t->maximized, t->fullscreen);
        }
    }
    workspace_clamp(server);
    return ok;
}

static void handle_apply(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, output_mgr_apply);
    struct wlr_output_configuration_v1 *config = data;
    if (apply_config(server, config, false)) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
    output_mgmt_publish(server);
}

static void handle_test(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, output_mgr_test);
    struct wlr_output_configuration_v1 *config = data;
    if (apply_config(server, config, true)) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
}

static void handle_layout_change(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, layout_change);
    output_mgmt_publish(server);
}

void output_mgmt_init(struct server *server)
{
    server->output_mgr = wlr_output_manager_v1_create(server->display);
    server->output_mgr_apply.notify = handle_apply;
    wl_signal_add(&server->output_mgr->events.apply, &server->output_mgr_apply);
    server->output_mgr_test.notify = handle_test;
    wl_signal_add(&server->output_mgr->events.test, &server->output_mgr_test);
    server->layout_change.notify = handle_layout_change;
    wl_signal_add(&server->output_layout->events.change, &server->layout_change);
}

void output_mgmt_finish(struct server *server)
{
    wl_list_remove(&server->output_mgr_apply.link);
    wl_list_remove(&server->output_mgr_test.link);
    wl_list_remove(&server->layout_change.link);
    server->output_mgr = NULL;
}
