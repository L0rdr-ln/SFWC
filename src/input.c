/* Seat, keyboards and virtual input devices, key handling. */
#include "server.h"

/* Any input resets the idle timers (ext-idle-notify). */
void input_activity(struct server *server)
{
    if (server->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
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
            if (server->session) {
                wlr_session_change_vt(server->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
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
void apply_keyboard_config(struct server *server, struct keyboard *keyboard, bool set_keymap)
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

void server_new_input(struct wl_listener *listener, void *data)
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

void server_new_virtual_pointer(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    server_new_pointer(server, &event->new_pointer->pointer.base);
    update_seat_capabilities(server);
}

void server_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vkbd = data;
    server_new_keyboard(server, &vkbd->keyboard, true);
    update_seat_capabilities(server);
}

void seat_request_cursor(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

void seat_request_set_primary_selection(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void seat_request_set_selection(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}
