/*
 * Shared definitions of the compositor: the state structs and the functions that the modules
 * (window.c, output.c, input.c, ...) use from each other. See docs/ARCHITECTURE.md.
 */
#ifndef SFWC_SERVER_H
#define SFWC_SERVER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
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
#include <wlr/types/wlr_output_management_v1.h>
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
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
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
    struct wlr_session *session; /* NULL unless running on a TTY (VT switching) */
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
    struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wl_listener request_cursor_shape;
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener request_activate;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
    struct wl_listener new_constraint;
    struct wlr_pointer_constraint_v1 *active_constraint; /* of the surface under the pointer */
    struct wl_listener active_constraint_destroy;
    double constraint_ox, constraint_oy; /* layout position of the constrained surface */
    struct wl_list keyboards;
    struct wlr_output_manager_v1 *output_mgr;
    struct wl_listener output_mgr_apply, output_mgr_test, layout_change;
    struct wl_list pointer_devs; /* libinput devices, see pointer_config.c */
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
    struct wl_list plugins;    /* struct plugin_inst (plugin.c) */
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
    struct wlr_scene_tree *frame_tree;
    struct wlr_scene_buffer *chrome;
    struct wlr_scene_buffer *shadow;
    int frame_cw, frame_ch, shadow_w, shadow_h;
    bool frame_focused;
    /* focus transition of the frame colors (driven by a plugin, see toplevel_focus) */
    bool focus_known;
    double focus_target; /* 1 = focused look, 0 = unfocused look */
    double focus_mix;    /* what the frame should show now, 0..1 */
    double frame_mix;    /* what was drawn */
    int plugin_busy; /* plugins animating this window (toplevel_busy) */
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

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;
    bool is_virtual; /* virtual keyboards bring their own keymap */
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

#define MAX_OUTPUTS 16

/* settings.c */
char *expand_command(struct server *server, const char *in);
void spawn(const char *cmd);
void reload_config(struct server *server);
void init_config(struct server *server, struct wl_event_loop *loop);

uint32_t now_msec(void);

/* plugin.c: the hooks of include/sfwc-plugin.h that the rest of the compositor calls */
void plugins_reload(struct server *server);
void plugins_frame(struct server *server, uint32_t now);
void plugins_toplevel_map(struct toplevel *t);
void plugins_toplevel_commit(struct toplevel *t);
void plugins_toplevel_unmap(struct toplevel *t);
bool plugins_toplevel_move(struct toplevel *t, double from_x, double from_y, double to_x, double to_y);
void plugins_toplevel_cancel(struct toplevel *t);
bool plugins_toplevel_focus(struct toplevel *t, double from, double to);
void plugins_workspace_leaving(struct server *server, int old_ws, int new_ws);
void plugins_workspace_entered(struct server *server, int old_ws, int new_ws);
void plugins_layer_map(struct server *server, struct wlr_scene_tree *tree);
void plugins_layer_unmap(struct server *server, struct wlr_scene_tree *tree);
void plugins_finish(struct server *server);

/* protocols.c */
void protocols_init(struct server *server);
void protocols_finish(struct server *server);
void pointer_config_add(struct server *server, struct wlr_input_device *device);
void pointer_config_reload(struct server *server);
void constraints_init(struct server *server);
void constraints_finish(struct server *server);
/* Called with the surface under the pointer (NULL if none) and its origin in layout coordinates. */
void constraints_focus(struct server *server, struct wlr_surface *surface, double origin_x, double origin_y);
/* Limit a relative motion by the active constraint; returns false if the pointer must not move. */
bool constraints_limit(struct server *server, double *dx, double *dy);

/* window.c */
struct wlr_box toplevel_geometry(struct toplevel *t);
void toplevel_move_to_ex(struct toplevel *t, int x, int y, bool animate);
bool toplevel_shown(const struct toplevel *t);
struct toplevel *top_visible(struct server *server);
struct toplevel *focused_visible(struct server *server);
void toplevel_apply_state(struct toplevel *t, bool max, bool fs);
void focus_toplevel_ex(struct toplevel *toplevel, bool raise);
void focus_toplevel(struct toplevel *toplevel);
void toplevel_set_minimized(struct toplevel *t, bool minimize);
struct toplevel *toplevel_from_xdg(struct wlr_xdg_toplevel *xdg);
void handle_keyboard_focus_change(struct wl_listener *listener, void *data);
void toplevel_commit(struct wl_listener *listener, void *data);
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree);
void server_new_xdg_popup(struct wl_listener *listener, void *data);

/* workspace.c */
void switch_workspace(struct server *server, int ws);
void move_to_workspace(struct toplevel *t, int ws);
void workspace_clamp(struct server *server);

/* decoration.c */
void scene_buffer_set_cairo(struct wlr_scene_buffer *node, cairo_surface_t *surface);
bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy);
struct deco_insets toplevel_insets(struct toplevel *t);
struct wlr_box toplevel_outer(struct toplevel *t);
void frame_refresh(struct toplevel *t);
void decoration_apply(struct toplevel *t);
void server_new_toplevel_decoration(struct wl_listener *listener, void *data);

/* output.c */
struct wlr_box output_box_at(struct server *server, double x, double y);
struct wlr_box work_area_at(struct server *server, double x, double y);
void move_to_next_output(struct toplevel *t);
void focus_next_output(struct server *server);
void server_new_output(struct wl_listener *listener, void *data);
struct output *output_of(struct server *server, struct wlr_output *wlr_output);

/* input.c */
void input_activity(struct server *server);
void apply_keyboard_config(struct server *server, struct keyboard *keyboard, bool set_keymap);
void server_new_input(struct wl_listener *listener, void *data);
void server_new_virtual_pointer(struct wl_listener *listener, void *data);
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
void seat_request_cursor(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);

/* actions.c */
void dispatch_action(struct server *server, enum action action, const char *arg);

/* cursor.c */
void reset_cursor_mode(struct server *server);
void begin_interactive(struct toplevel *toplevel, enum cursor_mode mode, uint32_t edges,
                       bool check_focus);
void process_cursor_motion(struct server *server, uint32_t time);
void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);

/* layers.c */
void focus_layer_surface(struct server *server, struct wlr_layer_surface_v1 *layer);
void arrange_layers(struct output *output);
void output_attach(struct output *output, bool has_pos, int x, int y);

/* wlr-output-management (kanshi, wlr-randr): see output_mgmt.c */
void output_mgmt_init(struct server *server);
void output_mgmt_finish(struct server *server);
void output_mgmt_publish(struct server *server);
void server_new_layer_surface(struct wl_listener *listener, void *data);

/* lock.c */
void lock_update_bg(struct server *server);
void lock_new_surface(struct wl_listener *listener, void *data);
void server_new_lock(struct wl_listener *listener, void *data);

/* foreign.c */
void fth_sync(struct toplevel *t);
void fth_create(struct toplevel *t);
void fth_destroy(struct toplevel *t);

/* idle.c */
void server_new_idle_inhibitor(struct wl_listener *listener, void *data);

#endif
