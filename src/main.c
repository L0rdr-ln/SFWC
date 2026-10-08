/*
 * SFWC - Simple Floating Wayland Compositor
 *
 * M1/M2: outputs + scene graph, xdg-shell windows, keyboard/pointer input,
 * click-to-focus, Alt+drag move/resize, maximize/fullscreen/minimize, edge
 * snapping, cascading placement, a few hard-coded keybinds.
 * Config, themes, decorations and animations come in later milestones
 * (see docs/ROADMAP.md). Structure follows wlroots' tinywl example.
 *
 * Built-in keybinds (modifier: Alt, so it works when nested in another
 * compositor that owns Super):
 *   Alt+Return  spawn terminal ($SFWC_TERMINAL, default "foot")
 *   Alt+q       close focused window
 *   Alt+Tab     cycle windows
 *   Alt+f       toggle maximize
 *   Alt+F11     toggle fullscreen
 *   Alt+m       minimize focused window
 *   Alt+Shift+m restore the most recently minimized window
 *   Alt+Escape  quit
 *   Alt+LMB     move window, Alt+RMB resize window
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define BIND_MODIFIER WLR_MODIFIER_ALT
#define GAP 8            /* px between windows/maximized windows and screen edge */
#define SNAP_DISTANCE 12 /* px within which a moved window snaps to screen edges */
#define CASCADE_STEP 32

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

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
};

struct output {
    struct wl_list link;
    struct server *server;
    struct wlr_output *wlr_output;
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
    struct wlr_box saved; /* window geometry before maximize/fullscreen */
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
};

struct popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* ---------------------------------------------------------------- spawn */

/* Run a shell command detached: double fork so we never leave zombies. */
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
static void toplevel_move_to(struct toplevel *t, int x, int y)
{
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(t->xdg_toplevel->base, &geo);
    wlr_scene_node_set_position(&t->scene_tree->node, x - geo.x, y - geo.y);
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

static struct toplevel *top_visible(struct server *server)
{
    struct toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (!t->minimized) {
            return t;
        }
    }
    return NULL;
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

    struct wlr_box box = output_box_at(t->server, ref.x + ref.width / 2.0, ref.y + ref.height / 2.0);
    if ((fs || max) && box.width > 0) {
        int inset = fs ? 0 : GAP;
        toplevel_move_to(t, box.x + inset, box.y + inset);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, box.width - 2 * inset, box.height - 2 * inset);
    } else if (!fs && !max) {
        toplevel_move_to(t, t->saved.x, t->saved.y);
        wlr_xdg_toplevel_set_size(t->xdg_toplevel, t->saved.width, t->saved.height);
    }
    if (t->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
    }
}

/* Cascade new windows from the top-left of the output under the cursor. */
static void place_new_toplevel(struct toplevel *t)
{
    struct server *server = t->server;
    struct wlr_box box = output_box_at(server, server->cursor->x, server->cursor->y);
    if (box.width <= 0) {
        return;
    }
    struct wlr_box geo = toplevel_geometry(t);
    int x, y;
    if (t->xdg_toplevel->parent) { /* dialog: center */
        x = box.x + (box.width - geo.width) / 2;
        y = box.y + (box.height - geo.height) / 2;
    } else {
        x = box.x + 48 + server->cascade;
        y = box.y + 48 + server->cascade;
        server->cascade = (server->cascade + CASCADE_STEP) % (6 * CASCADE_STEP);
    }
    int max_x = box.x + box.width - GAP - geo.width;
    int max_y = box.y + box.height - GAP - geo.height;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < box.x + GAP) {
        x = box.x + GAP;
    }
    if (y < box.y + GAP) {
        y = box.y + GAP;
    }
    toplevel_move_to(t, x, y);
}

/* -------------------------------------------------------------- focusing */

static void focus_toplevel(struct toplevel *toplevel)
{
    if (toplevel == NULL) {
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

    /* Raise to the top of the stacking order. */
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
}


static void toplevel_set_minimized(struct toplevel *t, bool minimize)
{
    struct server *server = t->server;
    if (!t->mapped || t->minimized == minimize) {
        return;
    }
    t->minimized = minimize;
    wlr_scene_node_set_enabled(&t->scene_tree->node, !minimize);
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
}

/* ------------------------------------------------------------- keyboard */

static bool handle_keybinding(struct server *server, xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server->display);
        return true;
    case XKB_KEY_Return: {
        const char *term = getenv("SFWC_TERMINAL");
        spawn(term && *term ? term : "foot");
        return true;
    }
    case XKB_KEY_q: {
        struct toplevel *top = top_visible(server);
        if (top) {
            wlr_xdg_toplevel_send_close(top->xdg_toplevel);
        }
        return true;
    }
    case XKB_KEY_Tab: {
        /* Focus the bottom-most visible window, which raises it. */
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (!t->minimized) {
                focus_toplevel(t);
                break;
            }
        }
        return true;
    }
    case XKB_KEY_f: {
        struct toplevel *top = top_visible(server);
        if (top) {
            toplevel_apply_state(top, !top->maximized, false);
        }
        return true;
    }
    case XKB_KEY_F11: {
        struct toplevel *top = top_visible(server);
        if (top) {
            toplevel_apply_state(top, top->maximized, !top->fullscreen);
        }
        return true;
    }
    case XKB_KEY_m: {
        struct toplevel *top = top_visible(server);
        if (top) {
            toplevel_set_minimized(top, true);
        }
        return true;
    }
    case XKB_KEY_M: { /* Alt+Shift+m */
        struct toplevel *t;
        wl_list_for_each_reverse(t, &server->toplevels, link) {
            if (t->minimized) {
                toplevel_set_minimized(t, false);
                break;
            }
        }
        return true;
    }
    default:
        return false;
    }
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

    uint32_t keycode = event->keycode + 8; /* libinput -> xkb */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if ((modifiers & BIND_MODIFIER) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled |= handle_keybinding(server, syms[i]);
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

static void server_new_keyboard(struct server *server, struct wlr_keyboard *wlr_keyboard)
{
    struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

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
        server_new_keyboard(server, wlr_keyboard_from_input_device(device));
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
    server_new_keyboard(server, &vkbd->keyboard);
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

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* --------------------------------------------------------------- cursor */

static struct toplevel *toplevel_at(struct server *server, double lx, double ly,
                                    struct wlr_surface **surface, double *sx, double *sy)
{
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;

    /* Walk up to the tree that belongs to a toplevel. */
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree ? tree->node.data : NULL;
}

static void reset_cursor_mode(struct server *server)
{
    server->cursor_mode = CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

static void begin_interactive(struct toplevel *toplevel, enum cursor_mode mode, uint32_t edges)
{
    struct server *server = toplevel->server;
    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
    if (toplevel->maximized || toplevel->fullscreen) {
        return; /* un-maximize first */
    }
    if (focused == NULL ||
        toplevel->xdg_toplevel->base->surface != wlr_surface_get_root_surface(focused)) {
        return; /* only the window under the pointer may start a grab */
    }
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

static void process_cursor_move(struct server *server)
{
    struct toplevel *toplevel = server->grabbed_toplevel;
    double nx = server->cursor->x - server->grab_x;
    double ny = server->cursor->y - server->grab_y;

    /* Snap the window's geometry to the edges of the output under the cursor. */
    struct wlr_box geo = {0};
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
    struct wlr_box box = output_box_at(server, server->cursor->x, server->cursor->y);
    if (box.width > 0) {
        double x = nx + geo.x, y = ny + geo.y;
        double left = box.x + GAP, right = box.x + box.width - GAP - geo.width;
        double top = box.y + GAP, bottom = box.y + box.height - GAP - geo.height;
        if (fabs(x - left) < SNAP_DISTANCE) {
            x = left;
        } else if (fabs(x - right) < SNAP_DISTANCE) {
            x = right;
        }
        if (fabs(y - top) < SNAP_DISTANCE) {
            y = top;
        } else if (fabs(y - bottom) < SNAP_DISTANCE) {
            y = bottom;
        }
        nx = x - geo.x;
        ny = y - geo.y;
    }
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
    struct toplevel *toplevel =
        toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
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
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
        reset_cursor_mode(server);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct toplevel *toplevel =
        toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    focus_toplevel(toplevel); /* click to focus + raise */

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    if (toplevel && (mods & BIND_MODIFIER) &&
        (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
        /* Alt+drag: do not forward the click to the client. */
        if (event->button == BTN_LEFT) {
            begin_interactive(toplevel, CURSOR_MOVE, 0);
        } else {
            struct wlr_box geo;
            wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
            uint32_t edges = (sx < geo.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                             (sy < geo.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
            begin_interactive(toplevel, CURSOR_RESIZE, edges);
        }
        return;
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
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

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
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

    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    wlr_log(WLR_INFO, "output %s added", wlr_output->name);
}

/* ------------------------------------------------------ xdg-shell windows */

static void toplevel_map(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wlr_log(WLR_INFO, "window mapped: title=%s app_id=%s",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(none)",
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(none)");
    toplevel->mapped = true;
    if (!toplevel->maximized && !toplevel->fullscreen) {
        place_new_toplevel(toplevel);
    }
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    wlr_log(WLR_INFO, "window unmapped");
    struct server *server = toplevel->server;
    if (toplevel == server->grabbed_toplevel) {
        reset_cursor_mode(server);
    }
    wl_list_remove(&toplevel->link);
    toplevel->mapped = false;
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
        if (toplevel->maximized || toplevel->fullscreen) {
            /* requested before the first commit: size it for the output now */
            toplevel_apply_state(toplevel, toplevel->maximized, toplevel->fullscreen);
        } else {
            /* Let the client pick its own size. */
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
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
    free(toplevel);
}

static void toplevel_request_move(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, CURSOR_MOVE, 0);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data)
{
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    begin_interactive(toplevel, CURSOR_RESIZE, event->edges);
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
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
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

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_popup *xdg_popup = data;

    struct popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ----------------------------------------------------------------- main */

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

    wlr_log_init(WLR_INFO, NULL);

    struct server server = {0};
    server.display = wl_display_create();
    if (!server.display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return EXIT_FAILURE;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    wl_event_loop_add_signal(loop, SIGINT, handle_signal, server.display);
    wl_event_loop_add_signal(loop, SIGTERM, handle_signal, server.display);

    server.backend = wlr_backend_autocreate(loop, NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return EXIT_FAILURE;
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
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    wl_list_init(&server.toplevels);
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
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

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
    wl_list_remove(&server.new_output.link);
    if (server.virtual_pointer_mgr) {
        wl_list_remove(&server.new_virtual_pointer.link);
        wl_list_remove(&server.new_virtual_keyboard.link);
    }

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return EXIT_SUCCESS;
}
