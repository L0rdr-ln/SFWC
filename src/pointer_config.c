/*
 * [input] / [input:touchpad]: libinput settings (tap to click, natural scrolling, acceleration ...)
 * for the pointing devices that libinput drives. Other devices (virtual pointers, nested
 * backends) are left alone. Settings are applied when a device appears and again on every config
 * reload; a setting that is not in the config leaves the device's own default.
 */
#include "server.h"

#include <libinput.h>
#include <wlr/backend/libinput.h>

struct pointer_dev {
    struct wl_list link;
    struct server *server;
    struct wlr_input_device *device;
    struct libinput_device *handle;
    struct wl_listener destroy;
};

static void apply(struct libinput_device *dev, const struct input_cfg *c)
{
    if (c->tap >= 0 && libinput_device_config_tap_get_finger_count(dev) > 0) {
        libinput_device_config_tap_set_enabled(dev, c->tap ? LIBINPUT_CONFIG_TAP_ENABLED
                                                           : LIBINPUT_CONFIG_TAP_DISABLED);
    }
    if (c->tap_drag >= 0 && libinput_device_config_tap_get_finger_count(dev) > 0) {
        libinput_device_config_tap_set_drag_enabled(dev, c->tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED
                                                                      : LIBINPUT_CONFIG_DRAG_DISABLED);
    }
    if (c->tap_button_map[0] && libinput_device_config_tap_get_finger_count(dev) > 0) {
        libinput_device_config_tap_set_button_map(dev, !strcmp(c->tap_button_map, "lmr")
                                                           ? LIBINPUT_CONFIG_TAP_MAP_LMR
                                                           : LIBINPUT_CONFIG_TAP_MAP_LRM);
    }
    if (c->natural_scroll >= 0 && libinput_device_config_scroll_has_natural_scroll(dev)) {
        libinput_device_config_scroll_set_natural_scroll_enabled(dev, c->natural_scroll);
    }
    if (c->disable_while_typing >= 0 && libinput_device_config_dwt_is_available(dev)) {
        libinput_device_config_dwt_set_enabled(dev, c->disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED
                                                                            : LIBINPUT_CONFIG_DWT_DISABLED);
    }
    if (c->middle_emulation >= 0 && libinput_device_config_middle_emulation_is_available(dev)) {
        libinput_device_config_middle_emulation_set_enabled(
            dev, c->middle_emulation ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                                     : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
    }
    if (c->left_handed >= 0 && libinput_device_config_left_handed_is_available(dev)) {
        libinput_device_config_left_handed_set(dev, c->left_handed);
    }
    if (libinput_device_config_accel_is_available(dev)) {
        if (c->has_accel) {
            libinput_device_config_accel_set_speed(dev, c->accel_speed);
        }
        if (c->accel_profile[0]) {
            libinput_device_config_accel_set_profile(dev, !strcmp(c->accel_profile, "flat")
                                                              ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                                                              : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);
        }
    }
    if (c->click_method[0]) {
        enum libinput_config_click_method m = !strcmp(c->click_method, "clickfinger")
                                                  ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
                                                  : LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
        if (libinput_device_config_click_get_methods(dev) & m) {
            libinput_device_config_click_set_method(dev, m);
        }
    }
    if (c->scroll_method[0]) {
        enum libinput_config_scroll_method m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
        if (!strcmp(c->scroll_method, "two-finger")) {
            m = LIBINPUT_CONFIG_SCROLL_2FG;
        } else if (!strcmp(c->scroll_method, "edge")) {
            m = LIBINPUT_CONFIG_SCROLL_EDGE;
        } else if (!strcmp(c->scroll_method, "on-button-down")) {
            m = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
        }
        if (m == LIBINPUT_CONFIG_SCROLL_NO_SCROLL || (libinput_device_config_scroll_get_methods(dev) & m)) {
            libinput_device_config_scroll_set_method(dev, m);
        }
    }
}

static void apply_device(struct server *server, struct pointer_dev *pd)
{
    bool touchpad = libinput_device_config_tap_get_finger_count(pd->handle) > 0;
    struct input_cfg eff;
    if (touchpad) {
        input_cfg_merge(&eff, &server->config.input, &server->config.touchpad);
    } else {
        eff = server->config.input;
    }
    apply(pd->handle, &eff);
    wlr_log(WLR_DEBUG, "libinput settings applied to %s (%s)", pd->device->name,
            touchpad ? "touchpad" : "pointer");
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct pointer_dev *pd = wl_container_of(listener, pd, destroy);
    wl_list_remove(&pd->destroy.link);
    wl_list_remove(&pd->link);
    free(pd);
}

void pointer_config_add(struct server *server, struct wlr_input_device *device)
{
    if (!wlr_input_device_is_libinput(device)) {
        return;
    }
    struct pointer_dev *pd = calloc(1, sizeof *pd);
    if (!pd) {
        return;
    }
    pd->server = server;
    pd->device = device;
    pd->handle = wlr_libinput_get_device_handle(device);
    pd->destroy.notify = handle_destroy;
    wl_signal_add(&device->events.destroy, &pd->destroy);
    wl_list_insert(&server->pointer_devs, &pd->link);
    apply_device(server, pd);
}

void pointer_config_reload(struct server *server)
{
    struct pointer_dev *pd;
    wl_list_for_each(pd, &server->pointer_devs, link) {
        apply_device(server, pd);
    }
}
